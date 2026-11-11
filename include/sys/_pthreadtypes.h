/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_PTHREADTYPES_H
#define _SYS_PTHREADTYPES_H

typedef int32_t pthread_t;
typedef int32_t pthread_key_t;

typedef struct _sync {
    long count;
    unsigned long owner;
} sync_t;

typedef struct _sync_attr {
    int protocol;
    unsigned int flags;
    int prioceiling;
    int clockid;
    int reserved[4];
} sync_attr_t;

typedef struct sched_param {
    int32_t sched_priority;
    int32_t sched_curpriority;
    union {
        int32_t _reserved[8];
        struct {
            int32_t ss_low_priority;
            int32_t ss_max_repl;
            struct timespec ss_repl_period;
            struct timespec ss_init_budget;
        };
    };
} sched_param_t;

typedef sync_t pthread_mutex_t;
typedef sync_attr_t pthread_mutexattr_t;
typedef sync_t pthread_cond_t;
typedef sync_attr_t pthread_condattr_t;

/* Spin locks are a single busy-waited atomic word with no kernel sync object,
 * which is what lets one live in shared memory as PTHREAD_PROCESS_SHARED.
 */
typedef struct _pthread_spinlock {
    _Atomic int __lock;
} pthread_spinlock_t;

/* Barriers are built from a process-private mutex + cond and a generation
 * counter, so PTHREAD_PROCESS_SHARED is not offered (the sync objects carry
 * per-process state).
 */
typedef sync_attr_t pthread_barrierattr_t;

typedef struct _pthread_barrier {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned int count;
    unsigned int waiting;
    unsigned long generation; /* 64-bit on LP64: never wraps under a waiter */
} pthread_barrier_t;

/* Reader/writer locks: a private mutex guards the state, one cond wakes waiting
 * readers and another waiting writers. `active` is the reader count, -1 for a
 * held writer, 0 when free. A writer is preferred only when the lock is free; a
 * new reader joins readers that already hold it so a nested read lock cannot
 * deadlock behind a queued writer. PTHREAD_PROCESS_SHARED is not offered.
 */
typedef sync_attr_t pthread_rwlockattr_t;

typedef struct _pthread_rwlock {
    pthread_mutex_t lock;
    pthread_cond_t readers;
    pthread_cond_t writers;
    int active;
    unsigned int waiting_writers;
} pthread_rwlock_t;

/* Structure for pthread_once() */
typedef struct _pthread_once {
    int once;
    sync_t mutex;
} pthread_once_t;

/* Thread creation attributes. */
typedef struct _thread_attr {
    int flags;
    size_t stacksize;
    void *stackaddr;
    void (*exitfunc)(void *status);
    int policy;
    sched_param_t sched_param;
    unsigned guardsize;
    unsigned prealloc;
    int spare[2];
} pthread_attr_t;

#endif
