/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _CORE_GLOBALS_H
#define _CORE_GLOBALS_H


#define __KERNEL_H_INCLUDED

#include <inttypes.h>
#include <setjmp.h>

/* Configuration (CONFIG_PROCESSORS_MAX etc.) must come first */
#include <config.h>

/* include this early so that atomic ops can be overridden */
#include <atomic.h>

#include <arch/runtime.h>
#include <sys/syspage.h>

#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <minmax.h>

#include <sched.h>
#include <sys/sched-ops.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/trace.h>
#include <sys/node.h>

#include <startup.h>
#include <objects.h>
#include <arch/pcpu.h> /* struct pcpu — needs objects.h above */
#include <macros.h>
#include <pthread.h>
#include <kdebug.h>
#include <sys/fault.h>
#include <sys/debug.h>
#include <debug.h>
#include <priv.h>
#include <decls.h>
#include <query.h>
#include <sys/calls.h>
#include <private/priv-calls.h> /* PRIV sub-namespace + syscall_priv */
#include <smpswitch.h>

#include <paging.h>
#include <options.h>
#include <core/macros.h>
#include <core/args.h>
#include <trace/events.h>

/* Kernel-call dispatch function signature. The second argument is a pointer to
 * a per-call kcall_args_* struct; each entry casts it to its own struct type
 * internally. MK_KSYSTABLE casts each handler back to this type so
 * ksys_call_table[] is well-typed under C23 strict prototypes. Declared here
 * (before decls.h) because kcall_attach uses it in its declaration.
 */
typedef int kdecl ksys_call_t(thread_t *act, void *kap);

#include <core/decls.h>
#include "percpu.h"
#include "lock.h"
#include <log.h>

/* DBGOUT — now in <kernel/log.h> */

/* Declaration order matters for cache locality: globals.c emits storage for
 * every EXT here in the same order, so high-traffic globals declared near each
 * other share cache lines. Group hot pairs (e.g. scheduler hook + dispatch
 * pointer) adjacent; pad cold/rare globals to the ends.
 */

#ifdef KERNEL_DEFN
#define COND_EXT(x) x
#define EXT
#define INIT1(a) = {a}
#define INIT2(a, b) = {a, b}
#define INIT3(a, b, c) = {a, b, c}
#define INIT7(a, b, c, d, e, f, g) = {a, b, c, d, e, f, g}
#else
#define COND_EXT(x) 0
#define EXT extern
#define INIT1(a)
#define INIT2(a, b)
#define INIT3(a, b, c)
#define INIT7(a, b, c, d, e, f, g)
#undef INITSOUL
#define INITSOUL(a, b, c, d, e)
#endif

EXT int proc_service_scoid;

/* Additional variables used for debug etc. */
EXT struct cpu_extra_state *kdextra; /* for debugging */

/* Low bandwidth globals */
EXT int pid_unique;
#ifdef CONFIG_MONITORING_KTHREAD

/* monitor_hart_id — the hart captured for the monitoring kthread. Set in
 * kernel_entry's syspage_init before num_processors is decremented. ~0U if no
 * capture happened (only 1 hart, etc.).
 */
EXT unsigned monitor_hart_id INIT1(~0U);
#endif
EXT unsigned long user_boundary_addr INIT1(VM_USER_SPACE_BOUNDARY);
EXT unsigned clk_tck INIT1(100);
EXT unsigned long tick_minimum INIT1(RTMUX_TICKSIZE_MIN);
EXT int _argc;
EXT char **_argv;
EXT char **environ;
EXT vector_t interrupt_vector;
EXT vector_t process_vector;

/* process_rcu — RCU instance guarding the lifetime of process_t pointers
 * obtained via lookup_pid / direct process_vector access from BKL-less calls.
 * See kernel/sync/rcu.c for the definition and kernel/include/rcu.h for usage
 * notes.
 */
#include "rcu.h"

/* Defined with static initializer in kernel/sync/rcu.c — explicit "extern" (not
 * EXT) to keep globals.c from emitting a second tentative definition.
 */
extern rcu_t process_rcu;

