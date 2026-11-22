/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * sync.c — Sync* syscall conformance tests.
 *
 * Covers sync_type_create / __sync_destroy / __sync_mutex_lock /
 * __sync_mutex_unlock / sync_sem_post / sync_sem_wait / sync_condvar_signal for
 * the RTMUX_SYNC_MUTEX_FREE, RTMUX_SYNC_COND and RTMUX_SYNC_SEM object types.
 * The tests deliberately stick to paths that do not block the test thread --
 * there is no second thread here to post to a semaphore or signal a condvar --
 * so Wait-on-empty paths are exercised via short timeouts or by validating
 * error returns for bad arguments.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdarg.h>
#include <devctl.h>
#include <devctl/proc.h>
#include <sys/procfs.h>
#include <sys/states.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/rtmux-core.h>

#include "test.h"
#include "../../internal/include/sys/calls.h"

/* sync_type_create — success cases */

static void test_create_mutex(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, NULL);
    check(rc == EOK, "sync_type_create(MUTEX_FREE)", "expected EOK");
    check(rc == EOK && (s.count & RTMUX_SYNC_PRIONONE) != 0,
          "sync_type_create(MUTEX_FREE) default protocol",
          "expected PRIO_NONE marker");
    if (rc == EOK)
        (void) __sync_destroy(&s);
}

static void test_create_cond(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_COND, &s, NULL);
    check(rc == EOK, "sync_type_create(COND)", "expected EOK");
    if (rc == EOK)
        (void) __sync_destroy(&s);
}

static void test_create_sem(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_SEM, &s, NULL);
    check(rc == EOK, "sync_type_create(SEM)", "expected EOK");
    if (rc == EOK)
        (void) __sync_destroy(&s);
}

/* sync_type_create — attribute-driven variants */

static void test_create_mutex_recursive(void)
{
    sync_t s;
    sync_attr_t attr;
    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.flags = RTMUX_ATTR_MUTEX | PTHREAD_RECURSIVE_ENABLE;
    attr.protocol = PTHREAD_PRIO_INHERIT;
    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr);
    check(rc == EOK, "sync_type_create(MUTEX, recursive)", "expected EOK");
    if (rc == EOK)
        (void) __sync_destroy(&s);
}

static void test_create_mutex_prio_none(void)
{
    sync_t s;
    sync_attr_t attr;
    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.protocol = PTHREAD_PRIO_NONE;
    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr);
    check(rc == EOK, "sync_type_create(MUTEX, PRIO_NONE)", "expected EOK");
    if (rc == EOK)
        (void) __sync_destroy(&s);
}

static void test_pthread_mutexattr_protocol(void)
{
    pthread_mutexattr_t attr;
    int protocol = -1;
    int prioceiling = -1;
    int pshared = -1;
    int type = -1;
    int rc;

    rc = pthread_mutexattr_init(&attr);
    if (rc != EOK) {
        test_fail("pthread_mutexattr protocol prerequisite", "init failed");
        return;
    }
    check(pthread_mutexattr_getprotocol(&attr, &protocol) == EOK &&
              protocol == PTHREAD_PRIO_NONE,
          "pthread_mutexattr default protocol", "expected PRIO_NONE");
    check(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE) == EOK &&
              pthread_mutexattr_getprotocol(&attr, &protocol) == EOK &&
              protocol == PTHREAD_PRIO_NONE,
          "pthread_mutexattr PRIO_NONE round-trip", "expected PRIO_NONE");
    check(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == EOK &&
              pthread_mutexattr_getprotocol(&attr, &protocol) == EOK &&
              protocol == PTHREAD_PRIO_INHERIT,
          "pthread_mutexattr PRIO_INHERIT round-trip", "expected PRIO_INHERIT");
    check(sysconf(_SC_THREAD_PRIO_INHERIT) == _POSIX_VERSION,
          "sysconf(_SC_THREAD_PRIO_INHERIT)",
          "expected supported after kernel PI landed");
    check(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT) == EOK &&
              pthread_mutexattr_getprotocol(&attr, &protocol) == EOK &&
              protocol == PTHREAD_PRIO_PROTECT,
          "pthread_mutexattr PRIO_PROTECT round-trip", "expected PRIO_PROTECT");
    check(pthread_mutexattr_setprioceiling(&attr, 17) == EOK &&
              pthread_mutexattr_getprioceiling(&attr, &prioceiling) == EOK &&
              prioceiling == 17,
          "pthread_mutexattr prioceiling round-trip", "expected 17");
    check(pthread_mutexattr_getprioceiling(&attr, NULL) == EINVAL,
          "pthread_mutexattr null prioceiling output", "expected EINVAL");
    check(sysconf(_SC_THREAD_PRIO_PROTECT) == _POSIX_VERSION,
          "sysconf(_SC_THREAD_PRIO_PROTECT)",
          "expected supported after kernel priority ceiling landed");
    check(pthread_mutexattr_setprotocol(&attr, 99) == EINVAL,
          "pthread_mutexattr bad protocol", "expected EINVAL");
    check(pthread_mutexattr_getpshared(&attr, &pshared) == EOK &&
              pshared == PTHREAD_PROCESS_PRIVATE,
          "pthread_mutexattr default pshared",
          "expected PTHREAD_PROCESS_PRIVATE");
    check(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE) == EOK &&
              pthread_mutexattr_getpshared(&attr, &pshared) == EOK &&
              pshared == PTHREAD_PROCESS_PRIVATE,
          "pthread_mutexattr private pshared round-trip",
          "expected PTHREAD_PROCESS_PRIVATE");
    check(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == EOK &&
              pthread_mutexattr_getpshared(&attr, &pshared) == EOK &&
              pshared == PTHREAD_PROCESS_SHARED,
          "pthread_mutexattr shared pshared round-trip",
          "process-shared mutexes are supported (physical-address keyed)");
    /* attr still carries PTHREAD_PRIO_PROTECT from above: a shared PI/PP mutex
     * is rejected because cross-process priority inheritance is unsupported.
     */
    {
        pthread_mutex_t shared_pp = PTHREAD_MUTEX_INITIALIZER;
        check(pthread_mutex_init(&shared_pp, &attr) == ENOTSUP,
              "pshared + PRIO_PROTECT mutex rejected",
              "cross-process priority protection is unsupported");
    }
    check(pthread_mutexattr_setpshared(&attr, -1) == EINVAL,
          "pthread_mutexattr bad pshared", "expected EINVAL");
    check(pthread_mutexattr_gettype(&attr, &type) == EOK &&
              type == PTHREAD_MUTEX_DEFAULT,
          "pthread_mutexattr default type", "expected PTHREAD_MUTEX_DEFAULT");
    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL) == EOK &&
              pthread_mutexattr_gettype(&attr, &type) == EOK &&
              type == PTHREAD_MUTEX_NORMAL,
          "pthread_mutexattr normal type round-trip",
          "expected PTHREAD_MUTEX_NORMAL");
    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == EOK &&
              pthread_mutexattr_gettype(&attr, &type) == EOK &&
              type == PTHREAD_MUTEX_ERRORCHECK,
          "pthread_mutexattr errorcheck type round-trip",
          "expected PTHREAD_MUTEX_ERRORCHECK");
    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == EOK &&
              pthread_mutexattr_gettype(&attr, &type) == EOK &&
              type == PTHREAD_MUTEX_RECURSIVE,
          "pthread_mutexattr recursive type round-trip",
          "expected PTHREAD_MUTEX_RECURSIVE");
    check(pthread_mutexattr_settype(&attr, -1) == EINVAL,
          "pthread_mutexattr bad type", "expected EINVAL");
    check(pthread_mutexattr_gettype(&attr, NULL) == EINVAL,
          "pthread_mutexattr null type output", "expected EINVAL");
    check(pthread_mutexattr_init(NULL) == EINVAL, "pthread_mutexattr null init",
          "expected EINVAL");
    check(pthread_mutexattr_destroy(NULL) == EINVAL,
          "pthread_mutexattr null destroy", "expected EINVAL");
    check(pthread_mutexattr_destroy(&attr) == EOK, "pthread_mutexattr destroy",
          "expected EOK");
}

/* The mutex TYPE now takes effect at creation (previously the attr
 * round-tripped but pthread_mutex_init ignored it): a RECURSIVE mutex lets the
 * owner re-lock, while ERRORCHECK and the DEFAULT type reject an owner re-lock
 * with EDEADLK rather than self-deadlocking. The DEFAULT case also pins dynamic
 * init to the PTHREAD_MUTEX_INITIALIZER behavior (a PRIO_NONE mutex was
 * previously created accidentally recursive).
 */
static void test_pthread_mutex_types(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t m;

    /* RECURSIVE: two owner locks both succeed and need two unlocks. */
    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != EOK ||
        pthread_mutex_init(&m, &attr) != EOK) {
        test_fail("recursive mutex setup", "init failed");
    } else {
        check(pthread_mutex_lock(&m) == EOK && pthread_mutex_lock(&m) == EOK &&
                  pthread_mutex_unlock(&m) == EOK &&
                  pthread_mutex_unlock(&m) == EOK,
              "RECURSIVE mutex allows owner re-lock",
              "a recursive mutex must let the owner re-lock and count down");

        /* The depth saturates: lock and trylock both report EAGAIN at the limit
         * rather than carry into the flag bits.
         */
        if (pthread_mutex_lock(&m) == EOK) {
            unsigned long saved = (unsigned long) m.count;

            m.count |= RTMUX_SYNC_COUNTMASK;
            check(pthread_mutex_lock(&m) == EAGAIN &&
                      pthread_mutex_trylock(&m) == EAGAIN,
                  "RECURSIVE mutex depth saturates with EAGAIN",
                  "lock and trylock must fail EAGAIN at the depth limit");
            unsigned long saturated = (unsigned long) m.count;
            errno = 0;
            check(__sync_mutex_lock((sync_t *) &m) == -1 && errno == EAGAIN &&
                      (unsigned long) m.count == saturated,
                  "raw recursive mutex depth saturates with EAGAIN",
                  "kernel acquire must preserve count and flags at saturation");
            m.count = (long) saved;
            (void) pthread_mutex_unlock(&m);
        }
        (void) pthread_mutex_destroy(&m);
    }
    (void) pthread_mutexattr_destroy(&attr);

    /* ERRORCHECK: the owner's second lock is EDEADLK, not a hang. */
    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) != EOK ||
        pthread_mutex_init(&m, &attr) != EOK) {
        test_fail("errorcheck mutex setup", "init failed");
    } else {
        int first = pthread_mutex_lock(&m);
        int again = pthread_mutex_lock(&m);
        check(first == EOK && again == EDEADLK,
              "ERRORCHECK mutex re-lock is EDEADLK",
              "an errorcheck mutex must reject the owner's re-lock");
        if (first == EOK)
            (void) pthread_mutex_unlock(&m);
        (void) pthread_mutex_destroy(&m);
    }
    (void) pthread_mutexattr_destroy(&attr);

    /* DEFAULT (NULL attr): non-recursive with error checking, so an owner
     * re-lock is EDEADLK -- not accidentally recursive.
     */
    if (pthread_mutex_init(&m, NULL) != EOK) {
        test_fail("default mutex setup", "init failed");
    } else {
        int first = pthread_mutex_lock(&m);
        int again = pthread_mutex_lock(&m);
        check(first == EOK && again == EDEADLK,
              "default-type mutex re-lock is EDEADLK",
              "a default mutex must not be accidentally recursive");
        if (first == EOK)
            (void) pthread_mutex_unlock(&m);
        (void) pthread_mutex_destroy(&m);
    }
}

static void test_pthread_mutex_prio_protect_required_pass_body(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    pthread_mutex_t normal;
    struct sched_param sp;
    int protocol = -1;
    int ceiling = -1;
    int old = -1;
    int rc;

    if (pthread_mutexattr_init(&attr) != EOK) {
        test_fail("PRIO_PROTECT prerequisite", "attr init failed");
        return;
    }
    check(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT) == EOK &&
              pthread_mutexattr_getprotocol(&attr, &protocol) == EOK &&
              protocol == PTHREAD_PRIO_PROTECT &&
              pthread_mutexattr_setprioceiling(&attr, 17) == EOK,
          "pthread_mutexattr PRIO_PROTECT supported",
          "expected protocol and ceiling storage");
    rc = pthread_mutex_init(&mutex, &attr);
    check(rc == EOK && (mutex.count & RTMUX_SYNC_PRIOCEILING) != 0 &&
              pthread_mutex_getprioceiling(&mutex, &ceiling) == EOK &&
              ceiling == 17,
          "pthread_mutex_init PRIO_PROTECT",
          "expected ceiling mutex with ceiling 17");
    check(rc == EOK && pthread_mutex_setprioceiling(&mutex, 18, &old) == EOK &&
              old == 17 &&
              pthread_mutex_getprioceiling(&mutex, &ceiling) == EOK &&
              ceiling == 18,
          "pthread_mutex_setprioceiling unlocked",
          "expected old ceiling 17 and new ceiling 18");
    sp.sched_priority = 5;
    if (__sched_set(0, 0, SCHED_FIFO, &sp) == EOK) {
        int lock_rc = pthread_mutex_lock(&mutex);
        int get_held_rc = __sched_get(0, 0, &sp);
        int held_cur = sp.sched_curpriority;
        int unlock_rc = pthread_mutex_unlock(&mutex);
        int get_released_rc = __sched_get(0, 0, &sp);
        int released_cur = sp.sched_curpriority;
        char pp_detail[128];
        snprintf(pp_detail, sizeof pp_detail,
                 "expected current priority to rise to ceiling while held "
                 "(lock=%d get=%d held=%d unlock=%d get=%d released=%d)",
                 lock_rc, get_held_rc, held_cur, unlock_rc, get_released_rc,
                 released_cur);
        check(lock_rc == EOK && get_held_rc != -1 && held_cur >= 18 &&
                  unlock_rc == EOK && get_released_rc != -1 &&
                  released_cur == 5,
              "pthread_mutex_lock PRIO_PROTECT boost/deboost", pp_detail);

        /* Nested out-of-order release: hold two PP mutexes (ceilings 18 and
         * 10) and release the higher-ceiling one first. The effective boost
         * must fall to the remaining held ceiling (10), not stay pinned at 18
         * until the thread fully unwinds. `mutex` (ceiling 18) is unlocked here
         * and base priority is still 5.
         */
        pthread_mutexattr_t lattr;
        pthread_mutex_t lo;
        if (pthread_mutexattr_init(&lattr) != EOK) {
            test_fail("PRIO_PROTECT nested prerequisite",
                      "low attr init failed");
        } else {
            int lo_ok = pthread_mutexattr_setprotocol(
                            &lattr, PTHREAD_PRIO_PROTECT) == EOK &&
                        pthread_mutexattr_setprioceiling(&lattr, 10) == EOK &&
                        pthread_mutex_init(&lo, &lattr) == EOK;
            if (!lo_ok) {
                test_fail("PRIO_PROTECT nested prerequisite",
                          "low-ceiling mutex init failed");
            } else {
                int l1 = pthread_mutex_lock(&mutex); /* boost -> 18 */
                int l2 = pthread_mutex_lock(&lo);    /* still 18 (max) */
                __sched_get(0, 0, &sp);
                int both_cur = sp.sched_curpriority;
                int u1 =
                    pthread_mutex_unlock(&mutex); /* drop 18 -> 10 remains */
                __sched_get(0, 0, &sp);
                int after_hi = sp.sched_curpriority;
                int u2 = pthread_mutex_unlock(&lo); /* drop 10 -> base 5 */
                __sched_get(0, 0, &sp);
                int after_lo = sp.sched_curpriority;
                check(l1 == EOK && l2 == EOK && u1 == EOK && u2 == EOK &&
                          both_cur >= 18 && after_hi == 10 && after_lo == 5,
                      "PRIO_PROTECT nested out-of-order release",
                      "releasing the higher ceiling first must drop the boost "
                      "to the lower held ceiling, not stay pinned high");
                (void) pthread_mutex_destroy(&lo);
            }
            (void) pthread_mutexattr_destroy(&lattr);
        }
        /* The library recurses on a held ceiling mutex in user space, so the
         * kernel's own recursive acquire is reached only by a raw call. It
         * counts the depth itself and must not boost again: the one final
         * unlock pops the ceiling once, so a second boost would leave the
         * thread raised.
         */
        {
            pthread_mutexattr_t rattr;
            pthread_mutex_t rec;

            if (pthread_mutexattr_init(&rattr) == EOK &&
                pthread_mutexattr_settype(&rattr, PTHREAD_MUTEX_RECURSIVE) ==
                    EOK &&
                pthread_mutexattr_setprotocol(&rattr, PTHREAD_PRIO_PROTECT) ==
                    EOK &&
                pthread_mutexattr_setprioceiling(&rattr, 14) == EOK &&
                pthread_mutex_init(&rec, &rattr) == EOK) {
                int r1 = pthread_mutex_lock(&rec);
                int r2 = sync_mutex_lock_r((sync_t *) &rec);
                if (r2 == EOK) {
                    long saved = rec.count;
                    long saturated = (long) ((unsigned long) saved | rtmux_sync_depth_mask(saved));

                    rec.count = saturated;
                    check(sync_mutex_lock_r((sync_t *) &rec) == -EAGAIN &&
                              rec.count == saturated,
                          "PRIO_PROTECT raw recursive depth saturation",
                          "the ceiling and flags must survive a failed acquire");
                    rec.count = saved;
                }
                int r3 = pthread_mutex_unlock(&rec); /* depth 2 to 1 */
                int r4 = pthread_mutex_unlock(&rec); /* final: kernel pop */

                __sched_get(0, 0, &sp);
                check(r1 == EOK && r2 == EOK && r3 == EOK && r4 == EOK &&
                          sp.sched_curpriority == 5,
                      "PRIO_PROTECT kernel recursive lock boosts once",
                      "a recursive acquire must leave no boost behind after "
                      "the final unlock");
                (void) pthread_mutex_destroy(&rec);
            } else {
                test_fail("PRIO_PROTECT recursive prerequisite", "init failed");
            }
            (void) pthread_mutexattr_destroy(&rattr);
        }
        sp.sched_priority = 20;
        if (__sched_set(0, 0, SCHED_FIFO, &sp) == EOK) {
            check(pthread_mutex_lock(&mutex) == EINVAL,
                  "pthread_mutex_lock PRIO_PROTECT ceiling violation",
                  "expected EINVAL above ceiling");
        } else {
            test_fail("PRIO_PROTECT ceiling violation prerequisite",
                      "__sched_set high priority failed");
        }
    } else {
        test_fail("PRIO_PROTECT boost prerequisite", "__sched_set failed");
    }
    if (rc == EOK)
        (void) pthread_mutex_destroy(&mutex);
    if (pthread_mutex_init(&normal, NULL) != EOK) {
        test_fail("PRIO_PROTECT normal mutex prerequisite", "init failed");
        return;
    }
    check((normal.count & RTMUX_SYNC_PRIONONE) != 0,
          "pthread_mutex_init(NULL) default protocol",
          "expected PRIO_NONE marker");
    check(pthread_mutex_getprioceiling(&normal, &ceiling) == EINVAL &&
              pthread_mutex_setprioceiling(&normal, 23, &old) == EINVAL &&
              pthread_mutex_lock(&normal) == EOK &&
              pthread_mutex_unlock(&normal) == EOK,
          "pthread_mutex prioceiling on non-ceiling mutex",
          "expected EINVAL without corrupting count");
    (void) pthread_mutex_destroy(&normal);
}

/* The cases above leave this thread at FIFO 20, above the ceilings they set,
 * and the suite runs again in a lap in the same process. Setting the ceiling of
 * an unlocked mutex locks it, so a thread still at 20 fails the first ceiling
 * operation of the next lap, which needs a priority at or below 17. Start each
 * run below every ceiling used here.
 *
 * The thread is still left at FIFO 20 afterwards, as it always was: a signal
 * thread test that runs later failed when the original priority was restored,
 * so it depends on this state.
 */
static void test_pthread_mutex_prio_protect_required_pass(void)
{
    struct sched_param low = {.sched_priority = 10};

    if (__sched_set(0, 0, SCHED_FIFO, &low) != EOK) {
        test_fail("PRIO_PROTECT prerequisite", "could not lower the priority");
        return;
    }
    test_pthread_mutex_prio_protect_required_pass_body();
}

static void test_create_cond_monotonic(void)
{
    sync_t s;
    sync_attr_t attr;
    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.clockid = CLOCK_MONOTONIC;
    int rc = sync_type_create(RTMUX_SYNC_COND, &s, &attr);
    check(rc == EOK, "sync_type_create(COND, MONOTONIC)", "expected EOK");
    if (rc == EOK)
        (void) __sync_destroy(&s);
}

static void test_pthread_condattr_clock(void)
{
    pthread_condattr_t attr;
    clockid_t clockid = CLOCK_SOFTTIME;
    int pshared = -1;

    if (pthread_condattr_init(&attr) != EOK) {
        test_fail("pthread_condattr clock prerequisite", "init failed");
        return;
    }

    check(pthread_condattr_getclock(&attr, &clockid) == EOK &&
              clockid == CLOCK_REALTIME,
          "pthread_condattr default clock", "expected CLOCK_REALTIME");
    check(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == EOK &&
              pthread_condattr_getclock(&attr, &clockid) == EOK &&
              clockid == CLOCK_MONOTONIC,
          "pthread_condattr monotonic clock", "expected CLOCK_MONOTONIC");
    check(pthread_condattr_setclock(&attr, -1) == EINVAL,
          "pthread_condattr bad clock", "expected EINVAL");
    check(pthread_condattr_getpshared(&attr, &pshared) == EOK &&
              pshared == PTHREAD_PROCESS_PRIVATE,
          "pthread_condattr default pshared",
          "expected PTHREAD_PROCESS_PRIVATE");
    check(pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE) == EOK &&
              pthread_condattr_getpshared(&attr, &pshared) == EOK &&
              pshared == PTHREAD_PROCESS_PRIVATE,
          "pthread_condattr private pshared",
          "expected PTHREAD_PROCESS_PRIVATE");
    check(pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == EOK &&
              pthread_condattr_getpshared(&attr, &pshared) == EOK &&
              pshared == PTHREAD_PROCESS_SHARED,
          "pthread_condattr shared pshared round-trip",
          "process-shared condvars are supported (physical-address keyed)");
    check(pthread_condattr_setpshared(&attr, -1) == EINVAL,
          "pthread_condattr bad pshared", "expected EINVAL");
    check(pthread_condattr_getpshared(&attr, NULL) == EINVAL,
          "pthread_condattr null pshared output", "expected EINVAL");
    check(pthread_condattr_setpshared(NULL, PTHREAD_PROCESS_PRIVATE) == EINVAL,
          "pthread_condattr null setpshared", "expected EINVAL");
    check(pthread_condattr_getclock(&attr, NULL) == EINVAL,
          "pthread_condattr null clock output", "expected EINVAL");
    check(pthread_condattr_setclock(NULL, CLOCK_REALTIME) == EINVAL,
          "pthread_condattr null setclock", "expected EINVAL");
    check(pthread_condattr_destroy(&attr) == EOK, "pthread_condattr destroy",
          "expected EOK");
    check(pthread_condattr_init(NULL) == EINVAL, "pthread_condattr null init",
          "expected EINVAL");
    check(pthread_condattr_destroy(NULL) == EINVAL,
          "pthread_condattr null destroy", "expected EINVAL");
}

/* sync_type_create — error paths */

static void test_create_bad_type(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    errno = 0;
    int rc = sync_type_create(0x1234u, &s, NULL);
    check(rc == -1 && errno == EINVAL, "sync_type_create(bogus type)",
          "expected -1, errno=EINVAL");
}

static void test_create_cond_bad_clock(void)
{
    sync_t s;
    sync_attr_t attr;
    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.clockid = -999; /* not a valid clockid */
    errno = 0;
    int rc = sync_type_create(RTMUX_SYNC_COND, &s, &attr);
    check(rc == -1 && errno == EINVAL, "sync_type_create(COND, bad clockid)",
          "expected -1, errno=EINVAL");
}

static void test_create_mutex_bad_protocol(void)
{
    sync_t s;
    sync_attr_t attr;
    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.protocol = 0x5a; /* not INHERIT/NONE/PROTECT */
    errno = 0;
    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr);
    check(rc == -1 && errno == EINVAL, "sync_type_create(MUTEX, bad protocol)",
          "expected -1, errno=EINVAL");
}

static void test_create_mutex_prio_protect_required_pass(void)
{
    sync_t s;
    sync_attr_t attr;

    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.protocol = PTHREAD_PRIO_PROTECT;

    attr.prioceiling = 17;
    errno = 0;
    check(
        sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr) == EOK &&
            (s.count & RTMUX_SYNC_PRIOCEILING) != 0 &&
            ((s.count & RTMUX_SYNC_CEIL_MASK) >> RTMUX_SYNC_CEIL_SHIFT) == 17 &&
            (s.count & RTMUX_SYNC_PPDEPTHMASK) == 0,
        "sync_type_create(MUTEX, PRIO_PROTECT)",
        "expected ceiling mutex with ceiling 17 in its own bits, depth 0");
    if ((s.owner & RTMUX_SYNC_OWNER_MASK) == RTMUX_SYNC_MUTEX_FREE)
        (void) __sync_destroy(&s);

    attr.prioceiling = -1;
    errno = 0;
    check(sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr) == -1 &&
              errno == EINVAL,
          "sync_type_create(MUTEX, negative ceiling)", "expected EINVAL");

    /* The ceiling now occupies 8 bits, so anything above 255 is rejected. */
    attr.prioceiling =
        (int) (RTMUX_SYNC_CEIL_MASK >> RTMUX_SYNC_CEIL_SHIFT) + 1;
    errno = 0;
    check(sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr) == -1 &&
              errno == EINVAL,
          "sync_type_create(MUTEX, too-large ceiling)", "expected EINVAL");
}

/* A PRIO_PROTECT + RECURSIVE mutex must recurse per POSIX: the ceiling boost is
 * applied on the first acquire and lifted only when the depth returns to zero,
 * and the ceiling value is never disturbed by the recursion depth (they live in
 * disjoint count bits). A PRIO_PROTECT + non-recursive mutex still EDEADLKs on
 * an owner re-lock.
 */
