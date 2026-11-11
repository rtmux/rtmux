/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * priv-calls.h -- private kernel/roottask kcall sub-namespace.
 */

#ifndef _KER_PRIV_KCALLS_H
#define _KER_PRIV_KCALLS_H

/* Coarse capability classes for the private kernel/roottask ABI. */
#define RTMUX_PRIV_CAP_SYSTEM (1ULL << 0)
#define RTMUX_PRIV_CAP_VM (1ULL << 1)
#define RTMUX_PRIV_CAP_PTE (1ULL << 2)
#define RTMUX_PRIV_CAP_IPC (1ULL << 3)
#define RTMUX_PRIV_CAP_CRED (1ULL << 4)
#define RTMUX_PRIV_CAP_DEBUG (1ULL << 5)
#define RTMUX_PRIV_CAP_PROCESS (1ULL << 6)
#define RTMUX_PRIV_CAP_PA (1ULL << 7)
#define RTMUX_PRIV_CAP_NOTIFICATION (1ULL << 8)
#define RTMUX_PRIV_CAP_THREAD (1ULL << 9)
#define RTMUX_PRIV_CAP_TIME (1ULL << 10)
#define RTMUX_PRIV_CAP_TRACE (1ULL << 11)
#define RTMUX_PRIV_CAP_SCHED (1ULL << 12)

/* Hardware interrupt ownership: attaching an IST to a real IRQ and taking the
 * IO privilege it needs. A distinct delegable authority, not folded into
 * SYSTEM.
 */
#define RTMUX_PRIV_CAP_IRQ (1ULL << 13)

/* Debug fault injection (CONFIG_DEBUG_INJECT only): stall or hang a hart, fail
 * an allocation. Its own capability so that no server holds it by accident.
 */
#define RTMUX_PRIV_CAP_INJECT (1ULL << 14)
#define RTMUX_PRIV_CAP_ALL ((1ULL << 15) - 1)

#ifdef CONFIG_INSTRUMENTATION
#define RTMUX_PRIV_TRACE_HANDLER kext_trace
#else
#define RTMUX_PRIV_TRACE_HANDLER NULL
#endif

#ifdef CONFIG_DEBUG_INJECT
#define RTMUX_PRIV_INJECT_HANDLER kext_debug_inject
#else
#define RTMUX_PRIV_INJECT_HANDLER NULL
#endif

/* Single source of truth for private-call IDs, required capability class, and
 * kernel-side handler. Consumers pass X(ID, CAP, HANDLER).
 */