/* tthread_rcu — RCU instance guarding thread_t pointer lifetime for any kcall
 * that does "vector_lookup(&prp->threads, …)" and then dereferences the result.
 * Writer is thread_destroy in kernel/thread/lifecycle.c; readers are catalogued
 * in doc/tex/KerFuncRef/ch04-threads.tex.
 */
extern rcu_t tthread_rcu;

/* connobj_rcu — RCU instance guarding connect_t AND channel_t pointer lifetime
 * for lookups via per-process "chancons" / "fdcons" and the global
 * "global_channel_vector". One instance covers both object families because
 * chancons carries both. Writers: ksys_connect_detach (connect_t) and
 * ksys_channel_destroy (channel_t + cleanup-loop connect_t frees).
 */
extern rcu_t connobj_rcu;

EXT vector_t query_vector;
EXT vector_t soul_vector;
EXT vector_t vthread_vector;
EXT vector_t global_channel_vector;
EXT credential_t *credential_list;
EXT limits_t *limits_list;
EXT process_t *kernel_threads_prp;

/* Cached pointer to the roottask (ROOTTASK_PID=1) process container. Set in
 * roottask.c when the U-mode roottask launches. Used by set_safe_as() to swap a
 * remote hart's SATP off a dying as before kext_process_shutdown frees the
 * pgdir. We use the roottask as because kernel_threads_prp holds only
 * kernel-resident tThreads, not a pgdir, so calling
 * as_switch(kernel_threads_prp, ...) is a no-op. That would leave as_prp[cpu]
 * pointing at the dying process and loop the IPI_TLB_SAFE retry forever.
 */
EXT process_t *roottask_prp;
EXT unsigned boot_cpunum;

/* Stall detector — kernel/debug/stall-detector.c (CONFIG_STALL_DETECTOR) */
#ifdef CONFIG_STALL_DETECTOR
void stall_detector_tick(void);

/* Per-hart neighbour watchdog — kernel/debug/hartwatch.c. Called from the
 * supervisor-timer trap on every hart, on both the in-kernel and full paths.
 */
void hartwatch_tick(void);

/* Absolute monotonic-ns deadline by which the heartbeat must advance before the
 * watchdog declares a stall, or all-ones when disarmed (early boot) or already
 * fired. Read-only; contributes to clock_next_deadline().
 */
uint64_t stall_next_deadline(void);
#else
static inline void stall_detector_tick(void) {}
static inline uint64_t stall_next_deadline(void)
{
    return ~(uint64_t) 0;
}
#endif

/* connect_t.links is a single refcount shared across CPUs. Route every
 * decrement through this so the last-drop test cannot be gotten wrong:
 * atomic_sub_value returns the OLD value, so old == 1 is the last drop and old
 * == 0 is a fatal double-drop underflow. Callers that must not free the soul
 * (the client ConnectDetach path, a failed send) use connect_client_drop().
 */
static inline int connect_links_drop(connect_t *cop)
{
    unsigned old = atomic_sub_value(&cop->links, 1);

    if (old == 0)
        crash();
    return old == 1;
}

/* connect_link_get - take a sender's reference on a connection found by lookup.
 *
 * links counts client references only (the fd slots plus one per sender blocked
 * on the connection); it reaches 0 exactly once, at the last drop. Fails when
 * the count already is 0: the last client reference went away
 * (connect_client_drop sent the disconnect notice) between the caller's lookup
 * and now, so the connection may not be used. Succeeding while a concurrent
 * client drop is in flight is fine: that drop then is not the last, and the
 * reply-side drop of this reference is.
 */