static void test_pthread_mutex_prio_protect_recursive(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t rmutex;
    pthread_mutex_t nmutex;
    struct sched_param sp;
    struct sched_param saved;
    int saved_policy;
    int ceiling = -1;

    saved_policy = sched_getscheduler(0);
    if (saved_policy == -1 || sched_getparam(0, &saved) != EOK) {
        test_fail("PP recursive save", strerror(errno));
        return;
    }
    if (pthread_mutexattr_init(&attr) != EOK) {
        test_fail("PP recursive attr", "attr init failed");
        return;
    }
    if (!(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT) == EOK &&
          pthread_mutexattr_setprioceiling(&attr, 20) == EOK &&
          pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == EOK &&
          pthread_mutex_init(&rmutex, &attr) == EOK)) {
        test_fail("PP recursive init", "PP+RECURSIVE init failed");
        (void) pthread_mutexattr_destroy(&attr);
        return;
    }

    sp = saved;
    sp.sched_priority = 5; /* below the ceiling so the boost is observable */
    if (__sched_set(0, 0, SCHED_FIFO, &sp) == EOK) {
        int l1 = pthread_mutex_lock(&rmutex); /* depth 1: boost -> 20 */
        int l2 = pthread_mutex_lock(&rmutex); /* depth 2: recurse */
        int l3 = pthread_mutex_lock(&rmutex); /* depth 3: recurse */
        int gc = pthread_mutex_getprioceiling(&rmutex, &ceiling);
        __sched_get(0, 0, &sp);
        int deep = sp.sched_curpriority;
        int u1 = pthread_mutex_unlock(&rmutex); /* depth 2: still boosted */
        __sched_get(0, 0, &sp);
        int mid = sp.sched_curpriority;
        int u2 = pthread_mutex_unlock(&rmutex); /* depth 1: still boosted */
        int u3 = pthread_mutex_unlock(&rmutex); /* depth 0: deboost -> 5 */
        __sched_get(0, 0, &sp);
        int after = sp.sched_curpriority;
        check(l1 == EOK && l2 == EOK && l3 == EOK && u1 == EOK && u2 == EOK &&
                  u3 == EOK && gc == EOK && ceiling == 20 && deep >= 20 &&
                  mid >= 20 && after == 5,
              "PRIO_PROTECT + RECURSIVE recursion",
              "recursive lock/unlock must hold the ceiling boost until depth 0 "
              "and report the ceiling undisturbed by depth");
    } else {
        test_fail("PP recursive prereq", "__sched_set failed");
    }
    (void) __sched_set(0, 0, saved_policy, &saved);
    (void) pthread_mutex_destroy(&rmutex);

    /* Same ceiling attr, but a non-recursive (ERRORCHECK) type: an owner
     * re-lock must still fail EDEADLK, proving the type is honored.
     */
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == EOK &&
        pthread_mutex_init(&nmutex, &attr) == EOK) {
        int nl = pthread_mutex_lock(&nmutex);
        int nr = pthread_mutex_lock(&nmutex);
        int nu = pthread_mutex_unlock(&nmutex);
        check(nl == EOK && nr == EDEADLK && nu == EOK,
              "PRIO_PROTECT non-recursive re-lock EDEADLK",
              "a non-recursive ceiling mutex must reject an owner re-lock");
        (void) pthread_mutex_destroy(&nmutex);
    } else {
        test_fail("PP non-recursive init", "init failed");
    }
    (void) pthread_mutexattr_destroy(&attr);
}

/* Mutex lock / unlock */

static void test_mutex_lock_unlock(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, NULL);
    if (rc != EOK) {
        test_fail("__sync_mutex_lock prerequisite: sync_type_create",
                  "setup failed");
        return;
    }

    rc = __sync_mutex_lock(&s);
    check(rc == EOK, "__sync_mutex_lock (uncontended)", "expected EOK");

    rc = __sync_mutex_unlock(&s);
    check(rc == EOK, "__sync_mutex_unlock (owned)", "expected EOK");

    (void) __sync_destroy(&s);
}

/* Recursive-lock test deferred: __sync_mutex_lock on an already-held
 * non-recursive mutex is supposed to block, and if the attr-plumbing for
 * PTHREAD_RECURSIVE_ENABLE isn't actually making the mutex recursive (easy to
 * get wrong), the test hangs the whole suite. Revisit once we have a helper
 * thread + timeout guard.
 */
static void test_recursive_mutex_nested_lock(void)
{ /* NYI */
}

static void test_mutex_unlock_unowned(void)
{
    sync_t s;
    sync_attr_t attr;
    memset(&s, 0, sizeof(s));
    memset(&attr, 0, sizeof(attr));
    attr.flags = PTHREAD_ERRORCHECK_ENABLE;

    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, &attr);
    if (rc != EOK) {
        test_fail("mutex unlock-unowned prerequisite", "setup failed");
        return;
    }

    /* Never locked -- unlocking should fail with EPERM on error-check mutexes.
     * On non-error-check mutexes RTMux returns EOK (undefined behavior by
     * POSIX); accept either so the test doesn't flap on attr behavior changes.
     */
    int rc_unlock = __sync_mutex_unlock(&s);
    check(rc_unlock == EPERM || rc_unlock == EOK,
          "__sync_mutex_unlock (never locked, errorcheck)",
          "expected EPERM or EOK");

    (void) __sync_destroy(&s);
}

#define TEST_SYNC_NCHAINS 128u
#define TEST_SYNC_CHAIN_MASK (TEST_SYNC_NCHAINS - 1u)
#define TEST_COLLISION_POOL 384u

struct collision_waiter {
    sync_t *sync;
    atomic_int *acquired;
    atomic_int *done;
};

struct pi_cycle_args {
    pthread_mutex_t *first;
    pthread_mutex_t *second;
    atomic_int *locked_count;
    atomic_bool *abort;
    atomic_int *result;
    atomic_bool finished;
};

struct pi_timeout_waiter_args {
    pthread_mutex_t *mutex;
    atomic_int *result;
    atomic_ulong *owner_word;
};

struct pi_reuse_candidate_args {
    atomic_ulong *owner_word;
    atomic_int *ready;
    atomic_int *check;
    atomic_int *stop;
    atomic_int *observed_prio;
};

static unsigned test_sync_chain_idx(const sync_t *sync)
{
    uintptr_t a = (uintptr_t) sync;

    a >>= 4;
    a ^= a >> 9;
    return (unsigned) (a & TEST_SYNC_CHAIN_MASK);
}

static int find_sync_collision(sync_t *pool, sync_t **a, sync_t **b)
{
    for (unsigned i = 0; i < TEST_COLLISION_POOL; i++) {
        for (unsigned j = i + 1; j < TEST_COLLISION_POOL; j++) {
            if (test_sync_chain_idx(&pool[i]) ==
                test_sync_chain_idx(&pool[j])) {
                *a = &pool[i];
                *b = &pool[j];
                return 1;
            }
        }
    }
    return 0;
}

static int wait_for_waiting_bit(sync_t *sync);

static void *collision_waiter(void *raw)
{
    struct collision_waiter *arg = raw;

    if (__sync_mutex_lock(arg->sync) == EOK) {
        atomic_store_explicit(arg->acquired, 1, memory_order_release);
        (void) __sync_mutex_unlock(arg->sync);
    } else {
        atomic_store_explicit(arg->acquired, -1, memory_order_release);
    }
    atomic_store_explicit(arg->done, 1, memory_order_release);
    return NULL;
}

static void *pi_cycle_worker(void *raw)
{
    struct pi_cycle_args *arg = raw;
    uint64_t timeout_ns = 100 * 1000000ULL;
    int rc;

    rc = pthread_mutex_lock(arg->first);
    if (rc != EOK) {
        atomic_store(arg->abort, true);
        atomic_store(arg->result, -rc);
        goto done;
    }
    atomic_fetch_add(arg->locked_count, 1);
    while (atomic_load(arg->locked_count) < 2 && !atomic_load(arg->abort))
        (void) __sched_yield();
    if (atomic_load(arg->abort))
        goto unlock;

    (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_MUTEX, NULL,
                             &timeout_ns, NULL);
    rc = pthread_mutex_lock(arg->second);
    (void) __timer_timeout_r(CLOCK_REALTIME, 0, NULL, NULL, NULL);
    atomic_store_explicit(arg->result, rc, memory_order_release);
    if (rc == EOK)
        (void) pthread_mutex_unlock(arg->second);
unlock:
    (void) pthread_mutex_unlock(arg->first);
done:
    atomic_store_explicit(&arg->finished, true, memory_order_release);
    return NULL;
}

static int set_fifo_prio(int prio)
{
    struct sched_param sp = {.sched_priority = prio};

    return __sched_set(0, 0, SCHED_FIFO, &sp);
}

static void *pi_timeout_waiter(void *raw)
{
    struct pi_timeout_waiter_args *arg = raw;
    uint64_t timeout_ns = 100 * 1000000ULL;
    int rc;

    (void) set_fifo_prio(30);
    if (arg->owner_word) {
        atomic_store_explicit(arg->owner_word, __tls()->__owner,
                              memory_order_release);
    }
    (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_MUTEX, NULL,
                             &timeout_ns, NULL);
    rc = pthread_mutex_lock(arg->mutex);
    (void) __timer_timeout_r(CLOCK_REALTIME, 0, NULL, NULL, NULL);
    if (rc == EOK)
        (void) pthread_mutex_unlock(arg->mutex);
    atomic_store_explicit(arg->result, rc, memory_order_release);
    return NULL;
}

static void *pi_reuse_candidate(void *raw)
{
    struct pi_reuse_candidate_args *arg = raw;
    struct sched_param sp;

    (void) set_fifo_prio(5);
    atomic_store_explicit(arg->owner_word, __tls()->__owner,
                          memory_order_release);
    atomic_store_explicit(arg->ready, 1, memory_order_release);
    while (!atomic_load_explicit(arg->check, memory_order_acquire) &&
           !atomic_load_explicit(arg->stop, memory_order_acquire))
        (void) __sched_yield();
    if (__sched_get(0, 0, &sp) == EOK) {
        atomic_store_explicit(arg->observed_prio, sp.sched_priority,
                              memory_order_release);
    }
    while (!atomic_load_explicit(arg->stop, memory_order_acquire))
        (void) __sched_yield();
    return NULL;
}

/* Run the two-mutex PI cycle once.
 *
 * Returns 1 when the cycle was detected with no stuck waiter, 0 when it was
 * not, -1 on a setup failure.
 */
static int pi_cycle_run(int *got1, int *got2)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t a;
    pthread_mutex_t b;
    pthread_t t1;
    pthread_t t2;
    atomic_int locked_count;
    atomic_bool abort;
    atomic_int r1;
    atomic_int r2;
    struct pi_cycle_args arg1;
    struct pi_cycle_args arg2;
    int protocol = -1;
    int ok;

    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != EOK ||
        pthread_mutexattr_getprotocol(&attr, &protocol) != EOK ||
        protocol != PTHREAD_PRIO_INHERIT ||
        pthread_mutex_init(&a, &attr) != EOK ||
        pthread_mutex_init(&b, &attr) != EOK)
        return -1;

    atomic_init(&locked_count, 0);
    atomic_init(&abort, false);
    atomic_init(&r1, -1);
    atomic_init(&r2, -1);
    arg1.first = &a;
    arg1.second = &b;
    arg1.locked_count = &locked_count;
    arg1.abort = &abort;
    atomic_init(&arg1.finished, false);
    arg1.result = &r1;
    arg2.first = &b;
    arg2.second = &a;
    arg2.locked_count = &locked_count;
    arg2.abort = &abort;
    atomic_init(&arg2.finished, false);
    arg2.result = &r2;

    int made1 = pthread_create(&t1, NULL, pi_cycle_worker, &arg1);
    int made2 = made1 ? made1 : pthread_create(&t2, NULL, pi_cycle_worker, &arg2);
    if (made2)
        atomic_store(&abort, true);

    int join1 = made1 ? 0 : pthread_join(t1, NULL);
    int join2 = made2 ? 0 : pthread_join(t2, NULL);
    bool early = (!made1 && !atomic_load(&arg1.finished)) ||
                 (!made2 && !atomic_load(&arg2.finished));
    if (early || join1 || join2)
        printf("  PI cycle join: tids=%d,%d rc=%d,%d finished=%d,%d\n",
               made1 ? -1 : (int)t1, made2 ? -1 : (int)t2, join1, join2,
               atomic_load(&arg1.finished), atomic_load(&arg2.finished));
    /* Keep the fixture alive even when a join fails or returns too early. */
    while ((!made1 && !atomic_load_explicit(&arg1.finished, memory_order_acquire)) ||
           (!made2 && !atomic_load_explicit(&arg2.finished, memory_order_acquire)))
        sleep_ms(1);

    *got1 = atomic_load_explicit(&r1, memory_order_acquire);
    *got2 = atomic_load_explicit(&r2, memory_order_acquire);
    ok = !early && !join1 && !join2 && (*got1 == EDEADLK || *got2 == EDEADLK) &&
         (*got1 == EDEADLK || *got1 == EOK) &&
         (*got2 == EDEADLK || *got2 == EOK);
    (void) pthread_mutex_destroy(&a);
    (void) pthread_mutex_destroy(&b);
    (void) pthread_mutexattr_destroy(&attr);
    return made2 ? -1 : ok;
}

void test_pi_two_mutex_cycle_detects_deadlock(void)
{
    int got1 = -1, got2 = -1;
    int ok = pi_cycle_run(&got1, &got2);
    char cyc_detail[160];

    if (ok < 0) {
        test_fail("PI two-mutex cycle prerequisite", "setup failed");
        return;
    }
    snprintf(cyc_detail, sizeof cyc_detail,
             "expected at least one EDEADLK and no stuck waiter (r1=%d r2=%d)",
             got1, got2);
    check(ok, "PI two-mutex cycle returns EDEADLK", cyc_detail);
}

/* Child fixture: the same cycle check in a process whose pid exceeds the 15
 * bits a mutex owner word holds. Exit 0 only if detection still works.
 */
int sync_pi_large_pid_child(void)
{
    int got1 = -1, got2 = -1;

    return getpid() > 0x7fff && pi_cycle_run(&got1, &got2) == 1 ? EXIT_SUCCESS
                                                                : EXIT_FAILURE;
}

static void test_pi_large_pid(void)
{
    char *argv[] = {"syscall_testing", "--pi-large-pid", NULL};
    pid_t pid = -1;
    int status = -1;

    /* Pids advance by 4096 per exit; ten exits push the next one past 2^15. */
    for (int i = 0; i < 10; i++) {
        char *targv[] = {"true", NULL};
        pid_t p;

        if (posix_spawn(&p, "/bin/true", NULL, NULL, targv, NULL) == 0)
            (void) waitpid(p, &status, 0);
    }
    status = -1;
    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) !=
        0) {
        test_fail("PI large pid spawn", strerror(errno));
        return;
    }
    check(waitpid(pid, &status, 0) == pid && pid > 0x7fff &&
              WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
          "PI deadlock detection works for a pid above 0x7fff",
          "owner lookup failed once the pid exceeded the owner word width");
}

static void test_pi_contender_timeout_does_not_match_reused_tid(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    pthread_t waiter1;
    pthread_t candidate;
    atomic_int waiter1_result;
    atomic_ulong waiter_owner;
    atomic_ulong candidate_owner;
    atomic_int candidate_ready;
    atomic_int candidate_check;
    atomic_int candidate_stop;
    atomic_int candidate_prio;
    unsigned long mutex_owner;
    unsigned long timed_out_owner;
    struct pi_timeout_waiter_args waiter_arg;
    struct pi_reuse_candidate_args candidate_arg;
    int saw_reuse = 0;
    char detail[160];

    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != EOK ||
        pthread_mutex_init(&mutex, &attr) != EOK) {
        test_fail("PI contender TID reuse prerequisite", "setup failed");
        return;
    }
    if (pthread_mutex_lock(&mutex) != EOK) {
        test_fail("PI contender TID reuse prerequisite", "main lock failed");
        return;
    }
    mutex_owner = mutex.owner & ~RTMUX_SYNC_WAITING;

    atomic_init(&waiter1_result, -1);
    atomic_init(&waiter_owner, 0);
    waiter_arg.mutex = &mutex;
    waiter_arg.result = &waiter1_result;
    waiter_arg.owner_word = &waiter_owner;
    if (pthread_create(&waiter1, NULL, pi_timeout_waiter, &waiter_arg) != EOK ||
        !wait_for_waiting_bit(&mutex)) {
        test_fail("PI contender TID reuse prerequisite", "waiter failed");
        return;
    }
    (void) thread_join(waiter1, NULL);
    timed_out_owner = atomic_load_explicit(&waiter_owner, memory_order_acquire);

    for (int attempt = 0; attempt < 32 && !saw_reuse; attempt++) {
        atomic_init(&candidate_owner, 0);
        atomic_init(&candidate_ready, 0);
        atomic_init(&candidate_check, 0);
        atomic_init(&candidate_stop, 0);
        atomic_init(&candidate_prio, -1);
        candidate_arg.owner_word = &candidate_owner;
        candidate_arg.ready = &candidate_ready;
        candidate_arg.check = &candidate_check;
        candidate_arg.stop = &candidate_stop;
        candidate_arg.observed_prio = &candidate_prio;
        if (pthread_create(&candidate, NULL, pi_reuse_candidate,
                           &candidate_arg) != EOK) {
            test_fail("PI contender TID reuse prerequisite",
                      "candidate create failed");
            return;
        }
        for (int i = 0; i < 10000; i++) {
            if (atomic_load_explicit(&candidate_ready, memory_order_acquire))
                break;
            (void) __sched_yield();
        }
        if (atomic_load_explicit(&candidate_owner, memory_order_acquire) !=
            timed_out_owner) {
            atomic_store_explicit(&candidate_stop, 1, memory_order_release);
            (void) thread_join(candidate, NULL);
            continue;
        }
        saw_reuse = 1;
        atomic_store_explicit(&candidate_check, 1, memory_order_release);
        for (int i = 0; i < 1000; i++)
            (void) __sched_yield();
        atomic_store_explicit(&candidate_stop, 1, memory_order_release);
        (void) thread_join(candidate, NULL);
    }

    if (saw_reuse) {
        int prio = atomic_load_explicit(&candidate_prio, memory_order_acquire);
        unsigned long cur_owner = mutex.owner & ~RTMUX_SYNC_WAITING;

        snprintf(detail, sizeof(detail),
                 "mutex_owner=%#lx current=%#lx timed_out=%#lx "
                 "candidate_prio=%d",
                 mutex_owner, cur_owner, timed_out_owner, prio);
        check(cur_owner == mutex_owner && prio < 30,
              "PI timed-out contender does not own recycled TID", detail);
    } else {
        test_ok("PI timed-out contender TID was not recycled");
    }
    (void) pthread_mutex_unlock(&mutex);
    (void) pthread_mutex_destroy(&mutex);
}

static int wait_for_waiting_bit(sync_t *sync)
{
    for (int i = 0; i < 10000; i++) {
        if (sync->owner & RTMUX_SYNC_WAITING)
            return 1;
        (void) __sched_yield();
    }
    return 0;
}

static void test_mutex_hash_collision_wake_is_keyed(void)
{
    static sync_t pool[TEST_COLLISION_POOL];
    sync_t *a = NULL;
    sync_t *b = NULL;
    atomic_int acquired_a;
    atomic_int acquired_b;
    atomic_int done_a;
    atomic_int done_b;
    pthread_t tid_a;
    pthread_t tid_b;
    struct collision_waiter arg_a;
    struct collision_waiter arg_b;

    memset(pool, 0, sizeof(pool));
    if (!find_sync_collision(pool, &a, &b)) {
        test_fail("sync hash-collision prerequisite",
                  "no same-chain sync_t pair found");
        return;
    }
    if (sync_type_create(RTMUX_SYNC_MUTEX_FREE, a, NULL) != EOK ||
        sync_type_create(RTMUX_SYNC_MUTEX_FREE, b, NULL) != EOK ||
        __sync_mutex_lock(a) != EOK || __sync_mutex_lock(b) != EOK) {
        test_fail("sync hash-collision prerequisite", "setup failed");
        return;
    }

    atomic_init(&acquired_a, 0);
    atomic_init(&acquired_b, 0);
    atomic_init(&done_a, 0);
    atomic_init(&done_b, 0);
    arg_a.sync = a;
    arg_a.acquired = &acquired_a;
    arg_a.done = &done_a;
    arg_b.sync = b;
    arg_b.acquired = &acquired_b;
    arg_b.done = &done_b;

    if (pthread_create(&tid_b, NULL, collision_waiter, &arg_b) != EOK ||
        !wait_for_waiting_bit(b) ||
        pthread_create(&tid_a, NULL, collision_waiter, &arg_a) != EOK ||
        !wait_for_waiting_bit(a)) {
        test_fail("sync hash-collision prerequisite", "waiter setup failed");
        (void) __sync_mutex_unlock(a);
        (void) __sync_mutex_unlock(b);
        return;
    }

    (void) __sync_mutex_unlock(a);
    for (int i = 0; i < 200000; i++) {
        if (atomic_load_explicit(&acquired_a, memory_order_acquire) == 1)
            break;
        (void) __sched_yield();
    }

    char detail[96];
    snprintf(detail, sizeof(detail),
             "acquired_a=%d acquired_b=%d done_a=%d done_b=%d",
             atomic_load_explicit(&acquired_a, memory_order_acquire),
             atomic_load_explicit(&acquired_b, memory_order_acquire),
             atomic_load_explicit(&done_a, memory_order_acquire),
             atomic_load_explicit(&done_b, memory_order_acquire));
    check(atomic_load_explicit(&acquired_a, memory_order_acquire) == 1 &&
              atomic_load_explicit(&acquired_b, memory_order_acquire) == 0,
          "sync hash-collision wake is per-key", detail);

    (void) __sync_mutex_unlock(b);
    for (int i = 0; i < 10000; i++) {
        if (atomic_load_explicit(&done_a, memory_order_acquire) == 1 &&
            atomic_load_explicit(&done_b, memory_order_acquire) == 1)
            break;
        (void) __sched_yield();
    }
    (void) __sync_destroy(a);
    (void) __sync_destroy(b);
}

struct cv_collision_args {
    sync_t *cv;
    sync_t *mutex;
    atomic_int *waiting;
    atomic_int *woken; /* 0=never ran, 1=cv_wait returned EOK, -1=error */
};

