/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_CPUINLINE_H
#define _RISCV_CPUINLINE_H

#include <inttypes.h>
#include <sys/spinlock.h>

/* __cpu_membarrier — full hardware memory barrier (read+write).
 *
 * Maps to RISC-V "fence rw, rw". Callers use this macro when they need hardware
 * ordering across harts; a compiler-only barrier is not sufficient on RVWMO.
 */
#define __cpu_membarrier() \
    ({ __asm__ __volatile__("fence rw, rw" : : : "memory"); })

/* cpu_smp_mb — full SMP memory barrier (read/write, hardware).
 *
 * Explicit-intent alias for __cpu_membarrier at cross-CPU lock-free sites, such
 * as the RTMUX_ITF_WAIT_PENDING handshake.
 */
#define cpu_smp_mb() ({ __asm__ __volatile__("fence rw, rw" : : : "memory"); })

#define wfi()                                     \
    do {                                          \
        __asm__ __volatile__("wfi" ::: "memory"); \
    } while (0)
#define cpu_halt() \
    do {           \
        for (;;)   \
            wfi(); \
    } while (0)

/* Return the current hart id */
static inline uint64_t r_mhartid(void)
{
    uint64_t x;
    __asm__ volatile("csrr %0, mhartid" : "=r"(x));
    return x;
}

/* Machine Status Register, mstatus */

#define MSTATUS_MPP_MASK (3L << 11) /* previous mode. */
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)
#define MSTATUS_MIE (1L << 3) /* machine-mode interrupt enable. */

static inline uint64_t r_mstatus(void)
{
    uint64_t x;
    __asm__ volatile("csrr %0, mstatus" : "=r"(x));
    return x;
}

static inline void w_mstatus(uint64_t x)
{
    __asm__ volatile("csrw mstatus, %0" ::"r"(x));
}

/* machine exception program counter, holds the instruction address to which a
 * return from exception will go.
 */
static inline void w_mepc(uint64_t x)
{
    __asm__ volatile("csrw mepc, %0" ::"r"(x));
}

/* Supervisor Status Register, sstatus */

#define SSTATUS_SUM (1UL << 18)        /* S-mode User Memory access */
#define SSTATUS_FS (3UL << 13)         /* Floating-point Status mask */
#define SSTATUS_FS_OFF (0UL << 13)     /* FPU off (FP instructions trap) */
#define SSTATUS_FS_INITIAL (1UL << 13) /* FPU initial (clean state) */
#define SSTATUS_FS_CLEAN (2UL << 13)   /* FPU clean */
#define SSTATUS_FS_DIRTY (3UL << 13)   /* FPU dirty */
#define SSTATUS_SPP (1UL << 8)  /* Supervisor Previous Privilege (1=S, 0=U) */
#define SSTATUS_SPIE (1UL << 5) /* Supervisor Previous Interrupt Enable */
#define SSTATUS_UPIE (1UL << 4) /* User Previous Interrupt Enable */
#define SSTATUS_SIE (1UL << 1)  /* Supervisor Interrupt Enable */
#define SSTATUS_UIE (1UL << 0)  /* User Interrupt Enable */

static inline uint64_t r_sstatus()
{
    uint64_t x;
    __asm__ volatile("csrr %0, sstatus" : "=r"(x));
    return x;
}

static inline void w_sstatus(uint64_t x)
{
    __asm__ volatile("csrw sstatus, %0" ::"r"(x));
}

/* Supervisor Interrupt Pending */
static inline uint64_t r_sip()
{
    uint64_t x;
    __asm__ volatile("csrr %0, sip" : "=r"(x));
    return x;
}

static inline void w_sip(uint64_t x)
{
    __asm__ volatile("csrw sip, %0" ::"r"(x));
}

/* Supervisor Interrupt Enable */
#define SIE_SEIE (1UL << 9) /* external */
#define SIE_STIE (1UL << 5) /* timer */
#define SIE_SSIE (1UL << 1) /* software */

#define SIP_SSIP (1UL << 1) /* software interrupt pending */
#define SIP_SSIP (1UL << 1) /* software interrupt pending */
static inline uint64_t r_sie()
{
    uint64_t x;
    __asm__ volatile("csrr %0, sie" : "=r"(x));
    return x;
}

