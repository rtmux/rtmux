/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

/* Polling PL011 console. QEMU raspi3b routes its first serial port here. */
void pi3_uart_init(void)
{
    pi3_write32(PI3_UART + 0x30, 0);
    pi3_write32(PI3_UART + 0x38, 0);
    pi3_write32(PI3_UART + 0x44, 0x7ff);
    /* Divisors for a 48 MHz UART clock: 115200, 8N1, FIFOs enabled.
     * Physical-board clock and GPIO setup are outside this QEMU bootstrap. */
    pi3_write32(PI3_UART + 0x24, 26);
    pi3_write32(PI3_UART + 0x28, 3);
    pi3_write32(PI3_UART + 0x2c, 0x70);
    pi3_write32(PI3_UART + 0x30, 0x301);
}

static void putc(char c)
{
    while (pi3_read32(PI3_UART + 0x18) & (1U << 5))
        __asm__ volatile("yield");
    pi3_write32(PI3_UART, (unsigned char) c);
}

void pi3_puts(const char *text)
{
    while (*text) {
        if (*text == '\n')
            putc('\r');
        putc(*text++);
    }
}

void pi3_hex(uint64_t value)
{
    pi3_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        putc("0123456789abcdef"[(value >> shift) & 15]);
}

void pi3_pass(const char *test)
{
    pi3_puts("PASS ");
    pi3_puts(test);
    pi3_puts("\n");
}

_Noreturn void pi3_halt(void)
{
    pi3_irq_disable();
    for (;;)
        __asm__ volatile("wfi");
}

_Noreturn void pi3_fail(const char *test)
{
    pi3_irq_disable();
    pi3_puts("FAIL ");
    pi3_puts(test);
    pi3_puts("\n");
    pi3_halt();
}