static inline int connect_link_get(connect_t *cop)
{
    unsigned old = __atomic_load_n(&cop->links, __ATOMIC_RELAXED);

    do {
        if (old == 0)
            return 0;
    } while (!__atomic_compare_exchange_n(&cop->links, &old, old + 1, 1,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    return 1;
}

/* Device base addresses discovered from FDT (defined in board.c) */
extern paddr_t uart_base;
extern paddr_t plic_base;
extern size_t plic_size;
extern paddr_t clint_base;
EXT unsigned legal_cpu_bitmask;
EXT uintptr_t run_ker_stack_bot;
EXT uintptr_t run_ker_stack_top;
EXT struct cpupage_entry *_cpupage_ptr;
EXT struct system_private_entry *privateptr;
EXT unsigned intrinfo_num;
EXT unsigned __cpu_flags;
EXT remote_ipc_state_t remote_ipc;

/* Defined in objects.c — pointer at roottask-resident dispatch table after
 * kext_register_proc_hooks() lands; before that, points at kernel storage.
 */
extern proc_hooks_t *proc_hooks;
EXT int scheduler_type; /* one of SCHEDULER_TYPE_* values in macros.h */

EXT int(rdecl *debug_thread_fault)(thread_t *thp, siginfo_t *info);
EXT void(rdecl *debug_attach_brkpts)(debug_t *dep);
EXT void(rdecl *debug_detach_brkpts)(debug_t *dep);
EXT void(rdecl *debug_moduleinfo)(process_t *prp, thread_t *thp, void *dbg);
EXT void (*sched_trace_initial_parms)(void);
EXT kdispatch_t *(*init_scheduler)(void) INIT1(init_scheduler_default);

EXT uint64_t
    startup_stack[STARTUP_STACK_NBYTES / sizeof(uint64_t)] INIT1(__STACK_SIG);
EXT unsigned rr_interval_mul INIT1(RTMUX_RR_INTERVAL_MUL_DEFAULT);


/* Main kernel variables used in all paths - group those that are modified
 * together and those that are static afterwards.
 */
#if !COND_EXT(HAVE_INKERNEL_STORAGE)
EXT volatile unsigned inkernel;
#endif

/* Wrappers around pcpu_array[cpu].pc_curthread. Use these instead of indexing
 * pcpu_array directly — they centralize the NULL-check (_chk variant) and the
 * bogus-pointer guard on store, so a stray write crashes at the store site
 * rather than at some downstream deref.
 */
thread_t *get_active_thread(unsigned cpu);
thread_t *get_active_thread_chk(unsigned cpu);
void set_active_thread(unsigned cpu, thread_t *thp);

EXT int inspecret;

/* actives_prp[] now lives at pcpu_array[hart].pc_active_prp as_prp[] now lives
 * at pcpu_array[hart].pc_as_prp xfer_handlers[] now lives at
 * pcpu_array[hart].pc_xfer_handlers
 */
EXT struct cpupage_entry *cpupageptr[PROCESSORS_MAX];
EXT struct _thread_local_storage *idle_tls[PROCESSORS_MAX];

/* idle_thp[] now lives at pcpu_array[hart].pc_idle_thp Per-CPU dispatch (ready)
 * queues, now at pcpu_array[hart].pc_dispatch. Indexed by hart ID. Slot is
 * non-NULL for every active hart; undefined slots stay NULL. Allocated by
 * init_scheduler_default(). Thread placement: a thread is enqueued on the
 * dispatch instance for the hart it should run on next; thp->dpp points at the
 * same instance. Migration changes thp->dpp. Each per-CPU dispatch instance
 * carries its own kspinlock_t lock.
 */
#if !COND_EXT(HAVE_KERSTACK_STORAGE)
EXT uintptr_t ksys_stack[PROCESSORS_MAX];
#endif

/* actives_fpu[] now lives at pcpu_array[hart].pc_active_fpu Defined in
 * objects.c — see proc_hooks_t *proc_hooks above for the same shape.
 */
extern vm_ops_t *vm_ops;
EXT void(rdecl *mark_running)(thread_t *act);
/* actives_pcr[] now lives at pcpu_array[hart].pc_active_pcr */
EXT hash_t sync_hash INIT1(0x3ff); /* Must be a mask */
EXT kspinlock_t sync_slock;        /* protects sync_hash + ksync_t chain */
/* The dispatch ready queues are per-CPU: each pcpu_array[hart].pc_dispatch
 * instance carries its own kspinlock_t lock field, initialized in
 * init_scheduler_default(). Same-CPU dispatch ops take LOCAL_DPP->lock
 * (macros.h macro); cross-CPU ops take the target dpp's lock with
 * single-lock-held-at-a-time discipline.
 */
EXT kspinlock_t alloc_slock; /* protects kernel heap free lists */
EXT kspinlock_t soul_slock;  /* protects soul_t freelists + accounting */
EXT kspinlock_t pa_slock;    /* serializes the physical quantum */
                             /* allocator (pa_alloc_body, pa_alloc_given,
                              * _pa_free) free-list + bh-generation +
                              * mem_free_size mutations.
                              */
EXT kspinlock_t global_channel_slock; /* protects global_channel_vector
                                         (global-channel directory)
                                         */
EXT kspinlock_t as_slock;             /* serializes ksys_manipulate's */
/* do_manipulation state machine on adp. Rank MEMPART. */
EXT kspinlock_t process_vector_slock; /* protects process_vector */
                                      /* mutations (vector_add/_rem) and the
                                       * parent->{child,sibling,num_processes}
                                       * chain (rank PROCVEC).
                                       */
EXT kspinlock_t cred_slock;           /* protects credential_t */
/* mutations + credential_list (rank PROCESS). */
EXT kspinlock_t limits_slock; /* protects limits_t chain */
                              /* mutations + limits_list (rank PROCESS). */
EXT kspinlock_t trace_slock;  /* protects trace ring buffer */
                              /* write index (rank INTREVENT). */
/* Protects the roottask_stack_cache free list (push/pop race between
 * kext_register_pool_stack, roottask_stack_alloc, roottask_stack_free,
 * roottask_stack_purge). Rank MEMPART.
 */
EXT kspinlock_t roottask_stack_slock;

/* Interrupt path globals pending_preempt_prio[] now lives at
 * pcpu_array[hart].pc_pending_preempt_prio — the per-CPU preemption-priority
 * sentinel.
 */
EXT struct intrinfo_entry *intrinfoptr;
EXT intr_level_t *interrupt_level;
EXT struct _thread_local_storage intr_tls INIT3(0, 0, &intr_tls.__errval);
/* intr_fault_event removed — faults handled directly, no signal delivery */


/* Clock timer */
EXT char overrun;
EXT interrupt_t *clock_isr;
#if defined(CONFIG_COUNT_CYCLES)
EXT uint64_t cycles;
#endif
EXT struct qtime_entry *qtimeptr;

EXT struct syspage_entry *_syspage_ptr;
EXT sched_replenish_t *ss_replenish_list;
EXT int(rdecl *scheduler_tick_hook)(); /* return true if the current thread's
                                          timeslice should be ended
                                          */
EXT int(rdecl *kcall_op_thread_create_hook)(thread_t *act,
                                            process_t *prp,
                                            const struct sigevent *evp,
                                            unsigned thread_create_flags,
                                            thread_t *thp);
EXT void(rdecl *kcall_op_thread_destroy_hook)(thread_t *thp);
EXT void(rdecl *kcall_op_clock_handler_hook)(thread_t *act);
extern unsigned arch_timer_value(struct qtime_entry *);

EXT int(rdecl *kcall_op_thread_ctl_hook)(thread_t *act,
                                         thread_t *op,
                                         struct kcall_args_thread_ctl *kap);

/* Object allocator - put most-used object pools at beginning. */
EXT int alloc_critical;
EXT soul_t notification_souls
    INITSOUL(LIMITS_NOTIFICATION, sizeof(notification_t), 64, SOUL_CRITICAL, 0);
EXT soul_t sync_souls INITSOUL(LIMITS_SYNC, sizeof(ksync_t), 64, 0, 0);
EXT soul_t timer_souls INITSOUL(LIMITS_TIMER, sizeof(ktimer_t), 12, 0, 0);
EXT soul_t thread_souls
    INITSOUL(LIMITS_THREAD, sizeof(thread_t), 24, SOUL_NOCLEAR, 0);
EXT soul_t connect_souls INITSOUL(LIMITS_CONNECT, sizeof(connect_t), 64, 0, 0);
EXT soul_t channel_souls INITSOUL(LIMITS_CHANNEL, sizeof(channel_t), 8, 0, 0);
EXT soul_t process_souls INITSOUL(LIMITS_PROCESS, sizeof(process_t), 12, 0, 0);
EXT soul_t global_channel_souls
    INITSOUL(LIMITS_CHANNEL, sizeof(global_channel_t), 8, 0, 0);
EXT soul_t interrupt_souls
    INITSOUL(LIMITS_INTERRUPT, sizeof(interrupt_t), 8, 0, 0);
EXT soul_t syncevent_souls
    INITSOUL(LIMITS_SYNCEVENT, sizeof(sync_event_t), 4, 0, 0);
EXT soul_t fpu_souls INITSOUL(0, sizeof(FPU_REGISTERS), 4, 0, FPUDATA_ALIGN);
EXT soul_t credential_souls INITSOUL(0, sizeof(credential_t), 4, 0, 0);
EXT soul_t limits_souls INITSOUL(0, sizeof(limits_t), 4, 0, 0);
EXT soul_t vthread_souls INITSOUL(0, SIZEOF_VTHREAD, 4, 0, 0);
EXT soul_t debug_souls INITSOUL(0, sizeof(debug_t), 1, 0, 0);
EXT soul_t breakpt_souls INITSOUL(0, sizeof(breakpoint_t), 1, SOUL_CRITICAL, 0);
EXT soul_t ssinfo_souls INITSOUL(0, sizeof(sched_info_t), 1, 0, 0);
EXT soul_t threadname_souls INITSOUL(0, THREAD_NAME_FIXED_SIZE, 1, 0, 0);


/* High-bandwidth scheduler variables/func pointers */
EXT volatile uint64_t ss_replenish_time;
EXT int(rdecl *may_thread_run)(thread_t *thp);
EXT void(rdecl *block_and_ready)(thread_t *thp);
EXT void(rdecl *ready)(thread_t *thp);
EXT thread_t *(rdecl *select_thread)(int cpu, int prio);
EXT void(rdecl *adjust_priority)(thread_t *thp,
                                 unsigned prio,
                                 kdispatch_t *dpp,
                                 int priority_inherit);
EXT void(rdecl *resched)(void);
EXT void(rdecl *yield)(void);


/* SMP variables */
EXT int smp_dispatch_enabled
    INIT1(0); /* gate: APs don't pick threads until init is ready */
EXT thread_t *need_to_run INIT1(NULL);
EXT int need_to_run_cpu INIT1(-1);
EXT void *kcallptr;
/* alives[] now lives at pcpu_array[hart].pc_alive */

/* clockcycles_offset[] now lives at pcpu_array[hart].pc_clockcycles_offset */
#if CONFIG_PROCESSORS_MAX > 1

/* ipicmds[] now lives at pcpu_array[hart].pc_ipicmds */
EXT intrspin_t intr_slock;
EXT intrspin_t clock_slock;
EXT intrspin_t debug_slock;

#endif


/* Prototypes for external declarations */
extern const struct fault_handlers xfer_src_handlers;
extern const struct fault_handlers xfer_dst_handlers;
extern const struct fault_handlers xfer_fault_handlers;
extern const struct fault_handlers xfer_async_handlers;

/* Per-hart xfer longjmp targets (see macros.h SET_XFER_ENV/GET_XFER_ENV for why
 * it must be per-CPU).
 */
extern jmp_buf *xfer_env[CONFIG_PROCESSORS_MAX];
extern const uint8_t byte_log2[1 << 8]; /* defined in globals.c */

extern ksys_call_t *ksys_call_table[];
extern const int ksys_call_entry_num;

#if defined(CONFIG_INSTRUMENTATION)

EXT struct trace_masks {
    /* Mask declarations/definitions */
    uint32_t ring_mode;
    uint32_t main_flags;
    uint32_t control_mask;
    uint32_t pr_mask;
    uint32_t th_mask;
    uint32_t vth_mask;
    uint32_t system_mask[_TRACE_MAX_SYSTEM_NUM];
    uint32_t comm_mask[_TRACE_MAX_COMM_NUM];
    uint32_t int_masks[_TRACE_MAX_INT_NUM];
    uint32_t int_handler_masks[_TRACE_MAX_INT_NUM];
    uint32_t ksys_call_masks[_TRACE_MAX_KER_CALL_NUM];
    uint32_t user_mask[_TRACE_MAX_USER_NUM];

    /* emitting handle function process pointers and their argument areas */
    ehandler_data_t *control_time_ehd_p;
    ehandler_data_t *pr_create_ehd_p;
    ehandler_data_t *pr_destroy_ehd_p;
    ehandler_data_t *pr_create_name_ehd_p;
    ehandler_data_t *pr_destroy_name_ehd_p;
    ehandler_data_t *th_name_ehd_p; /* Part of the process class */

    ehandler_data_t *thread_ehd_p[_TRACE_MAX_TH_STATE_NUM];
    ehandler_data_t *vthread_ehd_p[_TRACE_MAX_TH_STATE_NUM];
    ehandler_data_t *int_enter_ehd_p[_TRACE_MAX_INT_NUM];
    ehandler_data_t *int_exit_ehd_p[_TRACE_MAX_INT_NUM];
    ehandler_data_t *int_handler_enter_ehd_p[_TRACE_MAX_INT_NUM];
    ehandler_data_t *int_handler_exit_ehd_p[_TRACE_MAX_INT_NUM];
    ehandler_data_t *ksys_call_enter_ehd_p[_TRACE_MAX_KER_CALL_NUM];
    ehandler_data_t *ksys_call_exit_ehd_p[_TRACE_MAX_KER_CALL_NUM];
    ehandler_data_t *system_ehd_p[_TRACE_MAX_SYSTEM_NUM];
    ehandler_data_t *comm_ehd_p[_TRACE_MAX_COMM_NUM];

    uint32_t ksys_call_pid[_TRACE_MAX_KER_CALL_NUM];
    uint32_t ksys_call_tid[_TRACE_MAX_KER_CALL_NUM];
    uint32_t thread_pid[_TRACE_MAX_TH_STATE_NUM];
    uint32_t thread_tid[_TRACE_MAX_TH_STATE_NUM];
    uint32_t vthread_pid[_TRACE_MAX_TH_STATE_NUM];
    uint32_t vthread_tid[_TRACE_MAX_TH_STATE_NUM];
    uint32_t system_pid[_TRACE_MAX_SYSTEM_NUM];
    uint32_t system_tid[_TRACE_MAX_SYSTEM_NUM];
    uint32_t comm_pid[_TRACE_MAX_COMM_NUM];
    uint32_t comm_tid[_TRACE_MAX_COMM_NUM];

    /* class emitting (global) handle function process pointers and their
     * argument areas
     */
    ehandler_data_t *class_control_ehd_p;
    ehandler_data_t *class_ker_call_enter_ehd_p;
    ehandler_data_t *class_ker_call_exit_ehd_p;
    ehandler_data_t *class_int_enter_ehd_p;
    ehandler_data_t *class_int_exit_ehd_p;
    ehandler_data_t *class_int_handler_enter_ehd_p;
    ehandler_data_t *class_int_handler_exit_ehd_p;
    ehandler_data_t *class_process_ehd_p;
    ehandler_data_t *class_thread_ehd_p;
    ehandler_data_t *class_vthread_ehd_p;
    ehandler_data_t *class_system_ehd_p;
    ehandler_data_t *class_comm_ehd_p;

    /* Event handlers storage and synchronization */
    ehandler_data_t eh_storage[_TRACE_MAX_EV_HANDLER_NUM];
    uint32_t eh_num;
    struct intrspin eh_spin;

    /* State management */
    struct intrspin state_spin;
    void *buff_0_ptr;
    uint32_t buff_num;
    uint32_t send_buff_num;
    uint32_t skip_buff;
    uint32_t max_events;
} trace_masks;
EXT uint32_t trace_force_flush;

/* global masks */
EXT uint32_t int_enter_enable_mask;
EXT uint32_t int_exit_enable_mask;

#endif /* KERNEL_DEFN */

#include <arch/defs.h>

#endif /* _CORE_GLOBALS_H */
