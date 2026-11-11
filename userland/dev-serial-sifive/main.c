/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-serial-sifive — SiFive FU540/FU740 UART serial driver for RTMux
 *
 * Target: HiFive Unmatched (FU740). Also works on FU540 (Unleashed) and
 * anything that exposes the same "sifive,uart0" IP block.
 *
 * User-space I/O service that registers /dev/ser1. The hardware-specific code
 * owns MMIO and interrupt delivery; dev-serial-common supplies the terminal
 * line discipline and I/O service handlers.
 *
 * Usage: dev-serial-sifive [-b baud] [-p phys_addr] [-i irq] [-c pclk_hz]
 *
 * Register map (SiFive FU740-C000 manual v1p6, section 16): 0x00 TXDATA — bit
 * 31 = full; bits 7:0 = TX byte 0x04 RXDATA — bit 31 = empty; bits 7:0 = RX
 * byte 0x08 TXCTRL — bit 0 TXEN; bit 1 NSTOP; bits 18:16 TXCNT 0x0C RXCTRL —
 * bit 0 RXEN; bits 18:16 RXCNT 0x10 IE — bit 0 TXWM; bit 1 RXWM 0x14 IP — bit 0
 * TXWM; bit 1 RXWM 0x18 DIV — baud divisor: PCLK / baud - 1 on FU740 (the 16×
 * oversampling is internal — do NOT multiply by 16 in the divisor, contrary to
 * the 8250 rule)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/virtio-mmio-msg.h>
#include <arch/plic-vector.h>
#include <devctl/common.h>
#include <devctl/tty.h>

#define DBGPRINT_LITERAL(s) dbgprint((s), sizeof(s) - 1)

/* SiFive UART register offsets (32-bit accesses) */
#define SIFIVE_TXDATA 0x00
#define SIFIVE_RXDATA 0x04
#define SIFIVE_TXCTRL 0x08
#define SIFIVE_RXCTRL 0x0C
#define SIFIVE_IE 0x10
#define SIFIVE_IP 0x14
#define SIFIVE_DIV 0x18

#define TXDATA_FULL (1u << 31)
#define RXDATA_EMPTY (1u << 31)
#define DATA_MASK 0xffu

#define TXCTRL_TXEN (1u << 0)
#define TXCTRL_NSTOP (1u << 1) /* 0 = 1 stop bit, 1 = 2 */
#define TXCTRL_TXCNT_SHIFT 16
#define RXCTRL_RXEN (1u << 0)
#define RXCTRL_RXCNT_SHIFT 16

#define IE_TXWM (1u << 0)
#define IE_RXWM (1u << 1)

#define IP_TXWM (1u << 0) /* TX count below watermark (FIFO drained) */

/* Notification code used by the IST to notify the dispatch thread. */
#define NOTIFICATION_CODE_RX (_NOTIFICATION_CODE_MINAVAIL + 1)

/* Driver state */
static volatile uint32_t *uart_base;
static int uart_irq;
static int uart_vector;

static dispatch_t *dpp;
static int notification_coid;

/* UART register access (32-bit) */
static inline uint32_t uart_rd(unsigned off)
{
    return uart_base[off / 4];
}

static inline void uart_wr(unsigned off, uint32_t val)
{
    uart_base[off / 4] = val;
}

#include "../dev-serial-common/serial-rxbuf.h"

