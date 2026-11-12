/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_CPU_INLINE_H
#define RTMUX_AARCH64_CPU_INLINE_H
#include <stdint.h>
#include <sys/spinlock.h>
static inline void __cpu_membarrier(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}
#define cpu_smp_mb() __cpu_membarrier()
#define __cpu_store_barrier() __asm__ volatile("dmb ish" ::: "memory")
#define wfi() __asm__ volatile("wfi" ::: "memory")
#define cpu_halt() \
    do {           \
        for (;;)   \
            wfi(); \
    } while (0)
#define cpu_relax() __asm__ volatile("yield" ::: "memory")
static inline void *cpu_get_tls(void)
{
    void *p;
    __asm__("mrs %0, tpidr_el0" : "=r"(p));
    return p;
}
static inline uint64_t r_sp(void)
{
    uint64_t v;
    __asm__("mov %0, sp" : "=r"(v));
    return v;
}
static inline uint64_t r_ra(void)
{
    return (uintptr_t) __builtin_return_address(0);
}
static inline uint64_t r_time(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static inline void interrupt_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}
static inline void interrupt_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}
#define intr_disable interrupt_disable
#define intr_enable interrupt_enable
static inline unsigned long cpu_irq_save(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(v)::"memory");
    return v;
}
static inline void cpu_irq_restore(unsigned long v)
{
    __asm__ volatile("msr daif, %0" ::"r"(v) : "memory");
}
static inline int intr_get(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, daif" : "=r"(v));
    return !(v & 128);
}
static inline uintptr_t cpu_page_table_root(void)
{
    uintptr_t v;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v & 0x0000fffffffff000UL;
}
static inline void cpu_tlb_flush(void)
{
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}
static inline void cpu_icache_sync(void)
{
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
}
static inline void chk_kernel_sp(uintptr_t sp, const char *site)
{
    if (sp < 0xffffffc000080000UL || sp >= 0xffffffc03c000000UL) {
        extern void crash_halt(const char *, ...) __attribute__((noreturn));
        crash_halt("invalid kernel SP %lx at %s", sp, site);
    }
}
static inline void interrupt_lock(struct intrspin *s)
{
    interrupt_disable();
    while (__atomic_exchange_n(&s->value, 1, __ATOMIC_ACQUIRE))
        cpu_relax();
}
static inline void interrupt_unlock(struct intrspin *s)
{
    __atomic_store_n(&s->value, 0, __ATOMIC_RELEASE);
    interrupt_enable();
}
static inline void DebugBreak(void)
{
    __asm__ volatile("brk #0");
}
static inline void debug_kdbreak(void)
{
    DebugBreak();
}

static inline uintptr_t cpu_exception_pc(void)
{
    uintptr_t v;
    __asm__ volatile("mrs %0, elr_el1" : "=r"(v));
    return v;
}

#endif
