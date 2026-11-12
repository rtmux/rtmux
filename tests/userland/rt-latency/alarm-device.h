/* SPDX-License-Identifier: Apache-2.0 */
/* Platform alarm backends for the SAME IRQ outcome/timeout/priority oracles.
 * RV virt: goldfish RTC. Pi3: BCM2835 system timer compare 3 (1 MHz).
 * Scheduling continues to use each architecture's separate CPU timer. */
#ifndef RTMUX_TEST_ALARM_DEVICE_H
#define RTMUX_TEST_ALARM_DEVICE_H
#include <stdint.h>
#include <sys/rtmux-core.h>
#include <sys/virtio-mmio-msg.h>

#if defined(__aarch64__)
#define TEST_ALARM_DEVICE RTMUX_DEVICE_BCM2835_TIMER
#define TEST_ALARM_NAME "bcm2835-system-timer"
#else
#define TEST_ALARM_DEVICE RTMUX_DEVICE_GOLDFISH_RTC
#define TEST_ALARM_NAME "goldfish-rtc"
#endif

static inline uint32_t alarm_rd(volatile unsigned char *base, unsigned off)
{
    return *(volatile uint32_t *) (base + off);
}
static inline void alarm_wr(volatile unsigned char *base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *) (base + off) = v;
#if defined(__aarch64__)
    __asm__ volatile("dsb osh" ::: "memory");
#endif
}
static inline uint64_t alarm_now(volatile unsigned char *base)
{
#if defined(__aarch64__)
    uint32_t hi, lo, after;
    do {
        hi = alarm_rd(base, 8);
        lo = alarm_rd(base, 4);
        after = alarm_rd(base, 8);
    } while (hi != after);
    return (((uint64_t) hi << 32) | lo) * 1000;
#else
    uint32_t lo = alarm_rd(base, 0), hi = alarm_rd(base, 4);
    return ((uint64_t) hi << 32) | lo;
#endif
}
static inline void alarm_clear(volatile unsigned char *base)
{
#if defined(__aarch64__)
    alarm_wr(base, 0, 1U << 3);
#else
    alarm_wr(base, 0x1c, 1);
#endif
}
static inline void alarm_reset(volatile unsigned char *base)
{
#if defined(__aarch64__)
    /* No enable bit: move compare 3 behind the counter, away for one wrap. */
    alarm_wr(base, 0x18, alarm_rd(base, 4) - 1);
#else
    alarm_wr(base, 0x10, 0);
    alarm_wr(base, 0x14, 1);
#endif
    alarm_clear(base);
}
static inline void alarm_arm(volatile unsigned char *base, uint64_t when, int enabled)
{
#if defined(__aarch64__)
    /* Keep the alarm running in the negative control, but gate its IRQ at
     * the controller because the BCM timer itself has no IRQ-enable bit.
     * The next attempt's prepare() balances this mask. */
    if (!enabled)
        (void) __interrupt_mask(32 + 3, -1);
    alarm_wr(base, 0x18, (uint32_t) ((when + 999) / 1000));
#else
    alarm_wr(base, 0x10, enabled ? 1 : 0);
    alarm_wr(base, 0x0c, (uint32_t) (when >> 32));
    alarm_wr(base, 0x08, (uint32_t) when);
#endif
}
#endif
