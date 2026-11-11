/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* trace/events.h Trace data structures and definitions */

#ifndef __KERTRACE_H_INCLUDED

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif
#ifndef _STDDEF_H_INCLUDED
#include <stddef.h>
#endif
#ifndef __TRACE_H_INCLUDED
#include <sys/trace.h>
#endif

typedef struct {
    uint32_t time_off;
    uint32_t data_emitted; /* Data has been emitted */
    uint32_t di;           /* Byte offset filled */
    union {
        uint32_t idata[2];
        char cdata[8];
    } data;
} trace_state_t;

typedef struct ehandler_data {
    int (*handler)(event_data_t *);
    event_data_t *area;
    thread_t *thp;
    process_t *process;
    struct ehandler_data **location;
    struct cpu_intrsave cpu;
} ehandler_data_t;

/* Prototype of event emitting function */
int em_event(tracebuf_t *);


/* Getting/setting events/classes */
#define _TRACE_ROUND_UP(n) (((n) & 3) ? (((n) >> 2) + 1) : ((n) >> 2))
#define _TRACE_SET_STRUCT(c, st) \
    ((c) &= ~(uint32_t) (0x3u << 30), (c) |= (uint32_t) (st))
#define _TRACE_MAKE_CODE(c, f, cl, e) \
    (((uint32_t) (c) << 24) | (uint32_t) (f) | (uint32_t) (cl) | (uint32_t) (e))


/* platform dependent pointer argument access and clock_cycles() ret. */
#define _TRACE_CLOCK_MSB (1 ^ _TRACE_CLOCK_SWAP)
#define _TRACE_CLOCK_LSB (0 ^ _TRACE_CLOCK_SWAP)

#include <arch/ktrace.h>

#define _TRACEDATANUM (2)

/* Size of the kernel-formatted name/string buffers (process and thread names);
 * independent of TRACE_USRSTR_MAX, which caps caller-supplied payloads.
 */
#define _TRACE_MAX_STR_SIZE (128)

/* Byte cap on the kernel copy of a caller trace payload (user string with its
 * terminator, or word payload); see ksys_trace_event.
 */
#define TRACE_USRSTR_MAX 256
#define _TRACE_MAX_SMP_CPU_NUM (64)
#define _TRACE_MAX_EV_HANDLER_NUM (128)

#define _TRACE_ENTER_CALL (0x00000001U << 0)
#define _TRACE_EXIT_CALL (0x00000001U << 1)
#define _TRACE_CALL_ARG_WIDE (0x00000001U << 2)
#define _TRACE_CALL_RET_WIDE (0x00000001U << 3)

#define _TRACE_ENTER_SYSTEM (0x00000001U << 0)
#define _TRACE_SYSTEM_ARG_WIDE (0x00000001U << 1)

#define _TRACE_ENTER_COMM (0x00000001U << 0)
#define _TRACE_COMM_ARG_WIDE (0x00000001U << 1)

/* Trace-arg packing macros.
 *
 * The kernel trace wire format reserves 32-bit slots for argument values. On
 * LP64 a native pointer does not fit in such a slot — trace consumers see the
 * low 32 bits only. This is accepted debt: trace arguments are identifiers /
 * codes / short offsets, not native pointers to be dereferenced on the consumer
 * side.
 *
 * Use these at pack/unpack sites instead of bare casts so the truncation is (a)
 * greppable, (b) explicitly acknowledged, and (c) doesn't trigger
 * -W{int,pointer}-to-{pointer,int}-cast at every call site. Strictly for the
 * trace-format boundary; do not use to launder arbitrary int/pointer casts
 * elsewhere.
 */
#define TRACE_PACK_PTR(type, v) ((type) (uintptr_t) (v))
#define TRACE_PACK_INT(p) ((uintptr_t) (p))
#define TRACE_PTR_LOW32(p) ((uint32_t) (uintptr_t) (p))

#define _TRACE_THREAD_ARG_WIDE (0x80000000U)

#define _TRACE_ENTER_INT (0x00000001U << 0)
#define _TRACE_EXIT_INT (0x00000001U << 1)

#define _TRACE_ENTER_USER (0x00000001U << 0)

