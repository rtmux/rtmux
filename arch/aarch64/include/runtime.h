/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_RUNTIME_H
#define RTMUX_AARCH64_RUNTIME_H
#include <stdbool.h>
#include <sys/types.h>
#include <posix-types.h>
#include <arch/cpu-def.h>
#include <arch/pcpu.h>
#define xfer_setjmp(e) _setjmp(e)
#define xfer_longjmp(e, r, regs) _longjmp(e, r)
#define _KSLOT2(line, arg)    \
    union {                   \
        arg;                  \
        uint64_t _slot##line; \
    }
#define _KSLOT1(line, arg) _KSLOT2(line, arg)
#define KARGSLOT(arg) _KSLOT1(__LINE__, arg)
#define REGIP(r) ((r)->pc)
#define REGSP(r) ((r)->gpr[31])
#define SETREGIP(r, v) (REGIP(r) = (uintptr_t) (v))
#define SETREGSP(r, v) (REGSP(r) = (uintptr_t) (v))
#define REGSTATUS(r) ((r)->gpr[0])
#define REGTYPE(r) ((r)->gpr[8])
#define SETREGSTATUS(r, v) (REGSTATUS(r) = (uintptr_t) (v))
#define SETREGTYPE(r, v) (REGTYPE(r) = (uintptr_t) (v))
#define CPU_REG_ARG(r, n) ((r)->gpr[n])
#define CPU_REG_RA(r) ((r)->gpr[30])
#define SETKIP_FUNC(t, v) cpu_invoke_func(t, (uintptr_t) (v))
#define KSYS_ENTRY_SIZE 4
#define KCALL_ERR_SKIPAHEAD 0
#define KSYS_RESTORE_KCALL_C_ARGS(t)                  \
    do {                                              \
        for (unsigned _i = 0; _i < 6; _i++)           \
            (t)->reg.gpr[_i] = (t)->kcall_c_args[_i]; \
    } while (0)
#define PHYS_TO_PTR(p) ((void *) ((uintptr_t) (p) + KERNEL_VIRT_BASE))
#define PTR_TO_PHYS(p) ((uintptr_t) (p) - KERNEL_VIRT_BASE)
#define INKERNEL_BITS_SHIFT 8
static inline struct pcpu *get_pcpu(void)
{
    struct pcpu *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}
static inline unsigned get_cpunum(void)
{
    return get_pcpu()->pc_hart;
}
static inline struct thread_entry *get_curthread(void)
{
    return get_pcpu()->pc_curthread;
}
static inline void cpu_set_kernel_tp(void *p)
{
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(p) : "memory");
}
extern unsigned in_kcall[];
void in_kcall_inc(void);
void in_kcall_phase(int phase);
void in_kcall_dec(void);
unsigned in_kcall_get_self(void);
int priv_call_phase(unsigned id);
static inline bool in_kernel_context(void)
{
    return in_kcall_get_self() != 0;
}
int cpu_console_getc(void);
void idle_run_thread(struct thread_entry *) __attribute__((noreturn));
uintptr_t next_instruction(CPU_REGISTERS *);
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
#endif