static inline void w_sie(uint64_t x)
{
    __asm__ volatile("csrw sie, %0" ::"r"(x));
}

/* Machine-mode Interrupt Enable */
#define MIE_MEIE (1L << 11) /* external */
#define MIE_MTIE (1L << 7)  /* timer */
#define MIE_MSIE (1L << 3)  /* software */
static inline uint64_t r_mie()
{
    uint64_t x;
    __asm__ volatile("csrr %0, mie" : "=r"(x));
    return x;
}

static inline void w_mie(uint64_t x)
{
    __asm__ volatile("csrw mie, %0" ::"r"(x));
}

/* machine exception program counter, holds the instruction address to which a
 * return from exception will go.
 */
static inline void w_sepc(uint64_t x)
{
    __asm__ volatile("csrw sepc, %0" ::"r"(x));
}

static inline uint64_t r_sepc()
{
    uint64_t x;
    __asm__ volatile("csrr %0, sepc" : "=r"(x));
    return x;
}

/* Machine Exception Delegation */
static inline uint64_t r_medeleg()
{
    uint64_t x;
    __asm__ volatile("csrr %0, medeleg" : "=r"(x));
    return x;
}

static inline void w_medeleg(uint64_t x)
{
    __asm__ volatile("csrw medeleg, %0" ::"r"(x));
}

/* Machine Interrupt Delegation */
static inline uint64_t r_mideleg()
{
    uint64_t x;
    __asm__ volatile("csrr %0, mideleg" : "=r"(x));
    return x;
}

static inline void w_mideleg(uint64_t x)
{
    __asm__ volatile("csrw mideleg, %0" ::"r"(x));
}

/* Supervisor Trap-Vector Base Address low two bits are mode. */
static inline void w_stvec(uint64_t x)
{
    __asm__ volatile("csrw stvec, %0" ::"r"(x));
}

static inline uint64_t r_stvec()
{
    uint64_t x;
    __asm__ volatile("csrr %0, stvec" : "=r"(x));
    return x;
}

/* Machine-mode interrupt vector */
static inline void w_mtvec(uint64_t x)
{
    __asm__ volatile("csrw mtvec, %0" ::"r"(x));
}

/* use riscv's sv39 page table scheme. */
#define SATP_SV39 (8UL << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64_t) pagetable) >> 12))

/* supervisor address translation and protection; holds the address of the page
 * table.
 */
static inline void w_satp(uint64_t x)
{
    __asm__ volatile("csrw satp, %0" ::"r"(x));
}

static inline uint64_t r_satp()
{
    uint64_t x;
    __asm__ volatile("csrr %0, satp" : "=r"(x));
    return x;
}

/* Supervisor Scratch register, for early trap handler in trampoline.S. */
static inline void w_sscratch(uint64_t x)
{
    __asm__ volatile("csrw sscratch, %0" ::"r"(x));
}

static inline void w_mscratch(uint64_t x)
{
    __asm__ volatile("csrw mscratch, %0" ::"r"(x));
}

/* Supervisor Trap Cause */
static inline uint64_t r_scause()
{
    uint64_t x;
    __asm__ volatile("csrr %0, scause" : "=r"(x));
    return x;
}

/* Supervisor Trap Value */
static inline uint64_t r_stval()
{
    uint64_t x;
    __asm__ volatile("csrr %0, stval" : "=r"(x));
    return x;
}

/* Machine-mode Counter-Enable */
static inline void w_mcounteren(uint64_t x)
{
    __asm__ volatile("csrw mcounteren, %0" ::"r"(x));
}

static inline uint64_t r_mcounteren()
{
    uint64_t x;
    __asm__ volatile("csrr %0, mcounteren" : "=r"(x));
    return x;
}

/* machine-mode cycle counter */
static inline uint64_t r_time()
{
    uint64_t x;
    __asm__ volatile("csrr %0, time" : "=r"(x));
    return x;
}

/* Supervisor Counter-Enable: which hardware counters U-mode may read directly.
 * Bit 0 CY (cycle), bit 1 TM (time), bit 2 IR (instret).
 */
static inline void w_scounteren(uint64_t x)
{
    __asm__ volatile("csrw scounteren, %0" ::"r"(x));
}

