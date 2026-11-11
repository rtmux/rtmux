/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#ifndef _SBI_H
#define _SBI_H

#include <arch/sbi-ecall-interface.h>

#define SBI_ECALL(__num, __a0, __a1, __a2)                             \
    ({                                                                 \
        register unsigned long a0 asm("a0") = (unsigned long) (__a0);  \
        register unsigned long a1 asm("a1") = (unsigned long) (__a1);  \
        register unsigned long a2 asm("a2") = (unsigned long) (__a2);  \
        register unsigned long a7 asm("a7") = (unsigned long) (__num); \
        asm volatile("ecall"                                           \
                     : "+r"(a0)                                        \
                     : "r"(a1), "r"(a2), "r"(a7)                       \
                     : "memory");                                      \
        a0;                                                            \
    })

/* SBI v0.1 convenience wrappers (a7=extension, a0-a2=args) */
#define SBI_ECALL_0(__num) SBI_ECALL(__num, 0, 0, 0)
#define SBI_ECALL_1(__num, __a0) SBI_ECALL(__num, __a0, 0, 0)
#define SBI_ECALL_2(__num, __a0, __a1) SBI_ECALL(__num, __a0, __a1, 0)

/* SBI v0.2+ ecall: a7=extension ID, a6=function ID, a0/a1=args.
 * Returns sbiret.error in a0, sbiret.value in a1.
 */
#define SBI_ECALL_V2(__ext, __fid, __a0, __a1)                         \
    ({                                                                 \
        register unsigned long a0 asm("a0") = (unsigned long) (__a0);  \
        register unsigned long a1 asm("a1") = (unsigned long) (__a1);  \
        register unsigned long a6 asm("a6") = (unsigned long) (__fid); \
        register unsigned long a7 asm("a7") = (unsigned long) (__ext); \
        asm volatile("ecall"                                           \
                     : "+r"(a0), "+r"(a1)                              \
                     : "r"(a6), "r"(a7)                                \
                     : "memory");                                      \
        a0;                                                            \
    })

/* SBI v0.2+ ecall with three input args (a0/a1/a2 + a6 fid + a7 ext). Used by
 * SBI_EXT_DBCN_CONSOLE_WRITE which takes (num_bytes, addr_lo, addr_hi). On RV64
 * the address fits in a1 alone and a2 is 0.
 */
#define SBI_ECALL_V2_3(__ext, __fid, __a0, __a1, __a2)                 \
    ({                                                                 \
        register unsigned long a0 asm("a0") = (unsigned long) (__a0);  \
        register unsigned long a1 asm("a1") = (unsigned long) (__a1);  \
        register unsigned long a2 asm("a2") = (unsigned long) (__a2);  \
        register unsigned long a6 asm("a6") = (unsigned long) (__fid); \
        register unsigned long a7 asm("a7") = (unsigned long) (__ext); \
        asm volatile("ecall"                                           \
                     : "+r"(a0)                                        \
                     : "r"(a1), "r"(a2), "r"(a6), "r"(a7)              \
                     : "memory");                                      \
        a0;                                                            \
    })

/* SBI SRST (System Reset) — a0=reset_type, a1=reset_reason */
#define sbi_system_reset(__type, __reason) \
    SBI_ECALL_V2(SBI_EXT_SRST, SBI_EXT_SRST_RESET, (__type), (__reason))

/* SBI Timer — program mtimecmp via OpenSBI firmware */
static inline void sbi_set_timer(uint64_t stime_value)
{
    SBI_ECALL_V2(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER, stime_value, 0);
}

/* SBI IPI — send inter-processor interrupt to specified harts */
static inline long sbi_send_ipi(unsigned long hart_mask,
                                unsigned long hart_mask_base)
{
    return (long) SBI_ECALL_V2(SBI_EXT_IPI, SBI_EXT_IPI_SEND_IPI, hart_mask,
                               hart_mask_base);
}