#define RTMUX_PRIV_CALL_LIST(X)                                   \
    X(SHUTDOWN, SYSTEM, do_shutdown)                              \
    X(VADDRINFO, VM, ext_vaddrinfo)                               \
    X(SYSADDR_MAP, VM, sysaddr_map)                               \
    X(MANIPULATE, PTE, ksys_manipulate)                           \
    X(SYNCHASH_REM, SYSTEM, ksys_synchash_rem)                    \
    X(REBOOT, SYSTEM, reboot)                                     \
    X(CLOCK_START, TIME, kext_clock_start)                        \
    X(CONNECT_DETACH, IPC, kext_connect_detach)                   \
    X(CRED_DIRTY, CRED, kext_cred_dirty)                          \
    X(CRED_GET, CRED, kext_cred_get)                              \
    X(CRED_SET, CRED, kext_cred_set)                              \
    X(DEBUG_ATTACH, DEBUG, kext_debug_attach)                     \
    X(DEBUG_DETACH, DEBUG, kext_debug_detach)                     \
    X(DEBUG_INSTALL, DEBUG, kext_debug_install)                   \
    X(DEBUG_PROCESS, DEBUG, kext_debug_process)                   \
    X(EXIT_DESTROY_THREADS, PROCESS, kext_exit_destroy_threads)   \
    X(PA_ALLOC_GIVEN, PA, kext_pa_alloc_given)                    \
    X(PA_FREE, PA, kext_pa_free)                                  \
    X(PA_FREE_INFO, PA, kext_pa_free_info)                        \
    X(PA_FREE_PADDR, PA, kext_pa_free_paddr)                      \
    X(PA_FREE_LIST, PA, kext_pa_free_list)                        \
    X(PA_INIT, PA, kext_pa_init)                                  \
    X(PA_START_BACKGROUND, PA, kext_pa_start_background)          \
    X(PA_SYSRAM_OVERLAP, PA, kext_pa_sysram_overlap)              \
    X(PA_GET_SIZES, PA, kext_pa_get_sizes)                        \
    X(PA_QUANTUM_TO_PADDR, PA, kext_pa_quantum_to_paddr)          \
    X(PA_PADDR_TO_QUANTUM, PA, kext_pa_paddr_to_quantum)          \
    X(CON_GETC, SYSTEM, kext_con_getc)                            \
    X(REGISTER_MM_HELPERS, VM, kext_register_mm_helpers_kex)      \
    X(LEGAL_CPU_MASK, SYSTEM, kext_legal_cpu_mask)                \
    X(HEARTBEAT, SYSTEM, kext_heartbeat)                          \
    X(STOP_THREADS, THREAD, kext_stop_threads)                    \
    X(GET_KERN_PGDIR, PTE, kext_get_kern_pgdir)                   \
    X(PROCESS_BIND, PROCESS, kext_process_bind)                   \
    X(PROCESS_CREATE, PROCESS, kext_process_create)               \
    X(PROCESS_DESTROY, PROCESS, kext_process_destroy)             \
    X(PROCESS_DESTROYALL, PROCESS, kext_process_destroyall)       \
    X(PROCESS_EXEC, PROCESS, kext_process_exec)                   \
    X(PROCESS_SHUTDOWN, PROCESS, kext_process_shutdown)           \
    X(PROCESS_STARTUP, PROCESS, kext_process_startup)             \
    X(QUERY_OBJECT, PROCESS, kext_query_object)                   \
    X(REGISTER_PROC_HOOKS, PROCESS, kext_register_proc_hooks)     \
    X(REPARENT, PROCESS, kext_reparent)                           \
    X(SELF_PRP, PROCESS, kext_self_prp)                           \
    X(SIGNAL_DELIVER, NOTIFICATION, kext_signal_deliver)          \
    X(SMP_BRINGUP, SYSTEM, kext_smp_bringup)                      \
    X(STACK_CONT, THREAD, kext_stack_cont)                        \
    X(STACK_PURGE, THREAD, kext_stack_purge)                      \
    X(STACK_WAIT_INFO, THREAD, kext_stack_wait_info)              \
    X(THREAD_CTL, THREAD, kext_thread_ctl)                        \
    X(THREAD_TLS, THREAD, kext_thread_tls)                        \
    X(TIMER_DESTROY, TIME, kext_timer_destroy)                    \
    X(TRACE, TRACE, RTMUX_PRIV_TRACE_HANDLER)                     \
    X(AS_MEMCPY, VM, kext_as_memcpy)                              \
    X(REGISTER_POOL_STACK, THREAD, kext_register_pool_stack)      \
    X(QUERY_OBJECT_DONE, PROCESS, kext_query_object_done)         \
    X(PROC_QUERY, PROCESS, kext_proc_query)                       \
    X(CHANNEL_HAS_RECEIVERS, IPC, kext_channel_has_receivers)     \
    X(MMAP_PHYS_USER, VM, kext_root_mmap_phys_user)               \
    X(MMAP_ANON_USER, VM, kext_root_mmap_anon_user)               \
    X(PA_ALLOC_FULL, PA, kext_pa_alloc_full)                      \
    X(PA_PQ_OP, PA, kext_pa_pq_op)                                \
    X(KPAGE_ZERO, PA, kext_kpage_zero)                            \
    X(SELF_ADP, PROCESS, kext_self_adp)                           \
    X(PROC_SET_SPAWN, PROCESS, kext_proc_set_spawn)               \
    X(PROC_REGISTER_SYSCHAN, PROCESS, kext_proc_register_syschan) \
    X(PROC_LIST_CHILDREN, PROCESS, kext_proc_list_children)       \
    X(PROC_WAIT_FINALIZE, PROCESS, kext_proc_wait_finalize)       \
    X(PROC_NOZOMBIE_MARK, PROCESS, kext_proc_nozombie_mark)       \
    X(PROC_EXIT_MARK, PROCESS, kext_proc_exit_mark)               \
    X(PROC_DESTROY_CHANCONS, PROCESS, kext_proc_destroy_chancons) \
    X(PROC_DESTROY_FDCONS, PROCESS, kext_proc_destroy_fdcons)     \
    X(PROC_CLEAR_PLS, PROCESS, kext_proc_clear_pls)               \
    X(PROC_EXIT_FINALIZE, PROCESS, kext_proc_exit_finalize)       \
    X(PROC_FLAGS_OP, PROCESS, kext_proc_flags_op)                 \
    X(PROC_SET_RLIMIT, PROCESS, kext_proc_set_rlimit)             \
    X(PROC_CLEAR_LCP, PROCESS, kext_proc_clear_lcp)               \
    X(PROC_INDEX_TO_PID, PROCESS, kext_proc_index_to_pid)         \
    X(PROC_SET_OCB_LIST, PROCESS, kext_proc_set_ocb_list)         \
    X(PROC_CAP_STATS, IPC, kext_proc_cap_stats)                   \
    X(PROC_ENDPOINT_CAP, IPC, kext_proc_endpoint_cap)             \
    X(PROC_CAP_MINT, PROCESS, kext_proc_cap_mint)                 \
    X(SOUL_STATS, SYSTEM, kext_soul_stats)                        \
    X(DEBUG_INJECT, INJECT, RTMUX_PRIV_INJECT_HANDLER)            \
    X(ICACHE_SYNC, PTE, kext_icache_sync_all)

enum {
#define RTMUX_PRIV_ENUM(id, cap, handler) __KSYS_PRIV_##id,
    RTMUX_PRIV_CALL_LIST(RTMUX_PRIV_ENUM)
#undef RTMUX_PRIV_ENUM
        __KSYS_PRIV_NR
};

/* syscall_priv -- invoke a private kcall by ID.
 *
 * Kernel-side dispatch verifies the ID and the calling process's capability
 * class, then runs the registered handler.
 */
long syscall_priv(int id, void *arg);

#endif /* _KER_PRIV_KCALLS_H */