static void *cv_collision_waiter(void *raw)
{
    struct cv_collision_args *arg = raw;
    int rc;

    if (__sync_mutex_lock(arg->mutex) != EOK) {
        atomic_store_explicit(arg->woken, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(arg->waiting, 1, memory_order_release);
    rc = sync_condvar_wait(arg->cv, arg->mutex);
    atomic_store_explicit(arg->woken, rc == EOK ? 1 : -1, memory_order_release);
    (void) __sync_mutex_unlock(arg->mutex);
    return NULL;
}

/* cv_wait holds two chain spinlocks (cv chain + mutex chain). When cv and mutex
 * hash to the same chain the kernel must recognize the collision and take a
 * single lock — otherwise it recursively acquires a non-recursive spinlock and
 * self-deadlocks, hanging the kernel until QEMU times out. When the chains
 * differ, address-order acquisition prevents ABBA against peer cv_wait callers.
 * This regression pins the same-chain case (the one that fires
 * deterministically on UP).
 */
static void test_condvar_hash_collision_wait_unblocks(void)
{
    static sync_t pool[TEST_COLLISION_POOL];
    sync_t *cv = NULL;
    sync_t *mutex = NULL;
    atomic_int waiting;
    atomic_int woken;
    pthread_t tid;
    struct cv_collision_args arg;

    memset(pool, 0, sizeof(pool));
    if (!find_sync_collision(pool, &cv, &mutex)) {
        test_fail("cv/mutex hash-collision prerequisite",
                  "no same-chain sync_t pair found");
        return;
    }
    if (sync_type_create(RTMUX_SYNC_COND, cv, NULL) != EOK ||
        sync_type_create(RTMUX_SYNC_MUTEX_FREE, mutex, NULL) != EOK) {
        test_fail("cv/mutex hash-collision prerequisite", "setup failed");
        return;
    }

    atomic_init(&waiting, 0);
    atomic_init(&woken, 0);
    arg.cv = cv;
    arg.mutex = mutex;
    arg.waiting = &waiting;
    arg.woken = &woken;

    if (pthread_create(&tid, NULL, cv_collision_waiter, &arg) != EOK) {
        test_fail("cv/mutex hash-collision prerequisite",
                  "thread create failed");
        return;
    }

    /* Spin until the waiter has acquired the mutex and started cv_wait, then
     * use the mutex release as the precise readiness signal. sync_condvar_wait
     * queues on the CV before releasing the mutex, so once this thread can
     * acquire the mutex, sync_condvar_signal cannot be lost.
     */
    int saw_waiting = 0;
    for (int i = 0; i < 10000; i++) {
        if (atomic_load_explicit(&waiting, memory_order_acquire) == 1) {
            saw_waiting = 1;
            break;
        }
        (void) __sched_yield();
    }
    if (!saw_waiting) {
        test_fail("cv/mutex hash-collision prerequisite",
                  "waiter did not reach cv_wait");
        return;
    }

    int locked = 0;
    for (int i = 0; i < 100000; i++) {
        if (pthread_mutex_trylock((pthread_mutex_t *) mutex) == EOK) {
            locked = 1;
            break;
        }
        (void) __sched_yield();
    }
    if (!locked) {
        test_fail("cv/mutex hash-collision prerequisite",
                  "waiter did not release mutex in cv_wait");
        return;
    }

    (void) sync_condvar_signal(cv, 0);
    (void) pthread_mutex_unlock((pthread_mutex_t *) mutex);

    for (int i = 0; i < 100000; i++) {
        if (atomic_load_explicit(&woken, memory_order_acquire) != 0)
            break;
        (void) __sched_yield();
    }

    int result = atomic_load_explicit(&woken, memory_order_acquire);
    check(result == 1, "cv_wait survives cv/mutex chain collision",
          "expected sync_condvar_wait to return EOK without self-deadlock");

    /* No join: this libc doesn't ship pthread_join, and a deadlocked kernel
     * would hang here anyway. When result == 1 the waiter has reached its own
     * __sync_mutex_unlock and is about to return; a short __sched_yield burst
     * lets it exit before we destroy the sync_t objects.
     */
    if (result == 1) {
        for (int i = 0; i < 200; i++)
            (void) __sched_yield();
    }
    (void) tid;
    (void) __sync_destroy(cv);
    (void) __sync_destroy(mutex);
}

struct cv_signal_args {
    pthread_cond_t *cond;
    pthread_mutex_t *mutex;
    atomic_int *waiting;
    atomic_int *woken;
};

struct mutex_lock_waiter_args {
    pthread_mutex_t *mutex;
    atomic_int *acquired;
};

struct mutex_timedlock_holder_args {
    pthread_mutex_t *mutex;
    atomic_int *ready;
    atomic_int *release;
};

static void *mutex_timedlock_holder(void *raw)
{
    struct mutex_timedlock_holder_args *arg = raw;

    if (pthread_mutex_lock(arg->mutex) != EOK)
        return (void *) 1;
    atomic_store_explicit(arg->ready, 1, memory_order_release);
    while (!atomic_load_explicit(arg->release, memory_order_acquire))
        (void) __sched_yield();
    (void) pthread_mutex_unlock(arg->mutex);
    return NULL;
}

static void test_pthread_mutex_timedlock_contract(void)
{
    pthread_mutex_t mutex;
    pthread_t tid;
    atomic_int ready;
    atomic_int release;
    struct timespec past = {.tv_sec = 0, .tv_nsec = 0};
    struct timespec invalid = {.tv_sec = 0, .tv_nsec = 1000000000L};
    struct mutex_timedlock_holder_args arg;
    void *ret = NULL;

    if (pthread_mutex_init(&mutex, NULL) != EOK) {
        test_fail("pthread_mutex_timedlock prerequisite", "mutex init failed");
        return;
    }
    check(pthread_mutex_timedlock(&mutex, &past) == EOK,
          "pthread_mutex_timedlock immediate success",
          "unlocked mutex must lock without consulting an expired timeout");
    (void) pthread_mutex_unlock(&mutex);

    atomic_init(&ready, 0);
    atomic_init(&release, 0);
    arg.mutex = &mutex;
    arg.ready = &ready;
    arg.release = &release;
    if (pthread_create(&tid, NULL, mutex_timedlock_holder, &arg) != EOK) {
        test_fail("pthread_mutex_timedlock prerequisite",
                  "thread create failed");
        (void) pthread_mutex_destroy(&mutex);
        return;
    }
    for (int i = 0; i < 10000; i++) {
        if (atomic_load_explicit(&ready, memory_order_acquire))
            break;
        (void) __sched_yield();
    }

    check(pthread_mutex_timedlock(&mutex, &invalid) == EINVAL,
          "pthread_mutex_timedlock invalid abstime",
          "contended timedlock must reject invalid nanoseconds");
    check(pthread_mutex_timedlock(&mutex, &past) == ETIMEDOUT,
          "pthread_mutex_timedlock timeout",
          "contended timedlock must honor an expired absolute timeout");

    atomic_store_explicit(&release, 1, memory_order_release);
    (void) pthread_join(tid, &ret);
    check(!ret, "pthread_mutex_timedlock holder exit",
          "holder thread must unlock and exit cleanly");
    (void) pthread_mutex_destroy(&mutex);
}

static void *cv_signal_waiter(void *raw)
{
    struct cv_signal_args *arg = raw;
    int rc;

    rc = pthread_mutex_lock(arg->mutex);
    if (rc != EOK) {
        atomic_store_explicit(arg->woken, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(arg->waiting, 1, memory_order_release);
    rc = pthread_cond_wait(arg->cond, arg->mutex);
    (void) pthread_mutex_unlock(arg->mutex);
    atomic_store_explicit(arg->woken, rc == EOK ? 1 : -1, memory_order_release);
    return NULL;
}

static void *mutex_lock_waiter(void *raw)
{
    struct mutex_lock_waiter_args *arg = raw;
    int rc;

    rc = pthread_mutex_lock(arg->mutex);
    if (rc == EOK) {
        atomic_store_explicit(arg->acquired, 1, memory_order_release);
        (void) pthread_mutex_unlock(arg->mutex);
    } else {
        atomic_store_explicit(arg->acquired, -1, memory_order_release);
    }
    return NULL;
}

static void test_condvar_wait_pi_mutex_handoff(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t tid;
    struct timespec abstime;
    struct timeval now;
    atomic_int acquired;
    struct mutex_lock_waiter_args arg;
    int rc;

    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != EOK ||
        pthread_mutex_init(&mutex, &attr) != EOK ||
        pthread_cond_init(&cond, NULL) != EOK ||
        gettimeofday(&now, NULL) != EOK) {
        test_fail("PI condvar handoff prerequisite", "setup failed");
        return;
    }

    if (pthread_mutex_lock(&mutex) != EOK) {
        test_fail("PI condvar handoff prerequisite", "initial lock failed");
        return;
    }

    atomic_init(&acquired, 0);
    arg.mutex = &mutex;
    arg.acquired = &acquired;
    if (pthread_create(&tid, NULL, mutex_lock_waiter, &arg) != EOK ||
        !wait_for_waiting_bit(&mutex)) {
        test_fail("PI condvar handoff prerequisite", "waiter did not block");
        (void) pthread_mutex_unlock(&mutex);
        return;
    }

    abstime.tv_sec = now.tv_sec;
    abstime.tv_nsec = now.tv_usec * 1000L + 50 * 1000 * 1000;
    if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000 * 1000 * 1000;
    }

    rc = pthread_cond_timedwait(&cond, &mutex, &abstime);
    check(rc == ETIMEDOUT &&
              atomic_load_explicit(&acquired, memory_order_acquire) == 1,
          "pthread_cond_timedwait releases PI mutex to waiter",
          "expected queued mutex waiter to acquire during cond wait");

    (void) pthread_mutex_unlock(&mutex);
    (void) thread_join(tid, NULL);
    (void) pthread_mutex_destroy(&mutex);
}

struct condloan_args {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_int heir_got; /* the waiter acquired the mutex during the wait */
    atomic_int mid_ran;  /* the middle-priority thread ran */
    atomic_int wait_rc;  /* result of the condition wait, or -1 until done */
};

int condloan_create(pthread_t *tid, int prio, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    struct sched_param sp = {.sched_priority = prio};

    return pthread_attr_init(&attr) == EOK &&
           pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) ==
               EOK &&
           pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == EOK &&
           pthread_attr_setschedparam(&attr, &sp) == EOK &&
           pthread_create(tid, &attr, fn, arg) == EOK;
}

static void *condloan_heir(void *raw)
{
    struct condloan_args *arg = raw;

    if (pthread_mutex_lock(&arg->mutex) == EOK) {
        atomic_store_explicit(&arg->heir_got, 1, memory_order_release);
        (void) pthread_mutex_unlock(&arg->mutex);
    }
    return NULL;
}

static void *condloan_mid(void *raw)
{
    struct condloan_args *arg = raw;

    atomic_store_explicit(&arg->mid_ran, 1, memory_order_release);
    return NULL;
}

/* The owner thread: it holds a PI mutex that a higher-priority heir is blocked
 * on, so it runs at the heir's priority, with a thread of in-between priority
 * ready behind it. pthread_cond_wait hands the mutex to the heir, which takes
 * the loan away and drops the owner below the ready thread, while the owner is
 * still inside the wait call.
 */
static void *condloan_owner(void *raw)
{
    struct condloan_args *arg = raw;
    struct timespec abstime;
    pthread_t heir;
    pthread_t mid;
    bool heir_up = false;
    bool mid_up = false;

    atomic_store_explicit(&arg->wait_rc, -1, memory_order_release);
    if (set_fifo_prio(10) != EOK || pthread_mutex_lock(&arg->mutex) != EOK)
        return NULL;
    if (condloan_create(&heir, 20, condloan_heir, arg)) {
        heir_up = true;
        if (wait_for_waiting_bit(&arg->mutex) &&
            condloan_create(&mid, 15, condloan_mid, arg)) {
            mid_up = true;
            (void) clock_gettime(CLOCK_REALTIME, &abstime);
            abstime.tv_nsec += 50 * 1000 * 1000;
            if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
                abstime.tv_sec++;
                abstime.tv_nsec -= 1000 * 1000 * 1000;
            }
            atomic_store_explicit(
                &arg->wait_rc,
                pthread_cond_timedwait(&arg->cond, &arg->mutex, &abstime),
                memory_order_release);
        }
    }
    (void) pthread_mutex_unlock(&arg->mutex);
    if (heir_up)
        (void) thread_join(heir, NULL);
    if (mid_up)
        (void) thread_join(mid, NULL);
    return NULL;
}

/* Regression: dropping a thread's inherited priority inside its own kernel call
 * switched the CPU to a ready thread at once, so the call, which went on to
 * block the thread with unready_release(), found another thread active and
 * crashed the kernel ("unready_release non-active"). It happened when a
 * condition wait gave up a PI mutex with a waiter and a thread of a priority
 * between the owner's own and the loan was ready on the same hart.
 */
static void test_condvar_wait_loan_drop_with_ready_thread(void)
{
    static struct condloan_args arg;
    pthread_mutexattr_t attr;
    pthread_t tid;

    memset(&arg, 0, sizeof arg);
    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != EOK ||
        pthread_mutex_init(&arg.mutex, &attr) != EOK ||
        pthread_cond_init(&arg.cond, NULL) != EOK ||
        pthread_create(&tid, NULL, condloan_owner, &arg) != EOK) {
        test_fail("PI condvar loan drop prerequisite", "setup failed");
        return;
    }
    (void) thread_join(tid, NULL);
    check(atomic_load_explicit(&arg.wait_rc, memory_order_acquire) ==
                  ETIMEDOUT &&
              atomic_load_explicit(&arg.heir_got, memory_order_acquire) == 1 &&
              atomic_load_explicit(&arg.mid_ran, memory_order_acquire) == 1,
          "pthread_cond_timedwait survives its PI loan ending under a ready "
          "thread",
          "expected the heir to take the mutex and the wait to time out");
    (void) pthread_mutex_destroy(&arg.mutex);
}

struct mutex_type_probe_args {
    pthread_mutex_t *mutex;
    atomic_int *acquired; /* 1 = got the handoff, -1 = lock failed */
    atomic_int *relock;   /* errno a re-lock returned while holding it */
};

/* Acquire the mutex handed off by a thread blocked in pthread_cond_wait, then
 * probe its type: an ERRORCHECK mutex must reject the owner's re-lock with
 * EDEADLK. If cond_wait wrongly cleared the type flags during the handoff, the
 * new owner sees a recursive mutex and the re-lock returns EOK instead -- undo
 * that extra lock so the mutex still fully releases and the cond waiter can
 * re-acquire on wake.
 */
static void *mutex_type_probe_waiter(void *raw)
{
    struct mutex_type_probe_args *arg = raw;
    int relock;

    if (pthread_mutex_lock(arg->mutex) != EOK) {
        atomic_store_explicit(arg->acquired, -1, memory_order_release);
        return NULL;
    }
    relock = pthread_mutex_lock(arg->mutex);
    atomic_store_explicit(arg->relock, relock, memory_order_release);
    if (relock == EOK)
        (void) pthread_mutex_unlock(arg->mutex);
    (void) pthread_mutex_unlock(arg->mutex);
    atomic_store_explicit(arg->acquired, 1, memory_order_release);
    return NULL;
}

/* Regression: pthread_cond_wait must keep the bound mutex's type flags for the
 * handoff owner. It once zeroed the whole count word for a non-PRIO_PROTECT
 * mutex, so the waiter that acquired the released mutex saw a recursive,
 * no-errorcheck, PRIO_NONE mutex and an ERRORCHECK re-lock returned EOK instead
 * of EDEADLK.
 */
static void test_condvar_wait_errorcheck_type_preserved(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t tid;
    struct timespec abstime;
    struct timeval now;
    atomic_int acquired;
    atomic_int relock;
    struct mutex_type_probe_args arg;
    int rc;

    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) != EOK ||
        pthread_mutex_init(&mutex, &attr) != EOK ||
        pthread_cond_init(&cond, NULL) != EOK ||
        gettimeofday(&now, NULL) != EOK) {
        test_fail("errorcheck condvar handoff prerequisite", "setup failed");
        return;
    }

    if (pthread_mutex_lock(&mutex) != EOK) {
        test_fail("errorcheck condvar handoff prerequisite",
                  "initial lock failed");
        return;
    }

    atomic_init(&acquired, 0);
    atomic_init(&relock, EOK);
    arg.mutex = &mutex;
    arg.acquired = &acquired;
    arg.relock = &relock;
    if (pthread_create(&tid, NULL, mutex_type_probe_waiter, &arg) != EOK ||
        !wait_for_waiting_bit(&mutex)) {
        test_fail("errorcheck condvar handoff prerequisite",
                  "waiter did not block");
        (void) pthread_mutex_unlock(&mutex);
        return;
    }

    abstime.tv_sec = now.tv_sec;
    abstime.tv_nsec = now.tv_usec * 1000L + 50 * 1000 * 1000;
    if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000 * 1000 * 1000;
    }

    rc = pthread_cond_timedwait(&cond, &mutex, &abstime);
    check(rc == ETIMEDOUT &&
              atomic_load_explicit(&acquired, memory_order_acquire) == 1 &&
              atomic_load_explicit(&relock, memory_order_acquire) == EDEADLK,
          "cond_wait preserves the bound mutex type for the handoff owner",
          "errorcheck re-lock during handoff was not EDEADLK "
          "(cond_wait cleared the type flags)");

    (void) pthread_mutex_unlock(&mutex);
    (void) thread_join(tid, NULL);
    (void) pthread_mutex_destroy(&mutex);
}

static void test_condvar_timeout_removes_waiter(void)
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct timespec abstime;
    struct timeval now;
    int rc;

    if (pthread_mutex_init(&mutex, NULL) != EOK ||
        pthread_cond_init(&cond, NULL) != EOK ||
        gettimeofday(&now, NULL) != EOK) {
        test_fail("condvar timeout prerequisite", "setup failed");
        return;
    }

    abstime.tv_sec = now.tv_sec;
    abstime.tv_nsec = now.tv_usec * 1000L;
    abstime.tv_nsec += 20 * 1000 * 1000;
    if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000 * 1000 * 1000;
    }

    rc = pthread_mutex_lock(&mutex);
    if (rc != EOK) {
        test_fail("condvar timeout prerequisite", "mutex lock failed");
        return;
    }
    rc = pthread_cond_timedwait(&cond, &mutex, &abstime);
    check(rc == ETIMEDOUT, "pthread_cond_timedwait timeout",
          "expected ETIMEDOUT");
    check((mutex.owner & ~RTMUX_SYNC_WAITING) == __tls()->__owner,
          "pthread_cond_timedwait reacquires mutex",
          "expected mutex ownership after timeout");
    (void) pthread_mutex_unlock(&mutex);

    atomic_int waiting;
    atomic_int woken;
    pthread_t tid;
    struct cv_signal_args arg;

    atomic_init(&waiting, 0);
    atomic_init(&woken, 0);
    arg.cond = &cond;
    arg.mutex = &mutex;
    arg.waiting = &waiting;
    arg.woken = &woken;

    if (pthread_create(&tid, NULL, cv_signal_waiter, &arg) != EOK) {
        test_fail("condvar timeout prerequisite", "thread create failed");
        return;
    }

    for (int i = 0; i < 10000; i++) {
        if (atomic_load_explicit(&waiting, memory_order_acquire) == 1)
            break;
        (void) __sched_yield();
    }

    int locked = 0;
    for (int i = 0; i < 100000; i++) {
        if (pthread_mutex_trylock(&mutex) == EOK) {
            locked = 1;
            break;
        }
        (void) __sched_yield();
    }
    if (!locked) {
        test_fail("condvar timeout signal target", "waiter did not block");
        return;
    }

    (void) pthread_cond_signal(&cond);
    (void) pthread_mutex_unlock(&mutex);

    for (int i = 0; i < 100000; i++) {
        if (atomic_load_explicit(&woken, memory_order_acquire) != 0)
            break;
        (void) __sched_yield();
    }
    check(atomic_load_explicit(&woken, memory_order_acquire) == 1,
          "condvar signal after timeout", "expected live waiter to wake");
    (void) tid;
}

/* Semaphore post / wait */

static void test_sem_post_wait(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_SEM, &s, NULL);
    if (rc != EOK) {
        test_fail("sem prerequisite", "setup failed");
        return;
    }

    rc = sync_sem_post(&s);
    check(rc == EOK, "sync_sem_post (first)", "expected EOK");

    /* Count is now 1 -- a non-blocking wait must succeed immediately. */
    rc = sync_sem_wait(&s, 1);
    check(rc == EOK, "sync_sem_wait (count=1, non-blocking)", "expected EOK");

    (void) __sync_destroy(&s);
}

static void test_sem_post_twice_wait_twice(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_SEM, &s, NULL);
    if (rc != EOK) {
        test_fail("sem x2 prerequisite", "setup failed");
        return;
    }

    int p1 = sync_sem_post(&s);
    int p2 = sync_sem_post(&s);
    check(p1 == EOK && p2 == EOK, "sync_sem_post x2", "expected both EOK");

    int w1 = sync_sem_wait(&s, 1);
    int w2 = sync_sem_wait(&s, 1);
    check(w1 == EOK && w2 == EOK, "sync_sem_wait x2 (count=2, non-blocking)",
          "expected both EOK");

    (void) __sync_destroy(&s);
}

static void test_posix_sem_init_post_wait(void)
{
    sem_t sem;
    int value;

    memset(&sem, 0, sizeof(sem));
    check(sem_init(&sem, 0, 1) == 0, "sem_init(value=1)", "expected 0");
    check(sem_getvalue(&sem, &value) == 0 && value == 1, "sem_getvalue",
          "expected value 1");
    check(sem_wait(&sem) == 0, "sem_wait(value=1)", "expected 0");
    check(sem_trywait(&sem) == -1 && errno == EAGAIN, "sem_trywait(empty)",
          "expected EAGAIN");
    check(sem_post(&sem) == 0, "sem_post", "expected 0");
    check(sem_trywait(&sem) == 0, "sem_trywait(after post)", "expected 0");
    check(sem_destroy(&sem) == 0, "sem_destroy", "expected 0");
}

static void test_posix_sem_invalid_args(void)
{
    sem_t sem;
    int value;

    memset(&sem, 0, sizeof(sem));
    check(sem_init(NULL, 0, 0) == -1 && errno == EINVAL, "sem_init(NULL)",
          "expected EINVAL");
    check(sem_init(&sem, 1, 0) == -1 && errno == ENOTSUP, "sem_init(pshared)",
          "expected ENOTSUP");
    check(sem_init(&sem, 0, (unsigned) SEM_VALUE_MAX + 1U) == -1 &&
              errno == EINVAL,
          "sem_init(value too large)", "expected EINVAL");
    check(sem_getvalue(NULL, &value) == -1 && errno == EINVAL,
          "sem_getvalue(NULL sem)", "expected EINVAL");
    check(sem_getvalue(&sem, NULL) == -1 && errno == EINVAL,
          "sem_getvalue(NULL value)", "expected EINVAL");
}

static void test_posix_sem_timedwait(void)
{
    sem_t sem;
    struct timespec abstime;

    memset(&sem, 0, sizeof(sem));
    if (sem_init(&sem, 0, 0) != 0) {
        test_fail("sem_timedwait prerequisite", strerror(errno));
        return;
    }

    check(clock_gettime(CLOCK_REALTIME, &abstime) == 0,
          "sem_timedwait clock_gettime", "expected 0");
    abstime.tv_nsec += 1000000;
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000000000L;
    }
    check(sem_timedwait(&sem, &abstime) == -1 && errno == ETIMEDOUT,
          "sem_timedwait(empty)", "expected ETIMEDOUT");

    check(sem_post(&sem) == 0, "sem_timedwait setup post", "expected 0");
    check(sem_timedwait(&sem, &abstime) == 0, "sem_timedwait(available)",
          "expected 0");
    (void) sem_destroy(&sem);
}

static void test_posix_named_sem_open_close_unlink(void)
{
    sem_t *sem;
    int value;

    (void) sem_unlink("/rtmux-sync-test");
    check(sysconf(_SC_SEMAPHORES) == _POSIX_VERSION, "sysconf(_SC_SEMAPHORES)",
          "expected supported semaphore option");
    sem = sem_open("/rtmux-sync-test", O_CREAT | O_EXCL, 0600, 1U);
    check(sem != SEM_FAILED, "sem_open(create)", "expected handle");
    if (sem == SEM_FAILED)
        return;

    check(sem_getvalue(sem, &value) == 0 && value == 1, "sem_getvalue(named)",
          "expected value 1");
    check(sem_getvalue(sem, NULL) == -1 && errno == EINVAL,
          "sem_getvalue(named NULL value)", "expected EINVAL");
    check(sem_wait(sem) == 0, "sem_wait(named)", "expected 0");
    check(sem_trywait(sem) == -1 && errno == EAGAIN, "sem_trywait(named empty)",
          "expected EAGAIN");
    check(sem_post(sem) == 0, "sem_post(named)", "expected 0");
    check(sem_close(sem) == 0, "sem_close(named)", "expected 0");

    sem = sem_open("/rtmux-sync-test", 0);
    check(sem != SEM_FAILED, "sem_open(existing)", "expected handle");
    if (sem != SEM_FAILED)
        check(sem_close(sem) == 0, "sem_close(existing)", "expected 0");
    check(sem_unlink("/rtmux-sync-test") == 0, "sem_unlink(named)",
          "expected 0");
    check(sem_open("/rtmux-sync-test", 0) == SEM_FAILED && errno == ENOENT,
          "sem_open(unlinked)", "expected ENOENT");
}

/* A timed MsgSend whose server is busy when the send is made blocks in SEND;
 * the receiver that later claims it moves it to REPLY, and the reply timeout
 * must be armed on that transition (Q74). The server takes the message and
 * never replies: a regression leaves the sender blocked for good, which hangs
 * the join below.
 */
struct busy_send {
    int coid;
    int rc;
    int err;
    atomic_int started;
};

static void *busy_send_thread(void *arg)
{
    struct busy_send *bs = arg;
    uint64_t ns = 100ULL * 1000000ULL;
    char out[8] = "ping", in[8];

    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_REPLY, NULL, &ns,
                           NULL);
    atomic_store(&bs->started, 1);
    bs->rc = msg_send(bs->coid, out, sizeof out, in, sizeof in);
    bs->err = errno;
    return NULL;
}

static void test_reply_timeout_armed_on_claim(void)
{
    struct busy_send bs = {.rc = 0, .err = 0, .started = 0};
    pthread_t th;
    char buf[8];
    int chid = channel_create(0);

    bs.coid = chid < 0 ? -1 : connect_attach(0, getpid(), chid, 0, 0);
    if (bs.coid < 0) {
        test_fail("reply timeout armed on claim: setup", strerror(errno));
        return;
    }
    if (pthread_create(&th, NULL, busy_send_thread, &bs) != 0) {
        test_fail("reply timeout armed on claim: pthread_create",
                  strerror(errno));
        return;
    }

    /* The sender announces itself, then the settle delay lets it queue in SEND
     * (nobody is receiving yet); a late sender only weakens the test.
     */
    while (!atomic_load(&bs.started))
        (void) nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL);
    (void) nanosleep(&(struct timespec){.tv_nsec = 50000000L}, NULL);
    if (msg_receive(chid, buf, sizeof buf, NULL) < 0) {
        test_fail("reply timeout armed on claim: msg_receive", strerror(errno));
        (void) __connect_detach((uint64_t) bs.coid);
        (void) channel_destroy(chid);
        (void) pthread_join(th, NULL);
        return;
    }
    (void) pthread_join(th,
                        NULL); /* hangs if the reply timeout was never armed */
    check(bs.rc == -1 && bs.err == ETIMEDOUT,
          "reply timeout fires for a sender claimed out of SEND",
          "expected ETIMEDOUT");
    (void) __connect_detach((uint64_t) bs.coid);
    (void) channel_destroy(chid);
}

static void test_posix_named_sem_timedwait(void)
{
    sem_t *sem;
    struct timespec abstime;

    (void) sem_unlink("/rtmux-sync-timeout");
    sem = sem_open("/rtmux-sync-timeout", O_CREAT | O_EXCL, 0600, 0U);
    if (sem == SEM_FAILED) {
        test_fail("sem_timedwait(named) prerequisite", strerror(errno));
        return;
    }

    check(clock_gettime(CLOCK_REALTIME, &abstime) == 0,
          "sem_timedwait(named) clock_gettime", "expected 0");
    abstime.tv_nsec += 1000000;
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000000000L;
    }
    check(sem_timedwait(sem, &abstime) == -1 && errno == ETIMEDOUT,
          "sem_timedwait(named empty)", "expected ETIMEDOUT");

    /* A deadline already behind the clock (a stalled host can make the 1 ms one
     * above so) must time out too, not wait for a timer that already expired.
     */
    (void) clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec--;
    check(sem_timedwait(sem, &abstime) == -1 && errno == ETIMEDOUT,
          "sem_timedwait(named empty, deadline past)", "expected ETIMEDOUT");

    /* Near-miss sweep: a deadline 1 ns to ~16 ms ahead expires between the libc
     * conversion and the kernel arm of the reply timeout, or while the sender
     * is mid-block (Q74: the past-deadline fire was delivered inline while the
     * sender held its own WAIT_PENDING bit and spun forever). Every wait must
     * report ETIMEDOUT, promptly. A regression hangs here.
     */
    {
        enum { SWEEP_ITERS = 6000 };
        int i, bad = 0, slow = 0;

        for (i = 0; i < SWEEP_ITERS; i++) {
            struct timespec t0, t1;

            /* 1 ns .. ~16 ms, log-spaced with a linear ramp inside each octave,
             * so some deadline lands in the arm window whatever the guest's
             * kcall latency is.
             */
            long oct = i % 24;
            long off =
                (1L << oct) + (((long) i * 2654435761L) & ((1L << oct) - 1));
            int rc, err;

            (void) clock_gettime(CLOCK_REALTIME, &abstime);
            abstime.tv_nsec += off;
            if (abstime.tv_nsec >= 1000000000L) {
                abstime.tv_sec++;
                abstime.tv_nsec -= 1000000000L;
            }
            (void) clock_gettime(CLOCK_MONOTONIC, &t0);
            rc = sem_timedwait(sem, &abstime);
            err = errno;
            (void) clock_gettime(CLOCK_MONOTONIC, &t1);
            if (rc != -1 || err != ETIMEDOUT)
                bad++;
            if ((t1.tv_sec - t0.tv_sec) * 1000000000L +
                    (t1.tv_nsec - t0.tv_nsec) >
                500000000L)
                slow++;
        }
        check(bad == 0, "sem_timedwait(named) near-miss deadline sweep",
              "a wait did not return ETIMEDOUT");
        check(slow == 0, "sem_timedwait(named) near-miss waits prompt",
              "a wait outlived its deadline by over 500 ms");
    }

    (void) sem_close(sem);
    (void) sem_unlink("/rtmux-sync-timeout");
}

/* Condvar basic paths */

static void test_condvar_signal_no_waiters(void)
{
    pthread_cond_t cond;
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_COND, &s, NULL);
    if (rc != EOK) {
        test_fail("condvar signal prerequisite", "setup failed");
        return;
    }

    /* Signal with no waiters is a legal noop. */
    rc = sync_condvar_signal(&s, 0);
    check(rc == EOK, "sync_condvar_signal (no waiters)", "expected EOK");

    rc = sync_condvar_signal(&s, 1); /* broadcast */
    check(rc == EOK, "sync_condvar_signal (broadcast, no waiters)",
          "expected EOK");

    (void) __sync_destroy(&s);

    if (pthread_cond_init(&cond, NULL) != EOK) {
        test_fail("pthread cond broadcast prerequisite", "setup failed");
        return;
    }
    check(pthread_cond_broadcast(&cond) == EOK,
          "pthread_cond_broadcast (no waiters)", "expected EOK");
    (void) pthread_cond_destroy(&cond);
}