/* SBI RFENCE — remote instruction-cache fence (fence.i) on the harts selected
 * by (hart_mask, hart_mask_base). RISC-V does NOT keep the I-cache coherent
 * with data stores: after writing instructions (e.g. loading a program's text)
 * every hart that will *execute* them must run fence.i. The loading hart does a
 * local cpu_icache_sync(); this call makes OpenSBI run fence.i on the other
 * harts so a freshly-loaded program is safe to run wherever the scheduler
 * dispatches it.
 */
static inline long sbi_remote_fence_i(unsigned long hart_mask,
                                      unsigned long hart_mask_base)
{
    return (long) SBI_ECALL_V2(SBI_EXT_RFENCE, SBI_EXT_RFENCE_REMOTE_FENCE_I,
                               hart_mask, hart_mask_base);
}

/* SBI HSM (Hart State Management) — start a secondary hart. a0=hartid,
 * a1=start_addr (physical), a2=opaque (passed to hart in a1).
 */
static inline long sbi_hart_start(unsigned long hartid,
                                  unsigned long start_addr,
                                  unsigned long opaque)
{
    register unsigned long a0 __asm__("a0") = hartid;
    register unsigned long a1 __asm__("a1") = start_addr;
    register unsigned long a2 __asm__("a2") = opaque;
    register unsigned long a6 __asm__("a6") = SBI_EXT_HSM_HART_START;
    register unsigned long a7 __asm__("a7") = SBI_EXT_HSM;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a6), "r"(a7)
                     : "memory");
    return (long) a0;
}

/* Console I/O via the SBI Debug Console (DBCN) extension — SBI v2.0+.
 *
 * RTMux requires OpenSBI v1.6 or later; DBCN is mandatory. See
 * sbi-ecall-interface.h for the rationale (atomic per-call writes eliminate the
 * byte-level interleaving the SBI v0.1 putchar loop produces under SMP).
 *
 * Buffer arguments to sbi_dbcn_write must be in the kernel direct-map RAM
 * region: PA = VA - KERNEL_VIRT_BASE. All static strings, BSS, and per-CPU
 * kernel stacks satisfy that.
 */
#include <cpu-paging.h> /* KERNEL_VIRT_BASE */

static inline long sbi_dbcn_write(const void *buf, unsigned long len)
{
    /* OpenSBI needs the buffer's PHYSICAL address. In the kernel direct-map era
     * (post-MMU-enable) every kernel VA is at KERNEL_VIRT_BASE + PA, so PA = VA
     * - KERNEL_VIRT_BASE. Before MMU is enabled (very early startup), code runs
     * at raw physical addresses (~0x80000000+), and "buf" is already the
     * physical address — pass it straight through. Distinguish by the high bit
     * of the kernel VA region: if buf >= KERNEL_VIRT_BASE we're in the direct
     * map and need to translate; otherwise it is already physical.
     */
    unsigned long va = (unsigned long) buf;
    unsigned long pa = (va >= (unsigned long) KERNEL_VIRT_BASE)
                           ? (va - (unsigned long) KERNEL_VIRT_BASE)
                           : va;
    return (long) SBI_ECALL_V2_3(SBI_EXT_DBCN, SBI_EXT_DBCN_CONSOLE_WRITE, len,
                                 pa, 0);
}

/* Emit one character to the SBI debug console. Parameters:
 *   c : character to write (low 8 bits used)
 */
static inline void sbi_ecall_console_putc(int c)
{
    SBI_ECALL_V2(SBI_EXT_DBCN, SBI_EXT_DBCN_CONSOLE_WRITE_BYTE,
                 (unsigned long) (c & 0xff), 0);
}

#define sbi_ecall_console_getc() \
    ((int) SBI_ECALL_0(SBI_EXT_0_1_CONSOLE_GETCHAR))

/* Emit a NUL-terminated string to the SBI debug console. Parameters:
 *   str : string to write (NULL is a no-op)
 */
static inline void sbi_ecall_console_puts(const char *str)
{
    if (str == 0)
        return;
    unsigned long n = 0;
    while (str[n])
        n++;
    if (n != 0)
        sbi_dbcn_write(str, n);
}

#endif