/* UART hardware init */
static void uart_hw_init(unsigned baud, unsigned pclk_hz, int set_baud)
{
    /* Disable interrupts while programming the peripheral. */
    uart_wr(SIFIVE_IE, 0);

    /* DIV register. By default we INHERIT whatever the bootloader programmed —
     * on real hardware the user's terminal, U-Boot and OpenSBI's console are
     * all running at that baud, and reprogramming with a wrong PCLK guess
     * instantly corrupts kernel SBI output.
     *
     * DIV is only written when the command line supplies both -b and -c, so the
     * divisor inputs are explicit.
     *
     *     F_baud = F_in / (DIV + 1)
     *
     * 16× oversampling is internal to the peripheral — do NOT multiply by 16
     * here (contrary to the 8250 convention). Matches U-Boot's sifive_serial
     * and Linux drivers/tty/serial/sifive.c.
     */
    if (set_baud) {
        uint32_t div = pclk_hz / baud;
        if (div > 0)
            div -= 1;
        uart_wr(SIFIVE_DIV, div);
    }

    /* 1 stop bit, watermark = 1 (interrupt as soon as TX FIFO has room) */
    uart_wr(SIFIVE_TXCTRL, TXCTRL_TXEN | (1u << TXCTRL_TXCNT_SHIFT));

    /* RX enabled, watermark = 0 (interrupt as soon as a byte arrives) */
    uart_wr(SIFIVE_RXCTRL, RXCTRL_RXEN | (0u << RXCTRL_RXCNT_SHIFT));

    /* Drain any garbage left in the RX FIFO from the bootloader */
    while (!(uart_rd(SIFIVE_RXDATA) & RXDATA_EMPTY))
        ;

    /* Enable RX watermark interrupt */
    uart_wr(SIFIVE_IE, IE_RXWM);
}

static int uart_drain_rx(void)
{
    int drained = 0;

    for (;;) {
        uint32_t r = uart_rd(SIFIVE_RXDATA);
        if (r & RXDATA_EMPTY)
            break;
        rxbuf_put((uint8_t) (r & DATA_MASK));
        drained = 1;
    }
    return drained;
}

/* Polled TX helper used by write() and echo processing. SiFive TX FIFO is 8
 * bytes deep. If full, wait on TXDATA_FULL.
 */
static inline void uart_putc(uint8_t c)
{
    while (uart_rd(SIFIVE_TXDATA) & TXDATA_FULL)
        ;
    uart_wr(SIFIVE_TXDATA, c);
}

/* IST (Interrupt Service Thread) */
static void *ist_thread(void *arg)
{
    int iid;
    (void) arg;

    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        DBGPRINT_LITERAL("dev-serial-sifive: thread_ctl IO failed\n");
        return NULL;
    }

    iid = interrupt_attach_thread(uart_vector, 0);
    if (iid == -1) {
        DBGPRINT_LITERAL("dev-serial-sifive: interrupt_attach_thread failed\n");
        return NULL;
    }

    /* Drain once before first unmask; UART state and notification channel are
     * initialized before the interrupt source can fire.
     */
    if (uart_drain_rx())
        msg_send_notification(notification_coid, 10, NOTIFICATION_CODE_RX, 0);
    __interrupt_unmask(uart_vector, iid);

    for (;;) {
        interrupt_wait(0, NULL);

        /* Drain the RX FIFO */
        (void) uart_drain_rx();

        msg_send_notification(notification_coid, 10, NOTIFICATION_CODE_RX, 0);

        __interrupt_unmask(uart_vector, iid);
    }

    return NULL;
}

/* TX helpers */
static inline void tx_byte(uint8_t c)
{
    uart_putc(c);
}

/* Block until the TX FIFO has drained. TXCTRL's watermark is 1, so IP_TXWM
 * asserts once the FIFO count drops below 1 (empty). The SiFive UART exposes no
 * shift-register-empty bit, so the very last byte may still be shifting out;
 * this is the closest drain the hardware reports.
 */
static void tx_drain(void)
{
    while (!(uart_rd(SIFIVE_IP) & IP_TXWM))
        ;
}

/* The SiFive UART has no break-control bit, so it cannot assert a line break.
 */
static int tx_break(void)
{
    return ENOSYS;
}

#include "../dev-serial-common/serial-service.h"

static int serial_delegate_mmio(unsigned long *phys_addr, int *irq)
{
    struct rtmux_resource_handle h;
    int rc = rtmux_delegate_device(RTMUX_DEVICE_SIFIVE_UART, 0x1000,
                                   RTMUX_DELEGATE_ANY_BASE, &h);

    if (rc != EOK)
        return rc;
    *phys_addr = (unsigned long) h.base;
    *irq = h.irq_vector;
    return EOK;
}

