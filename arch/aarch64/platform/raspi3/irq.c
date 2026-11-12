/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

/* BCM2836 ARM-local block. No GIC, PSCI or SBI on this board. */
#define TIMER_CONTROL(cpu) (PI3_LOCAL + 0x40U + (cpu) * 4U)
#define MAIL_CONTROL(cpu) (PI3_LOCAL + 0x50U + (cpu) * 4U)
#define IRQ_SOURCE(cpu) (PI3_LOCAL + 0x60U + (cpu) * 4U)
#define MAIL_SET(cpu) (PI3_LOCAL + 0x80U + (cpu) * 16U)
#define MAIL_CLEAR(cpu) (PI3_LOCAL + 0xc0U + (cpu) * 16U)
#define TIMER_IRQ (1U << 1) /* CNTPNSIRQ */
#define MAIL_IRQ (1U << 4)  /* mailbox 0 */

static unsigned online[PI3_CPUS];
static unsigned ipi_received[PI3_CPUS];
static unsigned acknowledgements;
static volatile unsigned timer_ticks;
static uint64_t timer_interval;
static bool user_timer;

static void timer_arm(void)
{
    uint64_t deadline = pi3_counter() + timer_interval;
    uint64_t enable = 1;
    __asm__ volatile(
        "msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" ::"r"(deadline),
        "r"(enable)
        : "memory");
}

void pi3_irq(bool from_user)
{
    unsigned cpu = pi3_cpu();
    if (cpu >= PI3_CPUS)
        pi3_fail("irq-cpu-id");
    uint32_t pending = pi3_read32(IRQ_SOURCE(cpu));
    if (!pending || (pending & ~(TIMER_IRQ | MAIL_IRQ)))
        pi3_fail("unexpected-irq-source");
    if (pending & TIMER_IRQ) {
        if (cpu != 0 || !timer_interval)
            pi3_fail("unexpected-timer");
        timer_ticks++;
        if (user_timer) {
            if (!from_user)
                pi3_fail("timer-not-from-el0");
            *(volatile uint64_t *) (uintptr_t) PI3_USER_DATA = timer_ticks;
        }
        timer_arm();
    }
    if (pending & MAIL_IRQ) {
        uint32_t message = pi3_read32(MAIL_CLEAR(cpu));
        pi3_write32(MAIL_CLEAR(cpu), message);
        if (cpu) {
            if (message != 1)
                pi3_fail("ipi-message");
            __atomic_fetch_add(&ipi_received[cpu], 1, __ATOMIC_RELEASE);
            pi3_write32(MAIL_SET(0), 1U << cpu);
        } else {
            __atomic_fetch_or(&acknowledgements, message, __ATOMIC_RELEASE);
        }
    }
}

void pi3_user_timer_start(void)
{
    timer_ticks = 0;
    user_timer = true;
    *(volatile uint64_t *) (uintptr_t) PI3_USER_DATA = 0;
    pi3_write32(TIMER_CONTROL(0), TIMER_IRQ);
    timer_arm();
}

void pi3_user_timer_finish(void)
{
    __asm__ volatile("msr cntp_ctl_el0, xzr; isb" ::: "memory");
    pi3_write32(TIMER_CONTROL(0), 0);
    if (!user_timer || timer_ticks < 5)
        pi3_fail("el0-timer-count");
    user_timer = false;
    pi3_pass("el0-timer-preemption");
}

void pi3_timer_test(void)
{
    uint64_t frequency = pi3_frequency();
    if (frequency < 1000 || frequency > 10000000000UL)
        pi3_fail("counter-frequency");
    pi3_puts("counter-hz=");
    pi3_hex(frequency);
    pi3_puts("\n");
    timer_interval = frequency / 100;
#ifndef PI3_TEST_DISABLE_TIMER
    pi3_write32(TIMER_CONTROL(0), TIMER_IRQ);
#endif
    timer_arm();
    pi3_irq_enable();
    uint64_t deadline = pi3_counter() + frequency;
    /* Poll a counter deadline rather than hanging forever in WFI if IRQ
     * routing is broken. This deadline is also exercised by a negative test. */
    while (timer_ticks < 5 && pi3_counter() < deadline)
        __asm__ volatile("yield");
    pi3_irq_disable();
    __asm__ volatile("msr cntp_ctl_el0, xzr; isb" ::: "memory");
    pi3_write32(TIMER_CONTROL(0), 0);
    if (timer_ticks < 5)
        pi3_fail("timer-irq-timeout");
    pi3_pass("timer-irq-5-ticks");
}

_Noreturn void pi3_secondary(void)
{
    unsigned cpu = pi3_cpu();
    pi3_exception_init();
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el != 4 || cpu == 0 || cpu >= PI3_CPUS)
        pi3_fail("secondary-el1");
    pi3_mmu_enable();
    pi3_write32(MAIL_CLEAR(cpu), UINT32_MAX);
    pi3_write32(MAIL_CONTROL(cpu), 1);
    __atomic_store_n(&online[cpu], 1, __ATOMIC_RELEASE);
    pi3_irq_enable();
    for (;;)
        __asm__ volatile("wfi");
}

void pi3_smp_test(void)
{
    /* Pi firmware/QEMU spin-table cells are read with caches off. Publish
     * the cell to PoC before SEV, rather than relying on TCG cache behavior.
     * Also publish all bootstrap BSS/data/page-table cache lines before APs
     * enable their caches and participate in the coherent mapping. */
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t line = (uintptr_t) 4 << ((ctr >> 16) & 15);
    for (uintptr_t p = (uintptr_t) __text_start; p < (uintptr_t) __image_end;
         p += line)
        __asm__ volatile("dc cvac, %0" ::"r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned cpu = 1; cpu < PI3_CPUS; cpu++) {
        uintptr_t cell = PI3_SPIN_TABLE + (uintptr_t) cpu * 8;
        *(volatile uint64_t *) cell = (uintptr_t) pi3_secondary_start;
        __asm__ volatile("dc cvac, %0; dsb sy; sev" ::"r"(cell) : "memory");
    }
    uint64_t deadline = pi3_counter() + pi3_frequency();
    for (unsigned cpu = 1; cpu < PI3_CPUS; cpu++) {
        while (!__atomic_load_n(&online[cpu], __ATOMIC_ACQUIRE)) {
            if (pi3_counter() >= deadline)
                pi3_fail("secondary-boot-timeout");
            __asm__ volatile("yield");
        }
    }
    pi3_pass("smp-4-cpus-el1");
    pi3_write32(MAIL_CLEAR(0), UINT32_MAX);
    pi3_write32(MAIL_CONTROL(0), 1);
    pi3_irq_enable();
    for (unsigned round = 1; round <= 32; round++) {
        __atomic_store_n(&acknowledgements, 0, __ATOMIC_RELEASE);
        for (unsigned cpu = 1; cpu < PI3_CPUS; cpu++)
            pi3_write32(MAIL_SET(cpu), 1);
        deadline = pi3_counter() + pi3_frequency();
        while (__atomic_load_n(&acknowledgements, __ATOMIC_ACQUIRE) != 0xe) {
            if (pi3_counter() >= deadline)
                pi3_fail("mailbox-ipi-timeout");
            __asm__ volatile("yield");
        }
        for (unsigned cpu = 1; cpu < PI3_CPUS; cpu++) {
            if (__atomic_load_n(&ipi_received[cpu], __ATOMIC_ACQUIRE) != round)
                pi3_fail("mailbox-ipi-count");
        }
    }
    pi3_pass("mailbox-ipi-roundtrip");
}