/* Sanity-check a kernel-side stack pointer.
 *
 * A legitimate kernel SP must lie in the direct-mapped RAM region
 * (KERNEL_VIRT_BASE + RAM). RAM size is bounded above by the Sv39 1 GiB L2 slot
 * at index 258 in our layout (4 GiB upper bound is safely conservative for any
 * plausible board). Anything outside this range — most notably anything in L2
 * slots 256/257 (0xffffffc000000000..0xffffffc080000000) — is a programming
 * bug: stale per-CPU state, page-table-propagation miss, uninitialized stack,
 * etc. Crash with a useful message so we don't end up taking a nested
 * store-page-fault during the next trap-save and dying without a banner (the
 * "romfs hang" fingerprint).
 */
#define KERN_SP_LO 0xFFFFFFC080000000UL
#define KERN_SP_HI 0xFFFFFFC100000000UL
static inline void chk_kernel_sp(uintptr_t sp, const char *site)
{
    if (sp < KERN_SP_LO || sp >= KERN_SP_HI) {
        extern void crash_halt(const char *fmt, ...) __attribute__((noreturn));
        crash_halt(
            "BAD kernel sp 0x%lx at %s — outside direct-map RAM "
            "VA range %lx..%lx\n",
            sp, site, (unsigned long) KERN_SP_LO, (unsigned long) KERN_SP_HI);
    }
}

/* enable device interrupts */
static inline void interrupt_enable()
{
    w_sstatus(r_sstatus() | SSTATUS_SIE);
}

/* disable device interrupts */
static inline void interrupt_disable()
{
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

/* are device interrupts enabled? */
static inline int intr_get()
{
    uint64_t x = r_sstatus();
    return (x & SSTATUS_SIE) != 0;
}

static inline uint64_t r_sp()
{
    uint64_t x;
    __asm__ volatile("mv %0, sp" : "=r"(x));
    return x;
}

static inline uint64_t r_ra()
{
    uint64_t x;
    __asm__ volatile("mv %0, ra" : "=r"(x));
    return x;
}

/* flush the TLB. */
static inline void sfence_vma()
{
    /* the zero, zero means flush all TLB entries. */
    __asm__ volatile("sfence.vma zero, zero");
}

static inline void cpu_icache_sync(void)
{
    __asm__ volatile("fence.i" ::: "memory");
}

/**
 * \brief Disable interrupts and wait on a spinlock
 */
static inline void interrupt_lock(struct intrspin *__spin)
{
    interrupt_disable();
#if CONFIG_PROCESSORS_MAX > 1
    {
        __asm__ __volatile__(
            "       li           t0, 1;"        /* Initialize swap value */
            "1:     lw           t1, (%0);"     /* Check if lock is held */
            "       bnez         t1, 1b;"       /* Retry if held */
            "       amoswap.w.aq t1, t0, (%0);" /* Attempt to acquire lock */
            "       bnez         t1, 1b"        /* Retry if held */
            :
            : "r"(&__spin->value)

            /* "memory": the .aq bit orders the hardware, but without a compiler
             * barrier gcc may hoist critical-section accesses above the
             * acquire. The matching release in interrupt_unlock() already
             * carries it.
             */
            : "t0", "t1", "memory");
    }
#else
    /* acquire the spin lock */
    __spin->value = 1;
#endif
}

/**
 * \brief Release the spinlock and enable interrupts
 */
static inline void interrupt_unlock(struct intrspin *__spin)
{
    __asm__ __volatile__("amoswap.w.rl x0, x0, (%0)" ::"r"(&__spin->value)
                         : "memory");
    interrupt_enable();
}

/**
 * \brief Cause a breakpoint in the debugger.
 */
static inline void DebugBreak(void)
{
    __asm__ __volatile__("ebreak");
}

/**
 * \brief Enter the kernel debugger.
 */
static inline void debug_kdbreak(void)
{
    __asm__ __volatile__(
        "slli x0, x0, 0x1f\n" /* Entry NOP */
        "ebreak\n"            /* Break to debugger */
        "srai x0, x0, 7\n"    /* NOP encoding the semihosting call number 7 */
    );
}

#endif