#define _TRACE_SEC(a) ((uint32_t) (((uint64_t) (a)) / ((uint64_t) 1000000000)))
#define _TRACE_NSEC(a) ((uint32_t) (((uint64_t) (a)) % ((uint64_t) 1000000000)))
#define _TRACE_GET_LOCK() (get_inkernel() & INKERNEL_LOCK)
#define _TRACE_CHK_UNLOCK(l) (!l && get_inkernel() & INKERNEL_LOCK)

#define _TRACE_INT_LEV(l)                                                \
    ((uint32_t) ((TRACE_PACK_INT(l) - TRACE_PACK_INT(interrupt_level)) / \
                 sizeof(struct interrupt_level)))
#define _TRACE_INT_NUM(l)                                                      \
    (((struct interrupt_level *) (l))->info->vector_base + _TRACE_INT_LEV(l) - \
     ((struct interrupt_level *) (l))->level_base)

#if defined(CONFIG_INSTRUMENTATION)

#if !defined(NDEBUG)
#define _TRACE_PRINTF(code, x, ...)                                  \
    add_trace_string(_TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, \
                                      _TRACE_USER_C, (code)),        \
                     (x), __VA_ARGS__)
#endif

/* enter/exit states/arguments control macros */
#define _TRACE_GETSYSCALL(c) ((c) & 0xff)
#define _TRACE_NOARGS(thp) (thp)->syscall |= (0x1 << 10)
#define _TRACE_GETARGSFLAG(thp) (!((thp)->syscall & (0x1 << 10)))
#define _TRACE_COMM_IPC_EXIT (0x4000)
#define _TRACE_COMM_CALL_EXIT (0x8000)

/* Dereference-and-truncate: if p is non-NULL, return low 32 bits of p->m.
 * Returns uint32_t to match trace-format arg slots; same accepted truncation as
 * TRACE_PTR_LOW32 when the field is itself a pointer.
 */
#define _TRACE_DER_PTR(p, m) ((p) ? (uint32_t) (uintptr_t) (p)->m : 0u)

#define _TRACE_PIDTID(a, p, t)                                           \
    (!(trace_masks.p) ||                                                 \
     ((trace_masks.p) == (uint32_t) _TRACE_DER_PTR((a)->process, pid) && \
      (!(trace_masks.t) || (trace_masks.t) == ((uint32_t) (a)->tid + 1))))

#define _TRACE_THPTID(a, n) _TRACE_PIDTID(a, thread_pid[n], thread_tid[n])
#define _TRACE_VTHPTID(a, n) _TRACE_PIDTID(a, vthread_pid[n], vthread_tid[n])
#define _TRACE_IPCPTID(a, e) _TRACE_PIDTID(a, comm_pid[e], comm_tid[e])

/* Control/process/thread state macro definitions */


/* For some macros, the argument can be either a thread or vthread. Do the right
 * thing.
 */
