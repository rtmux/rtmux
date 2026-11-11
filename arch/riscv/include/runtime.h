/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <sys/types.h>
#include <posix-types.h>
#include <arch/cpu-def.h>
#include <arch/pcpu.h> /* struct pcpu — uses struct tags only */

#define xfer_setjmp(_env) _setjmp(_env)
#define xfer_longjmp(_env, _ret, _regs) _longjmp(_env, _ret)

/* KARGSLOT: each syscall argument occupies exactly one 64-bit register slot. On
 * ILP32 a uint32_t filler was enough, but on LP64 that over-pads pointer
 * arguments (8 bytes + 4 filler = 12, rounds to 16). Using an anonymous union
 * forces every slot to be exactly 8 bytes regardless of the argument type.
 */
#define _KSLOT2(line, arg)    \
    union {                   \
        arg;                  \
        uint64_t _slot##line; \
    }
#define _KSLOT1(line, arg) _KSLOT2(line, arg)
#define KARGSLOT(arg) _KSLOT1(__LINE__, arg)

#define REGIP(reg) ((reg)->pc)
#define REGSP(reg) ((reg)->gpr[RISCV_REG_SP])
#define SETREGIP(reg, v) (REGIP(reg) = (uintptr_t) (v))
#define SETREGSP(reg, v) (REGSP(reg) = (uintptr_t) (v))

#define REGSTATUS(reg) ((reg)->gpr[RISCV_REG_A0])
#define REGTYPE(reg) ((reg)->gpr[RISCV_REG_A7])
#define SETREGSTATUS(reg, v) (REGSTATUS(reg) = (uintptr_t) (v))
#define SETREGTYPE(reg, v) (REGTYPE(reg) = (uintptr_t) (v))

#define SETKIP_FUNC(thp, v) cpu_invoke_func(thp, (uintptr_t) v)

/* Restore the syscall argument registers (a0..a5) saved at kcall entry
 * (thp->kcall_c_args[]). Invoked by KCALL_RESTART before the user PC is rewound
 * to the ecall, so the re-executed syscall sees its original arguments rather
 * than a return/status value left in a0.
 */
#define KSYS_RESTORE_KCALL_C_ARGS(thp)                         \
    do {                                                       \
        (thp)->reg.gpr[RISCV_REG_A0] = (thp)->kcall_c_args[0]; \
        (thp)->reg.gpr[RISCV_REG_A1] = (thp)->kcall_c_args[1]; \
        (thp)->reg.gpr[RISCV_REG_A2] = (thp)->kcall_c_args[2]; \
        (thp)->reg.gpr[RISCV_REG_A3] = (thp)->kcall_c_args[3]; \
        (thp)->reg.gpr[RISCV_REG_A4] = (thp)->kcall_c_args[4]; \
        (thp)->reg.gpr[RISCV_REG_A5] = (thp)->kcall_c_args[5]; \
    } while (0)

#define KSYS_ENTRY_SIZE 4 /* size of kernel entry opcode */

/* KCALL_ERR_SKIPAHEAD: PC increment applied by kcall_err() on error.
 *
 * The design assumed a specific libc wrapper layout where the instructions
 * after "ecall" were two "error handling" instructions that the kernel could
 * skip over on success, effectively using the sign of the return value to drive
 * control flow. Our libc wrappers (__scN in lib/libc/riscv/syscalls.c) are C
 * functions that simply return a0 — there is nothing to skip. Advancing the PC
 * here would jump past the function epilogue and run garbage.
 *
 * Set to 0 so kcall_err() only sets the status & flag; callers check the
 * negative return value and branch in software.
 */
#define KCALL_ERR_SKIPAHEAD 0

/* Convert physical address to pointer value and vice-versa when in a physical
 * memory model. Identity mapping for standalone kernel test.
 */
#define PHYS_TO_PTR(phys) ((void *) ((uintptr_t) (phys) + KERNEL_VIRT_BASE))
#define PTR_TO_PHYS(ptr) ((uintptr_t) (ptr) - KERNEL_VIRT_BASE)

/* Memory probes — simple volatile access for physical mode. In the future,
 * these should handle faults properly. HAVE_KERSTACK_STORAGE /
 * HAVE_INKERNEL_STORAGE are the knobs an arch can define to supply its own
 * storage for ksys_stack[] and inkernel (e.g. per-CPU sections). RISC-V doesn't
 * override them, so globals.c declares them in shared .bss.
 */

#define INKERNEL_BITS_SHIFT 8

#if CONFIG_PROCESSORS_MAX > 1

/* get_cpunum — return the RISC-V hart ID of the CPU we are running on.
 *
 * The value is NOT a sequential 0..N-1 index. It is the hardware hart
 * identifier assigned by the platform (e.g. QEMU virt may boot on hart 3 even
 * with -smp 1). All per-CPU arrays are indexed by hart ID. Code that iterates
 * over all CPUs must use hart_ids[0..num_processors-1] for indirection.
 *
 * Implementation: one load from offset PC_HART of *tp. tp is invariant per hart
 * in S-mode (set once at bootstrap, never reassigned), so this is always
 * correct — tp follows the hart, not the thread.
 *
 * Return value: this CPU's hart ID.
 */
/* This is an AXIOM, not a proved contract, and it is the only one in the tree.
 * The body is a load through tp, and no prover can look inside inline asm, so
 * WP cannot establish the bound; it is instead a property of bootstrap, which
 * writes PC_HART before any of this code runs, and of the per-CPU arrays being
 * sized CONFIG_PROCESSORS_MAX and indexed by hart ID.
 *
 * It has to be stated because every per-CPU array access depends on it: leave
 * it out and each one is an unprovable index_bound obligation.  A source that
 * relies on it names it in a "framac-assume:" marker, so every gate run prints
 * the assumption rather than letting it pass silently.  If the platform ever
 * hands out a hart ID at or above CONFIG_PROCESSORS_MAX, this is false and
 * every proof resting on it is void.
 */
