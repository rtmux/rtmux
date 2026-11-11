/*
 * Shared raw RX ring for user-space serial drivers.
 *
 * The IST writes bytes with rxbuf_put(); the dispatch thread consumes bytes
 * through rxbuf_get() before the terminal line discipline cooks them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_DEVC_COMMON_SERIAL_RXBUF_H
#define RTMUX_DEVC_COMMON_SERIAL_RXBUF_H

#include <stdint.h>

#define RTMUX_SERIAL_RXBUF_SIZE 256

static uint8_t rxbuf[RTMUX_SERIAL_RXBUF_SIZE];
static volatile unsigned rx_head;
static volatile unsigned rx_tail;

static inline int rxbuf_empty(void)
{
    return rx_head == rx_tail;
}

static int rxbuf_get(void)
{
    uint8_t c;

    if (rxbuf_empty())
        return -1;
    c = rxbuf[rx_tail % RTMUX_SERIAL_RXBUF_SIZE];
    rx_tail++;
    return c;
}

static void rxbuf_put(uint8_t c)
{
    unsigned next = rx_head + 1;

    if ((next - rx_tail) <= RTMUX_SERIAL_RXBUF_SIZE) {
        rxbuf[rx_head % RTMUX_SERIAL_RXBUF_SIZE] = c;
        rx_head = next;
    }
}

#endif
