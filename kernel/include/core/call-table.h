/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * MK_KSYSTABLE — shared macro that emits the kernel-call dispatch table
 * entries. Used by:
 * - core/sys.c (the real dispatch table)
 * - trace/instr/wrappers.c (parallel trace-wrapper tables)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KSYS_CALL_TBL_H
#define KSYS_CALL_TBL_H

/* Each entry is cast to ksys_call_t * (declared in kernel/core/globals.h) so
 * the resulting table is well-typed under C23 strict prototypes. Individual
 * handlers' second-argument type (struct kcall_args_*) is hidden behind the
 * void * in ksys_call_t; each handler re-narrows it at the function head.
 */
#define _KT(fn) ((ksys_call_t *) (fn))

#define MK_KSYSTABLE(pref)                                                     \
    _KT(pref##_nop), _KT(pref##_trace_event), _KT(pref##_priv),                \
        _KT(pref##_dbgprint), _KT(pref##_bad), _KT(pref##_bad),                \
        _KT(pref##_bad),                                                       \
                                                                               \
        _KT(pref##_sys_cpupage_get), _KT(pref##_sys_cpupage_set),              \
        _KT(pref##_bad),                                                       \
                                                                               \
        _KT(pref##_msg_current), _KT(pref##_msg_sendv), _KT(pref##_msg_sendv), \
        _KT(pref##_msg_error), _KT(pref##_msg_receivev),                       \
        _KT(pref##_msg_replyv), _KT(pref##_msg_readv), _KT(pref##_msg_writev), \
        _KT(pref##_bad), /* msg_readwritev — removed, ENOSYS */                \
        _KT(pref##_msg_info), _KT(pref##_msg_sendnotification),                \
        _KT(pref##_msg_deliver_event), _KT(pref##_msg_keydata),                \
        _KT(pref##_msg_readiov), _KT(pref##_msg_receivev),                     \
        _KT(pref##_msg_verify_event),                                          \
                                                                               \
        _KT(pref##_bad), /* signal_kill — removed */                           \
        _KT(pref##_bad), /* signal_return — removed */                         \
        _KT(pref##_bad), /* signal_fault — removed */                          \
        _KT(pref##_bad), /* signal_action — removed */                         \
        _KT(pref##_bad), /* signal_procmask — removed */                       \
        _KT(pref##_bad), /* signal_suspend — removed */                        \
        _KT(pref##_bad), /* signal_waitinfo — removed */                       \
        _KT(pref##_bad), _KT(pref##_bad),                                      \
                                                                               \
        _KT(pref##_channel_create), _KT(pref##_channel_destroy),               \
        _KT(pref##_bad), /* channel_connect_attr — removed */                  \
        _KT(pref##_bad),                                                       \
                                                                               \
        _KT(pref##_connect_attach), _KT(pref##_connect_detach),                \
        _KT(pref##_connect_server_info), _KT(pref##_connect_client_info),      \
        _KT(pref##_connect_flags), _KT(pref##_bad), _KT(pref##_bad),           \
                                                                               \
        _KT(pref##_thread_create), _KT(pref##_thread_destroy),                 \
        _KT(pref##_thread_destroyall), _KT(pref##_thread_detach),              \
        _KT(pref##_thread_join), _KT(pref##_thread_cancel),                    \
        _KT(pref##_thread_ctl), _KT(pref##_bad), _KT(pref##_bad),              \
                                                                               \
        _KT(pref##_interrupt_attach_thread),                                   \
        _KT(pref##_interrupt_attach_event), _KT(pref##_interrupt_detach),      \
        _KT(pref##_interrupt_wait), _KT(pref##_interrupt_mask),                \
        _KT(pref##_interrupt_unmask), _KT(pref##_bad), _KT(pref##_bad),        \
        _KT(pref##_bad), _KT(pref##_bad),                                      \
                                                                               \
        _KT(pref##_clock_time), _KT(pref##_bad), /* removed clock slot */      \
        _KT(pref##_clock_period), _KT(pref##_clock_id), _KT(pref##_bad),       \
                                                                               \
        _KT(pref##_timer_create), _KT(pref##_timer_destroy),                   \
        _KT(pref##_timer_settime), _KT(pref##_timer_info),                     \
        _KT(pref##_bad), /* removed timer slot */                              \
        _KT(pref##_timer_timeout), _KT(pref##_bad), _KT(pref##_bad),           \
                                                                               \
        _KT(pref##_sync_create), _KT(pref##_sync_destroy),                     \
        _KT(pref##_sync_mutex_lock), _KT(pref##_sync_mutex_unlock),            \
        _KT(pref##_sync_condvar_wait), _KT(pref##_sync_condvar_signal),        \
        _KT(pref##_sync_sem_post), _KT(pref##_sync_sem_wait),                  \
        _KT(pref##_sync_ctl), _KT(pref##_sync_mutex_revive),                   \
                                                                               \
        _KT(pref##_sched_get), _KT(pref##_sched_set), _KT(pref##_sched_yield), \
        _KT(pref##_sched_info), _KT(pref##_bad),                               \
                                                                               \
        _KT(pref##_bad), _KT(pref##_bad), _KT(pref##_bad), _KT(pref##_bad),    \
        _KT(pref##_bad),                                                       \
                                                                               \
        _KT(pref##_bad), _KT(pref##_bad),                                      \
                                                                               \
        _KT(pref##_bad) /* mt_ctl */

#endif /* KSYS_CALL_TBL_H */
