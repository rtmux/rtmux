/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-serial-8250 — 16550a UART serial driver for RTMux
 *
 * User-space I/O service that registers /dev/ser1. The hardware-specific code
 * owns MMIO and interrupt delivery; dev-serial-common supplies the terminal
 * line discipline and I/O service handlers.
 *
 * Usage: dev-serial-8250 [-b baud] [-p phys_addr] [-i irq]
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
#include <time.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/virtio-mmio-msg.h>
#include <arch/plic-vector.h>
#include <devctl/common.h>
#include <devctl/tty.h>

#define DBGPRINT_LITERAL(s) dbgprint((s), sizeof(s) - 1)

/* 16550a register offsets */
#define RHR 0 /* Receive Holding Register */
#define THR 0 /* Transmit Holding Register */
#define IER 1 /* Interrupt Enable Register */
#define IER_RX_ENABLE (1 << 0)
#define IER_TX_ENABLE (1 << 1)
#define FCR 2 /* FIFO Control Register */
#define FCR_FIFO_ENABLE (1 << 0)
#define FCR_FIFO_CLEAR (3 << 1)
#define ISR 2 /* Interrupt Status Register */
#define LCR 3 /* Line Control Register */
#define LCR_EIGHT_BITS (3 << 0)
#define LCR_BREAK (1 << 6) /* force a continuous break (spacing) on TX */
#define LCR_BAUD_LATCH (1 << 7)
#define MCR 4             /* Modem Control Register */
#define MCR_OUT2 (1 << 3) /* Gate interrupt to bus */
#define LSR 5             /* Line Status Register */
#define LSR_RX_READY (1 << 0)
#define LSR_TX_IDLE (1 << 5)  /* THR empty: room for another byte */
#define LSR_TX_EMPTY (1 << 6) /* THR and shift register both empty */
#define MSR 6                 /* Modem Status Register */

/* Notification code used by the IST to notify the dispatch thread. */
#define NOTIFICATION_CODE_RX (_NOTIFICATION_CODE_MINAVAIL + 1)

/* Driver state */
static volatile uint8_t *uart_base;
static int uart_irq;    /* raw PLIC IRQ number */
static int uart_vector; /* PLIC_VECTOR_BASE + uart_irq */

static dispatch_t *dpp;
static int notification_coid; /* connection for IST → main notifications */

/* UART register access */
static inline uint8_t uart_read(int reg)
{
    return uart_base[reg];
}

static inline void uart_write(int reg, uint8_t val)
{
    uart_base[reg] = val;
}

#include "../dev-serial-common/serial-rxbuf.h"

/* UART hardware init */
static void uart_hw_init(unsigned baud)
{
    /* Disable all interrupts while programming the UART. */
    uart_write(IER, 0x00);

    /* Set baud rate */
    uart_write(LCR, LCR_BAUD_LATCH);
    unsigned divisor = 115200 / baud; /* assuming 1.8432 MHz crystal */
    uart_write(0, (uint8_t) (divisor & 0xff));
    uart_write(1, (uint8_t) (divisor >> 8));

    /* 8N1, leave baud latch mode */
    uart_write(LCR, LCR_EIGHT_BITS);

    /* Enable and clear FIFOs */
    uart_write(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

    /* Clear stale interrupt conditions. Read LSR, RHR, IIR, MSR to deassert the
     * interrupt line so the PLIC can observe the next edge/level transition.
     */
    (void) uart_read(LSR);
    (void) uart_read(RHR);
    (void) uart_read(ISR);
    (void) uart_read(MSR);

    /* Enable OUT2 to gate interrupt output to the PLIC */
    uart_write(MCR, MCR_OUT2);

    /* Enable RX interrupt (TX stays polled) */
    uart_write(IER, IER_RX_ENABLE);

    /* Clear again after enabling RX interrupts. */
    (void) uart_read(LSR);
    (void) uart_read(RHR);
    (void) uart_read(ISR);
    (void) uart_read(MSR);
}

static int uart_drain_rx(void)
{
    int drained = 0;

    while (uart_read(LSR) & LSR_RX_READY) {
        rxbuf_put(uart_read(RHR));
        drained = 1;
    }
    return drained;
}

/* IST (Interrupt Service Thread) */
static void *ist_thread(void *arg)
{
    int iid;
    (void) arg;

    /* The IST needs I/O privilege for interrupt_attach_thread/interrupt_wait.
     */
    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        DBGPRINT_LITERAL("dev-serial-8250: thread_ctl IO failed\n");
        return NULL;
    }

    /* Attach to the UART interrupt */
    iid = interrupt_attach_thread(uart_vector, 0);
    if (iid == -1) {
        DBGPRINT_LITERAL("dev-serial-8250: interrupt_attach_thread failed\n");
        return NULL;
    }

    /* Drain once before first unmask; UART state and notification channel are
     * initialized before the interrupt source can fire.
     */
    if (uart_drain_rx())
        msg_send_notification(notification_coid, 10, NOTIFICATION_CODE_RX, 0);
    __interrupt_unmask(uart_vector, iid);

    for (;;) {
        /* Block until UART interrupt fires */
        interrupt_wait(0, NULL);

        /* Drain UART RX FIFO into circular buffer */
        (void) uart_drain_rx();

        /* Notify main thread that data is available */
        msg_send_notification(notification_coid, 10, NOTIFICATION_CODE_RX, 0);

        /* Re-enable the interrupt */
        __interrupt_unmask(uart_vector, iid);
    }

    return NULL;
}

