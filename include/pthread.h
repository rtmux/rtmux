/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PTHREAD_H_INCLUDED
#define _PTHREAD_H_INCLUDED

#include <sys/platform.h>

#ifndef _TIME_H_INCLUDED
#include <time.h>
#endif

#ifndef _SCHED_H_INCLUDED
#include <sched.h>
#endif

#ifndef __STORAGE_H_INCLUDED
#include <sys/storage.h>
#endif

__BEGIN_DECLS

/* Synchronization manifests and structures. On count */
#define RTMUX_SYNC_NONRECURSIVE 0x80000000 /* mutexes */
#define RTMUX_SYNC_NOERRORCHECK 0x40000000 /* mutexes */
#define RTMUX_SYNC_PRIOCEILING 0x20000000  /* mutexes */
#define RTMUX_SYNC_PRIONONE 0x10000000     /* mutexes */
#define RTMUX_SYNC_PRIOINHERIT 0x08000000  /* mutexes */
#define RTMUX_SYNC_COUNTMASK 0x00ffffff    /* mutexes */

/* For PRIO_PROTECT mutexes the low 24 bits (COUNTMASK) are split: the priority
 * ceiling lives in bits 16-23 (values 1-255) and the recursion depth in bits
 * 0-15. Non-ceiling mutexes use the whole COUNTMASK as the recursion depth.
 * Keeping the ceiling out of the depth field lets a recursive lock/unlock
 * adjust the depth (a plain ++/-- of count) without disturbing the ceiling, so
 * the kernel reads the ceiling directly with no held-depth correction.
 */
#define RTMUX_SYNC_CEIL_SHIFT 16
#define RTMUX_SYNC_CEIL_MASK 0x00ff0000   /* mutexes: PRIO_PROTECT ceiling */
#define RTMUX_SYNC_PPDEPTHMASK 0x0000ffff /* mutexes: PRIO_PROTECT depth */
/* On owner */
#define RTMUX_SYNC_MUTEX_FREE \
    0x00000000U /*    0  mutexes, and old cond, sem, spin */
#define RTMUX_SYNC_WAITING 0x80000000U    /* Top bit used with mutexes */
#define RTMUX_SYNC_OWNER_MASK 0x7fffffffU /* Owner used with mutexes */
#define RTMUX_SYNC_OWNER_PID_SHIFT 16     /* owner word: pid in bits 16..30 */
/* Pid bits kept in the owner word. Pids are generation-tagged, so one process
 * slot is ambiguous across eight generations; only a stale word left in shared
 * memory by an owner that died holding the mutex can alias, and the effect is a
 * misplaced PI boost (see sync_lookup_owner_pin). Widening the word changes the
 * sync_t ABI and is deliberately not done.
 */
#define RTMUX_SYNC_OWNER_PID_MASK 0x7fff
#define RTMUX_SYNC_INITIALIZER \
    0xffffffffU /*   -1  count is 0=mutexes, 0(old),-5(new)=cond */
#define RTMUX_SYNC_DESTROYED 0xfffffffe /*   -2  mutexes, cond, sem, spin */
#define RTMUX_SYNC_NAMED_SEM 0xfffffffd /*   -3  sem (count is handle) */
#define RTMUX_SYNC_SEM 0xfffffffc       /*   -4  sem (count is value) */
#define RTMUX_SYNC_COND 0xfffffffb      /*   -5  cond (count is clockid) */
#define RTMUX_SYNC_SPIN 0xfffffffa      /*   -6  spin (count is internal) */
#define RTMUX_SYNC_DEAD \
    0xffffff00 /* -256  mutex (when a process dies with a mutex locked) */

#define PTHREAD_DETACHSTATE_MASK 0x01
#define PTHREAD_CREATE_JOINABLE 0x00
#define PTHREAD_CREATE_DETACHED 0x01

#define PTHREAD_INHERITSCHED_MASK 0x02
#define PTHREAD_INHERIT_SCHED 0x00
#define PTHREAD_EXPLICIT_SCHED 0x02

#define PTHREAD_CONTENTIONSCOPE_MASK 0x04
#define PTHREAD_SCOPE_SYSTEM 0x00
#define PTHREAD_SCOPE_PROCESS 0x04
#define PTHREAD_CSTATE_MASK 0x10
#define PTHREAD_CANCEL_ENABLE 0x00
#define PTHREAD_CANCEL_DISABLE 0x10
#define PTHREAD_CTYPE_MASK 0x20
#define PTHREAD_CANCEL_DEFERRED 0x00
#define PTHREAD_CANCEL_ASYNCHRONOUS 0x20
#define PTHREAD_CANCEL_PENDING 0x40
#define PTHREAD_CANCELED ((void *) -1)