/*@ assigns \nothing;
    ensures \result < CONFIG_PROCESSORS_MAX;
 */
static inline unsigned get_cpunum(void)
{
#if CONFIG_PROCESSORS_MAX > 1
    unsigned hart;
    __asm__ volatile("lw %0, %c1(tp)" : "=&r"(hart) : "i"(PC_HART));
    return hart;
#else
    return 0;
#endif
}

/* get_pcpu — return a pointer to this CPU's struct pcpu.
 *
 * Expands to a single "mv reg, tp" — zero memory loads. tp holds
 * &pcpu_array[hart] on every hart in S-mode.
 *
 * Return value: pointer to this CPU's struct pcpu.
 */
static inline struct pcpu *get_pcpu(void)
{
    struct pcpu *p;
    __asm__ volatile("mv %0, tp" : "=&r"(p));
    return p;
}

/* get_curthread — return a pointer to the currently-running thread.
 *
 * Lowered to a single "ld reg, PC_CURTHREAD(tp)".
 *
 * Returns NULL while pc_curthread is unwritten; the canonical "current thread"
 * lives in _actives[hart] until set_active_thread() updates pc_curthread
 * atomically.
 *
 * Return value: pointer to the current thread, or NULL.
 */
static inline struct thread_entry *get_curthread(void)
{
    struct thread_entry *t;
    __asm__ volatile("ld %0, %c1(tp)"
                     : "=&r"(t)
                     : "i"(offsetof(struct pcpu, pc_curthread)));
    return t;
}

/* in_kernel_context — true if this CPU is executing inside a kernel critical
 * section. A pure read of the per-CPU in_kcall counter that __kcall_dispatch
 * increments at trap entry and decrements at kernel_exit.
 *
 * Used by sites that ask "am I in the kernel" for bookkeeping — timer-trap
 * routing, stall-detector debug, page-fault classification.
 */
extern unsigned in_kcall[CONFIG_PROCESSORS_MAX];
/* in_specret[] now lives at pcpu_array[hart].pc_in_specret */

/* in_kcall accessors. Use these in preference to "in_kcall[get_cpunum()]++"
 * etc. at self-cpu call sites — they cannot be misused for cross-hart writes,
 * and the dec path catches underflow with a caller-PC diagnostic. Cross-hart
 * READS of the raw array (e.g. monitor.c) are still allowed. See globals.c for
 * the rationale.
 */
void in_kcall_inc(void);
void in_kcall_phase(int phase);
void in_kcall_dec(void);
unsigned in_kcall_get_self(void);
int priv_call_phase(unsigned id);

#define IN_KCALL_PHASE_NONE (-1)
#define IN_KCALL_PHASE_DISPATCH (-2)
#define IN_KCALL_PHASE_TIMER (-3)
#define IN_KCALL_PHASE_FAULT (-4)
#define IN_KCALL_PHASE_PRIV (-5)
#define IN_KCALL_PHASE_HEARTBEAT (-6)
#define IN_KCALL_PHASE_PRIV_SYSTEM (-7)
#define IN_KCALL_PHASE_PRIV_VM (-8)
#define IN_KCALL_PHASE_PRIV_IPC (-9)
#define IN_KCALL_PHASE_PRIV_PROCESS (-10)
#define IN_KCALL_PHASE_PRIV_THREAD (-11)
#define IN_KCALL_PHASE_PRIV_PA (-12)
#define IN_KCALL_PHASE_PRIV_PA_ALLOC (-13)
#define IN_KCALL_PHASE_PRIV_PA_FREE (-14)
#define IN_KCALL_PHASE_PRIV_PA_ZERO (-15)
#define IN_KCALL_PHASE_PRIV_PA_INFO (-16)
#define IN_KCALL_PHASE_PRIV_PA_XLAT (-17)
#define IN_KCALL_PHASE_PRIV_PA_PQ (-18)

static inline bool in_kernel_context(void)
{
    return in_kcall_get_self() != 0;
}

#endif /* SMP */

#if !defined(RISCV_CAUSE2SIGMAP_CONST)
#define RISCV_CAUSE2SIGMAP_CONST const
#endif
extern RISCV_CAUSE2SIGMAP_CONST unsigned long __riscv_cause2sig_map[];

/* cpu_set_kernel_tp — set tp to point to this CPU's cpupage. In S-mode, tp
 * always holds &cpupage[cpu]. get_cpunum() reads cpupage->cpu via tp.
 */
static inline void cpu_set_kernel_tp(void *cpupage)
{
    __asm__ volatile("mv tp, %0" ::"r"((uint64_t) (uintptr_t) cpupage));
}

/* CPU_REG_ARG — access the n-th argument register from a saved context. On
 * RISC-V, arguments are in a0-a7 (x10-x17).
 */
#define CPU_REG_ARG(reg, n) ((reg)->gpr[RISCV_REG_A0 + (n)])

/* CPU_REG_RA — access the return address register from a saved context. */
#define CPU_REG_RA(reg) ((reg)->gpr[RISCV_REG_RA])

/* cpu_console_getc — poll for a character from the platform console. */
int cpu_console_getc(void);

/* idle_run_thread — switch to a thread and start running it (never returns). */
struct thread_entry;
void __attribute__((noreturn)) idle_run_thread(struct thread_entry *thp);

extern uintptr_t next_instruction(CPU_REGISTERS *ctx);