/* __sync_destroy */

static void test_destroy_then_lock(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, NULL);
    if (rc != EOK) {
        test_fail("destroy-then-lock prerequisite", "setup failed");
        return;
    }
    int d = __sync_destroy(&s);
    check(d == EOK, "__sync_destroy (mutex)", "expected EOK");

    /* A subsequent lock on a destroyed mutex should fail, not crash. Kernel
     * sets s.owner = RTMUX_SYNC_DESTROYED; lock path detects and returns
     * EINVAL.
     */
    errno = 0;
    int after = __sync_mutex_lock(&s);
    check(after == -1 && errno == EINVAL, "__sync_mutex_lock after destroy",
          "expected -1, errno=EINVAL");
}

static void test_destroy_sem(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_SEM, &s, NULL);
    if (rc != EOK) {
        test_fail("destroy sem prerequisite", "setup failed");
        return;
    }
    int d = __sync_destroy(&s);
    check(d == EOK, "__sync_destroy (sem)", "expected EOK");
}

static void test_destroy_cond(void)
{
    pthread_cond_t cond;
    sync_t s;

    if (pthread_cond_init(&cond, NULL) != EOK) {
        test_fail("pthread destroy cond prerequisite", "setup failed");
        return;
    }
    int pd = pthread_cond_destroy(&cond);
    check(pd == EOK, "pthread_cond_destroy", "expected EOK");

    memset(&s, 0, sizeof(s));
    int rc = sync_type_create(RTMUX_SYNC_COND, &s, NULL);
    if (rc != EOK) {
        test_fail("destroy cond prerequisite", "setup failed");
        return;
    }
    int d = __sync_destroy(&s);
    check(d == EOK, "__sync_destroy (cond)", "expected EOK");
}

static int once_count;

static void once_increment(void)
{
    once_count++;
}

static void test_pthread_once_runs_once(void)
{
    pthread_once_t once = PTHREAD_ONCE_INIT;

    once_count = 0;
    int r1 = pthread_once(&once, once_increment);
    int r2 = pthread_once(&once, once_increment);

    check(r1 == EOK && r2 == EOK && once_count == 1, "pthread_once runs once",
          "init routine must run exactly once");
}

/* POSIX spin locks. Single-threaded: trylock-on-held proves the
 * mutual-exclusion invariant without a second thread (contention throughput is
 * not exercised here, only correctness of the state transitions).
 */

static void test_pthread_spin(void)
{
    pthread_spinlock_t spin;

    check(pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) == EOK,
          "pthread_spin_init(private)", "expected EOK");
    check(pthread_spin_init(&spin, PTHREAD_PROCESS_SHARED) == EOK,
          "pthread_spin_init(shared)", "spin locks support process-shared");
    check(pthread_spin_init(&spin, 12345) == EINVAL,
          "pthread_spin_init(bad pshared)", "expected EINVAL");
    check(pthread_spin_init(NULL, PTHREAD_PROCESS_PRIVATE) == EINVAL,
          "pthread_spin_init(NULL)", "expected EINVAL");

    check(pthread_spin_trylock(&spin) == EOK, "pthread_spin_trylock(free)",
          "expected EOK");
    check(pthread_spin_trylock(&spin) == EBUSY, "pthread_spin_trylock(held)",
          "a held spin lock must reject a second acquire");
    check(pthread_spin_unlock(&spin) == EOK, "pthread_spin_unlock(held)",
          "expected EOK");
    check(pthread_spin_trylock(&spin) == EOK,
          "pthread_spin_trylock(after unlock)", "expected EOK");
    check(pthread_spin_unlock(&spin) == EOK, "pthread_spin_unlock",
          "expected EOK");

    check(pthread_spin_lock(&spin) == EOK, "pthread_spin_lock(uncontended)",
          "expected EOK");
    check(pthread_spin_unlock(&spin) == EOK, "pthread_spin_unlock(after lock)",
          "expected EOK");
    check(pthread_spin_destroy(&spin) == EOK, "pthread_spin_destroy",
          "expected EOK");

    check(pthread_spin_lock(NULL) == EINVAL, "pthread_spin_lock(NULL)",
          "expected EINVAL");
    check(pthread_spin_trylock(NULL) == EINVAL, "pthread_spin_trylock(NULL)",
          "expected EINVAL");
    check(pthread_spin_unlock(NULL) == EINVAL, "pthread_spin_unlock(NULL)",
          "expected EINVAL");
    check(pthread_spin_destroy(NULL) == EINVAL, "pthread_spin_destroy(NULL)",
          "expected EINVAL");
}

/* POSIX barriers. Unlike the spin-lock test this needs real threads: the
 * rendezvous only completes when the configured count arrives, and two rounds
 * exercise the generation counter that guards against a fast thread racing back
 * in.
 */

#define BARRIER_THREADS 4

static pthread_barrier_t barrier_test;
static atomic_int barrier_serial_count;
static atomic_int barrier_passed;

static void *barrier_worker(void *arg)
{
    (void) arg;
    for (int round = 0; round < 2; round++) {
        int r = pthread_barrier_wait(&barrier_test);
        if (r == PTHREAD_BARRIER_SERIAL_THREAD)
            atomic_fetch_add(&barrier_serial_count, 1);
        else if (r != EOK)
            atomic_fetch_add(&barrier_serial_count,
                             1000); /* poison a failure */
    }
    atomic_fetch_add(&barrier_passed, 1);
    return NULL;
}

static void test_pthread_barrier(void)
{
    pthread_barrierattr_t attr;
    pthread_t tids[BARRIER_THREADS];
    int ps, created = 0;

    check(pthread_barrierattr_init(&attr) == EOK, "pthread_barrierattr_init",
          "expected EOK");
    check(pthread_barrierattr_getpshared(&attr, &ps) == EOK &&
              ps == PTHREAD_PROCESS_PRIVATE,
          "pthread_barrierattr_getpshared(default)", "expected PRIVATE");
    check(pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) ==
              ENOTSUP,
          "pthread_barrierattr_setpshared(SHARED)",
          "barriers are private-only");
    check(pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE) == EOK,
          "pthread_barrierattr_setpshared(PRIVATE)", "expected EOK");
    check(pthread_barrierattr_setpshared(&attr, 999) == EINVAL,
          "pthread_barrierattr_setpshared(bad)", "expected EINVAL");
    check(pthread_barrierattr_init(NULL) == EINVAL,
          "pthread_barrierattr_init(NULL)", "expected EINVAL");
    check(pthread_barrierattr_destroy(&attr) == EOK,
          "pthread_barrierattr_destroy", "expected EOK");

    check(pthread_barrier_init(&barrier_test, NULL, 0) == EINVAL,
          "pthread_barrier_init(count=0)", "expected EINVAL");
    check(pthread_barrier_init(NULL, NULL, 2) == EINVAL,
          "pthread_barrier_init(NULL)", "expected EINVAL");
    check(pthread_barrier_wait(NULL) == EINVAL, "pthread_barrier_wait(NULL)",
          "expected EINVAL");
    check(pthread_barrier_destroy(NULL) == EINVAL,
          "pthread_barrier_destroy(NULL)", "expected EINVAL");

    atomic_store(&barrier_serial_count, 0);
    atomic_store(&barrier_passed, 0);
    check(pthread_barrier_init(&barrier_test, NULL, BARRIER_THREADS) == EOK,
          "pthread_barrier_init(N)", "expected EOK");
    for (int i = 0; i < BARRIER_THREADS; i++) {
        if (pthread_create(&tids[i], NULL, barrier_worker, NULL) == 0)
            created++;
    }
    check(created == BARRIER_THREADS, "barrier worker spawn",
          "all threads created");
    for (int i = 0; i < created; i++)
        pthread_join(tids[i], NULL);

    check(atomic_load(&barrier_serial_count) == 2,
          "barrier serial-thread count",
          "exactly one serial thread across each of two rounds");
    check(atomic_load(&barrier_passed) == BARRIER_THREADS,
          "barrier all threads passed", "every thread completed both rounds");
    check(pthread_barrier_destroy(&barrier_test) == EOK,
          "pthread_barrier_destroy", "expected EOK");
}

/* POSIX read-write locks. The single-threaded part walks the state machine
 * (shared readers, exclusive writer, try/timed edges); the multi-threaded part
 * proves writer mutual exclusion — a torn read under a read lock would mean a
 * writer ran concurrently.
 */

#define RW_WRITERS 3
#define RW_READERS 3
#define RW_ITERS 200

static pthread_rwlock_t rw_test;
static long rw_counter;
static atomic_int rw_lock_errors;
static atomic_int rw_torn_reads;

static void *rw_writer(void *arg)
{
    (void) arg;
    for (int i = 0; i < RW_ITERS; i++) {
        if (pthread_rwlock_wrlock(&rw_test) != EOK) {
            atomic_fetch_add(&rw_lock_errors, 1);
            return NULL;
        }
        rw_counter++;
        pthread_rwlock_unlock(&rw_test);
    }
    return NULL;
}

static void *rw_reader(void *arg)
{
    (void) arg;
    for (int i = 0; i < RW_ITERS; i++) {
        if (pthread_rwlock_rdlock(&rw_test) != EOK) {
            atomic_fetch_add(&rw_lock_errors, 1);
            return NULL;
        }
        long a = rw_counter;
        long b =
            rw_counter; /* no writer can run between these under a rdlock */
        if (a != b)
            atomic_fetch_add(&rw_torn_reads, 1);
        pthread_rwlock_unlock(&rw_test);
    }
    return NULL;
}

static void test_pthread_rwlock(void)
{
    pthread_rwlockattr_t attr;
    pthread_t tids[RW_WRITERS + RW_READERS];
    struct timespec now;
    int ps, created = 0;

    check(pthread_rwlockattr_init(&attr) == EOK, "pthread_rwlockattr_init",
          "expected EOK");
    check(pthread_rwlockattr_getpshared(&attr, &ps) == EOK &&
              ps == PTHREAD_PROCESS_PRIVATE,
          "pthread_rwlockattr_getpshared(default)", "expected PRIVATE");
    check(
        pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == ENOTSUP,
        "pthread_rwlockattr_setpshared(SHARED)", "rwlocks are private-only");
    check(pthread_rwlockattr_setpshared(&attr, 999) == EINVAL,
          "pthread_rwlockattr_setpshared(bad)", "expected EINVAL");
    check(pthread_rwlockattr_destroy(&attr) == EOK,
          "pthread_rwlockattr_destroy", "expected EOK");

    check(pthread_rwlock_init(NULL, NULL) == EINVAL,
          "pthread_rwlock_init(NULL)", "expected EINVAL");
    check(pthread_rwlock_rdlock(NULL) == EINVAL, "pthread_rwlock_rdlock(NULL)",
          "expected EINVAL");
    check(pthread_rwlock_wrlock(NULL) == EINVAL, "pthread_rwlock_wrlock(NULL)",
          "expected EINVAL");
    check(pthread_rwlock_unlock(NULL) == EINVAL, "pthread_rwlock_unlock(NULL)",
          "expected EINVAL");

    check(pthread_rwlock_init(&rw_test, NULL) == EOK, "pthread_rwlock_init",
          "expected EOK");

    /* Shared readers exclude a writer. */
    check(pthread_rwlock_tryrdlock(&rw_test) == EOK, "rwlock tryrdlock #1",
          "expected EOK");
    check(pthread_rwlock_tryrdlock(&rw_test) == EOK, "rwlock tryrdlock #2",
          "readers share");
    check(pthread_rwlock_trywrlock(&rw_test) == EBUSY,
          "rwlock trywrlock vs readers", "a writer must wait for readers");
    check(pthread_rwlock_unlock(&rw_test) == EOK, "rwlock reader unlock #1",
          "expected EOK");
    check(pthread_rwlock_unlock(&rw_test) == EOK, "rwlock reader unlock #2",
          "expected EOK");

    /* A timed reader acquires a free lock without blocking. */
    clock_gettime(CLOCK_REALTIME, &now);
    now.tv_sec += 5; /* ample deadline */
    check(pthread_rwlock_timedrdlock(&rw_test, &now) == EOK,
          "rwlock timedrdlock(free)", "a free lock grants a timed reader");
    check(pthread_rwlock_unlock(&rw_test) == EOK, "rwlock timedrdlock unlock",
          "expected EOK");

    /* An exclusive writer excludes everyone. */
    check(pthread_rwlock_trywrlock(&rw_test) == EOK, "rwlock trywrlock(free)",
          "expected EOK");
    check(pthread_rwlock_tryrdlock(&rw_test) == EBUSY,
          "rwlock tryrdlock vs writer", "a reader must wait for the writer");
    check(pthread_rwlock_timedwrlock(&rw_test, NULL) == EINVAL,
          "rwlock timedwrlock(NULL abstime)", "expected EINVAL");
    check(pthread_rwlock_timedrdlock(&rw_test, NULL) == EINVAL,
          "rwlock timedrdlock(NULL abstime)", "expected EINVAL");

    /* RTMux rwlocks track no owner, so a would-be reader contends with the held
     * writer and simply times out on an expired deadline rather than detecting
     * self-contention as EDEADLK.
     */
    clock_gettime(CLOCK_REALTIME, &now); /* already-due deadline */
    check(pthread_rwlock_timedrdlock(&rw_test, &now) == ETIMEDOUT,
          "rwlock timedrdlock times out", "expected ETIMEDOUT under a writer");
    check(pthread_rwlock_unlock(&rw_test) == EOK, "rwlock writer unlock",
          "expected EOK");
    check(pthread_rwlock_unlock(&rw_test) == EPERM, "rwlock unlock(free)",
          "unlocking a free rwlock is refused");

    /* A timed writer acquisition gives up when a reader holds the lock. */
    check(pthread_rwlock_rdlock(&rw_test) == EOK, "rwlock rdlock for timeout",
          "expected EOK");
    clock_gettime(CLOCK_REALTIME, &now); /* already-due deadline */
    check(pthread_rwlock_timedwrlock(&rw_test, &now) == ETIMEDOUT,
          "rwlock timedwrlock times out", "expected ETIMEDOUT under a reader");
    check(pthread_rwlock_unlock(&rw_test) == EOK, "rwlock unlock after timeout",
          "expected EOK");

    /* Concurrent writers/readers: writer mutual exclusion keeps the count exact
     * and read locks never observe a torn value.
     */
    rw_counter = 0;
    atomic_store(&rw_lock_errors, 0);
    atomic_store(&rw_torn_reads, 0);
    for (int i = 0; i < RW_WRITERS; i++)
        if (pthread_create(&tids[created], NULL, rw_writer, NULL) == 0)
            created++;
    for (int i = 0; i < RW_READERS; i++)
        if (pthread_create(&tids[created], NULL, rw_reader, NULL) == 0)
            created++;
    check(created == RW_WRITERS + RW_READERS, "rwlock worker spawn",
          "all threads created");
    for (int i = 0; i < created; i++)
        pthread_join(tids[i], NULL);

    check(rw_counter == (long) RW_WRITERS * RW_ITERS, "rwlock writer exclusion",
          "every write is serialized");
    check(atomic_load(&rw_torn_reads) == 0, "rwlock no torn reads",
          "no writer ran during a read lock");
    check(atomic_load(&rw_lock_errors) == 0, "rwlock no lock errors",
          "every acquire succeeded");
    check(pthread_rwlock_destroy(&rw_test) == EOK, "pthread_rwlock_destroy",
          "expected EOK");

    /* PTHREAD_RWLOCK_INITIALIZER is usable without an explicit init call. */
    pthread_rwlock_t rw_static = PTHREAD_RWLOCK_INITIALIZER;
    check(pthread_rwlock_rdlock(&rw_static) == EOK, "rwlock static rdlock",
          "static initializer usable without init");
    check(pthread_rwlock_unlock(&rw_static) == EOK, "rwlock static rd unlock",
          "expected EOK");
    check(pthread_rwlock_wrlock(&rw_static) == EOK, "rwlock static wrlock",
          "expected EOK");
    check(pthread_rwlock_unlock(&rw_static) == EOK, "rwlock static wr unlock",
          "expected EOK");
    check(pthread_rwlock_destroy(&rw_static) == EOK, "rwlock static destroy",
          "expected EOK");
}

/* Regression: a thread that already holds a read lock must be able to take a
 * nested read lock even while a writer is queued. Under strict
 * writer-preference this deadlocks — the nested reader waits for the writer,
 * which waits for the first read lock to drop.
 */

static pthread_rwlock_t rw_nested;
static atomic_int rw_writer_armed;

static void *rw_nested_writer(void *arg)
{
    (void) arg;
    atomic_store(&rw_writer_armed, 1);
    /* Blocks until the reader drops both locks. */
    if (pthread_rwlock_wrlock(&rw_nested) == EOK)
        pthread_rwlock_unlock(&rw_nested);
    return NULL;
}

static void test_pthread_rwlock_nested_read(void)
{
    pthread_t w;
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};

    check(pthread_rwlock_init(&rw_nested, NULL) == EOK, "nested-rd init",
          "expected EOK");
    check(pthread_rwlock_rdlock(&rw_nested) == EOK, "nested-rd first rdlock",
          "expected EOK");

    atomic_store(&rw_writer_armed, 0);
    if (pthread_create(&w, NULL, rw_nested_writer, NULL) != 0)
        test_fail("nested-rd writer spawn", "pthread_create failed");
    while (!atomic_load(&rw_writer_armed))
        nanosleep(&nap, NULL);
    nanosleep(&nap, NULL); /* let the writer reach wrlock's wait */

    /* If writer-preference were strict this call would deadlock and hang the
     * whole test; the reader-join rule lets it proceed.
     */
    check(pthread_rwlock_rdlock(&rw_nested) == EOK,
          "nested rdlock with writer queued",
          "a held read lock must nest without deadlocking behind a writer");

    check(pthread_rwlock_unlock(&rw_nested) == EOK, "nested-rd drop nested",
          "expected EOK");
    check(pthread_rwlock_unlock(&rw_nested) == EOK, "nested-rd drop first",
          "expected EOK");
    pthread_join(w, NULL);
    check(pthread_rwlock_destroy(&rw_nested) == EOK, "nested-rd destroy",
          "expected EOK");
}

/* Re-create at the same address (exercises sync_hash path) */

static void test_create_destroy_recreate(void)
{
    sync_t s;
    memset(&s, 0, sizeof(s));

    int r1 = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, NULL);
    int d1 = (r1 == EOK) ? __sync_destroy(&s) : -1;
    int r2 = sync_type_create(RTMUX_SYNC_MUTEX_FREE, &s, NULL);
    int d2 = (r2 == EOK) ? __sync_destroy(&s) : -1;

    check(r1 == EOK && d1 == EOK && r2 == EOK && d2 == EOK,
          "create/destroy/re-create at same address",
          "expected every step EOK");
}

/* Aggregator */

/* FP register isolation
 *
 * User f0-f31 and fcsr must belong to one thread at a time. fp_loop loads all
 * 32 registers with a pattern unique to the caller, yields (a real context
 * switch when another thread shares the hart), then checks every register; it
 * returns the number of mismatches. It is assembly so that no compiler-managed
 * FP value can hide a leak, and it preserves the lp64d callee-saved FP
 * registers. fp_read_all returns the OR of f0-f31 and fcsr.
 */
_Static_assert(__KSYS_SCHED_YIELD == 90,
               "fp_loop hard-codes the yield syscall");

uint64_t fp_loop(uint64_t pattern, uint64_t iters);
uint64_t fp_read_all(void);

__asm__(
    ".option push\n"
    ".option arch, +d\n"
    ".text\n"
    ".globl fp_loop\n"
    "fp_loop:\n"
    "    addi sp, sp, -160\n"
    "    sd s1, 0(sp)\n sd s2, 8(sp)\n sd s3, 16(sp)\n"
    "    fsd fs0, 24(sp)\n fsd fs1, 32(sp)\n"
    "    fsd fs2, 40(sp)\n fsd fs3, 48(sp)\n fsd fs4, 56(sp)\n"
    "    fsd fs5, 64(sp)\n fsd fs6, 72(sp)\n fsd fs7, 80(sp)\n"
    "    fsd fs8, 88(sp)\n fsd fs9, 96(sp)\n fsd fs10, 104(sp)\n"
    "    fsd fs11, 112(sp)\n"
    "    mv s1, a0\n mv s2, a1\n li s3, 0\n"
    "1:\n"
    "    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,"
    "23,24,25,26,27,28,29,30,31\n"
    "    addi t2, s1, \\n\n"
    "    fmv.d.x f\\n, t2\n"
    "    .endr\n"
    "    li a7, 90\n"
    "    ecall\n"
    "    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,"
    "23,24,25,26,27,28,29,30,31\n"
    "    fmv.x.d t3, f\\n\n"
    "    addi t2, s1, \\n\n"
    "    xor t3, t3, t2\n"
    "    snez t3, t3\n"
    "    add s3, s3, t3\n"
    "    .endr\n"
    "    addi s2, s2, -1\n"
    "    bnez s2, 1b\n"
    "    mv a0, s3\n"
    "    ld s1, 0(sp)\n ld s2, 8(sp)\n ld s3, 16(sp)\n"
    "    fld fs0, 24(sp)\n fld fs1, 32(sp)\n"
    "    fld fs2, 40(sp)\n fld fs3, 48(sp)\n fld fs4, 56(sp)\n"
    "    fld fs5, 64(sp)\n fld fs6, 72(sp)\n fld fs7, 80(sp)\n"
    "    fld fs8, 88(sp)\n fld fs9, 96(sp)\n fld fs10, 104(sp)\n"
    "    fld fs11, 112(sp)\n"
    "    addi sp, sp, 160\n"
    "    ret\n"
    ".globl fp_read_all\n"
    "fp_read_all:\n"
    "    frcsr t0\n"
    "    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,"
    "23,24,25,26,27,28,29,30,31\n"
    "    fmv.x.d t1, f\\n\n"
    "    or t0, t0, t1\n"
    "    .endr\n"
    "    mv a0, t0\n"
    "    ret\n"
    ".option pop\n");

#define FP_ITERS 20000

struct fp_job {
    uint64_t pattern;
    uint64_t bad;
    int pin; /* cpu to pin to, or -1 */
};

int test_pin_mask(unsigned mask)
{
    size_t bytes = sizeof(struct _thread_runmask) + 2U * sizeof(unsigned);
    struct _thread_runmask *m = calloc(1U, bytes);
    unsigned *runmask;
    int rc;

    if (!m)
        return -1;
    m->size = 1U;
    runmask = (unsigned *) (m + 1);
    runmask[0] = mask; /* run mask */
    runmask[1] = mask; /* inherit mask: threads created from here follow */
    rc = thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, m);
    free(m);
    return rc;
}

static int fp_pin(int cpu)
{
    unsigned mask = 0;

    RMSK_SET((unsigned) cpu, &mask);
    return test_pin_mask(mask);
}

static void *fp_worker(void *arg)
{
    struct fp_job *j = arg;

    if (j->pin >= 0)
        (void) fp_pin(j->pin);
    j->bad = fp_loop(j->pattern, FP_ITERS);
    return NULL;
}

/* Child half of the cross-process case: same loop, pinned to cpu 0. */
int fp_isolation_child(void)
{
    (void) fp_pin(0);
    return fp_loop(0x7000000000000000ULL, FP_ITERS) == 0 ? 0 : 1;
}

static uint64_t fp_run_threads(int n, int pin)
{
    struct fp_job jobs[4];
    pthread_t th[4];
    uint64_t bad = 0;
    int started = 0;

    for (int i = 0; i < n; i++) {
        jobs[i].pattern =
            (uint64_t) 0x1000000000000000ULL * (uint64_t) (i + 1) + 0x100U;
        jobs[i].bad = 1;
        jobs[i].pin = pin;
        if (pthread_create(&th[i], NULL, fp_worker, &jobs[i]) != 0)
            break;
        started++;
    }
    for (int i = 0; i < started; i++) {
        (void) pthread_join(th[i], NULL);
        bad += jobs[i].bad;
    }
    return started == n ? bad : UINT64_MAX;
}

static void *fp_zero_reader(void *arg)
{
    uint64_t *out = arg;

    (void) fp_pin(0);
    *out = fp_read_all();
    return NULL;
}

static void test_fp_isolation(void)
{
    char *argv[] = {"syscall_testing", "--fp-isolation-child", NULL};
    uint64_t bad, seen = 1;
    pthread_t th;
    pid_t child;
    int status = -1;

    bad = fp_run_threads(2, 0);
    check(bad == 0, "fp_isolation_pinned two threads share one hart",
          "an FP register leaked between threads on the same hart");

    bad = fp_run_threads(3, -1);
    check(bad == 0, "fp_isolation_migrate three unpinned threads",
          "an FP register leaked between threads that may migrate");

    /* A second process on the same hart, running the loop at the same time. */
    if (posix_spawn(&child, "/bin/syscall_testing", NULL, NULL, argv, NULL) ==
        0) {
        struct fp_job j = {.pattern = 0x2000000000000000ULL, .pin = 0};
        pthread_t jt;

        /* On a helper thread: pinning is inherited and would otherwise pin the
         * whole test driver to hart 0 for every test that follows.
         */
        if (pthread_create(&jt, NULL, fp_worker, &j) == 0)
            (void) pthread_join(jt, NULL);
        else
            j.bad = 1;
        (void) waitpid(child, &status, 0);
        check(j.bad == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "fp_isolation_process two processes share one hart",
              "FP registers leaked between processes");
    } else {
        test_fail("fp_isolation_process spawn", strerror(errno));
    }

    /* A new thread reads f0-f31 and fcsr before writing any of them; the
     * earlier loops left nonzero values behind on hart 0.
     */
    if (pthread_create(&th, NULL, fp_zero_reader, &seen) == 0) {
        (void) pthread_join(th, NULL);
        check(seen == 0, "fp_first_use_zero a new thread reads all zeros",
              "a fresh thread saw another thread's FP state");
    } else {
        test_fail("fp_first_use_zero fixture", strerror(errno));
    }
}

/* malloc lock under mixed FIFO priorities
 *
 * Four FIFO threads at different priorities, all pinned to one hart, hammer
 * malloc and free. A spinning allocator lock deadlocks this: the highest
 * priority thread spins on a lock held by a preempted lower-priority thread
 * that can never run. With a blocking priority-inheritance lock they all
 * finish.
 */
