/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-serial-pl011 — PL011 UART serial driver for RTMux
 *
 * User-space I/O service that registers /dev/ser1. The hardware-specific code
 * owns MMIO and interrupt delivery; dev-serial-common supplies the terminal
 * line discipline and I/O service handlers.
 *
 * Usage: dev-serial-pl011 (foreground; start with &)
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

/* PL011 register offsets, all 32-bit MMIO. */
#define DR 0x00
#define FR 0x18
#define LCRH 0x2c
#define CR 0x30
#define IMSC 0x38
#define ICR 0x44
#define RXFE (1U << 4)
#define TXFF (1U << 5)
#define BUSY (1U << 3)
#define RX_IRQ ((1U << 4) | (1U << 6))
#define NOTIFICATION_CODE_RX (_NOTIFICATION_CODE_MINAVAIL + 1)
static volatile uint32_t *uart_base;
static int uart_vector;
static dispatch_t *dpp;
static int notification_coid;
static int ist_ready;

static uint32_t uart_read(unsigned offset) { return uart_base[offset / 4]; }
static void uart_write(unsigned offset, uint32_t value)
{
    uart_base[offset / 4] = value;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
#include "../dev-serial-common/serial-rxbuf.h"

static void uart_hw_init(void)
{
    uart_write(IMSC, 0);
    /* Keep the firmware's baud divisor. Disable FIFO so a single received
     * byte raises RX rather than waiting for a FIFO threshold. */
    uart_write(LCRH, 3U << 5); /* eight data bits, no parity, one stop bit */
    uart_write(ICR, 0x7ff);
    uart_write(CR, (1U << 9) | (1U << 8) | 1U);
}
static int uart_drain_rx(void)
{
    int drained = 0;
    while (!(uart_read(FR) & RXFE)) {
        rxbuf_put((uint8_t) uart_read(DR));
        drained = 1;
    }
    uart_write(ICR, RX_IRQ);
    return drained;
}
static void tx_byte(uint8_t c)
{
    while (uart_read(FR) & TXFF)
        ;
    uart_write(DR, c);
}
static void tx_drain(void)
{
    while (uart_read(FR) & BUSY)
        ;
}
static int tx_break(void)
{
    struct timespec delay = {.tv_nsec = 300000000};
    uint32_t lcr;
    tx_drain();
    lcr = uart_read(LCRH);
    uart_write(LCRH, lcr | 1U);
    (void) nanosleep(&delay, NULL);
    uart_write(LCRH, lcr);
    return EOK;
}
/* IST (Interrupt Service Thread) */
static void *ist_thread(void *arg)
{
    int iid;
    (void) arg;

    /* The IST needs I/O privilege for interrupt_attach_thread/interrupt_wait.
     */
    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        DBGPRINT_LITERAL("dev-serial-pl011: thread_ctl IO failed\n");
        __atomic_store_n(&ist_ready, -1, __ATOMIC_RELEASE);
        return NULL;
    }

    /* Attach to the UART interrupt */
    iid = interrupt_attach_thread(uart_vector, 0);
    if (iid == -1) {
        DBGPRINT_LITERAL("dev-serial-pl011: interrupt_attach_thread failed\n");
        __atomic_store_n(&ist_ready, -1, __ATOMIC_RELEASE);
        return NULL;
    }

    /* Drain once before first unmask; UART state and notification channel are
     * initialized before the interrupt source can fire.
     */
    if (uart_drain_rx())
        msg_send_notification(notification_coid, 10, NOTIFICATION_CODE_RX, 0);
    uart_write(IMSC, RX_IRQ);
    __interrupt_unmask(uart_vector, iid);
    __atomic_store_n(&ist_ready, 1, __ATOMIC_RELEASE);

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

#include "../dev-serial-common/serial-service.h"

static int serial_delegate_mmio(unsigned long *phys_addr, int *irq)
{
    struct rtmux_resource_handle h;
    int rc = rtmux_delegate_device(RTMUX_DEVICE_PL011, 0x1000,
                                   RTMUX_DELEGATE_ANY_BASE, &h);

    if (rc != EOK)
        return rc;
    *phys_addr = (unsigned long) h.base;
    *irq = h.irq_vector;
    return EOK;
}

/* main */
int main(void)
{
    unsigned long phys_addr = 0;
    int irq = -1;
    pthread_t ist_tid;
    if (serial_delegate_mmio(&phys_addr, &irq) != EOK) {
        DBGPRINT_LITERAL("dev-serial-pl011: no delegated UART MMIO\n");
        return 1;
    }
    if (phys_addr == 0 || irq < 0) {
        DBGPRINT_LITERAL("dev-serial-pl011: invalid delegated UART MMIO\n");
        return 1;
    }

    uart_vector = PLIC_VECTOR_BASE + irq;

    /* Map UART MMIO registers */
    uart_base = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                     MAP_PHYS | MAP_SHARED, NOFD, (off_t) phys_addr);
    if (uart_base == MAP_FAILED) {
        DBGPRINT_LITERAL("dev-serial-pl011: mmap failed\n");
        rtmux_hwres_release_range(phys_addr, 0x1000, RTMUX_HWRES_MEMORY);
        return 1;
    }

    /* Initialize UART hardware */
    uart_hw_init();

    serial_termios_init();

    /* Create dispatch */
    dpp = dispatch_create();
    if (!dpp) {
        DBGPRINT_LITERAL("dev-serial-pl011: dispatch_create failed\n");
        return 1;
    }

    /* Attach I/O service at /dev/ser1 */
    if (ioserv_attach(dpp, NULL, "/dev/ser1", _FTYPE_ANY, 0, &ser_connect_funcs,
                      &ser_io_funcs, NULL) == -1) {
        DBGPRINT_LITERAL("dev-serial-pl011: ioserv_attach failed\n");
        return 1;
    }

    /* Create the side-channel connection used by the IST notification. */
    notification_coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (notification_coid == -1) {
        DBGPRINT_LITERAL("dev-serial-pl011: message_connect failed\n");
        return 1;
    }

    /* Register notification handler */
    notification_attach(dpp, 0, NOTIFICATION_CODE_RX, rx_notification_handler,
                        NULL);

    /* Spawn IST thread */
    if (pthread_create(&ist_tid, NULL, ist_thread, NULL) != 0) {
        DBGPRINT_LITERAL("dev-serial-pl011: pthread_create failed\n");
        return 1;
    }

    /* Do not announce readiness until IRQ attachment has succeeded. */
    for (int i = 0; i < 500 && !__atomic_load_n(&ist_ready, __ATOMIC_ACQUIRE); i++) {
        struct timespec delay = {.tv_nsec = 10000000};
        (void) nanosleep(&delay, NULL);
    }
    if (__atomic_load_n(&ist_ready, __ATOMIC_ACQUIRE) != 1)
        return 1;
    puts("dev-serial-pl011: READY");

    /* Enter dispatch loop */
    dispatch_context_t *dctp = dispatch_context_alloc(dpp);
    if (!dctp) {
        DBGPRINT_LITERAL("dev-serial-pl011: dispatch_context_alloc failed\n");
        return 1;
    }
    for (;;) {
        dctp = dispatch_block(dctp);
        if (dctp)
            dispatch_handler(dctp);
    }

    return 0; /* not reached */
}