#define PTHREAD_PRIO_INHERIT 0
#define PTHREAD_PRIO_NONE 1
#define PTHREAD_PRIO_PROTECT 2

/* thread attribute prototypes */
extern int pthread_attr_destroy(pthread_attr_t *__attr);
extern int pthread_attr_getdetachstate(const pthread_attr_t *__attr,
                                       int *__detachstate);
extern int pthread_attr_getstack(const pthread_attr_t *__attr,
                                 void **__stackaddr,
                                 size_t *__stacksize);
extern int pthread_attr_getstackaddr(const pthread_attr_t *__addr,
                                     void **__stackaddr);
extern int pthread_attr_getstacksize(const pthread_attr_t *__attr,
                                     size_t *__stacksize);
extern int pthread_attr_init(pthread_attr_t *__attr);
extern int pthread_attr_setdetachstate(pthread_attr_t *__attr,
                                       int __detachstate);
extern int pthread_attr_setstack(pthread_attr_t *__attr,
                                 void *__stackaddr,
                                 size_t __stacksize);
extern int pthread_attr_setstackaddr(pthread_attr_t *__attr, void *__stackaddr);
extern int pthread_attr_setstacksize(pthread_attr_t *__attr,
                                     size_t __stacksize);

extern int pthread_attr_getguardsize(const pthread_attr_t *__attr,
                                     size_t *__guardsize);
extern int pthread_attr_setguardsize(pthread_attr_t *__attr,
                                     size_t __guardsize);
extern int pthread_getconcurrency(void);
extern int pthread_setconcurrency(int __new_level);

/* scheduling related functions */
extern int pthread_attr_getinheritsched(const pthread_attr_t *__attr,
                                        int *__inheritsched);
extern int pthread_attr_getschedparam(const pthread_attr_t *__attr,
                                      struct sched_param *__param);
extern int pthread_attr_getschedpolicy(const pthread_attr_t *__attr,
                                       int *__policy);
extern int pthread_attr_getscope(const pthread_attr_t *__attr,
                                 int *__contentionscope);
extern int pthread_attr_setinheritsched(pthread_attr_t *__attr,
                                        int __inheritsched);
extern int pthread_attr_setschedparam(pthread_attr_t *__attr,
                                      const struct sched_param *__param);
extern int pthread_attr_setschedpolicy(pthread_attr_t *__attr, int __policy);
extern int pthread_attr_setscope(pthread_attr_t *__attr, int __contentionscope);

/* thread creation prototypes */
extern int pthread_cancel(pthread_t __thr);
extern int pthread_create(pthread_t *__thr,
                          const pthread_attr_t *__attr,
                          void *(*__start_routine)(void *),
                          void *__arg);
extern int pthread_detach(pthread_t __thr);
extern int pthread_equal(pthread_t __t1, pthread_t __t2);
extern void pthread_exit(void *__value_ptr) __attribute__((__noreturn__));
extern int pthread_join(pthread_t __thr, void **__value_ptr);
extern pthread_t pthread_self(void);
extern int pthread_setcancelstate(int __state, int *__oldstate);
extern int pthread_setcanceltype(int __type, int *__oldtype);

/* dynamic thread scheduling parameters */
extern int pthread_getschedparam(const pthread_t __thr,
                                 int *__policy,
                                 struct sched_param *__param);
extern int pthread_setschedparam(pthread_t __thr,
                                 int __policy,
                                 const struct sched_param *__param);
extern int pthread_setschedprio(pthread_t __thr, int __prio);

extern void pthread_testcancel(void);

/* thread cancelation handlers */

struct __cleanup_handler;
struct __cleanup_handler {
    struct __cleanup_handler *__next;
    void (*__routine)(void *__arg);
    void *__save;
};

#define pthread_cleanup_push(__func, __arg)                  \
    {                                                        \
        struct __cleanup_handler __cleanup_handler;          \
        __cleanup_handler.__routine = (__func);              \
        __cleanup_handler.__save = (__arg);                  \
        __cleanup_handler.__next =                           \
            (struct __cleanup_handler *) __tls()->__cleanup; \
        __tls()->__cleanup = (void *) &__cleanup_handler;

#define pthread_cleanup_pop(__ex)                                   \
    __tls()->__cleanup = (void *) __cleanup_handler.__next;         \
    ((__ex) ? __cleanup_handler.__routine(__cleanup_handler.__save) \
            : (void) 0);                                            \
    }