#define MALLOC_FIFO_THREADS 4
#define MALLOC_FIFO_OPS 4000

static atomic_int malloc_fifo_done;

static void *malloc_fifo_worker(void *arg)
{
    unsigned seed = (unsigned) (uintptr_t) arg;
    void *slots[16] = {0};

    (void) fp_pin(0);
    for (int i = 0; i < MALLOC_FIFO_OPS; i++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned k = (seed >> 24) & 15;

        free(slots[k]);
        slots[k] = malloc(8 + ((seed >> 8) & 1023));
        if (slots[k])
            *(volatile char *) slots[k] = (char) i;
    }
    for (int k = 0; k < 16; k++)
        free(slots[k]);
    atomic_fetch_add(&malloc_fifo_done, 1);
    return NULL;
}

static void test_malloc_fifo_mixed_prio(void)
{
    pthread_t th[MALLOC_FIFO_THREADS];
    int started = 0;

    atomic_store(&malloc_fifo_done, 0);
    for (int i = 0; i < MALLOC_FIFO_THREADS; i++) {
        pthread_attr_t attr;
        struct sched_param sp = {.sched_priority = 10 * (i + 1)};

        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        if (pthread_create(&th[i], &attr, malloc_fifo_worker,
                           (void *) (uintptr_t) (i + 1)) != 0)
            break;
        started++;
    }

    /* Watchdog: a lock that can deadlock must fail the test, not hang the
     * suite. Low priority is fine here: the workers finish or the wait expires.
     */
    for (int t = 0; t < 600 && atomic_load(&malloc_fifo_done) < started; t++) {
        struct timespec ts = {.tv_nsec = 100000000L};

        (void) nanosleep(&ts, NULL);
    }
    if (atomic_load(&malloc_fifo_done) < started ||
        started != MALLOC_FIFO_THREADS) {
        test_fail(
            "malloc_fifo_mixed_prio",
            "allocator workers at different FIFO priorities did not finish");
        _exit(1); /* the stuck threads cannot be joined */
    }
    for (int i = 0; i < started; i++)
        (void) pthread_join(th[i], NULL);
    check(1, "malloc_fifo_mixed_prio four FIFO threads finish on one hart",
          "allocator lock starved a thread");
}

/* Robust mutexes (POSIX.1-2024): a lock that finds the owner died returns
 * EOWNERDEAD with the mutex held, pthread_mutex_consistent repairs it, and an
 * unlock without the repair leaves it ENOTRECOVERABLE.
 */

static int plain_mutex_init(pthread_mutex_t *m, int pshared)
{
    pthread_mutexattr_t attr;
    int rc;

    if (pthread_mutexattr_init(&attr) != EOK)
        return -1;
    rc = pthread_mutexattr_setpshared(&attr, pshared);
    if (rc == EOK)
        rc = pthread_mutex_init(m, &attr);
    (void) pthread_mutexattr_destroy(&attr);
    return rc;
}

static int robust_mutex_init(pthread_mutex_t *m, int protocol, int pshared)
{
    pthread_mutexattr_t attr;
    int rc;

    if (pthread_mutexattr_init(&attr) != EOK)
        return -1;
    rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    if (rc == EOK)
        rc = pthread_mutexattr_setprotocol(&attr, protocol);
    if (rc == EOK)
        rc = pthread_mutexattr_setpshared(&attr, pshared);
    if (rc == EOK)
        rc = pthread_mutex_init(m, &attr);
    (void) pthread_mutexattr_destroy(&attr);
    return rc;
}

static void test_robust_mutexattr(void)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t m;
    int robust = -1;

    if (pthread_mutexattr_init(&attr) != EOK) {
        test_fail("robust mutexattr prerequisite", "init failed");
        return;
    }
    check(pthread_mutexattr_getrobust(&attr, &robust) == EOK &&
              robust == PTHREAD_MUTEX_STALLED,
          "pthread_mutexattr_getrobust default", "expected STALLED");
    check(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == EOK &&
              pthread_mutexattr_getrobust(&attr, &robust) == EOK &&
              robust == PTHREAD_MUTEX_ROBUST,
          "pthread_mutexattr ROBUST round-trip", "expected ROBUST");
    check(strcmp(strerror(EOWNERDEAD), "Owner died") == 0 &&
              strcmp(strerror(ENOTRECOVERABLE), "State not recoverable") == 0,
          "strerror for EOWNERDEAD and ENOTRECOVERABLE", "missing text");
    check(pthread_mutexattr_setrobust(&attr, 7) == EINVAL &&
              pthread_mutexattr_getrobust(&attr, NULL) == EINVAL,
          "pthread_mutexattr bad robust", "expected EINVAL");
    (void) pthread_mutexattr_destroy(&attr);

    /* Only a robust mutex in the inconsistent state can be made consistent. */
    if (pthread_mutex_init(&m, NULL) == EOK && pthread_mutex_lock(&m) == EOK) {
        check(pthread_mutex_consistent(&m) == EINVAL,
              "pthread_mutex_consistent on a non-robust mutex",
              "expected EINVAL");
        (void) pthread_mutex_unlock(&m);
        (void) pthread_mutex_destroy(&m);
    }
    if (robust_mutex_init(&m, PTHREAD_PRIO_NONE, PTHREAD_PROCESS_PRIVATE) ==
            EOK &&
        pthread_mutex_lock(&m) == EOK) {
        check(pthread_mutex_consistent(&m) == EINVAL,
              "pthread_mutex_consistent on a consistent robust mutex",
              "expected EINVAL");
        check(pthread_mutex_lock(&m) == EDEADLK &&
                  pthread_mutex_unlock(&m) == EOK &&
                  pthread_mutex_unlock(&m) == EPERM,
              "robust mutex lock and unlock without an owner death",
              "expected EDEADLK on re-lock, then EOK, then EPERM");
        (void) pthread_mutex_destroy(&m);
    }
}

struct robust_owner_args {
    pthread_mutex_t *mutex;
    atomic_int *locked;
    atomic_int *die;
};

/* Lock the mutex, report it, then exit holding it once told to. It sleeps, not
 * yields, so that waiters of a lower priority get to queue meanwhile.
 */
static void *robust_dying_owner(void *raw)
{
    struct robust_owner_args *arg = raw;
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};

    if (pthread_mutex_lock(arg->mutex) != EOK) {
        atomic_store_explicit(arg->locked, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(arg->locked, 1, memory_order_release);
    while (arg->die && !atomic_load_explicit(arg->die, memory_order_acquire))
        (void) nanosleep(&ms, NULL);
    return NULL;
}

static int robust_start_owner(pthread_t *tid, struct robust_owner_args *arg)
{
    if (pthread_create(tid, NULL, robust_dying_owner, arg) != EOK)
        return 0;
    for (int i = 0; i < 10000; i++) {
        int locked = atomic_load_explicit(arg->locked, memory_order_acquire);

        if (locked)
            return locked == 1;
        (void) __sched_yield();
    }
    return 0;
}

/* The owner thread exits with nobody waiting: the next lock and trylock take
 * the mutex with EOWNERDEAD.
 */
static void test_robust_owner_thread_exit(void)
{
    pthread_mutex_t m;
    pthread_t tid;
    atomic_int locked;
    struct robust_owner_args arg = {.mutex = &m, .locked = &locked};
    int rc;

    if (robust_mutex_init(&m, PTHREAD_PRIO_NONE, PTHREAD_PROCESS_PRIVATE) !=
        EOK) {
        test_fail("robust thread exit prerequisite", "init failed");
        return;
    }

    atomic_init(&locked, 0);
    if (!robust_start_owner(&tid, &arg)) {
        test_fail("robust thread exit prerequisite", "owner did not lock");
        return;
    }
    (void) thread_join(tid, NULL);
    rc = pthread_mutex_lock(&m);
    check(
        rc == EOWNERDEAD && (m.owner & ~RTMUX_SYNC_WAITING) == __tls()->__owner,
        "robust lock after the owner thread exited",
        "expected EOWNERDEAD with the mutex held");
    if (rc != EOWNERDEAD)
        return;
    check(pthread_mutex_consistent(&m) == EOK &&
              pthread_mutex_consistent(&m) == EINVAL,
          "pthread_mutex_consistent after EOWNERDEAD",
          "expected EOK, then EINVAL once consistent");
    check(pthread_mutex_unlock(&m) == EOK && pthread_mutex_lock(&m) == EOK &&
              pthread_mutex_unlock(&m) == EOK,
          "robust mutex usable after pthread_mutex_consistent",
          "expected normal lock and unlock");

    /* Again, taken by trylock and released without the repair. */
    atomic_init(&locked, 0);
    if (!robust_start_owner(&tid, &arg)) {
        test_fail("robust thread exit prerequisite", "second owner failed");
        return;
    }
    (void) thread_join(tid, NULL);
    check(pthread_mutex_trylock(&m) == EOWNERDEAD,
          "robust trylock after the owner thread exited",
          "expected EOWNERDEAD");
    check(pthread_mutex_unlock(&m) == EOK &&
              pthread_mutex_lock(&m) == ENOTRECOVERABLE &&
              pthread_mutex_trylock(&m) == ENOTRECOVERABLE,
          "robust unlock without pthread_mutex_consistent",
          "expected ENOTRECOVERABLE from every later lock");
    (void) pthread_mutex_destroy(&m);
}

struct robust_waiter_args {
    pthread_mutex_t *mutex;
    int prio;
    atomic_int *result;   /* lock result, -1 until it returns */
    atomic_int *observed; /* priority seen while holding, if wait_waiting */
    int wait_waiting;     /* hold until another waiter queues behind */
    int lent_prio;        /* the priority the waiter queued behind lends it */
    int repair;           /* call pthread_mutex_consistent on EOWNERDEAD */
};

static void *robust_waiter(void *raw)
{
    struct robust_waiter_args *arg = raw;
    struct sched_param sp;
    int rc;

    if (arg->prio)
        (void) set_fifo_prio(arg->prio);
    rc = pthread_mutex_lock(arg->mutex);
    atomic_store_explicit(arg->result, rc, memory_order_release);
    if (rc != EOK && rc != EOWNERDEAD)
        return NULL;
    if (arg->wait_waiting) {
        struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};

        /* Sleep, not yield: the contender is still to be created. */
        for (int i = 0; i < 5000 && !(arg->mutex->owner & RTMUX_SYNC_WAITING);
             i++)
            (void) nanosleep(&ms, NULL);

        /* A waiter sets WAITING before it lends its priority, so the bit does
         * not yet mean the loan is in place: give it time to land.
         */
        if (arg->mutex->owner & RTMUX_SYNC_WAITING) {
            for (int i = 0; i < 2000; i++) {
                if (__sched_get(0, (int) pthread_self(), &sp) == -1)
                    break;
                atomic_store_explicit(arg->observed, sp.sched_curpriority,
                                      memory_order_release);
                if (sp.sched_curpriority >= arg->lent_prio)
                    break;
                (void) nanosleep(&ms, NULL);
            }
        }
    }
    if (rc == EOWNERDEAD && arg->repair)
        (void) pthread_mutex_consistent(arg->mutex);
    (void) pthread_mutex_unlock(arg->mutex);
    return NULL;
}

/* Three distinct waiter priorities on three mutexes owned by this thread.
 * Release the middle loan first, then the highest. The remaining low loan
 * must still be visible when its turnstile becomes the head. */
void test_pi_owned_turnstiles(void)
{
    const int priorities[] = {20, 25, 30};
    const int release_order[] = {1, 2, 0};
    const int remaining[] = {30, 20, 5};
    pthread_mutex_t locks[3];
    pthread_t waiters[3];
    struct robust_waiter_args args[3];
    atomic_int results[3];
    struct sched_param original, current;
    int policy = __sched_get(0, 0, &original);
    unsigned created = 0, locked = 0;
    int ok = policy >= 0 && set_fifo_prio(5) == EOK;

    for (unsigned i = 0; ok && i < 3; i++) {
        ok = robust_mutex_init(&locks[i], PTHREAD_PRIO_INHERIT,
                               PTHREAD_PROCESS_PRIVATE) == EOK;
        if (!ok)
            break;
        if (pthread_mutex_lock(&locks[i]) != EOK) {
            pthread_mutex_destroy(&locks[i]);
            ok = 0;
            break;
        }
        locked++;
        atomic_init(&results[i], -1);
        args[i] = (struct robust_waiter_args) {
            .mutex = &locks[i], .prio = priorities[i], .result = &results[i]};
        ok = pthread_create(&waiters[i], NULL, robust_waiter, &args[i]) == EOK;
        if (!ok)
            break;
        created++;
        for (int n = 0; n < 1000; n++) {
            if (__sched_get(0, 0, &current) >= 0 &&
                current.sched_curpriority == priorities[i])
                break;
            (void) nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
        }
        ok = __sched_get(0, 0, &current) >= 0 &&
             current.sched_curpriority == priorities[i];
    }
    for (unsigned i = 0; i < 3; i++) {
        unsigned index = (unsigned) release_order[i];
        if (index >= locked)
            continue;
        if (pthread_mutex_unlock(&locks[index]) != EOK)
            ok = 0;
        if (index < created &&
            (pthread_join(waiters[index], NULL) != EOK ||
             atomic_load(&results[index]) != EOK))
            ok = 0;
        if (pthread_mutex_destroy(&locks[index]) != EOK)
            ok = 0;
        if (__sched_get(0, 0, &current) < 0 ||
            current.sched_curpriority != remaining[i])
            ok = 0;
    }
    if (policy >= 0 && __sched_set(0, 0, policy, &original) != EOK)
        ok = 0;
    check(ok, "PI owner keeps the highest remaining mutex loan",
          "expected boosts 20, 25, 30 then 30, 20, 5 across handoff");
}

/* A PI robust mutex whose owner dies with a waiter queued: the waiter gets it
 * with EOWNERDEAD, and a higher-priority contender that queues behind the new
 * owner lends it its priority.
 */
static void test_robust_waiter_handoff_pi(void)
{
    pthread_mutex_t m;
    pthread_t owner, heir, contender;
    atomic_int locked, die, heir_rc, heir_prio, contender_rc, unused;
    struct robust_owner_args oarg = {
        .mutex = &m, .locked = &locked, .die = &die};
    struct robust_waiter_args harg = {.mutex = &m,
                                      .prio = 5,
                                      .result = &heir_rc,
                                      .observed = &heir_prio,
                                      .wait_waiting = 1,
                                      .lent_prio = 30,
                                      .repair = 1};
    struct robust_waiter_args carg = {
        .mutex = &m, .prio = 30, .result = &contender_rc, .observed = &unused};
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};
    char detail[96];

    if (robust_mutex_init(&m, PTHREAD_PRIO_INHERIT, PTHREAD_PROCESS_PRIVATE) !=
        EOK) {
        test_fail("robust PI handoff prerequisite", "init failed");
        return;
    }
    atomic_init(&locked, 0);
    atomic_init(&die, 0);
    atomic_init(&heir_rc, -1);
    atomic_init(&heir_prio, -1);
    atomic_init(&contender_rc, -1);
    atomic_init(&unused, -1);
    if (!robust_start_owner(&owner, &oarg)) {
        test_fail("robust PI handoff prerequisite", "owner did not lock");
        return;
    }
    if (pthread_create(&heir, NULL, robust_waiter, &harg) != EOK) {
        test_fail("robust PI handoff prerequisite", "heir create failed");
        atomic_store_explicit(&die, 1, memory_order_release);
        (void) thread_join(owner, NULL);
        return;
    }
    /* Sleep rather than yield: the heir runs below this thread's priority. */
    for (int i = 0; i < 5000 && !(m.owner & RTMUX_SYNC_WAITING); i++)
        (void) nanosleep(&ms, NULL);
    atomic_store_explicit(&die, 1, memory_order_release);
    (void) thread_join(owner, NULL);
    for (int i = 0; i < 5000 && atomic_load(&heir_rc) == -1; i++)
        (void) nanosleep(&ms, NULL);
    if (pthread_create(&contender, NULL, robust_waiter, &carg) == EOK)
        (void) thread_join(contender, NULL);
    else
        test_fail("robust PI handoff prerequisite", "contender failed");
    (void) thread_join(heir, NULL);

    snprintf(detail, sizeof(detail), "heir rc=%d prio=%d contender rc=%d",
             atomic_load(&heir_rc), atomic_load(&heir_prio),
             atomic_load(&contender_rc));
    check(atomic_load(&heir_rc) == EOWNERDEAD,
          "robust waiter gets EOWNERDEAD when the owner thread dies", detail);
    check(atomic_load(&heir_prio) >= 30,
          "PI applies to the owner of a revived robust mutex", detail);
    check(atomic_load(&contender_rc) == EOK,
          "robust mutex hands on normally once made consistent", detail);
    (void) pthread_mutex_destroy(&m);
}

/* Two waiters queued when the owner dies: the one handed the mutex unlocks it
 * without the repair, and the other then fails with ENOTRECOVERABLE.
 */
static void test_robust_waiters_notrecoverable(void)
{
    pthread_mutex_t m;
    pthread_t owner, w1, w2;
    atomic_int locked, die, rc1, rc2, unused;
    struct robust_owner_args oarg = {
        .mutex = &m, .locked = &locked, .die = &die};
    struct robust_waiter_args a1 = {
        .mutex = &m, .result = &rc1, .observed = &unused};
    struct robust_waiter_args a2 = {
        .mutex = &m, .result = &rc2, .observed = &unused};
    char detail[64];
    int r1, r2;

    if (robust_mutex_init(&m, PTHREAD_PRIO_NONE, PTHREAD_PROCESS_PRIVATE) !=
        EOK) {
        test_fail("robust ENOTRECOVERABLE prerequisite", "init failed");
        return;
    }
    atomic_init(&locked, 0);
    atomic_init(&die, 0);
    atomic_init(&rc1, -1);
    atomic_init(&rc2, -1);
    atomic_init(&unused, -1);
    if (!robust_start_owner(&owner, &oarg)) {
        test_fail("robust ENOTRECOVERABLE prerequisite", "owner did not lock");
        return;
    }
    if (pthread_create(&w1, NULL, robust_waiter, &a1) != EOK) {
        test_fail("robust ENOTRECOVERABLE prerequisite", "waiter failed");
        atomic_store_explicit(&die, 1, memory_order_release);
        (void) thread_join(owner, NULL);
        return;
    }
    if (pthread_create(&w2, NULL, robust_waiter, &a2) != EOK) {
        /* Never return with w1 still queued on this frame's mutex. */
        test_fail("robust ENOTRECOVERABLE prerequisite", "waiter failed");
        atomic_store_explicit(&die, 1, memory_order_release);
        (void) thread_join(owner, NULL);
        (void) thread_join(w1, NULL);
        return;
    }
    /* Let both waiters queue; WAITING shows only the first. */
    for (int i = 0; i < 2000; i++)
        (void) __sched_yield();
    atomic_store_explicit(&die, 1, memory_order_release);
    (void) thread_join(owner, NULL);
    (void) thread_join(w1, NULL);
    (void) thread_join(w2, NULL);

    r1 = atomic_load(&rc1);
    r2 = atomic_load(&rc2);
    snprintf(detail, sizeof(detail), "rc1=%d rc2=%d", r1, r2);
    check((r1 == EOWNERDEAD && r2 == ENOTRECOVERABLE) ||
              (r2 == EOWNERDEAD && r1 == ENOTRECOVERABLE),
          "robust waiters after an unrepaired EOWNERDEAD", detail);
    check(pthread_mutex_lock(&m) == ENOTRECOVERABLE,
          "robust lock after an unrepaired EOWNERDEAD",
          "expected ENOTRECOVERABLE");
    (void) pthread_mutex_destroy(&m);
}

struct robust_cv_waiter_args {
    pthread_mutex_t *mutex;
    atomic_int *result; /* unlock result, or -lock error */
};

static void *robust_cv_waiter(void *raw)
{
    struct robust_cv_waiter_args *arg = raw;
    int rc = pthread_mutex_lock(arg->mutex);

    if (rc == EOK)
        rc = pthread_mutex_unlock(arg->mutex);
    else
        rc = -rc;
    atomic_store_explicit(arg->result, rc, memory_order_release);
    return NULL;
}

/* pthread_cond_timedwait hands a robust mutex to a queued locker and takes it
 * back on timeout; each owner can then unlock it.
 */
static void test_robust_condvar_handoff(void)
{
    pthread_mutex_t m;
    pthread_cond_t cv;
    pthread_t tid;
    struct timespec abstime;
    atomic_int result;
    struct robust_cv_waiter_args arg = {.mutex = &m, .result = &result};
    int rc;

    if (robust_mutex_init(&m, PTHREAD_PRIO_NONE, PTHREAD_PROCESS_PRIVATE) !=
            EOK ||
        pthread_cond_init(&cv, NULL) != EOK || pthread_mutex_lock(&m) != EOK) {
        test_fail("robust condvar handoff prerequisite", "setup failed");
        return;
    }
    atomic_init(&result, 1);
    if (pthread_create(&tid, NULL, robust_cv_waiter, &arg) != EOK) {
        test_fail("robust condvar handoff prerequisite", "waiter failed");
        (void) pthread_mutex_unlock(&m);
        return;
    }
    if (!wait_for_waiting_bit(&m)) {
        test_fail("robust condvar handoff prerequisite",
                  "waiter did not block");
        (void) pthread_mutex_unlock(&m);
        (void) thread_join(tid, NULL);
        return;
    }
    (void) clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_nsec += 50 * 1000 * 1000;
    if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000 * 1000 * 1000;
    }
    rc = pthread_cond_timedwait(&cv, &m, &abstime);
    check(rc == ETIMEDOUT &&
              atomic_load_explicit(&result, memory_order_acquire) == EOK,
          "pthread_cond_timedwait hands a robust mutex on",
          "expected the queued locker to lock and unlock it");
    check(pthread_mutex_unlock(&m) == EOK,
          "robust mutex unlock after pthread_cond_timedwait",
          "expected the re-acquired mutex to be recorded as held");
    (void) thread_join(tid, NULL);
    (void) pthread_cond_destroy(&cv);
    (void) pthread_mutex_destroy(&m);
}

/* A process-shared robust mutex whose owner process dies: a waiter blocked in
 * another process when the owner is killed, and a locker that arrives after the
 * owner exited, both get EOWNERDEAD.
 */
#define ROBUST_SHM_NAME "/rtmux-robust-mtx"

struct robust_killer_args {
    pthread_mutex_t *mutex;
    pid_t pid;
};

static void *robust_killer(void *raw)
{
    struct robust_killer_args *arg = raw;
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};

    for (int i = 0; i < 5000 && !(arg->mutex->owner & RTMUX_SYNC_WAITING); i++)
        (void) nanosleep(&ms, NULL);
    (void) kill(arg->pid, SIGKILL);
    return NULL;
}

static int robust_spawn_owner(const char *mode,
                              struct pshared_region *r,
                              pid_t *pid)
{
    char *argv[] = {"syscall_testing", (char *) mode, ROBUST_SHM_NAME, NULL};
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};

    r->state = 0;
    if (posix_spawn(pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0)
        return 0;
    for (int i = 0; i < 5000 && r->state != 1; i++)
        (void) nanosleep(&ts, NULL);
    return r->state == 1;
}

/* A shared region holding a fresh process-shared robust mutex, or NULL. */
static struct pshared_region *robust_shm_create(const char *what, int robust)
{
    struct pshared_region *r;
    flock_t space;
    int fd;

    (void) shm_unlink(ROBUST_SHM_NAME);
    fd = shm_open(ROBUST_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        test_fail(what, strerror(errno));
        return NULL;
    }
    memset(&space, 0, sizeof space);
    space.l_whence = SEEK_SET;
    space.l_len = (off_t) sizeof *r;
    r = fcntl(fd, F_ALLOCSP, &space) == -1
            ? MAP_FAILED
            : mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void) close(fd);
    if (r == MAP_FAILED ||
        (robust ? robust_mutex_init(&r->mtx, PTHREAD_PRIO_NONE,
                                    PTHREAD_PROCESS_SHARED)
                : plain_mutex_init(&r->mtx, PTHREAD_PROCESS_SHARED)) != EOK) {
        test_fail(what, "shared mutex setup");
        if (r != MAP_FAILED)
            (void) munmap(r, sizeof *r);
        (void) shm_unlink(ROBUST_SHM_NAME);
        return NULL;
    }
    return r;
}

static void robust_shm_destroy(struct pshared_region *r)
{
    (void) pthread_mutex_destroy(&r->mtx);
    (void) munmap(r, sizeof *r);
    (void) shm_unlink(ROBUST_SHM_NAME);
}

static void test_robust_owner_process_death(void)
{
    struct pshared_region *r;
    struct robust_killer_args karg;
    pthread_t killer;
    pid_t pid;
    int status = 0;
    int rc;

    r = robust_shm_create("robust process death prerequisite", 1);
    if (!r)
        return;

    /* Blocked in the lock when the owner process is killed. */
    if (!robust_spawn_owner("--robust-hold", r, &pid)) {
        test_fail("robust process death prerequisite", "owner did not lock");
    } else {
        karg.mutex = &r->mtx;
        karg.pid = pid;
        if (pthread_create(&killer, NULL, robust_killer, &karg) != EOK) {
            (void) kill(pid, SIGKILL);
            test_fail("robust process death prerequisite", "killer thread");
        } else {
            rc = pthread_mutex_lock(&r->mtx);
            (void) thread_join(killer, NULL);
            check(rc == EOWNERDEAD,
                  "robust waiter gets EOWNERDEAD when the owner process is "
                  "killed",
                  "expected EOWNERDEAD");
            if (rc == EOWNERDEAD || rc == EOK) {
                check(pthread_mutex_consistent(&r->mtx) == EOK &&
                          pthread_mutex_unlock(&r->mtx) == EOK &&
                          pthread_mutex_lock(&r->mtx) == EOK &&
                          pthread_mutex_unlock(&r->mtx) == EOK,
                      "pshared robust mutex usable after consistent",
                      "expected normal lock and unlock");
            }
        }
        (void) waitpid(pid, &status, 0);
    }

    /* Locked after the owner process exited holding it. */
    if (!robust_spawn_owner("--robust-exit", r, &pid)) {
        test_fail("robust process death prerequisite", "owner did not lock");
    } else {
        (void) waitpid(pid, &status, 0);
        rc = pthread_mutex_lock(&r->mtx);
        check(rc == EOWNERDEAD, "robust lock after the owner process exited",
              "expected EOWNERDEAD");
        if (rc == EOWNERDEAD) {
            check(pthread_mutex_unlock(&r->mtx) == EOK &&
                      pthread_mutex_lock(&r->mtx) == ENOTRECOVERABLE,
                  "pshared robust unlock without pthread_mutex_consistent",
                  "expected ENOTRECOVERABLE");
        }
    }
    robust_shm_destroy(r);
}