/* TX helpers */
static inline void tx_byte(uint8_t c)
{
    while (!(uart_read(LSR) & LSR_TX_IDLE))
        ;
    uart_write(THR, c);
}

/* Block until the transmitter is fully idle (holding and shift registers both
 * empty), so tcdrain() only returns once the last byte is on the wire.
 */
static void tx_drain(void)
{
    while (!(uart_read(LSR) & LSR_TX_EMPTY))
        ;
}

/* Assert a line break for tcsendbreak(). Wait for the transmitter to go idle so
 * the break does not truncate a byte in flight, hold the break bit for an
 * implementation-defined interval (POSIX allows 0.25-0.5s for duration 0), then
 * release it. The single dispatch thread is blocked meanwhile, which is
 * inherent — the line is unusable during a break anyway.
 */
static int tx_break(void)
{
    struct timespec req = {.tv_sec = 0, .tv_nsec = 300 * 1000 * 1000};
    uint8_t lcr;

    tx_drain();

    /* Preserve the current line settings (word length, parity, stop bits); only
     * toggle the break bit, so this stays correct if tcsetattr ever reprograms
     * LCR.
     */
    lcr = uart_read(LCR);
    uart_write(LCR, lcr | LCR_BREAK);
    (void) nanosleep(&req, NULL);
    uart_write(LCR, lcr);
    return EOK;
}

#include "../dev-serial-common/serial-service.h"

static int serial_delegate_mmio(unsigned long *phys_addr, int *irq)
{
    struct rtmux_resource_handle h;
    int rc = rtmux_delegate_device(RTMUX_DEVICE_NS16550, 0x1000,
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
    unsigned baud = 38400;
    int opt;
    pthread_t ist_tid;
    int manual = 0;

    /* Command-line overrides */
    while ((opt = getopt(argc, argv, "b:p:i:")) != -1) {
        switch (opt) {
        case 'b':
            baud = (unsigned) strtoul(optarg, NULL, 0);
            break;
        case 'p':
            phys_addr = strtoul(optarg, NULL, 0);
            manual = 1;
            break;
        case 'i':
            irq = (int) strtol(optarg, NULL, 0);
            manual = 1;
            break;
        default:
            DBGPRINT_LITERAL(
                "usage: dev-serial-8250 [-b baud] [-p addr] [-i irq]\n");
            return 1;
        }
    }

    if (manual) {
        DBGPRINT_LITERAL("dev-serial-8250: -p/-i raw MMIO is unsupported\n");
        return 1;
    }
    if (serial_delegate_mmio(&phys_addr, &irq) != EOK) {
        DBGPRINT_LITERAL("dev-serial-8250: no delegated UART MMIO\n");
        return 1;
    }
    if (phys_addr == 0 || irq < 0) {
        DBGPRINT_LITERAL("dev-serial-8250: invalid delegated UART MMIO\n");
        return 1;
    }

    uart_irq = irq;
    uart_vector = PLIC_VECTOR_BASE + irq;

    /* Map UART MMIO registers */
    uart_base = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                     MAP_PHYS | MAP_SHARED, NOFD, (off_t) phys_addr);
    if (uart_base == MAP_FAILED) {
        DBGPRINT_LITERAL("dev-serial-8250: mmap failed\n");
        rtmux_hwres_release_range(phys_addr, 0x1000, RTMUX_HWRES_MEMORY);
        return 1;
    }

    /* Initialize UART hardware */
    uart_hw_init(baud);

    serial_termios_init();

    /* Create dispatch */
    dpp = dispatch_create();
    if (!dpp) {
        DBGPRINT_LITERAL("dev-serial-8250: dispatch_create failed\n");
        return 1;
    }

    /* Attach I/O service at /dev/ser1 */
    if (ioserv_attach(dpp, NULL, "/dev/ser1", _FTYPE_ANY, 0, &ser_connect_funcs,
                      &ser_io_funcs, NULL) == -1) {
        DBGPRINT_LITERAL("dev-serial-8250: ioserv_attach failed\n");
        return 1;
    }

    /* Create the side-channel connection used by the IST notification. */
    notification_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (notification_coid == -1) {
        DBGPRINT_LITERAL("dev-serial-8250: message_connect failed\n");
        return 1;
    }

    /* Register notification handler */
    notification_attach(dpp, 0, NOTIFICATION_CODE_RX, rx_notification_handler,
                        NULL);

    /* Spawn IST thread */
    if (pthread_create(&ist_tid, NULL, ist_thread, NULL) != 0) {
        DBGPRINT_LITERAL("dev-serial-8250: pthread_create failed\n");
        return 1;
    }

    /* Detach from parent after initialization is complete. */
    rtmux_detach(EXIT_SUCCESS);

    /* Enter dispatch loop */
    dispatch_context_t *dctp = dispatch_context_alloc(dpp);
    if (!dctp) {
        DBGPRINT_LITERAL("dev-serial-8250: dispatch_context_alloc failed\n");
        return 1;
    }
    for (;;) {
        dctp = dispatch_block(dctp);
        if (dctp)
            dispatch_handler(dctp);
    }

    return 0; /* not reached */
}