/* main */
int main(int argc, char *argv[])
{
    unsigned long phys_addr = 0;
    int irq = -1;
    unsigned baud = 115200;
    unsigned pclk_hz = 0; /* must be supplied via -c to actually program DIV */
    int set_baud = 0; /* 1 when the user explicitly requested a baud change */
    int opt;
    pthread_t ist_tid;
    int manual = 0;

    while ((opt = getopt(argc, argv, "b:p:i:c:")) != -1) {
        switch (opt) {
        case 'b':
            baud = (unsigned) strtoul(optarg, NULL, 0);
            set_baud = 1;
            break;
        case 'p':
            phys_addr = strtoul(optarg, NULL, 0);
            manual = 1;
            break;
        case 'i':
            irq = (int) strtol(optarg, NULL, 0);
            manual = 1;
            break;
        case 'c':
            pclk_hz = (unsigned) strtoul(optarg, NULL, 0);
            break;
        default:
            DBGPRINT_LITERAL(
                "usage: dev-serial-sifive [-b baud] [-p addr] [-i irq] [-c "
                "pclk]\n");
            return 1;
        }
    }

    if (manual) {
        DBGPRINT_LITERAL("dev-serial-sifive: -p/-i raw MMIO is unsupported\n");
        return 1;
    }
    if (serial_delegate_mmio(&phys_addr, &irq) != EOK)
        return 0;
    if (phys_addr == 0 || irq < 0) {
        return 0;
    }

    uart_irq = irq;
    uart_vector = PLIC_VECTOR_BASE + irq;

    uart_base = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                     MAP_PHYS | MAP_SHARED, NOFD, (off_t) phys_addr);
    if (uart_base == MAP_FAILED) {
        DBGPRINT_LITERAL("dev-serial-sifive: mmap failed\n");
        rtmux_hwres_release_range(phys_addr, 0x1000, RTMUX_HWRES_MEMORY);
        return 1;
    }

    /* Only reprogram the baud when the user explicitly supplied BOTH -b (target
     * baud) and -c (peripheral clock). Otherwise inherit whatever
     * U-Boot/OpenSBI already programmed — that is the rate the terminal (and
     * the kernel's SBI console on the same UART) are currently running at.
     */
    uart_hw_init(baud, pclk_hz, set_baud && pclk_hz != 0);

    serial_termios_init();

    dpp = dispatch_create();
    if (!dpp) {
        DBGPRINT_LITERAL("dev-serial-sifive: dispatch_create failed\n");
        return 1;
    }

    if (ioserv_attach(dpp, NULL, "/dev/ser1", _FTYPE_ANY, 0, &ser_connect_funcs,
                      &ser_io_funcs, NULL) == -1) {
        DBGPRINT_LITERAL("dev-serial-sifive: ioserv_attach failed\n");
        return 1;
    }

    notification_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (notification_coid == -1) {
        DBGPRINT_LITERAL("dev-serial-sifive: message_connect failed\n");
        return 1;
    }

    notification_attach(dpp, 0, NOTIFICATION_CODE_RX, rx_notification_handler,
                        NULL);

    if (pthread_create(&ist_tid, NULL, ist_thread, NULL) != 0) {
        DBGPRINT_LITERAL("dev-serial-sifive: pthread_create failed\n");
        return 1;
    }

    rtmux_detach(EXIT_SUCCESS);

    dispatch_context_t *dctp = dispatch_context_alloc(dpp);
    if (!dctp) {
        DBGPRINT_LITERAL("dev-serial-sifive: dispatch_context_alloc failed\n");
        return 1;
    }
    for (;;) {
        dctp = dispatch_block(dctp);
        if (dctp)
            dispatch_handler(dctp);
    }

    return 0;
}