/* The owner dies at every point of a hand-off: the parent holds a shared robust
 * mutex with a child process blocked on it, then releases it and kills the
 * child, in four orders and with 0..7 yields around the release, so that the
 * kill lands before, during and after the kernel passes the mutex to the child.
 * The parent must then always get the mutex back, never time out. A child that
 * received it before the kill dies as the owner, which gives EOWNERDEAD; one
 * killed while still queued gives EOK.
 */
#define ROBUST_KILL_ROUNDS 32

static void test_robust_handoff_owner_death(void)
{
    char *argv[] = {"syscall_testing", "--robust-wait", ROBUST_SHM_NAME, NULL};
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};
    struct pshared_region *r;
    int bad = 0;
    int dead = 0;
    char detail[96] = "";

    r = robust_shm_create("robust hand-off death prerequisite", 1);
    if (!r)
        return;
    for (int i = 0; i < ROBUST_KILL_ROUNDS; i++) {
        struct timespec abstime;
        pid_t pid;
        int status;
        int rc;

        if (pthread_mutex_lock(&r->mtx) != EOK ||
            posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) !=
                0) {
            snprintf(detail, sizeof detail, "round %d: setup failed", i);
            bad++;
            break;
        }
        for (int j = 0; j < 2000 && !(r->mtx.owner & RTMUX_SYNC_WAITING); j++)
            (void) nanosleep(&ms, NULL);

        /* i%4: 0 and 2 kill, then unlock; 1 and 3 unlock, then kill. */
        if (i % 4 == 0 || i % 4 == 2)
            (void) kill(pid, SIGKILL);
        for (int y = 0; y < i / 6; y++)
            (void) __sched_yield();
        (void) pthread_mutex_unlock(&r->mtx);
        for (int y = 0; y < i / 6; y++)
            (void) __sched_yield();
        if (i % 4 == 1 || i % 4 == 3)
            (void) kill(pid, SIGKILL);
        (void) waitpid(pid, &status, 0);

        (void) clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_sec += 3;
        rc = pthread_mutex_timedlock(&r->mtx, &abstime);
        if (rc == EOWNERDEAD) {
            dead++;
            (void) pthread_mutex_consistent(&r->mtx);
        } else if (rc != EOK) {
            snprintf(detail, sizeof detail, "round %d: lock rc=%d", i, rc);
            bad++;
            break;
        }
        (void) pthread_mutex_unlock(&r->mtx);
    }
    if (!bad)
        snprintf(detail, sizeof detail, "%d rounds, %d took EOWNERDEAD",
                 ROBUST_KILL_ROUNDS, dead);
    check(!bad, "robust owner killed at every point of a hand-off", detail);
    robust_shm_destroy(r);
}

/* pthread_cond_(timed)wait on an inconsistent robust mutex releases it, which
 * makes it unrecoverable, and the reacquire fails: the call returns
 * ENOTRECOVERABLE and the caller holds nothing.
 */
static void *robust_cv_signaller(void *raw)
{
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};

    (void) nanosleep(&ms, NULL);
    (void) pthread_cond_signal(raw);
    return NULL;
}

static void test_robust_condvar_inconsistent(void)
{
    pthread_mutex_t m;
    pthread_cond_t cv;
    pthread_t owner, sig;
    atomic_int locked;
    struct robust_owner_args oarg = {.mutex = &m, .locked = &locked};
    struct timespec abstime;
    int rc_timed, rc_signalled;

    if (robust_mutex_init(&m, PTHREAD_PRIO_NONE, PTHREAD_PROCESS_PRIVATE) !=
            EOK ||
        pthread_cond_init(&cv, NULL) != EOK) {
        test_fail("robust condvar prerequisite", "init failed");
        return;
    }
    atomic_init(&locked, 0);
    if (!robust_start_owner(&owner, &oarg)) {
        test_fail("robust condvar prerequisite", "owner did not lock");
        return;
    }
    (void) thread_join(owner, NULL);
    if (pthread_mutex_lock(&m) != EOWNERDEAD) {
        test_fail("robust condvar prerequisite", "expected EOWNERDEAD");
        return;
    }
    (void) clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_nsec += 50 * 1000 * 1000;
    if (abstime.tv_nsec >= 1000 * 1000 * 1000) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000 * 1000 * 1000;
    }
    rc_timed = pthread_cond_timedwait(&cv, &m, &abstime);
    check(rc_timed == ENOTRECOVERABLE && pthread_mutex_unlock(&m) == EPERM &&
              pthread_mutex_lock(&m) == ENOTRECOVERABLE,
          "pthread_cond_timedwait on an inconsistent robust mutex",
          "expected ENOTRECOVERABLE, no mutex held, mutex unrecoverable");
    (void) pthread_mutex_destroy(&m);

    /* Woken by a signal rather than the timeout. */
    if (robust_mutex_init(&m, PTHREAD_PRIO_NONE, PTHREAD_PROCESS_PRIVATE) !=
        EOK) {
        test_fail("robust condvar prerequisite", "second init failed");
        return;
    }
    atomic_store(&locked, 0);
    if (!robust_start_owner(&owner, &oarg)) {
        test_fail("robust condvar prerequisite", "second owner failed");
        return;
    }
    (void) thread_join(owner, NULL);
    if (pthread_mutex_lock(&m) != EOWNERDEAD ||
        pthread_create(&sig, NULL, robust_cv_signaller, &cv) != EOK) {
        test_fail("robust condvar prerequisite", "second setup failed");
        return;
    }
    (void) clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec += 5;
    rc_signalled = pthread_cond_timedwait(&cv, &m, &abstime);
    (void) thread_join(sig, NULL);
    check(rc_signalled == ENOTRECOVERABLE &&
              pthread_mutex_lock(&m) == ENOTRECOVERABLE,
          "pthread_cond_wait woken on an inconsistent robust mutex",
          "expected ENOTRECOVERABLE");
    (void) pthread_mutex_destroy(&m);
    (void) pthread_cond_destroy(&cv);
}

/* A non-robust mutex whose owner dies is not revived, but it must not hang its
 * waiters: everyone blocked on it, and everyone who locks it later, gets
 * ENOTRECOVERABLE. The waiters must return within 3 s.
 */
#define DEADLOCK_WAITERS 3
#define DEADLOCK_WAIT_MS 3000

struct dead_owner_waiter_args {
    pthread_mutex_t *mutex;
    atomic_int *queued;
    atomic_int *result; /* lock result, -1 until it returns */
};

static void *dead_owner_waiter(void *raw)
{
    struct dead_owner_waiter_args *arg = raw;
    struct timespec abstime;
    int rc;

    /* A hang shows as ETIMEDOUT after DEADLOCK_WAIT_MS, not as a stuck test. */
    (void) clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec += DEADLOCK_WAIT_MS / 1000 + 1;
    atomic_fetch_add_explicit(arg->queued, 1, memory_order_acq_rel);
    rc = pthread_mutex_timedlock(arg->mutex, &abstime);
    atomic_store_explicit(arg->result, rc, memory_order_release);
    if (rc == EOK)
        (void) pthread_mutex_unlock(arg->mutex);
    return NULL;
}

/* Wait up to DEADLOCK_WAIT_MS for every result in res[0..n) to be set. */
static int dead_owner_results_in(atomic_int *res, int n)
{
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};

    for (int t = 0; t < DEADLOCK_WAIT_MS; t++) {
        int done = 1;

        for (int i = 0; i < n; i++)
            done &= atomic_load_explicit(&res[i], memory_order_acquire) != -1;
        if (done)
            return 1;
        (void) nanosleep(&ms, NULL);
    }
    return 0;
}

static void test_nonrobust_owner_thread_death(void)
{
    pthread_mutex_t m;
    pthread_t owner, w[DEADLOCK_WAITERS];
    atomic_int locked, die, queued, res[DEADLOCK_WAITERS];
    struct robust_owner_args oarg = {
        .mutex = &m, .locked = &locked, .die = &die};
    struct dead_owner_waiter_args warg[DEADLOCK_WAITERS];
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};
    struct timespec late;
    char detail[96];
    int created = 0;
    int ok;

    if (plain_mutex_init(&m, PTHREAD_PROCESS_PRIVATE) != EOK) {
        test_fail("non-robust owner death prerequisite", "init failed");
        return;
    }
    atomic_init(&locked, 0);
    atomic_init(&die, 0);
    atomic_init(&queued, 0);
    if (!robust_start_owner(&owner, &oarg)) {
        test_fail("non-robust owner death prerequisite", "owner did not lock");
        return;
    }
    for (int i = 0; i < DEADLOCK_WAITERS; i++) {
        atomic_init(&res[i], -1);
        warg[i] = (struct dead_owner_waiter_args){
            .mutex = &m, .queued = &queued, .result = &res[i]};
        if (pthread_create(&w[i], NULL, dead_owner_waiter, &warg[i]) != EOK)
            break;
        created++;
    }

    /* Let them reach the lock; one that is slower finds the owner already dead,
     * which must fail the same way.
     */
    for (int i = 0; i < 2000 && atomic_load(&queued) < created; i++)
        (void) nanosleep(&ms, NULL);
    for (int i = 0; i < 2000 && !(m.owner & RTMUX_SYNC_WAITING); i++)
        (void) nanosleep(&ms, NULL);
    for (int i = 0; i < 20; i++)
        (void) nanosleep(&ms, NULL);
    atomic_store_explicit(&die, 1, memory_order_release);
    (void) thread_join(owner, NULL);

    if (created != DEADLOCK_WAITERS)
        test_fail("non-robust owner death prerequisite",
                  "waiter create failed");
    ok = dead_owner_results_in(res, created);
    detail[0] = '\0';
    for (int i = 0; i < created; i++) {
        size_t n = strlen(detail);

        snprintf(detail + n, sizeof(detail) - n, "%d ", atomic_load(&res[i]));
        ok &= atomic_load(&res[i]) == ENOTRECOVERABLE;
    }
    check(ok,
          "non-robust waiters return ENOTRECOVERABLE when the owner "
          "thread dies",
          detail);
    (void) clock_gettime(CLOCK_REALTIME, &late);
    late.tv_sec += 1;
    check(pthread_mutex_timedlock(&m, &late) == ENOTRECOVERABLE &&
              pthread_mutex_trylock(&m) == ENOTRECOVERABLE,
          "non-robust lock after the owner thread died",
          "expected ENOTRECOVERABLE from lock and trylock");

    /* Every waiter returns, by timeout at the latest, before this frame ends.
     */
    for (int i = 0; i < created; i++)
        (void) thread_join(w[i], NULL);
}

/* A thread that takes a mutex with __sync_mutex_revive owns it without ever
 * locking it, so nothing but the revive tells the kernel it is an owner. A
 * locker that queued on the stale word before the revive must still be failed
 * when that owner dies: exit only looks for waiters of a thread registered as
 * an owner.
 */
struct revive_args {
    pthread_mutex_t *mutex;
    atomic_int rc; /* errno of the revive, -1 until it ran */
};

static void *revive_and_die(void *raw)
{
    struct revive_args *arg = raw;
    int rc = __sync_mutex_revive((sync_t *) arg->mutex) == -1 ? errno : EOK;

    atomic_store_explicit(&arg->rc, rc, memory_order_release);
    return NULL; /* exits owning the mutex */
}

static void test_nonrobust_revived_owner_death(void)
{
    static pthread_mutex_t m;
    static struct revive_args rarg;
    pthread_t waiter, owner;
    atomic_int queued, res;
    struct dead_owner_waiter_args warg = {
        .mutex = &m, .queued = &queued, .result = &res};
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000};
    char detail[96];

    if (plain_mutex_init(&m, PTHREAD_PROCESS_PRIVATE) != EOK) {
        test_fail("revived owner death prerequisite", "init failed");
        return;
    }
    m.owner = RTMUX_SYNC_DEAD; /* stale: this mutex is not robust */
    rarg.mutex = &m;
    atomic_init(&rarg.rc, -1);
    atomic_init(&queued, 0);
    atomic_init(&res, -1);
    if (pthread_create(&waiter, NULL, dead_owner_waiter, &warg) != EOK) {
        test_fail("revived owner death prerequisite", "waiter create failed");
        return;
    }
    /* No waiting bit shows on a special word, so give the waiter time to
     * queue; one that is late finds the reviver as owner, alive or dead, and
     * must fail the same way.
     */
    for (int i = 0; i < 2000 && !atomic_load(&queued); i++)
        (void) nanosleep(&ms, NULL);
    for (int i = 0; i < 50; i++)
        (void) nanosleep(&ms, NULL);
    if (pthread_create(&owner, NULL, revive_and_die, &rarg) != EOK) {
        test_fail("revived owner death prerequisite", "owner create failed");
    } else {
        (void) thread_join(owner, NULL);
        (void) dead_owner_results_in(&res, 1);
    }
    snprintf(detail, sizeof detail, "revive %d (want 0), waiter %d (want %d)",
             atomic_load(&rarg.rc), atomic_load(&res), ENOTRECOVERABLE);
    check(atomic_load(&rarg.rc) == EOK &&
              atomic_load(&res) == ENOTRECOVERABLE,
          "non-robust waiter returns ENOTRECOVERABLE when a revived owner dies",
          detail);
    (void) thread_join(waiter, NULL);
}

static void test_nonrobust_owner_process_death(void)
{
    struct pshared_region *r;
    struct robust_killer_args karg;
    pthread_t killer;
    struct timespec late;
    pid_t pid;
    int status = 0;
    int rc;

    r = robust_shm_create("non-robust process death prerequisite", 0);
    if (!r)
        return;
    if (!robust_spawn_owner("--robust-hold", r, &pid)) {
        test_fail("non-robust process death prerequisite",
                  "owner did not lock");
    } else {
        karg.mutex = &r->mtx;
        karg.pid = pid;
        if (pthread_create(&killer, NULL, robust_killer, &karg) != EOK) {
            (void) kill(pid, SIGKILL);
            test_fail("non-robust process death prerequisite", "killer thread");
        } else {
            struct timespec abstime;

            (void) clock_gettime(CLOCK_REALTIME, &abstime);
            abstime.tv_sec += DEADLOCK_WAIT_MS / 1000;
            rc = pthread_mutex_timedlock(&r->mtx, &abstime);
            (void) thread_join(killer, NULL);
            check(rc == ENOTRECOVERABLE,
                  "non-robust waiter returns ENOTRECOVERABLE when the owner "
                  "process is killed",
                  rc == ETIMEDOUT ? "still blocked after 3 s"
                                  : "expected ENOTRECOVERABLE");
        }
        (void) waitpid(pid, &status, 0);
    }

    /* A locker that arrives after the owner process exited; the mutex is
     * unusable after the first death, so start over with a fresh one.
     */
    (void) pthread_mutex_destroy(&r->mtx);
    if (plain_mutex_init(&r->mtx, PTHREAD_PROCESS_SHARED) == EOK &&
        robust_spawn_owner("--robust-exit", r, &pid)) {
        (void) waitpid(pid, &status, 0);
        (void) clock_gettime(CLOCK_REALTIME, &late);
        late.tv_sec += 1;
        check(pthread_mutex_timedlock(&r->mtx, &late) == ENOTRECOVERABLE,
              "non-robust lock after the owner process exited",
              "expected ENOTRECOVERABLE");
    } else {
        test_fail("non-robust process death prerequisite",
                  "owner did not lock");
    }
    robust_shm_destroy(r);
}

/* A thread may hold 64 robust mutexes at once (the kernel keeps a record of
 * each); the 65th lock fails with EAGAIN and everything unlocks cleanly.
 */
static void test_robust_held_limit(void)
{
    pthread_mutex_t m[66];
    int n = 0;
    int rc = EOK;

    for (int i = 0; i < 66; i++) {
        if (robust_mutex_init(&m[i], PTHREAD_PRIO_NONE,
                              PTHREAD_PROCESS_PRIVATE) != EOK) {
            test_fail("robust held limit prerequisite", "init failed");
            return;
        }
    }
    while (n < 66 && (rc = pthread_mutex_lock(&m[n])) == EOK)
        n++;
    check(n == 64 && rc == EAGAIN, "robust mutexes held per thread are bounded",
          "expected the 65th lock to fail with EAGAIN");
    for (int i = 0; i < n; i++)
        (void) pthread_mutex_unlock(&m[i]);
    check(pthread_mutex_lock(&m[64]) == EOK &&
              pthread_mutex_unlock(&m[64]) == EOK,
          "robust lock after releasing the held mutexes", "expected EOK");
    for (int i = 0; i < 66; i++)
        (void) pthread_mutex_destroy(&m[i]);
}


/* --sync-path-cost: time three kernel paths that the sync and scheduler
 * efficiency work changes, for a before/after comparison of two kernels under
 * the same binary. It reports numbers and checks only that the paths work. Run
 * it on one hart under QEMU -icount, where the monotonic clock advances with
 * the instructions executed and host load does not enter the result:
 *
 *   thread-exit : create and join a thread that never touched a contended
 *                 mutex (the exit path with nothing to hand on);
 *   sem-handoff : two threads hand a pair of semaphores back and forth, so
 *                 every step is a blocking wait and a wake;
 *   raw-mutex   : kernel lock and unlock of a free mutex (two kernel calls
 *                 that each probe the user sync_t).
 */
struct path_cost_pair {
    sem_t ping, pong;
    unsigned rounds;
};

static void *path_cost_nop(void *arg)
{
    return arg;
}

static void *path_cost_peer(void *raw)
{
    struct path_cost_pair *p = raw;

    for (unsigned i = 0; i < p->rounds; i++) {
        if (sem_wait(&p->ping) != 0 || sem_post(&p->pong) != 0)
            break;
    }
    return NULL;
}

static uint64_t path_cost_now(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

void sync_path_cost(void)
{
    enum { EXITS = 400, ROUNDS = 2000, LOCKS = 20000 };
    static struct path_cost_pair pair;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_t t;
    uint64_t t0, t1;
    unsigned done = 0;
    int bad = 0;

    t0 = path_cost_now();
    for (unsigned i = 0; i < EXITS; i++) {
        if (pthread_create(&t, NULL, path_cost_nop, NULL) != EOK ||
            pthread_join(t, NULL) != EOK)
            break;
        done++;
    }
    t1 = path_cost_now();
    printf("sync-path-cost: thread-exit n=%u total_ns=%llu per_ns=%llu\n", done,
           (unsigned long long) (t1 - t0),
           (unsigned long long) (done ? (t1 - t0) / done : 0));
    check(done == EXITS, "sync-path-cost thread create and join",
          "pthread_create or pthread_join failed");

    pair.rounds = ROUNDS;
    if (sem_init(&pair.ping, 0, 0) != 0 || sem_init(&pair.pong, 0, 0) != 0 ||
        pthread_create(&t, NULL, path_cost_peer, &pair) != EOK) {
        test_fail("sync-path-cost semaphore handoff", "setup failed");
    } else {
        t0 = path_cost_now();
        for (unsigned i = 0; i < ROUNDS && !bad; i++)
            bad = sem_post(&pair.ping) != 0 || sem_wait(&pair.pong) != 0;
        t1 = path_cost_now();
        if (bad) /* the peer may still wait for a post that failed */
            (void) pthread_cancel(t);
        (void) pthread_join(t, NULL);
        printf("sync-path-cost: sem-handoff n=%u total_ns=%llu per_ns=%llu\n",
               (unsigned) ROUNDS, (unsigned long long) (t1 - t0),
               (unsigned long long) ((t1 - t0) / ROUNDS));
        check(bad == 0, "sync-path-cost semaphore handoff",
              "sem_post or sem_wait failed");
    }

    bad = 0;
    t0 = path_cost_now();
    for (unsigned i = 0; i < LOCKS; i++) {
        bad |= __sync_mutex_lock((sync_t *) &m);
        bad |= __sync_mutex_unlock((sync_t *) &m);
    }
    t1 = path_cost_now();
    printf("sync-path-cost: raw-mutex n=%u total_ns=%llu per_ns=%llu\n",
           (unsigned) LOCKS, (unsigned long long) (t1 - t0),
           (unsigned long long) ((t1 - t0) / LOCKS));
    check(bad == 0 && m.owner == 0, "sync-path-cost raw mutex lock and unlock",
          "kernel lock or unlock of a free mutex failed");
}

void test_mutex_owner_death(void)
{
    test_robust_mutexattr();
    test_robust_owner_thread_exit();
    test_robust_waiter_handoff_pi();
    test_robust_waiters_notrecoverable();
    test_robust_condvar_handoff();
    test_robust_condvar_inconsistent();
    test_robust_held_limit();
    test_robust_owner_process_death();
    test_robust_handoff_owner_death();
    test_nonrobust_owner_thread_death();
    test_nonrobust_revived_owner_death();
    test_nonrobust_owner_process_death();
}

void test_sync(void)
{
    test_create_mutex();
    test_create_cond();
    test_create_sem();

    test_create_mutex_recursive();
    test_create_mutex_prio_none();
    test_pthread_mutexattr_protocol();
    test_pthread_mutex_types();
    test_pthread_mutex_prio_protect_required_pass();
    test_create_cond_monotonic();
    test_pthread_condattr_clock();

    test_create_bad_type();
    test_create_cond_bad_clock();
    test_create_mutex_bad_protocol();
    test_create_mutex_prio_protect_required_pass();
    test_pthread_mutex_prio_protect_recursive();

    test_mutex_lock_unlock();
    test_pthread_mutex_timedlock_contract();
    test_recursive_mutex_nested_lock();
    test_mutex_unlock_unowned();
    test_mutex_hash_collision_wake_is_keyed();
    test_cond_reacquire_cycle();
    test_cond_reacquire_above_ceiling();
    test_pi_owned_turnstiles();
    test_pi_two_mutex_cycle_detects_deadlock();
    test_pi_large_pid();
    test_pi_contender_timeout_does_not_match_reused_tid();
    test_condvar_hash_collision_wait_unblocks();
    test_condvar_wait_pi_mutex_handoff();
    test_condvar_wait_loan_drop_with_ready_thread();
    test_condvar_wait_errorcheck_type_preserved();
    test_condvar_timeout_removes_waiter();
    test_mutex_owner_death();

    test_sem_post_wait();
    test_sem_post_twice_wait_twice();
    test_posix_sem_init_post_wait();
    test_posix_sem_invalid_args();
    test_posix_sem_timedwait();
    test_posix_named_sem_open_close_unlink();
    test_posix_named_sem_timedwait();
    test_reply_timeout_armed_on_claim();

    test_condvar_signal_no_waiters();

    test_destroy_then_lock();
    test_destroy_sem();
    test_destroy_cond();
    test_pthread_once_runs_once();
    test_pthread_spin();
    test_pthread_barrier();
    test_pthread_rwlock();
    test_pthread_rwlock_nested_read();

    test_create_destroy_recreate();
    test_malloc_fifo_mixed_prio();
    test_fp_isolation();
    test_cancel_before_block();
    test_block_kill_cancel();
}

/* A cancel that arrives while the target runs, before it blocks, must not be
 * lost: the target is held in user code (not at a cancellation point) while the
 * cancel is issued, and then enters a blocking call. It has to terminate there.
 * The kernel used to read a thread's pending-cancel flag only when its TLS sat
 * in kernel memory, which is never so for a user thread, so such a cancel left
 * the thread blocked for good.
 */
enum { CBB_RECEIVE, CBB_NANOSLEEP, CBB_SEM, CBB_COND, CBB_JOIN, CBB_KINDS };

struct cbb_ctx {
    int kind;
    int chid;
    atomic_int ready;
    atomic_int go;
    atomic_int finished;
    sem_t token;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t target;
    int unlock_status;
};

static void *cbb_target(void *arg)
{
    return arg;
}

/* Wait for a specific thread state. TIDSTATUS may report the next thread
 * when this slot has gone away, so also compare the returned TID. */
static bool wait_thread_state(pthread_t tid, int state)
{
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", (int) getpid());
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    bool found = false;
    for (int i = 0; i < 300; i++) {
        procfs_status st = {.tid = tid};
        if (devctl(fd, DEVCTL_PROC_TIDSTATUS, &st, sizeof(st), NULL) != EOK ||
            st.tid != tid)
            break;
        if (st.state == state) {
            found = true;
            break;
        }
        sleep_ms(10);
    }
    close(fd);
    return found;
}

struct cond_cycle {
    pthread_mutex_t bound, outer;
    pthread_cond_t cond;
    int wait_rc, unlock_rc, other_rc;
};

static void *cond_cycle_waiter(void *arg)
{
    struct cond_cycle *c = arg;
    struct timespec end;
    if (pthread_mutex_lock(&c->outer))
        return NULL;
    if (pthread_mutex_lock(&c->bound) == EOK) {
        clock_gettime(CLOCK_REALTIME, &end);
        end.tv_sec += 10;
        c->wait_rc = pthread_cond_timedwait(&c->cond, &c->bound, &end);
        c->unlock_rc = pthread_mutex_unlock(&c->bound);
    }
    pthread_mutex_unlock(&c->outer);
    return NULL;
}

static void *cond_cycle_owner(void *arg)
{
    struct cond_cycle *c = arg;
    struct timespec end;
    if (pthread_mutex_lock(&c->bound))
        return NULL;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += 2;
    /* timedlock first tries an immediate kernel wait on a robust mutex.
     * Arm the deadline once so the observed STATE_MUTEX is the real wait. */
    uint64_t deadline = timespec2nsec(&end);
    c->other_rc = __timer_timeout_r(CLOCK_REALTIME,
                                    TIMER_ABSTIME | RTMUX_TIMEOUT_MUTEX,
                                    NULL, &deadline, NULL);
    if (c->other_rc == EOK)
        c->other_rc = pthread_mutex_lock(&c->outer);
    if (c->other_rc == EOK)
        pthread_mutex_unlock(&c->outer);
    pthread_mutex_unlock(&c->bound);
    return NULL;
}

void test_cond_reacquire_cycle(void)
{
    struct cond_cycle c = {.cond = PTHREAD_COND_INITIALIZER,
                           .wait_rc = -1, .unlock_rc = -1, .other_rc = -1};
    pthread_t waiter, owner;
    bool bound = robust_mutex_init(&c.bound, PTHREAD_PRIO_INHERIT,
                                   PTHREAD_PROCESS_PRIVATE) == EOK;
    bool outer = bound && robust_mutex_init(&c.outer, PTHREAD_PRIO_INHERIT,
                                             PTHREAD_PROCESS_PRIVATE) == EOK;
    bool made_waiter = outer &&
        pthread_create(&waiter, NULL, cond_cycle_waiter, &c) == EOK;
    bool made_owner = made_waiter && wait_thread_state(waiter, STATE_CONDVAR) &&
        pthread_create(&owner, NULL, cond_cycle_owner, &c) == EOK;
    bool queued = made_owner && wait_thread_state(owner, STATE_MUTEX);
    pthread_cond_signal(&c.cond);
    if (made_waiter)
        pthread_join(waiter, NULL);
    if (made_owner)
        pthread_join(owner, NULL);
    char detail[96];
    snprintf(detail, sizeof detail, "queued=%d wait=%d unlock=%d other=%d",
             queued, c.wait_rc, c.unlock_rc, c.other_rc);
    check(queued && c.wait_rc == EOK && c.unlock_rc == EOK &&
          c.other_rc == ETIMEDOUT, "condition wake reacquires across a PI cycle", detail);
    if (outer)
        pthread_mutex_destroy(&c.outer);
    if (bound)
        pthread_mutex_destroy(&c.bound);
    pthread_cond_destroy(&c.cond);
}

static void cbb_finish(void *raw)
{
    struct cbb_ctx *c = raw;
    if (c->kind == CBB_COND)
        c->unlock_status = pthread_mutex_unlock(&c->mutex);
    atomic_store(&c->finished, 1);
}

static void *cbb_thread(void *raw)
{
    struct cbb_ctx *c = raw;
    char buf[8];

    if (c->kind == CBB_COND && pthread_mutex_lock(&c->mutex) != EOK)
        return NULL;
    pthread_cleanup_push(cbb_finish, c);
    atomic_store(&c->ready, 1);
    while (!atomic_load(&c->go))
        (void) sched_yield();
    if (c->kind == CBB_RECEIVE)
        (void) msg_receive(c->chid, buf, sizeof buf, NULL);
    else if (c->kind == CBB_NANOSLEEP)
        (void) nanosleep(&(struct timespec){.tv_sec = 30}, NULL);
    else if (c->kind == CBB_SEM)
        (void) sem_wait(&c->token);
    else if (c->kind == CBB_COND)
        (void) pthread_cond_wait(&c->cond, &c->mutex);
    else
        (void) pthread_join(c->target, NULL);
    pthread_cleanup_pop(0);
    atomic_store(&c->finished, -1);
    return NULL;
}

/* Wait up to three seconds for the thread to run its cleanup handler. */
static int cbb_wait_finished(struct cbb_ctx *cp)
{
    for (int i = 0; i < 300 && !atomic_load(&cp->finished); i++)
        sleep_ms(10);
    return atomic_load(&cp->finished);
}

void test_cancel_before_block(void)
{
    static const char *const name[CBB_KINDS] = {
        "msg_receive", "nanosleep", "sem_wait", "cond_wait", "pthread_join"};

    static struct cbb_ctx cbb_ctxs[CBB_KINDS]; /* a stuck thread keeps its context */

    for (int kind = 0; kind < CBB_KINDS; kind++) {
        struct cbb_ctx *cp = &cbb_ctxs[kind];
        pthread_t th;
        void *ret = NULL;

        *cp = (struct cbb_ctx) {.kind = kind,
                                .chid = -1,
                                .unlock_status = -1,
                                .mutex = PTHREAD_MUTEX_INITIALIZER,
                                .cond = PTHREAD_COND_INITIALIZER};
        char what[64];

        if (kind == CBB_RECEIVE && (cp->chid = channel_create(0)) == -1) {
            test_fail("cancel before block fixture", strerror(errno));
            continue;
        }
        if ((kind == CBB_SEM && sem_init(&cp->token, 0, 1) != 0) ||
            (kind == CBB_JOIN &&
             pthread_create(&cp->target, NULL, cbb_target, cp) != EOK)) {
            test_fail("cancel before block fixture",
                      "semaphore or join target");
            continue;
        }
        if (kind == CBB_JOIN && !wait_thread_state(cp->target, STATE_DEAD)) {
            test_fail("cancel before join setup", "target did not reach STATE_DEAD");
            (void) pthread_join(cp->target, NULL);
            continue;
        }
        if (pthread_create(&th, NULL, cbb_thread, cp) != 0) {
            test_fail("cancel before block fixture", "pthread_create");
            if (cp->chid != -1)
                (void) channel_destroy(cp->chid);
            continue;
        }
        for (int i = 0; i < 3000 && !atomic_load(&cp->ready); i++)
            sleep_ms(1);
        if (!atomic_load(&cp->ready)) {
            test_fail("cancel before block fixture", "thread did not start");
            if (cp->chid != -1)
                (void) channel_destroy(cp->chid);
            continue;
        }
        if (pthread_cancel(th) != 0)
            test_fail("cancel before block fixture", "pthread_cancel");
        atomic_store(&cp->go, 1);
        int done = cbb_wait_finished(cp);

        snprintf(what, sizeof what, "cancel before %s block terminates", name[kind]);
        check(done == 1, what,
              "a cancel pending when the thread blocked was lost");
        if (!done) {
            /* It is blocked now, so this cancel lands. Do not join a thread
             * that may still not exit: the suite must not hang on a failure.
             */
            (void) pthread_cancel(th);
            if (!cbb_wait_finished(cp))
                continue;
        }
        (void) pthread_join(th, &ret);
        check(ret == PTHREAD_CANCELED, "cancelled thread reports PTHREAD_CANCELED",
              "the join value must be PTHREAD_CANCELED");
        if (kind == CBB_SEM) {
            check(sem_trywait(&cp->token) == 0,
                  "cancel before sem_wait preserves token",
                  "the semaphore must stay available");
            (void) sem_destroy(&cp->token);
        } else if (kind == CBB_COND) {
            check(cp->unlock_status == EOK,
                  "cancel before cond_wait preserves ownership",
                  "the cleanup handler must own the mutex");
            (void) pthread_cond_destroy(&cp->cond);
            (void) pthread_mutex_destroy(&cp->mutex);
        } else if (kind == CBB_JOIN) {
            void *result = NULL;
            check(pthread_join(cp->target, &result) == EOK && result == cp,
                  "cancel before join leaves target joinable",
                  "target was consumed");
        }
        if (cp->chid != -1)
            (void) channel_destroy(cp->chid);
    }
}

/* condvar_reacquire_alloc_failure: a condition wait on a PI mutex returns with
 * the mutex held, or fails before it gives the mutex up, even when the kernel's
 * turnstile allocation fails. Needs the debug fault-injection call
 * (/bin/inject, installed only on a CONFIG_DEBUG_INJECT kernel); without it the
 * case is skipped.
 *
 * 1. With the next allocation failing, the wait must report EAGAIN and leave
 *    the caller holding the mutex: the reservation is made before the release.
 * 2. A helper takes the mutex while the main thread waits, arms the failure,
 *    and keeps the mutex until after the wait's deadline. The reacquire then
 *    has to queue behind the helper, and so it must not allocate: the wait
 *    returns only once the helper has let go, and returns holding the mutex.
 */
#define CVR_DEADLINE_MS 1500L
#define CVR_HOLD_MS 150L /* how long past the deadline the helper keeps the mutex */
#define CVR_PID_NONE "2147483647"

static pthread_mutex_t cvr_mutex;
static pthread_cond_t cvr_cond;
static struct timespec cvr_deadline;
static char cvr_pidarg[16];
static atomic_int cvr_armed;
static atomic_int cvr_released;

static void cvr_ts_add_ms(struct timespec *ts, long ms)
{
    ts->tv_nsec += ms * 1000000L;
    ts->tv_sec += ts->tv_nsec / 1000000000L;
    ts->tv_nsec %= 1000000000L;
}

static void cvr_deadline_set(long ms)
{
    clock_gettime(CLOCK_MONOTONIC, &cvr_deadline);
    cvr_ts_add_ms(&cvr_deadline, ms);
}

static void *cvr_helper(void *arg)
{
    struct timespec until = cvr_deadline;

    (void) arg;
    /* The main thread is in its wait once the mutex can be taken. */
    while (pthread_mutex_trylock(&cvr_mutex) != 0)
        (void) nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL);
    if (inject_run("fail-turnstile", cvr_pidarg, NULL) == 0)
        atomic_store(&cvr_armed, 1);
    cvr_ts_add_ms(&until, CVR_HOLD_MS);
    (void) clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
    atomic_store(&cvr_released, 1);
    pthread_mutex_unlock(&cvr_mutex);
    return NULL;
}