/* pthread_key prototypes */
extern void *pthread_getspecific(pthread_key_t __key);
extern int pthread_key_create(pthread_key_t *__key,
                              void (*__destructor)(void *));
extern int pthread_key_delete(pthread_key_t __key);
extern int pthread_setspecific(pthread_key_t __key, const void *__value);


/* pthread synchronization prototypes */
#define PTHREAD_MUTEX_INITIALIZER \
    {RTMUX_SYNC_NONRECURSIVE | RTMUX_SYNC_PRIONONE, RTMUX_SYNC_INITIALIZER}
#define PTHREAD_MUTEX_DEFAULT 0    /* ((int)(RTMUX_SYNC_NONRECURSIVE)) */
#define PTHREAD_MUTEX_ERRORCHECK 1 /* ((int)(RTMUX_SYNC_NONRECURSIVE)) */
#define PTHREAD_MUTEX_RECURSIVE 2  /* (0) */
#define PTHREAD_MUTEX_NORMAL \
    3 /* ((int)(RTMUX_SYNC_NONRECURSIVE|RTMUX_SYNC_NOERRORCHECK)) */
#define PTHREAD_COND_INITIALIZER {(int) RTMUX_SYNC_COND, RTMUX_SYNC_INITIALIZER}

/* Static rwlock: unlocked, no waiters; the mutex/cond lazily create on first
 * use just as their own static initializers do.
 */
#define PTHREAD_RWLOCK_INITIALIZER                        \
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, \
     PTHREAD_COND_INITIALIZER, 0, 0}

#define PTHREAD_PROCESSSHARED_MASK 0x01
#define PTHREAD_PROCESS_PRIVATE 0x00
#define PTHREAD_PROCESS_SHARED 0x01

#define PTHREAD_RECURSIVE_MASK 0x02
#define PTHREAD_RECURSIVE_DISABLE 0x00
#define PTHREAD_RECURSIVE_ENABLE 0x02

#define PTHREAD_ERRORCHECK_MASK 0x04
#define PTHREAD_ERRORCHECK_ENABLE 0x00
#define PTHREAD_ERRORCHECK_DISABLE 0x04

#define RTMUX_ATTR_FLAGS                                 \
    0x0000ffff /* These flags are verified for each type \
                */

#define RTMUX_ATTR_MASK 0x000f0000
#define RTMUX_ATTR_UNKNOWN 0x00000000
#define RTMUX_ATTR_MUTEX 0x00010000
#define RTMUX_ATTR_COND 0x00020000
#define RTMUX_ATTR_RWLOCK 0x00030000
#define RTMUX_ATTR_BARRIER 0x00040000

#define PTHREAD_MUTEX_TYPE 0x40000000 /* non-default mutex type */

/* Used to identify that more than the first three ints of struct _sync_attr are
 * valid.
 */
#define RTMUX_ATTR_EXTRA_FLAG 0x80000000

/* synchronization stuff */
extern int pthread_mutexattr_destroy(pthread_mutexattr_t *__attr);
extern int pthread_mutexattr_init(pthread_mutexattr_t *__attr);

/* Process-shared attributes are always accepted. */
extern int pthread_mutexattr_getpshared(const pthread_mutexattr_t *__attr,
                                        int *__pshared);
extern int pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr,
                                        int __pshared);

/* synchronization scheduling */
extern int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *__attr,
                                            int *__prioceiling);
extern int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *__attr,
                                         int *__protocol);
extern int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *__attr,
                                            int __prioceiling);
extern int pthread_mutexattr_setprotocol(pthread_mutexattr_t *__attr,
                                         int __protocol);

/* mutex recursion */
extern int pthread_mutex_destroy(pthread_mutex_t *__mutex);
extern int pthread_mutex_init(pthread_mutex_t *__mutex,
                              const pthread_mutexattr_t *__attr);
extern int pthread_mutex_lock(pthread_mutex_t *__mutex);
extern int pthread_mutex_timedlock(pthread_mutex_t *__mutex,
                                   const struct timespec *__abs_timeout);