#define _TRACE_TH_EMIT_STATE(t, s)                     \
    if (TYPE_MASK(t->type) == TYPE_VTHREAD) {          \
        if (trace_masks.vth_mask & RTMUX_TRACE_VTH##s) \
            vth_em_st(t, STATE_##s);                   \
    } else {                                           \
        if (trace_masks.th_mask & RTMUX_TRACE_TH##s)   \
            th_em_st(t, STATE_##s, get_cpunum());      \
    }

#define _TRACE_TH_EMIT_CREATE(t)                                        \
    if (TYPE_MASK(t->type) == TYPE_VTHREAD) {                           \
        if (trace_masks.vth_mask & RTMUX_TRACE_VTHCREATE)               \
            vth_em_cd(t, _TRACE_THREAD_CREATE, _TRACE_PR_TH_CREATE_VT); \
    } else {                                                            \
        if (trace_masks.th_mask & RTMUX_TRACE_THCREATE)                 \
            th_em_cd(t, _TRACE_THREAD_CREATE, _TRACE_PR_TH_CREATE_T);   \
    }

#define _TRACE_TH_EMIT_DESTROY(t)                                         \
    if (TYPE_MASK(t->type) == TYPE_VTHREAD) {                             \
        if (trace_masks.vth_mask & RTMUX_TRACE_VTHDESTROY)                \
            vth_em_cd(t, _TRACE_THREAD_DESTROY, _TRACE_PR_TH_DESTROY_VT); \
    } else {                                                              \
        if (trace_masks.th_mask & RTMUX_TRACE_THDESTROY)                  \
            th_em_cd(t, _TRACE_THREAD_DESTROY, _TRACE_PR_TH_DESTROY_T);   \
    }

#define _TRACE_TH_EMIT_STATE_ONCPU(t, s, c)         \
    if (trace_masks.th_mask & (0x00000001U << (s))) \
        th_em_st(t, s, c);
#define _TRACE_TH_EMIT_ANY_STATE(t, s)              \
    if (trace_masks.th_mask & (0x00000001U << (s))) \
        th_em_st(t, s, get_cpunum());
#define _TRACE_TH_EMIT_NAME(t)                             \
    if (trace_masks.pr_mask & RTMUX_TRACE_PROCTHREAD_NAME) \
        th_em_name(t);
#define _TRACE_PR_EMIT_CREATE(p)                      \
    if (trace_masks.pr_mask & RTMUX_TRACE_PROCCREATE) \
        pr_em_create(p, (p)->pid);
#define _TRACE_PR_EMIT_DESTROY(p, id)                  \
    if (trace_masks.pr_mask & RTMUX_TRACE_PROCDESTROY) \
        pr_em_destroy(p, id);
#define _TRACE_PR_EMIT_CREATE_NAME(p)                      \
    if (trace_masks.pr_mask & RTMUX_TRACE_PROCCREATE_NAME) \
        pr_em_create_name(p);
#define _TRACE_PR_EMIT_DESTROY_NAME(p)                      \
    if (trace_masks.pr_mask & RTMUX_TRACE_PROCDESTROY_NAME) \
        pr_em_destroy_name(p);
#define _TRACE_EMIT_CONTROL_TIME(m, l)                      \
    if (trace_masks.control_mask & RTMUX_TRACE_CONTROLTIME) \
        time_em(m, l);
#define _TRACE_COMM_EMIT_SMSG(a, c, i)                                    \
    if (trace_masks.comm_mask[RTMUX_TRACE_COMM_SMSG] & _TRACE_ENTER_COMM) \
        comm_em(a, c, i, RTMUX_TRACE_COMM_SMSG);
#define _TRACE_COMM_EMIT_SNOTIFICATION(a, c, s, p)              \
    if (trace_masks.comm_mask[RTMUX_TRACE_COMM_SNOTIFICATION] & \
        _TRACE_ENTER_COMM)                                      \
        comm_em(a, c, s, RTMUX_TRACE_COMM_SNOTIFICATION);
#define _TRACE_COMM_EMIT_SNOTIFICATION_EV(c, p, s, e) \
    if (trace_masks.comm_mask[e] & _TRACE_ENTER_COMM) \
        comm_exe_em((c), (uint32_t) (s), (e));
#define _TRACE_COMM_EMIT_REPLY(a, c, i)                                    \
    if (trace_masks.comm_mask[RTMUX_TRACE_COMM_REPLY] & _TRACE_ENTER_COMM) \
        comm_em(a, c, i, RTMUX_TRACE_COMM_REPLY);
#define _TRACE_COMM_EMIT_ERROR(a, c, i)                                    \
    if (trace_masks.comm_mask[RTMUX_TRACE_COMM_REPLY] & _TRACE_ENTER_COMM) \
        comm_em(a, c, i, RTMUX_TRACE_COMM_ERROR);
#define _TRACE_COMM_EMIT_SNOTIFICATION_EXE(c, s, p) \
    _TRACE_COMM_EMIT_SNOTIFICATION_EV(c, p, s,      \
                                      RTMUX_TRACE_COMM_SNOTIFICATION_EXE)
#define _TRACE_COMM_EMIT_SNOTIFICATION_DIS(c, s, p) \
    _TRACE_COMM_EMIT_SNOTIFICATION_EV(c, p, s,      \
                                      RTMUX_TRACE_COMM_SNOTIFICATION_DIS)
#define _TRACE_COMM_EMIT_SNOTIFICATION_DEA(c, s, p) \
    _TRACE_COMM_EMIT_SNOTIFICATION_EV(c, p, s,      \
                                      RTMUX_TRACE_COMM_SNOTIFICATION_DEA)
#define _TRACE_COMM_EMIT_SNOTIFICATION_UN(c, s, p) \
    _TRACE_COMM_EMIT_SNOTIFICATION_EV(c, p, s,     \
                                      RTMUX_TRACE_COMM_SNOTIFICATION_UN)
#define _TRACE_COMM_EMIT_SNOTIFICATION_QUN(s, p)  \
    _TRACE_COMM_EMIT_SNOTIFICATION_EV(NULL, p, s, \
                                      RTMUX_TRACE_COMM_SNOTIFICATION_QUN)
#define _TRACE_COMM_IPC_RET(a) (a->syscall |= _TRACE_COMM_IPC_EXIT)
#define _TRACE_DESTROY_EH(p) destroy_eh(p)
#define _TRACE_COMM_EMIT_SIGNAL(t, s)                                       \
    if (trace_masks.comm_mask[RTMUX_TRACE_COMM_SIGNAL] & _TRACE_ENTER_COMM) \
    comm_em_signal(t, s)

#define _TRACE_SYS_EMIT_ADDRESS(thp, vaddr)                \
    if (trace_masks.system_mask[RTMUX_TRACE_SYS_ADDRESS] & \
        _TRACE_ENTER_SYSTEM)                               \
        trace_emit_address((thp), (uint32_t) (uintptr_t) (vaddr));
#define _TRACE_SYS_EMIT_PI_BOOST(thp, oldp, newp)           \
    if (trace_masks.system_mask[RTMUX_TRACE_SYS_PI_BOOST] & \
        _TRACE_ENTER_SYSTEM)                                \
        trace_emit_pi_boost((thp), (oldp), (newp));

/* Interrupt-to-IST latency hooks: trap-side claim to interrupt_wait return. */
#define _TRACE_IRQ_CLAIM(lp, itp) trace_emit_irq_claim((lp), (itp))
#define _TRACE_IST_DISPATCH(itp, thp) trace_emit_ist_dispatch((itp), (thp))

/* Per-kernel-call cost hooks: bracket the kcall handler in the trap dispatcher
 * and emit the elapsed time as the payload of one exit event, so the trace
 * ring's own lock and buffer write stay outside the measured window. The span
 * uses cpu_clock_ticks(), not clock_cycles(), because the consumer converts it
 * with qtimeptr->cycles_per_sec, which describes the time CSR.
 *
 * Three parts, so the mask is read once and the only work inside the measured
 * span is the span itself:
 *
 * _TRACE_KCALL_ARMED is the single load of the per-call mask, which keeps a
 * build that merely has CONFIG_INSTRUMENTATION compiled in down to one array
 * load when kcall tracing was never enabled. The caller has already rejected
 * num >= __KSYS_BAD, which is below _TRACE_MAX_KER_CALL_NUM, so the index is in
 * range.
 *
 * _TRACE_KCALL_PACK snapshots pid and tid BEFORE the clock starts: the
 * self-reap kcalls (__thread_destroy and __thread_destroyall on the calling
 * thread) free the thread_t and still return normally, so the exit hook must
 * not dereference the thread afterwards, and doing the packing ahead of
 * _TRACE_KCALL_TICKS keeps it out of the duration the block reports.
 *
 * _TRACE_KCALL_TICKS is taken last, so the span is the handler and nothing
 * else.
 */
#define _TRACE_KCALL_ARMED(num) \
    (trace_masks.ksys_call_masks[num] & _TRACE_EXIT_CALL)

/* Full-width pid and tid+1, snapshotted separately rather than pre-packed.
 *
 * They must stay full width because the RTMUX_TRACE_SETCLASSPID / SETEVENTPID
 * filters store a whole pid_t: pid_unique advances by PID_MASK + 1 and is
 * masked with INT_MAX, so a live pid routinely exceeds 16 bits (65556
 * observed). The predecessor _TRACE_CHK_PTID compared the full value, and
 * packing before the comparison would make every filtered event miss.
 * trace_emit_kcall packs them into the single d1 slot only after filtering.
 */
#define _TRACE_KCALL_PID(thp) \
    (((thp) && (thp)->process) ? (uint32_t) (thp)->process->pid : 0U)
#define _TRACE_KCALL_TID(thp) (((thp)) ? (uint32_t) (thp)->tid + 1U : 0U)
#define _TRACE_KCALL_TICKS(armed) ((armed) ? cpu_clock_ticks() : 0ULL)

/* A nonzero t0 means the mask was set when the call started, so the exit hook
 * does not re-read it: a call already running when another hart turned kcall
 * tracing on is skipped rather than reported as the whole uptime. Tracing
 * turned off mid-call is handled inside add_trace_event(), which bails on
 * main_flags == 0.
 */
#define _TRACE_KCALL_EMIT(pid, tid, num, t0)                                 \
    do {                                                                     \
        if (t0)                                                              \
            trace_emit_kcall((pid), (tid), (num), cpu_clock_ticks() - (t0)); \
    } while (0)

#else

#define _TRACE_TH_EMIT_CREATE(t)
#define _TRACE_TH_EMIT_DESTROY(t)
#define _TRACE_TH_EMIT_NAME(t)
#define _TRACE_TH_EMIT_STATE(t, s)
#define _TRACE_TH_EMIT_ANY_STATE(t, s)
#define _TRACE_PR_EMIT_CREATE(p)
#define _TRACE_PR_EMIT_CREATE_NAME(p)
#define _TRACE_PR_EMIT_DESTROY(p, id)
#define _TRACE_PR_EMIT_DESTROY_NAME(p)
#define _TRACE_DESTROY_EH(p)
#define _TRACE_EMIT_CONTROL_TIME(m, l)
#define _TRACE_COMM_EMIT_SMSG(a, c, i)
#define _TRACE_COMM_EMIT_SNOTIFICATION(a, c, s, p)
#define _TRACE_COMM_EMIT_SNOTIFICATION_EXE(c, s, p)
#define _TRACE_COMM_EMIT_SNOTIFICATION_DIS(c, s, p)
#define _TRACE_COMM_EMIT_SNOTIFICATION_DEA(c, s, p)
#define _TRACE_COMM_EMIT_SNOTIFICATION_UN(c, s, p)
#define _TRACE_COMM_EMIT_SNOTIFICATION_QUN(s, p)
#define _TRACE_COMM_EMIT_SIGNAL(t, s)
#define _TRACE_COMM_EMIT_REPLY(a, c, i)
#define _TRACE_COMM_EMIT_ERROR(a, c, i)
#define _TRACE_COMM_IPC_RET(a)
#define _TRACE_NOARGS(c)
#define _TRACE_GETSYSCALL(c) (c)
#define _TRACE_SYS_EMIT_ADDRESS(thp, vaddr)
#define _TRACE_SYS_EMIT_PI_BOOST(thp, oldp, newp) \
    do {                                          \
        (void) (thp);                             \
        (void) (oldp);                            \
        (void) (newp);                            \
    } while (0)
#define _TRACE_IRQ_CLAIM(lp, itp) \
    do {                          \
        (void) (lp);              \
        (void) (itp);             \
    } while (0)
#define _TRACE_IST_DISPATCH(itp, thp) \
    do {                              \
        (void) (itp);                 \
        (void) (thp);                 \
    } while (0)
#define _TRACE_KCALL_ARMED(num) ((void) (num), 0U)
#define _TRACE_KCALL_PID(thp) ((void) (thp), 0U)
#define _TRACE_KCALL_TID(thp) ((void) (thp), 0U)
#define _TRACE_KCALL_TICKS(armed) ((void) (armed), 0ULL)
#define _TRACE_KCALL_EMIT(pid, tid, num, t0) \
    do {                                     \
        (void) (pid);                        \
        (void) (tid);                        \
        (void) (num);                        \
        (void) (t0);                         \
    } while (0)

#endif

#define __KERTRACE_H_INCLUDED
#endif