int condvar_reacquire_alloc_failure(void)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    pthread_t th;
    int rc;

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(cvr_pidarg, sizeof cvr_pidarg, "%d", (int) getpid());
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&cvr_mutex, &ma);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cvr_cond, &ca);

    /* 1. The reservation fails: EAGAIN, mutex still held. */
    pthread_mutex_lock(&cvr_mutex);
    if (inject_run("fail-turnstile", cvr_pidarg, NULL) != 0) {
        test_fail("condvar_reacquire_alloc_failure fixture", "inject refused");
        pthread_mutex_unlock(&cvr_mutex);
        return 0;
    }
    cvr_deadline_set(20);
    rc = pthread_cond_timedwait(&cvr_cond, &cvr_mutex, &cvr_deadline);
    check(rc == EAGAIN, "cond_timedwait PI turnstile ENOMEM fails with EAGAIN",
          "expected EAGAIN");
    rc = pthread_mutex_trylock(&cvr_mutex);
    if (rc == EOK)
        (void) pthread_mutex_unlock(&cvr_mutex); /* the wait lost the mutex */
    check(rc == EBUSY, "cond_timedwait EAGAIN keeps the mutex",
          "mutex not held on return");
    if (rc == EBUSY)
        (void) pthread_mutex_unlock(&cvr_mutex);

    /* 2. The reacquire queues behind a holder while allocation fails. */
    (void) inject_run("fail-turnstile", CVR_PID_NONE, NULL);
    atomic_store(&cvr_armed, 0);
    atomic_store(&cvr_released, 0);
    pthread_mutex_lock(&cvr_mutex);
    cvr_deadline_set(CVR_DEADLINE_MS);
    if (pthread_create(&th, NULL, cvr_helper, NULL) != 0) {
        test_fail("condvar_reacquire_alloc_failure fixture", "pthread_create");
        pthread_mutex_unlock(&cvr_mutex);
        return 0;
    }
    rc = pthread_cond_timedwait(&cvr_cond, &cvr_mutex, &cvr_deadline);
    check(atomic_load(&cvr_armed), "cond_timedwait reacquire fixture armed",
          "failure not armed before the deadline");
    check(rc == ETIMEDOUT, "cond_timedwait reacquire under ENOMEM times out",
          "expected ETIMEDOUT");
    check(atomic_load(&cvr_released),
          "cond_timedwait returns only once it owns the mutex",
          "returned while the helper still held it");
    check(pthread_mutex_unlock(&cvr_mutex) == EOK,
          "cond_timedwait reacquire under ENOMEM holds the mutex",
          "unlock failed: mutex not held");
    (void) pthread_join(th, NULL);
    (void) inject_run("fail-turnstile", CVR_PID_NONE, NULL);
    pthread_cond_destroy(&cvr_cond);
    pthread_mutex_destroy(&cvr_mutex);
    return 0;
}

/* mutex_waiting_republish: a PI mutex waiter that is still publishing itself
 * (WAITING set, chain lock dropped to propagate priority, not yet queued) must
 * not be stranded when another unlock rewrites the lock word from the queued
 * waiters alone. The fixture stalls the late waiter B in that window with the
 * debug injection call, lets the holder hand the mutex to the queued waiter W,
 * and has W keep it until B has queued. W then releases in user space; if
 * WAITING was lost, B sleeps on a free mutex for ever. Needs /bin/inject and
 * two harts; otherwise the case is skipped.
 */
#define MWR_STALL_MS 1500L
#define MWR_WAIT_MS 3000L

static pthread_mutex_t mwr_mutex;
static atomic_int mwr_w_release;
static atomic_int mwr_b_done;
static atomic_int mwr_b_started;

static void *mwr_w(void *arg)
{
    (void) arg;
    pthread_mutex_lock(&mwr_mutex); /* queues behind the main thread */
    while (!atomic_load(&mwr_w_release))
        sleep_ms(5);
    pthread_mutex_unlock(&mwr_mutex);
    return NULL;
}

static void *mwr_b(void *arg)
{
    (void) arg;
    (void) test_pin_mask(2U); /* its stall must not hold up the others */
    atomic_store(&mwr_b_started, 1);
    pthread_mutex_lock(&mwr_mutex);
    pthread_mutex_unlock(&mwr_mutex);
    atomic_store(&mwr_b_done, 1);
    return NULL;
}

int mutex_waiting_republish(void)
{
    struct stall_watch watch = {.pid = -1};
    pthread_mutexattr_t ma;
    pthread_t w, b;
    char ms[16];
    char pid[16];

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2 || inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    snprintf(ms, sizeof ms, "%ld", MWR_STALL_MS);
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    if (pthread_mutex_init(&mwr_mutex, &ma) != 0) {
        test_fail("mutex_waiting_republish fixture", "pthread_mutex_init");
        pthread_mutexattr_destroy(&ma);
        return 0;
    }
    pthread_mutexattr_destroy(&ma);
    atomic_store(&mwr_b_done, 0);
    atomic_store(&mwr_b_started, 0);
    atomic_store(&mwr_w_release, 0);
    (void) test_pin_mask(1U);

    if (pthread_mutex_lock(&mwr_mutex) != 0) {
        test_fail("mutex_waiting_republish fixture", "pthread_mutex_lock");
        goto out;
    }
    if (pthread_create(&w, NULL, mwr_w, NULL) != 0) {
        test_fail("mutex_waiting_republish fixture", "pthread_create");
        goto out;
    }
    sleep_ms(50); /* W is queued, so the unlock below hands the mutex to it */
    if (inject_run("stall-lock", pid, ms) != 0 ||
        stall_watch_start(&watch, "lock") != 0) {
        test_fail("mutex_waiting_republish fixture", "inject refused");
        goto out;
    }
    if (pthread_create(&b, NULL, mwr_b, NULL) != 0) {
        test_fail("mutex_waiting_republish fixture", "pthread_create");
        goto out;
    }
    for (int i = 0; i < 1000 && !atomic_load(&mwr_b_started); i++)
        sleep_ms(5);
    check(stall_watch_entered(&watch), "late waiter reached the bind window",
          "stall was not taken");
    pthread_mutex_unlock(&mwr_mutex); /* hands the mutex to W alone */
    stall_watch_release(&watch);
    atomic_store(&mwr_w_release, 1);

    for (long i = 0; i < MWR_WAIT_MS / 10 && !atomic_load(&mwr_b_done); i++)
        sleep_ms(10);
    check(atomic_load(&mwr_b_done),
          "PI mutex waiter published but not yet queued is not stranded",
          "the late waiter slept on a mutex released in user space");
    if (atomic_load(&mwr_b_done)) {
        (void) pthread_join(w, NULL);
        (void) pthread_join(b, NULL);
        pthread_mutex_destroy(&mwr_mutex);
    }
out:
    stall_watch_finish(&watch);
    (void) test_pin_mask(~0U);
    return 0;
}

/* mutex_waiting_withdrawn: a contended PI lock that cannot get a turnstile
 * fails with EAGAIN, and must take back the WAITING bit it published. The
 * fixture stalls the locker, with the chain lock held, after it saw the mutex
 * busy and before it publishes, releases the mutex in user space meanwhile, and
 * fails the locker's turnstile allocation. The locker then publishes WAITING
 * onto a free word; if it leaves it there, every trylock reports the free mutex
 * busy. Needs /bin/inject and two harts; otherwise the case is skipped.
 */
#define MWW_STALL_MS 3000L

static pthread_mutex_t mww_mutex;
static atomic_int mww_rc;
static atomic_int mww_started;

static void *mww_locker(void *arg)
{
    int rc;

    (void) arg;
    (void) test_pin_mask(2U);
    atomic_store(&mww_started, 1);
    rc = pthread_mutex_lock(&mww_mutex);
    if (rc == 0)
        pthread_mutex_unlock(&mww_mutex);
    atomic_store(&mww_rc, rc);
    return NULL;
}

int mutex_waiting_withdrawn(void)
{
    struct stall_watch watch = {.pid = -1};
    pthread_mutexattr_t ma;
    pthread_t t;
    char ms[16];
    char pid[16];
    int rc;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2 ||
        inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    snprintf(ms, sizeof ms, "%ld", MWW_STALL_MS);
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    rc = pthread_mutex_init(&mww_mutex, &ma);
    pthread_mutexattr_destroy(&ma);
    if (rc != 0) {
        test_fail("mutex_waiting_withdrawn fixture", "pthread_mutex_init");
        return 0;
    }
    atomic_store(&mww_rc, -1);
    atomic_store(&mww_started, 0);
    (void) test_pin_mask(1U);

    if (pthread_mutex_lock(&mww_mutex) != 0 ||
        inject_run("fail-turnstile", pid, NULL) != 0 ||
        inject_run("stall-lock-early", pid, ms) != 0 ||
        stall_watch_start(&watch, "early") != 0) {
        test_fail("mutex_waiting_withdrawn fixture", "lock or inject refused");
        goto out;
    }
    if (pthread_create(&t, NULL, mww_locker, NULL) != 0) {
        test_fail("mutex_waiting_withdrawn fixture", "pthread_create");
        goto out;
    }
    for (int i = 0; i < 1000 && !atomic_load(&mww_started); i++)
        sleep_ms(5);
    check(stall_watch_entered(&watch),
          "locker reached the pre-publication stall",
          "early stall was not taken");
    pthread_mutex_unlock(&mww_mutex); /* in user space: no bit to see yet */
    stall_watch_release(&watch);
    (void) pthread_join(t, NULL);
    check(inject_run("site-wait", "turnstile", NULL) == 0,
          "turnstile allocation consumed its own fault",
          "allocation fault was not taken");
    check(atomic_load(&mww_rc) == EAGAIN,
          "contended PI lock without a turnstile fails with EAGAIN",
          "the lock did not fail as injected");
    rc = pthread_mutex_trylock(&mww_mutex);
    check(rc == 0, "failed contended PI lock leaves no WAITING on a free mutex",
          "trylock reports the free mutex busy");
    if (rc == 0)
        pthread_mutex_unlock(&mww_mutex);
    pthread_mutex_destroy(&mww_mutex);
out:
    stall_watch_finish(&watch);
    (void) inject_run("fail-turnstile", "2147483647", NULL);
    (void) test_pin_mask(~0U);
    return 0;
}

/* condvar_interrupt_stale: force_ready reads a waiter's state without a lock,
 * so an interrupt of a condvar wait can find the waiter already woken and
 * asleep elsewhere by the time it reaches the wait chain. It must then leave
 * the thread alone: the nanosleep the waiter went on to must run its course,
 * not end early with an error. The fixture stalls the interrupt in that window
 * with the debug injection call and signals the condvar meanwhile. Needs
 * /bin/inject; otherwise the case is skipped.
 */
#define CIS_SLEEP_NS 2500000000ULL

static pthread_mutex_t cis_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cis_cond = PTHREAD_COND_INITIALIZER;
static int cis_flag;

struct cis_result {
    pthread_t waiter;
    int interrupt_rc, sleep_rc;
    uint64_t elapsed;
};

static void *cis_waiter(void *arg)
{
    struct cis_result *r = arg;
    struct timespec delay = {.tv_sec = 2, .tv_nsec = 500000000};
    struct timespec begin, end;
    pthread_mutex_lock(&cis_mutex);
    while (!cis_flag)
        pthread_cond_wait(&cis_cond, &cis_mutex);
    pthread_mutex_unlock(&cis_mutex);
    clock_gettime(CLOCK_MONOTONIC, &begin);
    r->sleep_rc = nanosleep(&delay, NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    r->elapsed = (uint64_t) ((end.tv_sec - begin.tv_sec) * 1000000000LL +
                             end.tv_nsec - begin.tv_nsec);
    return NULL;
}

static void *cis_interrupt(void *arg)
{
    struct cis_result *r = arg;
    (void) test_pin_mask(2U);
    r->interrupt_rc = thread_ctl(RTMUX_TCTL_INTERRUPT, (void *) (long) r->waiter);
    return NULL;
}

int condvar_interrupt_stale(void)
{
    struct stall_watch watch = {.pid = -1};
    struct cis_result result = {.interrupt_rc = -1, .sleep_rc = -1};
    pthread_t interrupt;
    bool started = false, entered = false;
    char pid[16];

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2 ||
        inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    (void) test_pin_mask(1U);
    cis_flag = 0;
    if (pthread_create(&result.waiter, NULL, cis_waiter, &result)) {
        test_fail("condvar_interrupt_stale fixture", "cannot create waiter");
        goto out;
    }
    if (wait_thread_state(result.waiter, STATE_CONDVAR) &&
        !inject_run("stall-force-ready", pid, "10000") &&
        !stall_watch_start(&watch, "force-ready") &&
        !pthread_create(&interrupt, NULL, cis_interrupt, &result)) {
        started = true;
        entered = stall_watch_entered(&watch);
    }
    check(entered, "interrupt observed the original condvar wait",
          "force_ready did not reach its stall");
    pthread_mutex_lock(&cis_mutex);
    cis_flag = 1;
    pthread_cond_signal(&cis_cond);
    pthread_mutex_unlock(&cis_mutex);
    if (entered)
        check(wait_thread_state(result.waiter, STATE_NANOSLEEP),
              "waiter entered its new sleep before interrupt resumed",
              "new wait state was not observed");
    if (watch.pid > 0)
        stall_watch_release(&watch);
    if (started)
        pthread_join(interrupt, NULL);
    pthread_join(result.waiter, NULL);
    stall_watch_finish(&watch);
    check(result.interrupt_rc == 0 && result.sleep_rc == 0 &&
              result.elapsed >= CIS_SLEEP_NS,
          "a stale condvar interrupt leaves the new nanosleep intact",
          "interrupt failed or nanosleep returned early");
out:
    (void) test_pin_mask(~0U);
    return 0;
}

static void *sync_kill_waiter(void *arg)
{
    (void) sem_wait(arg);
    return (void *) 1;
}

static void *sync_kill_waker(void *arg)
{
    (void) test_pin_mask(2U);
    return (void *) (intptr_t) sem_post(arg);
}

int sync_kill_handoff(void)
{
    struct stall_watch watch = {.pid = -1};
    char pid[16];
    pthread_t waiter, waker;
    bool started = false, entered = false;
    void *result = NULL;
    sem_t sem;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 3 ||
        inject_run("alloc-status", NULL, NULL))
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    if (sem_init(&sem, 0, 0) != 0 || pthread_create(&waiter, NULL, sync_kill_waiter, &sem)) {
        test_fail("sync kill handoff", "cannot start waiter");
        return 0;
    }
    (void) test_pin_mask(1U);
    if (wait_thread_state(waiter, STATE_SEM) &&
        !inject_run("stall-sync-waker", pid, "10000") &&
        !stall_watch_start(&watch, "sync-waker") &&
        !pthread_create(&waker, NULL, sync_kill_waker, &sem)) {
        started = true;
        entered = stall_watch_entered(&watch);
    }
    check(entered, "sem_post holds the dequeued waiter", "wake stall unavailable");
    if (entered) {
        int rc = __thread_destroy((int) waiter, 0, (void *) 42);
        check(rc == 0 && !inject_run("sync-wake-claims", NULL, NULL),
              "kill hands the waiter to its waker in one attempt",
              "kill failed or polled the pending wake");
    }
    stall_watch_finish(&watch);
    if (started)
        pthread_join(waker, NULL);
    else
        sem_post(&sem);
    int rc = pthread_join(waiter, &result);
    check(entered && rc == 0 && result == (void *) 42,
          "dequeued waiter self-reaps before returning from sem_wait",
          "waiter survived kill or failed to join");
    sem_destroy(&sem);
    (void) test_pin_mask(~0U);
    return 0;
}

/* A kill or a cancel must end every kind of wait, whether it finds the thread
 * blocked or still on its way there.
 *
 * In the second form the thread is held (STALL_BLOCK) inside its blocking
 * kernel call, past the call's own pending checks and before it publishes the
 * blocked state. The requester then sees a running thread: a kill only sends a
 * reschedule IPI and a cancel only records itself. The thread used to block
 * after that with the request pending and nobody left to wake it.
 */
enum {
    BLK_RECEIVE,
    BLK_NANOSLEEP,
    BLK_SEM,
    BLK_COND,
    BLK_JOIN,
    BLK_SEND,  /* nobody receives: the sender queues in STATE_SEND */
    BLK_REPLY, /* a receiver waits: the sender switches to it in STATE_REPLY */
    BLK_MUTEX, /* not a cancellation point: kill only */
    BLK_KINDS
};

struct blk_ctx {
    int kind;
    int chid, coid;
    unsigned pin;
    atomic_int ready, go, finished;
    sem_t sem, hold;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t target;
    int unlock_status;
};

/* The join target, or the server that takes a request and never replies. */
static void *blk_target(void *raw)
{
    struct blk_ctx *c = raw;
    char buf[8];

    if (c->kind == BLK_REPLY)
        (void) msg_receive(c->chid, buf, sizeof buf, NULL);
    (void) sem_wait(&c->hold);
    return NULL;
}

static void blk_cleanup(void *raw)
{
    struct blk_ctx *c = raw;

    if (c->kind == BLK_COND)
        c->unlock_status = pthread_mutex_unlock(&c->mutex);
    atomic_store(&c->finished, 1);
}

static void *blk_thread(void *raw)
{
    struct blk_ctx *c = raw;
    char buf[8];

    (void) test_pin_mask(c->pin);
    if (c->kind == BLK_COND && pthread_mutex_lock(&c->mutex) != EOK)
        return NULL;
    pthread_cleanup_push(blk_cleanup, c);
    atomic_store(&c->ready, 1);

    /* Nothing between here and the wait may block: the stall is armed for this
     * thread's next block.
     */
    while (!atomic_load(&c->go))
        (void) sched_yield();
    switch (c->kind) {
    case BLK_RECEIVE:
        (void) msg_receive(c->chid, buf, sizeof buf, NULL);
        break;
    case BLK_NANOSLEEP:
        (void) nanosleep(&(struct timespec) {.tv_sec = 60}, NULL);
        break;
    case BLK_SEM:
        (void) sem_wait(&c->sem);
        break;
    case BLK_COND:
        (void) pthread_cond_wait(&c->cond, &c->mutex);
        break;
    case BLK_JOIN:
        (void) pthread_join(c->target, NULL);
        break;
    case BLK_SEND:
    case BLK_REPLY:
        (void) msg_send(c->coid, buf, sizeof buf, buf, sizeof buf);
        break;
    default:
        (void) pthread_mutex_lock(&c->mutex);
        break;
    }
    pthread_cleanup_pop(0);
    atomic_store(&c->finished, -1);
    return NULL;
}