extern int pthread_mutex_trylock(pthread_mutex_t *__mutex);
extern int pthread_mutex_unlock(pthread_mutex_t *__mutex);

/* POSIX spin locks (1003.1j) */
extern int pthread_spin_init(pthread_spinlock_t *__lock, int __pshared);
extern int pthread_spin_destroy(pthread_spinlock_t *__lock);
extern int pthread_spin_lock(pthread_spinlock_t *__lock);
extern int pthread_spin_trylock(pthread_spinlock_t *__lock);
extern int pthread_spin_unlock(pthread_spinlock_t *__lock);

/* POSIX barriers (1003.1j) */
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
extern int pthread_barrier_init(pthread_barrier_t *__barrier,
                                const pthread_barrierattr_t *__attr,
                                unsigned __count);
extern int pthread_barrier_destroy(pthread_barrier_t *__barrier);
extern int pthread_barrier_wait(pthread_barrier_t *__barrier);
extern int pthread_barrierattr_init(pthread_barrierattr_t *__attr);
extern int pthread_barrierattr_destroy(pthread_barrierattr_t *__attr);
extern int pthread_barrierattr_getpshared(const pthread_barrierattr_t *__attr,
                                          int *__pshared);
extern int pthread_barrierattr_setpshared(pthread_barrierattr_t *__attr,
                                          int __pshared);

/* POSIX read-write locks (1003.1j) */
extern int pthread_rwlock_init(pthread_rwlock_t *__rwlock,
                               const pthread_rwlockattr_t *__attr);
extern int pthread_rwlock_destroy(pthread_rwlock_t *__rwlock);
extern int pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock);
extern int pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock);
extern int pthread_rwlock_timedrdlock(pthread_rwlock_t *__rwlock,
                                      const struct timespec *__abstime);
extern int pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock);
extern int pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock);
extern int pthread_rwlock_timedwrlock(pthread_rwlock_t *__rwlock,
                                      const struct timespec *__abstime);
extern int pthread_rwlock_unlock(pthread_rwlock_t *__rwlock);
extern int pthread_rwlockattr_init(pthread_rwlockattr_t *__attr);
extern int pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr);
extern int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *__attr,
                                         int *__pshared);
extern int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr,
                                         int __pshared);

extern int pthread_mutexattr_gettype(const pthread_mutexattr_t *__attr,
                                     int *__type);
extern int pthread_mutexattr_settype(pthread_mutexattr_t *__attr, int __type);

/* dynamically change the priority ceiling of a mutex */
extern int pthread_condattr_destroy(pthread_condattr_t *__attr);
extern int pthread_condattr_init(pthread_condattr_t *__attr);
extern int pthread_mutex_getprioceiling(const pthread_mutex_t *__mutex,
                                        int *__prioceiling);
extern int pthread_mutex_setprioceiling(pthread_mutex_t *__mutex,
                                        int __prioceiling,
                                        int *__old_ceiling);

/* Next two functions are not supported currently */
extern int pthread_cond_broadcast(pthread_cond_t *__cond);
extern int pthread_cond_destroy(pthread_cond_t *__cond);
extern int pthread_cond_init(pthread_cond_t *__cond,
                             const pthread_condattr_t *__attr);
extern int pthread_cond_signal(pthread_cond_t *__cond);
extern int pthread_cond_wait(pthread_cond_t *__cond, pthread_mutex_t *__mutex);
extern int pthread_condattr_getpshared(const pthread_condattr_t *__attr,
                                       int *__pshared);
extern int pthread_condattr_setpshared(pthread_condattr_t *__attr,
                                       int __pshared);
extern int pthread_cond_timedwait(pthread_cond_t *__cond,
                                  pthread_mutex_t *__mutex,
                                  const struct timespec *__abstime);

/* pthread_once prototypes */
#ifdef __PTHREAD_ONCE_T
typedef __PTHREAD_ONCE_T pthread_once_t;
#undef __PTHREAD_ONCE_T
#endif

#define PTHREAD_ONCE_INIT {0, PTHREAD_MUTEX_INITIALIZER}

extern int pthread_once(pthread_once_t *__once_control,
                        void (*__init_routine)(void));

extern int pthread_getcpuclockid(pthread_t __id, clockid_t *__clock_id);

extern int pthread_condattr_getclock(const pthread_condattr_t *__attr,
                                     clockid_t *__id);
extern int pthread_condattr_setclock(pthread_condattr_t *__attr, int __id);

__END_DECLS
#endif
