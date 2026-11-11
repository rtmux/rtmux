/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Kernel resource gauges exported to roottask for /sys/kstats.
 *
 * The SOUL_STATS private call copies one fixed-width record per object pool.
 * Records carry aggregate counts only: no object ids and no pointers, so the
 * call is not an enumerate-global-object path. Pools are identified by this
 * list, not by soul_t.type, because many pools share type 0 and two share
 * LIMITS_CHANNEL. Append new pools at the end; roottask prints the names.
 *
 * Gated by RTMUX_PRIV_CAP_SYSTEM. See docs/kstats.md for the exported keys.
 */
#ifndef _PRIVATE_KSTATS_H
#define _PRIVATE_KSTATS_H

#include <stdint.h>

/* X(name, kernel soul_t variable) */
#define KSTAT_SOUL_LIST(X)                       \
    X(notification, notification_souls)          \
    X(sync, sync_souls)                          \
    X(timer, timer_souls)                        \
    X(thread, thread_souls)                      \
    X(connect, connect_souls)                    \
    X(channel, channel_souls)                    \
    X(process, process_souls)                    \
    X(global_channel, global_channel_souls)      \
    X(interrupt, interrupt_souls)                \
    X(syncevent, syncevent_souls)                \
    X(fpu, fpu_souls)                            \
    X(credential, credential_souls)              \
    X(limits, limits_souls)                      \
    X(vthread, vthread_souls)                    \
    X(debug, debug_souls)                        \
    X(breakpt, breakpt_souls)                    \
    X(ssinfo, ssinfo_souls)                      \
    X(threadname, threadname_souls)

enum kstat_soul_id {
#define KSTAT_SOUL_IDENT(name, var) KSTAT_SOUL_##name,
    KSTAT_SOUL_LIST(KSTAT_SOUL_IDENT)
#undef KSTAT_SOUL_IDENT
    KSTAT_SOUL_COUNT
};

/* Version 1 record. Tagged so later items can add per-CPU or interrupt records
 * without breaking readers.
 */
#define KSTAT_REC_SOUL 1

struct kstat_soul_rec {
    uint32_t kind; /* KSTAT_REC_SOUL */
    uint32_t id;   /* enum kstat_soul_id */
    uint32_t total;     /* allocated, free or in use */
    uint32_t used;      /* in use */
    uint32_t reserved;  /* free souls held back for realtime delivery */
    uint32_t highwater; /* peak in use */
};

struct kcall_args_soul_stats {
    struct kstat_soul_rec *out; /* caller buffer */
    unsigned n;                 /* capacity in records */
    unsigned *count_out;        /* records that exist (set even on ERANGE) */
    unsigned live_threads;      /* out: threads in some process's thread vector */
    unsigned live_timeouts;     /* out: those threads that own a timeout timer */
    uint64_t *timer_intr;       /* caller buffer: STIMER interrupts taken, per hart */
    unsigned timer_n;           /* capacity of timer_intr in entries */
    unsigned timer_count;       /* out: online harts (set even if timer_n is short) */
    int ret;
};

#endif