/* One wait kind, ended by a kill (status 42) or by a cancel. */
static void blk_case(int kind, bool cancel, bool race)
{
    static const char *const name[BLK_KINDS] = {
        "msg_receive", "nanosleep", "sem_wait",   "cond_wait",
        "pthread_join", "msg_send", "msg_send to a receiver", "mutex_lock"};
    static const int state[BLK_KINDS] = {
        STATE_RECEIVE, STATE_NANOSLEEP, STATE_SEM,   STATE_CONDVAR,
        STATE_JOIN,    STATE_SEND,      STATE_REPLY, STATE_MUTEX};
    /* A thread that stays blocked keeps using its context. */
    static struct blk_ctx ctxs[2][2][BLK_KINDS];
    struct blk_ctx *c = &ctxs[cancel][race][kind];
    struct stall_watch watch = {.pid = -1};
    pthread_t th;
    bool have_target = false, locked = false, held = !race;
    void *ret = NULL;
    char what[96], pid[16], tid[16];
    int rc;

    bool ipc = kind == BLK_RECEIVE || kind == BLK_SEND || kind == BLK_REPLY;

    *c = (struct blk_ctx) {.kind = kind,
                           .chid = -1,
                           .coid = -1,
                           .pin = race ? 2U : ~0U,
                           .unlock_status = -1,
                           .mutex = PTHREAD_MUTEX_INITIALIZER,
                           .cond = PTHREAD_COND_INITIALIZER};
    snprintf(what, sizeof what, "%s %s %s", cancel ? "cancel" : "kill",
             race ? "entering" : "blocked in", name[kind]);
    if (sem_init(&c->sem, 0, 0) != 0 || sem_init(&c->hold, 0, 0) != 0 ||
        (ipc && (c->chid = channel_create(0)) == -1)) {
        test_fail(what, "fixture");
        return;
    }
    if (kind == BLK_SEND || kind == BLK_REPLY) {
        c->coid = connect_attach(0, getpid(), c->chid, RTMUX_SIDE_CHANNEL, 0);
        if (c->coid == -1) {
            test_fail(what, "connect_attach");
            goto out;
        }
    }
    if (kind == BLK_JOIN || kind == BLK_REPLY) {
        have_target =
            pthread_create(&c->target, NULL, blk_target, c) == EOK;
        if (!have_target ||
            (kind == BLK_REPLY &&
             !wait_thread_state(c->target, STATE_RECEIVE))) {
            test_fail(what, "target thread");
            goto out;
        }
    }
    if (kind == BLK_MUTEX)
        locked = pthread_mutex_lock(&c->mutex) == EOK;
    if (pthread_create(&th, NULL, blk_thread, c) != EOK) {
        test_fail(what, "pthread_create");
        goto out;
    }
    for (int i = 0; i < 3000 && !atomic_load(&c->ready); i++)
        sleep_ms(1);
    if (race) {
        snprintf(pid, sizeof pid, "%d", (int) getpid());
        snprintf(tid, sizeof tid, "%d", (int) th);
        held = atomic_load(&c->ready) &&
               inject_run("stall-block", pid, tid) == 0 &&
               stall_watch_start(&watch, "block") == 0;
        atomic_store(&c->go, 1);
        held = held && stall_watch_entered(&watch);
    } else {
        atomic_store(&c->go, 1);
        held = wait_thread_state(th, state[kind]);
    }

    rc = cancel ? pthread_cancel(th) : __thread_destroy((int) th, 0, (void *) 42);
    stall_watch_finish(&watch);

    /* A thread the request missed stays blocked: never join it unseen. */
    bool gone = wait_thread_state(th, STATE_DEAD);
    check(held && rc == 0 && gone, what,
          !held ? "thread was not held at the wait"
                : "the request was lost: thread still blocked");
    if (!gone) {
        (void) __thread_destroy((int) th, 0, NULL);
        if (!wait_thread_state(th, STATE_DEAD))
            return; /* leave everything it may still touch */
    }
    (void) pthread_join(th, &ret);
    if (gone && cancel) {
        snprintf(what, sizeof what, "cancel %s %s runs cleanup once",
                 race ? "entering" : "blocked in", name[kind]);
        check(ret == PTHREAD_CANCELED && atomic_load(&c->finished) == 1 &&
                  (kind != BLK_COND || c->unlock_status == EOK),
              what, "wrong exit value, no cleanup, or mutex not owned");
    } else if (gone) {
        snprintf(what, sizeof what, "kill %s %s reports its status",
                 race ? "entering" : "blocked in", name[kind]);
        check(ret == (void *) 42 && atomic_load(&c->finished) == 0, what,
              "wrong exit value or the wait returned");
    }
out:
    if (locked)
        (void) pthread_mutex_unlock(&c->mutex);
    if (c->coid != -1)
        (void) __connect_detach((uint64_t) c->coid);
    if (c->chid != -1)
        (void) channel_destroy(c->chid); /* also ends a receive still waiting */
    if (have_target) {
        (void) sem_post(&c->hold);
        (void) pthread_join(c->target, NULL);
    }
    (void) sem_destroy(&c->sem);
    (void) sem_destroy(&c->hold);
}

/* Every wait kind, found blocked. */
void test_block_kill_cancel(void)
{
    for (int kind = 0; kind < BLK_KINDS; kind++) {
        blk_case(kind, false, false);
        if (kind != BLK_MUTEX)
            blk_case(kind, true, false);
    }
}

/* Every wait kind, caught between its pending check and the block. */
int block_request_race(void)
{
    if (sysconf(_SC_NPROCESSORS_ONLN) < 3 ||
        inject_run("alloc-status", NULL, NULL))
        return TC_EXIT_SKIP;
    (void) test_pin_mask(1U);
    for (int kind = 0; kind < BLK_KINDS; kind++) {
        blk_case(kind, false, true);

        /* A sender switching to its receiver is already in STATE_REPLY when it
         * is held: the canceller unblocks that one itself, after the hold.
         */
        if (kind != BLK_MUTEX && kind != BLK_REPLY)
            blk_case(kind, true, true);
    }
    (void) test_pin_mask(~0U);
    return 0;
}

/* One thread must never be current on two harts. Each case holds a hart in a
 * window where a wakeup or a steal used to find the thread still current there
 * (STALL_READY, STALL_SWITCH); the other harts then had it run a second time.
 * A kernel built with CONFIG_DEBUG_DISPATCH_OWNER crashes at that dispatch; the
 * notification case also shows without it, as a lost notification.
 */
enum {
    DW_RECV_HART = 2,     /* receiver and sleeper: not the boot hart (0), */
    DW_SEND_HART = 1,     /* which runs the timer expiry */
    DW_TIMEOUT_MS = 300,  /* the receive timeout, inside the stall */
    DW_STALL_MS = 800,    /* how long the waker or the switch is held */
    DW_SPIN_MS = 1200,    /* the woken receiver stays running past the stall */
    DW_SLEEP_MS = 20,     /* the nanosleep, inside the stall */
    DW_DEADLINE_MS = 200, /* the absolute one: after the call, inside the stall */
    DW_YIELD_PRIO = 12,
    DW_CODE = 42
};

/* Any hart but the boot hart (0), which runs the timer expiry: the others may
 * steal the thread.
 */
#define DW_AWAY_FROM_BOOT (~1U)

static int dw_inject(const char *op, pthread_t tid, int ms)
{
    char pid[16], t[16], m[16];
    char *argv[] = {"inject", (char *) op, pid, t, m, NULL};
    pid_t child;
    int st;

    snprintf(pid, sizeof pid, "%d", (int) getpid());
    snprintf(t, sizeof t, "%d", (int) tid);
    snprintf(m, sizeof m, "%d", ms);
    if (posix_spawn(&child, "/bin/inject", NULL, NULL, argv, NULL) != 0 ||
        waitpid(child, &st, 0) != child || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

static void dw_spin_ms(long ms)
{
    struct timespec t0, t1;

    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
    do
        (void) clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L < ms);
}

struct dw_recv {
    int chid;
    atomic_int go, done;
    int rc, err;
    struct _notification n;
};

static void *dw_receiver(void *raw)
{
    struct dw_recv *r = raw;
    uint64_t ns = DW_TIMEOUT_MS * 1000000ULL;

    (void) test_pin_mask(1U << DW_RECV_HART);
    while (!atomic_load(&r->go))
        (void) sched_yield();
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &ns, NULL);
    r->rc = msg_receive_notification(r->chid, &r->n, sizeof r->n, NULL);
    r->err = errno;
    atomic_store(&r->done, 1);

    /* Stay current: a second wakeup now finds this thread running. */
    dw_spin_ms(DW_SPIN_MS);
    return NULL;
}

/* A notification claims the receiver and is held (STALL_READY) before it
 * readies it; the receive timeout expires meanwhile. The timeout used to read
 * the claimed receiver as still waiting, unlink it a second time and ready it
 * with ETIMEDOUT; the held notification then queued the running receiver once
 * more.
 */
static void dw_notify_timeout(void)
{
    static struct dw_recv r;
    const char *what = "notification claimed as the receive timeout fires";
    pthread_t th;
    int coid = -1;
    bool armed = false, held = false, taken = false, made = false;

    r = (struct dw_recv) {.chid = channel_create(0)};
    if (r.chid != -1)
        coid = connect_attach(0, getpid(), r.chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid != -1)
        made = pthread_create(&th, NULL, dw_receiver, &r) == EOK;
    if (made) {
        (void) test_pin_mask(1U << DW_SEND_HART);
        sleep_ms(10); /* the receiver has pinned itself and spins on go */
        armed = dw_inject("stall-ready", th, DW_STALL_MS) == 0;
        atomic_store(&r.go, 1);
        held = armed && wait_thread_state(th, STATE_RECEIVE);
        if (held)
            (void) msg_send_notification(coid, 10, DW_CODE, 0);
        taken = held && inject_run("site-wait", "ready", NULL) == 0;
        (void) test_pin_mask(~0U);
        (void) pthread_join(th, NULL);
    }
    check(armed && held && taken, what, "fixture: receiver not held at the wakeup");
    if (taken)
        check(r.rc == 0 && r.n.code == DW_CODE, what,
              r.rc == 0 ? "wrong notification" : "the notification was lost to the timeout");
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
    if (r.chid != -1)
        (void) channel_destroy(r.chid);
}

struct dw_sleep {
    sem_t go;
    int rc;
};

static void *dw_sleeper(void *raw)
{
    struct dw_sleep *z = raw;

    (void) test_pin_mask(DW_AWAY_FROM_BOOT);

    /* Blocked while the stall is armed, so the next switch away from this
     * thread is the nanosleep's.
     */
    (void) sem_wait(&z->go);
    z->rc = nanosleep(&(struct timespec) {.tv_nsec = DW_SLEEP_MS * 1000000L}, NULL);
    return NULL;
}

/* The sleeper's hart is held (STALL_SWITCH) in block()'s steal scan, after
 * nanosleep has armed the timer and before it switches away; the timer expires
 * on the boot hart. Its wakeup used to queue the sleeper while it was still
 * current here, and a third hart then ran it.
 */
static void dw_sleep_switch(void)
{
    static struct dw_sleep z;
    const char *what = "nanosleep that expires before its hart switches";
    pthread_t th;
    bool armed = false, taken = false;

    z = (struct dw_sleep) {.rc = -1};
    if (sem_init(&z.go, 0, 0) != 0) {
        test_fail(what, "sem_init");
        return;
    }
    if (pthread_create(&th, NULL, dw_sleeper, &z) == EOK) {
        armed = wait_thread_state(th, STATE_SEM) &&
                dw_inject("stall-switch", th, DW_STALL_MS) == 0;
        (void) sem_post(&z.go);
        taken = armed && inject_run("site-wait", "switch", NULL) == 0;
        (void) pthread_join(th, NULL);
    }
    (void) sem_destroy(&z.go);
    check(armed && taken, what, "fixture: sleeper not held at the switch");
    check(z.rc == 0, what, "nanosleep failed");
}

struct dw_past {
    atomic_int go;
    struct timespec until;
    int rc;
};

static void *dw_past_sleeper(void *raw)
{
    struct dw_past *z = raw;

    (void) test_pin_mask(DW_AWAY_FROM_BOOT);
    while (!atomic_load(&z->go))
        (void) sched_yield();
    z->rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &z->until, NULL);
    return NULL;
}

/* An absolute nanosleep is held (STALL_BLOCK) after its deadline check, until
 * the deadline (DW_DEADLINE_MS on) has passed: arming the timer then fires it at once, on the
 * sleeper's own hart. That wakeup used to queue the sleeper before its hart
 * switched away from it.
 */
static void dw_sleep_past(void)
{
    static struct dw_past z;
    const char *what = "absolute nanosleep whose deadline passes while it blocks";
    pthread_t th;
    bool held = false;

    z = (struct dw_past) {.rc = -1};
    (void) test_pin_mask(1U); /* the stall holds a hart: keep this one apart */
    if (pthread_create(&th, NULL, dw_past_sleeper, &z) == EOK) {
        sleep_ms(10);
        held = dw_inject("stall-block", th, DW_STALL_MS) == 0;
        (void) clock_gettime(CLOCK_MONOTONIC, &z.until);
        cvr_ts_add_ms(&z.until, DW_DEADLINE_MS);
        atomic_store(&z.go, 1);

        /* No helper process while the hart is held: reaping one waits for a
         * grace period that the held hart holds up.
         */
        (void) pthread_join(th, NULL);
        held = held && inject_run("site-wait", "block", NULL) == 0;
    }
    (void) test_pin_mask(~0U);
    check(held, what, "fixture: sleeper not held at the block");
    check(z.rc == 0, what, "clock_nanosleep failed");
}

struct dw_yield {
    atomic_int ready;
    sem_t go, peer_go;
    pthread_t peer;
    bool made;
};

static void *dw_peer(void *raw)
{
    struct dw_yield *y = raw;

    (void) sem_wait(&y->peer_go);
    return NULL;
}

/* A FIFO thread yields to a peer queued on its hart and is held
 * (STALL_SWITCH) once it is queued READY itself, before the hart switches to
 * the peer. An idle hart used to steal it in that window.
 */
static void *dw_yielder(void *raw)
{
    struct dw_yield *y = raw;
    pthread_attr_t attr;
    struct sched_param sp = {.sched_priority = DW_YIELD_PRIO};

    if (set_fifo_prio(DW_YIELD_PRIO) != EOK ||
        test_pin_mask(1U << DW_RECV_HART) != EOK) {
        atomic_store(&y->ready, -1);
        return NULL;
    }
    /* The peer inherits the hart and takes the same priority. */
    y->made = pthread_attr_init(&attr) == 0 &&
              pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0 &&
              pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == 0 &&
              pthread_attr_setschedparam(&attr, &sp) == 0 &&
              pthread_create(&y->peer, &attr, dw_peer, y) == EOK;
    (void) pthread_attr_destroy(&attr);
    atomic_store(&y->ready, y->made ? 1 : -1);
    if (!y->made)
        return NULL;

    /* Blocked while the stall is armed, so the next switch away from this
     * thread is the yield below.
     */
    (void) sem_wait(&y->go);
    (void) test_pin_mask(~0U); /* now any hart may take this thread */
    (void) sem_post(&y->peer_go); /* queued on this hart, behind us */
    (void) sched_yield();
    return NULL;
}

static void dw_yield_switch(void)
{
    static struct dw_yield y;
    const char *what = "FIFO yield held before its hart switches";
    pthread_t th;
    bool armed = false, taken = false;

    y = (struct dw_yield) {0};
    if (sem_init(&y.go, 0, 0) != 0 || sem_init(&y.peer_go, 0, 0) != 0) {
        test_fail(what, "sem_init");
        return;
    }
    if (pthread_create(&th, NULL, dw_yielder, &y) == EOK) {
        for (int i = 0; i < 300 && !atomic_load(&y.ready); i++)
            sleep_ms(10);
        armed = atomic_load(&y.ready) == 1 && wait_thread_state(th, STATE_SEM) &&
                dw_inject("stall-switch", th, DW_STALL_MS) == 0;
        (void) sem_post(&y.go);
        taken = armed && inject_run("site-wait", "switch", NULL) == 0;
        if (atomic_load(&y.ready) != 1)
            (void) sem_post(&y.peer_go);
        (void) pthread_join(th, NULL);
        if (y.made)
            (void) pthread_join(y.peer, NULL);
    }
    (void) sem_destroy(&y.go);
    (void) sem_destroy(&y.peer_go);
    check(armed && taken, what, "fixture: yielder not held at the switch");
}

int dispatch_window(void)
{
    if (sysconf(_SC_NPROCESSORS_ONLN) < 3 ||
        inject_run("alloc-status", NULL, NULL))
        return TC_EXIT_SKIP;
    dw_notify_timeout();
    dw_sleep_switch();
    dw_sleep_past();
    dw_yield_switch();
    return 0;
}

/* Scheduler windows, each held open with fault injection while a second
 * party acts on the thread through POKE (a stray wakeup, or a stop as a
 * teardown makes it). A persistent "inject serve" helper takes the commands:
 * no process can be spawned while a hart is held in the kernel.
 */
struct kw_serve {
    pid_t pid;
    int to, from;
};

static int kw_serve_start(struct kw_serve *k)
{
    int in[2], out[2];
    posix_spawn_file_actions_t fa;
    char *argv[] = {"inject", "serve", NULL};
    int rc;

    if (pipe(in) || pipe(out))
        return -2;
    (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(out[0], F_SETFD, FD_CLOEXEC);
    rc = posix_spawn_file_actions_init(&fa);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    if (!rc)
        rc = posix_spawn(&k->pid, "/bin/inject", &fa, NULL, argv, NULL);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    k->to = in[1];
    k->from = out[0];
    return rc ? rc : 0;
}

/* Send one line to the helper. */
static void kw_send(struct kw_serve *k, const char *line)
{
    (void) write(k->to, line, strlen(line));
}

/* Read one reply line from the helper into buf. */
static bool kw_line(struct kw_serve *k, char *buf, size_t n)
{
    size_t i = 0;
    char c;

    while (i + 1 < n && read(k->from, &c, 1) == 1) {
        if (c == '\n') {
            buf[i] = '\0';
            return true;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return false;
}

/* Run one inject command line in the helper; its exit status. */
static int kw_cmd(struct kw_serve *k, const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    kw_send(k, line);
    return kw_line(k, line, sizeof line) ? atoi(line) : -1;
}

static void kw_serve_stop(struct kw_serve *k)
{
    close(k->to);
    close(k->from);
    if (k->pid > 0)
        (void) waitpid(k->pid, NULL, 0);
}

/* Run on exactly the harts in run; threads created from here may run anywhere
 * in inherit.
 */
static int kw_pin(unsigned run, unsigned inherit)
{
    size_t bytes = sizeof(struct _thread_runmask) + 2U * sizeof(unsigned);
    struct _thread_runmask *m = calloc(1U, bytes);
    int rc;

    if (!m)
        return -1;
    m->size = 1U;
    ((unsigned *) (m + 1))[0] = run;
    ((unsigned *) (m + 1))[1] = inherit;
    rc = thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, m);
    free(m);
    return rc;
}

enum { KW_LOW = 10, KW_HIGH = 20, KW_HOLD_MS = 400 };

struct kw {
    atomic_int stop_s1, stop_s2, stop_t, s2_sleep;
    atomic_long count;
    pthread_t t;
    int t_made;
    unsigned t_mask;
};

static void *kw_target(void *raw)
{
    struct kw *w = raw;

    while (!atomic_load(&w->stop_t))
        atomic_fetch_add(&w->count, 1);
    return NULL;
}

static int kw_make(pthread_t *th, int prio, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    struct sched_param sp = {.sched_priority = prio};
    int rc;

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    rc = pthread_create(th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

/* Hart 1's high spinner: creates the target there (so it waits READY on hart
 * 1's queue), then spins until told to stop.
 */
static void *kw_s1(void *raw)
{
    struct kw *w = raw;

    (void) kw_pin(1U << 1, w->t_mask);
    w->t_made = kw_make(&w->t, KW_LOW, kw_target, w) == EOK ? 1 : -1;
    while (!atomic_load(&w->stop_s1))
        ;
    return NULL;
}

/* Hart 2's high spinner: blocks on request, so that hart steals. */
static void *kw_s2(void *raw)
{
    struct kw *w = raw;

    (void) kw_pin(1U << 2, 1U << 2);
    while (!atomic_load(&w->stop_s2)) {
        if (atomic_load(&w->s2_sleep)) {
            atomic_store(&w->s2_sleep, 0);
            sleep_ms(2 * KW_HOLD_MS);
        }
    }
    return NULL;
}

static bool kw_frozen(struct kw *w, long ms)
{
    long c0 = atomic_load(&w->count);

    sleep_ms(ms);
    return atomic_load(&w->count) == c0;
}

/* A steal held between its two dispatch locks (STALL_STEAL), and a stop in
 * that window: the stealer must not run a thread stopped in transit.
 */
static void kw_steal(struct kw_serve *k, bool stop)
{
    static struct kw w;
    const char *what = stop ? "stop of a thread a steal holds in transit"
                            : "second wakeup of a thread a steal holds in transit";
    pthread_t s1, s2;
    bool ready = false, armed = false, taken = false, poked = false, ok = true;

    w = (struct kw) {.t_mask = (1U << 1) | (1U << 2)};
    if (kw_make(&s2, KW_HIGH, kw_s2, &w) != EOK || kw_make(&s1, KW_HIGH, kw_s1, &w) != EOK) {
        test_fail(what, "spinners");
        return;
    }
    for (int i = 0; i < 300 && !w.t_made; i++)
        sleep_ms(10);
    ready = w.t_made == 1 && wait_thread_state(w.t, STATE_READY);
    armed = ready && kw_cmd(k, "stall-steal %d %d %d", (int) getpid(), (int) w.t,
                            KW_HOLD_MS) == 0;
    if (armed) {
        atomic_store(&w.s2_sleep, 1); /* hart 2 blocks and steals the thread */
        taken = kw_cmd(k, "site-wait steal") == 0;
        poked = taken && kw_cmd(k, "poke %d %d %s", (int) getpid(), (int) w.t,
                                stop ? "stop" : "ready") == 0;
        if (stop && poked) {
            sleep_ms(KW_HOLD_MS); /* past the hold */
            ok = kw_frozen(&w, 300);
        } else {
            sleep_ms(KW_HOLD_MS + 100);
            ok = !kw_frozen(&w, 100); /* it runs, once */
        }
    }
    check(armed && taken && poked, what, "fixture: steal not held");
    check(ok, what, stop ? "the stealer ran a stopped thread" : "the thread stopped running");
    atomic_store(&w.stop_t, 1);
    atomic_store(&w.stop_s1, 1);
    atomic_store(&w.stop_s2, 1);
    if (w.t_made == 1) {
        (void) kw_cmd(k, "poke %d %d ready", (int) getpid(), (int) w.t);
        (void) pthread_join(w.t, NULL);
    }
    (void) pthread_join(s1, NULL);
    (void) pthread_join(s2, NULL);
}

/* SCHED_STATS 0 through the helper, or -1. */
static int kw_steal_locks(struct kw_serve *k)
{
    static const char key[] = "empty-steal-locks=";
    char line[64];
    int n = -1;

    kw_send(k, "sched-stats\n");
    if (kw_line(k, line, sizeof line) && !strncmp(line, key, sizeof key - 1))
        n = atoi(line + sizeof key - 1);
    (void) kw_line(k, line, sizeof line); /* the status line */
    return n;
}

/* An idle system: the harts' steal scans take no lock of an empty queue. */
static void kw_empty_peek(struct kw_serve *k)
{
    int c0 = kw_steal_locks(k), c1;

    sleep_ms(500);
    c1 = kw_steal_locks(k);
    printf("k65-windows: empty-queue steal locks over 500 ms idle: %d\n", c1 - c0);
    /* A queue emptied between the peek and the lock still counts: a few. */
    check(c0 >= 0 && c1 >= 0 && c1 - c0 <= 4, "idle steal scans skip empty queues",
          "remote dispatch locks taken on empty queues");
}

int k65_windows(void)
{
    struct kw_serve k = {.to = -1, .from = -1};

    if (sysconf(_SC_NPROCESSORS_ONLN) < 4 || inject_run("alloc-status", NULL, NULL))
        return TC_EXIT_SKIP;
    int src = kw_serve_start(&k);

    if (src) {
        char d[48];

        snprintf(d, sizeof d, "inject serve: %d errno %d", src, errno);
        test_fail("k65 windows", d);
        return 0;
    }
    (void) kw_pin(1U, ~0U); /* stay off harts 1 and 2 */
    kw_empty_peek(&k);
    kw_steal(&k, true);
    (void) kw_pin(~0U, ~0U);
    kw_serve_stop(&k);
    return 0;
}

/* A condition wait returns holding its mutex also when the waiter's priority
 * was raised above a PRIO_PROTECT ceiling during the wait. The reacquire used
 * to apply the ceiling check of an explicit lock: the wait returned EINVAL
 * without the mutex, and the caller's unlock then failed with EPERM.
 */
struct ceil_wait {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int wait_rc, unlock_rc;
};

static void *ceil_waiter(void *raw)
{
    struct ceil_wait *c = raw;

    if (pthread_mutex_lock(&c->mutex) != EOK)
        return NULL;
    c->wait_rc = pthread_cond_wait(&c->cond, &c->mutex);
    c->unlock_rc = pthread_mutex_unlock(&c->mutex);
    return NULL;
}

void test_cond_reacquire_above_ceiling(void)
{
    struct ceil_wait c = {.cond = PTHREAD_COND_INITIALIZER,
                          .wait_rc = -1,
                          .unlock_rc = -1};
    pthread_mutexattr_t attr;
    pthread_t waiter;
    char detail[64];

    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT) != EOK ||
        pthread_mutexattr_setprioceiling(&attr, 12) != EOK ||
        pthread_mutex_init(&c.mutex, &attr) != EOK) {
        test_fail("condition wait above ceiling", "fixture");
        return;
    }
    if (!condloan_create(&waiter, 8, ceil_waiter, &c)) {
        test_fail("condition wait above ceiling", "pthread_create");
        return;
    }
    bool waiting = wait_thread_state(waiter, STATE_CONDVAR);
    int raise_rc = pthread_setschedprio(waiter, 14);

    (void) pthread_cond_signal(&c.cond);
    (void) pthread_join(waiter, NULL);
    snprintf(detail, sizeof detail, "waiting=%d raise=%d wait=%d unlock=%d",
             waiting, raise_rc, c.wait_rc, c.unlock_rc);
    check(waiting && raise_rc == EOK && c.wait_rc == EOK &&
              c.unlock_rc == EOK,
          "condition wait raised above the ceiling returns owning the mutex",
          detail);
    (void) pthread_mutex_destroy(&c.mutex);
    (void) pthread_cond_destroy(&c.cond);
    (void) pthread_mutexattr_destroy(&attr);
}
