/*
 * posix-options.c — runtime macro⟺behavior probe for advertised POSIX options.
 *
 * For every option this file covers, it asserts two things: sysconf(_SC_X)
 * reports the advertised _POSIX_X value, and the option's runtime behavior
 * actually works. Advertising a macro therefore forces a behavioral test rather
 * than a bare symbol definition. scripts/gen-posix-matrix.py reads the _SC_*
 * names referenced here to mark the matching sysconf rows verified.
 *
 * Scope is the single-process, in-process surface (sync, timers, clocks, memory
 * locking, scheduling limits). Options that need a filesystem, a spawned
 * program, aio, or a realtime-budget measurement are evidenced elsewhere and
 * are intentionally not probed here.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/siginfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test.h"

static void *opt_thread_noop(void *arg)
{
    return arg;
}

/* Report the calling thread's own scheduling policy through *arg, so a probe
 * can confirm a thread created with SCHED_SPORADIC actually runs under it.
 */
static void *opt_thread_report_policy(void *arg)
{
    *(int *) arg = sched_getscheduler(0);
    return NULL;
}

void test_posix_options(void)
{
    /* _POSIX_VERSION — the reported version matches the compile-time macro. */
    check(sysconf(_SC_VERSION) == _POSIX_VERSION, "_SC_VERSION honors _POSIX_VERSION",
          "runtime version must equal the advertised macro");

    /* _POSIX_THREADS — a thread can be created and joined. */
    check(sysconf(_SC_THREADS) == _POSIX_THREADS, "_SC_THREADS honored", "value");
    {
        pthread_t t;
        check(pthread_create(&t, NULL, opt_thread_noop, NULL) == 0 &&
                  pthread_join(t, NULL) == 0,
              "_POSIX_THREADS create/join", "threads must work");
    }

    /* _POSIX_THREAD_ATTR_STACKSIZE — the stack size round-trips through attr.
     */
    check(sysconf(_SC_THREAD_ATTR_STACKSIZE) == _POSIX_THREAD_ATTR_STACKSIZE,
          "_SC_THREAD_ATTR_STACKSIZE honored", "value");
    {
        pthread_attr_t a;
        size_t sz = 0;
        check(pthread_attr_init(&a) == 0 &&
                  pthread_attr_setstacksize(&a, 64 * 1024) == 0 &&
                  pthread_attr_getstacksize(&a, &sz) == 0 && sz == 64 * 1024 &&
                  pthread_attr_destroy(&a) == 0,
              "_POSIX_THREAD_ATTR_STACKSIZE round-trip", "stack size must persist");
    }

    /* _POSIX_TIMERS — a timer can be created, armed, and deleted. */
    check(sysconf(_SC_TIMERS) == _POSIX_TIMERS, "_SC_TIMERS honored", "value");
    {
        timer_t tid;
        struct sigevent ev;
        struct itimerspec spec = {.it_value = {.tv_sec = 0, .tv_nsec = 1000000}};

        memset(&ev, 0, sizeof ev);
        ev.sigev_notify = SIGEV_NONE;
        if (timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0) {
            test_fail("_POSIX_TIMERS create/settime/delete", strerror(errno));
        } else {
            /* delete unconditionally so a failed settime cannot leak the timer
             */
            int set = timer_settime(tid, 0, &spec, NULL);
            int del = timer_delete(tid);
            check(set == 0 && del == 0, "_POSIX_TIMERS create/settime/delete",
                  "timers must work");
        }
    }

    /* _POSIX_MONOTONIC_CLOCK — CLOCK_MONOTONIC never runs backwards. */
    check(sysconf(_SC_MONOTONIC_CLOCK) == _POSIX_MONOTONIC_CLOCK,
          "_SC_MONOTONIC_CLOCK honored", "value");
    {
        struct timespec a, b;
        check(clock_gettime(CLOCK_MONOTONIC, &a) == 0 &&
                  clock_gettime(CLOCK_MONOTONIC, &b) == 0 &&
                  (b.tv_sec > a.tv_sec ||
                   (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec)),
              "_POSIX_MONOTONIC_CLOCK is monotonic", "clock must not go backwards");
    }

#if defined(_POSIX_CLOCK_SELECTION)
    /* _POSIX_CLOCK_SELECTION — clock_nanosleep honors an absolute deadline. */
    check(sysconf(_SC_CLOCK_SELECTION) == _POSIX_CLOCK_SELECTION,
          "_SC_CLOCK_SELECTION honored", "value");
    {
        struct timespec now;
        int got = clock_gettime(CLOCK_MONOTONIC, &now);
        now.tv_nsec += 1000000;
        if (now.tv_nsec >= 1000000000L) {
            now.tv_nsec -= 1000000000L;
            now.tv_sec++;
        }
        check(got == 0 &&
                  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL) == 0,
              "_POSIX_CLOCK_SELECTION abs clock_nanosleep", "absolute sleep must work");
    }

#endif /* _POSIX_CLOCK_SELECTION */
#if defined(_POSIX_TIMEOUTS)
    /* _POSIX_TIMEOUTS — a timed wait returns ETIMEDOUT past its deadline. */
    check(sysconf(_SC_TIMEOUTS) == _POSIX_TIMEOUTS, "_SC_TIMEOUTS honored", "value");
    {
        sem_t s;
        struct timespec past;
        int got = clock_gettime(CLOCK_REALTIME, &past);
        past.tv_sec -= 1; /* strictly in the past, not merely "now" */
        if (got != 0 || sem_init(&s, 0, 0) != 0) {
            test_fail("_POSIX_TIMEOUTS sem_timedwait", "setup failed");
        } else {
            errno = 0;
            int rc = sem_timedwait(&s, &past);
            check(rc == -1 && errno == ETIMEDOUT, "_POSIX_TIMEOUTS sem_timedwait",
                  "empty timed wait must time out");
            (void) sem_destroy(&s); /* always release, even on a failed wait */
        }
    }

#endif /* _POSIX_TIMEOUTS */
    /* _POSIX_THREAD_CPUTIME — a thread CPU clock is readable. */
    check(sysconf(_SC_THREAD_CPUTIME) == _POSIX_THREAD_CPUTIME,
          "_SC_THREAD_CPUTIME honored", "value");
    {
        clockid_t clk;
        struct timespec ts;
        check(pthread_getcpuclockid(pthread_self(), &clk) == 0 &&
                  clock_gettime(clk, &ts) == 0,
              "_POSIX_THREAD_CPUTIME clock readable", "thread cpu clock must work");
    }

#if defined(_POSIX_BARRIERS)
    /* _POSIX_BARRIERS — a single-thread barrier releases immediately. */
    check(sysconf(_SC_BARRIERS) == _POSIX_BARRIERS, "_SC_BARRIERS honored", "value");
    {
        pthread_barrier_t b;
        check(pthread_barrier_init(&b, NULL, 1) == 0 &&
                  pthread_barrier_wait(&b) == PTHREAD_BARRIER_SERIAL_THREAD &&
                  pthread_barrier_destroy(&b) == 0,
              "_POSIX_BARRIERS wait", "barriers must work");
    }

#endif /* _POSIX_BARRIERS */
#if defined(_POSIX_SPIN_LOCKS)
    /* _POSIX_SPIN_LOCKS — lock/unlock round-trips. */
    check(sysconf(_SC_SPIN_LOCKS) == _POSIX_SPIN_LOCKS, "_SC_SPIN_LOCKS honored", "value");
    {
        pthread_spinlock_t sp;
        check(pthread_spin_init(&sp, PTHREAD_PROCESS_PRIVATE) == 0 &&
                  pthread_spin_lock(&sp) == 0 && pthread_spin_unlock(&sp) == 0 &&
                  pthread_spin_destroy(&sp) == 0,
              "_POSIX_SPIN_LOCKS lock/unlock", "spin locks must work");
    }

#endif /* _POSIX_SPIN_LOCKS */
#if defined(_POSIX_READER_WRITER_LOCKS)
    /* _POSIX_READER_WRITER_LOCKS — read and write acquisition round-trip. */
    check(sysconf(_SC_READER_WRITER_LOCKS) == _POSIX_READER_WRITER_LOCKS,
          "_SC_READER_WRITER_LOCKS honored", "value");
    {
        pthread_rwlock_t rw;
        check(pthread_rwlock_init(&rw, NULL) == 0 &&
                  pthread_rwlock_rdlock(&rw) == 0 && pthread_rwlock_unlock(&rw) == 0 &&
                  pthread_rwlock_wrlock(&rw) == 0 && pthread_rwlock_unlock(&rw) == 0 &&
                  pthread_rwlock_destroy(&rw) == 0,
              "_POSIX_READER_WRITER_LOCKS rd/wr", "rwlocks must work");
    }

#endif /* _POSIX_READER_WRITER_LOCKS */
#if defined(_POSIX_PRIORITY_SCHEDULING)
    /* _POSIX_PRIORITY_SCHEDULING — the fixed-priority range is well-formed. */
    check(sysconf(_SC_PRIORITY_SCHEDULING) == _POSIX_PRIORITY_SCHEDULING,
          "_SC_PRIORITY_SCHEDULING honored", "value");
    {
        int hi = sched_get_priority_max(SCHED_FIFO);
        int lo = sched_get_priority_min(SCHED_FIFO);
        check(hi >= 0 && lo >= 0 && hi >= lo, "_POSIX_PRIORITY_SCHEDULING range",
              "SCHED_FIFO must expose a valid priority range");
    }

#endif /* _POSIX_PRIORITY_SCHEDULING */
    /* _POSIX_THREAD_PRIORITY_SCHEDULING — policy round-trips through attr. */
    check(sysconf(_SC_THREAD_PRIORITY_SCHEDULING) ==
              _POSIX_THREAD_PRIORITY_SCHEDULING,
          "_SC_THREAD_PRIORITY_SCHEDULING honored", "value");
    {
        pthread_attr_t a;
        int pol = -1;
        check(pthread_attr_init(&a) == 0 &&
                  pthread_attr_setschedpolicy(&a, SCHED_FIFO) == 0 &&
                  pthread_attr_getschedpolicy(&a, &pol) == 0 && pol == SCHED_FIFO &&
                  pthread_attr_destroy(&a) == 0,
              "_POSIX_THREAD_PRIORITY_SCHEDULING policy", "sched policy must persist");
    }

    /* _POSIX_MEMLOCK — whole-address-space locking round-trips. */
    check(sysconf(_SC_MEMLOCK) == _POSIX_MEMLOCK, "_SC_MEMLOCK honored", "value");
    check(mlockall(MCL_CURRENT) == 0 && munlockall() == 0, "_POSIX_MEMLOCK mlockall",
          "mlockall/munlockall must work");

    /* _POSIX_MEMLOCK_RANGE — range locking round-trips. */
    check(sysconf(_SC_MEMLOCK_RANGE) == _POSIX_MEMLOCK_RANGE,
          "_SC_MEMLOCK_RANGE honored", "value");
    {
        static char region[4096];
        check(mlock(region, sizeof region) == 0 &&
                  munlock(region, sizeof region) == 0,
              "_POSIX_MEMLOCK_RANGE mlock", "mlock/munlock must work");
    }

    /* _POSIX_SHARED_MEMORY_OBJECTS — a shm object opens, closes, and unlinks.
     */
    check(sysconf(_SC_SHARED_MEMORY_OBJECTS) == _POSIX_SHARED_MEMORY_OBJECTS,
          "_SC_SHARED_MEMORY_OBJECTS honored", "value");
    {
        const char *name = "/posix-opt-probe";
        (void) shm_unlink(name); /* clear any stale object first */
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd == -1) {
            test_fail("_POSIX_SHARED_MEMORY_OBJECTS shm_open", strerror(errno));
        } else {
            int cl = close(fd);
            int ul = shm_unlink(name); /* always unlink, even if close failed */
            check(cl == 0 && ul == 0, "_POSIX_SHARED_MEMORY_OBJECTS shm_open",
                  "shm object must round-trip");
        }
    }

    /* _POSIX_THREAD_ATTR_STACKADDR — a stack address round-trips through attr.
     */
    check(sysconf(_SC_THREAD_ATTR_STACKADDR) == _POSIX_THREAD_ATTR_STACKADDR,
          "_SC_THREAD_ATTR_STACKADDR honored", "value");
    {
        static char stack_region[16384];
        pthread_attr_t a;
        void *addr = NULL;
        check(pthread_attr_init(&a) == 0 &&
                  pthread_attr_setstackaddr(&a, stack_region) == 0 &&
                  pthread_attr_getstackaddr(&a, &addr) == 0 &&
                  addr == stack_region && pthread_attr_destroy(&a) == 0,
              "_POSIX_THREAD_ATTR_STACKADDR round-trip",
              "stack address must persist");
    }

#if defined(_POSIX_THREAD_PROCESS_SHARED)
    /* _POSIX_THREAD_PROCESS_SHARED — a mutex in shm serializes two processes.
     */
    check(sysconf(_SC_THREAD_PROCESS_SHARED) == _POSIX_THREAD_PROCESS_SHARED,
          "_SC_THREAD_PROCESS_SHARED honored", "value");
    {
        const char *name = PSHARED_SHM_NAME;
        char *count_argv[] = {"syscall_testing", "--pshared-count",
                              (char *) name, PSHARED_ITERS_ARG, NULL};
        char *cond_argv[] = {"syscall_testing", "--pshared-cond-wait",
                             (char *) name, NULL};
        pthread_mutexattr_t mattr;
        pthread_condattr_t cattr;
        struct pshared_region *r;
        flock_t space;
        pid_t pid;
        int status;
        int fd;

        (void) shm_unlink(name);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd == -1) {
            test_fail("_POSIX_THREAD_PROCESS_SHARED shm_open", strerror(errno));
        } else {
            memset(&space, 0, sizeof space);
            space.l_whence = SEEK_SET;
            space.l_len = (off_t) sizeof *r;
            if (fcntl(fd, F_ALLOCSP, &space) == -1) {
                test_fail("_POSIX_THREAD_PROCESS_SHARED size", strerror(errno));
                (void) close(fd);
                (void) shm_unlink(name);
            } else {
                r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, 0);
                (void) close(fd);
                if (r == MAP_FAILED) {
                    test_fail("_POSIX_THREAD_PROCESS_SHARED mmap",
                              strerror(errno));
                    (void) shm_unlink(name);
                } else if (pthread_mutexattr_init(&mattr) != EOK ||
                           pthread_mutexattr_setpshared(
                               &mattr, PTHREAD_PROCESS_SHARED) != EOK ||
                           pthread_mutex_init(&r->mtx, &mattr) != EOK ||
                           pthread_condattr_init(&cattr) != EOK ||
                           pthread_condattr_setpshared(
                               &cattr, PTHREAD_PROCESS_SHARED) != EOK ||
                           pthread_cond_init(&r->cv, &cattr) != EOK) {
                    test_fail("_POSIX_THREAD_PROCESS_SHARED init",
                              "shared mutex init failed");
                    (void) munmap(r, sizeof *r);
                    (void) shm_unlink(name);
                } else {
                    int ok = 1;

                    (void) pthread_mutexattr_destroy(&mattr);
                    (void) pthread_condattr_destroy(&cattr);
                    r->counter = 0;
                    r->state = 0;
                    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL,
                                    count_argv, NULL) != EOK) {
                        test_fail("_POSIX_THREAD_PROCESS_SHARED count child",
                                  "posix_spawn failed");
                        ok = 0;
                    } else {
                        for (long i = 0; i < PSHARED_ITERS; i++) {
                            if (pthread_mutex_lock(&r->mtx) != EOK) {
                                ok = 0;
                                break;
                            }
                            r->counter++;
                            (void) pthread_mutex_unlock(&r->mtx);
                        }
                        check(waitpid(pid, &status, 0) == pid &&
                                  WIFEXITED(status) &&
                                  WEXITSTATUS(status) == EXIT_SUCCESS && ok &&
                                  r->counter == 2L * PSHARED_ITERS,
                              "_POSIX_THREAD_PROCESS_SHARED mutex",
                              "cross-process mutex must lose no counter updates");
                    }
                    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL,
                                    cond_argv, NULL) != EOK) {
                        test_fail("_POSIX_THREAD_PROCESS_SHARED cond child",
                                  "posix_spawn failed");
                    } else {
                        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};

                        for (int i = 0; i < 100; i++) {
                            if (pthread_mutex_lock(&r->mtx) == EOK) {
                                int ready = r->state == 1;
                                (void) pthread_mutex_unlock(&r->mtx);
                                if (ready)
                                    break;
                            }
                            (void) nanosleep(&ts, NULL);
                        }
                        if (pthread_mutex_lock(&r->mtx) == EOK) {
                            r->state = 2;
                            (void) pthread_cond_signal(&r->cv);
                            (void) pthread_mutex_unlock(&r->mtx);
                        }
                        check(waitpid(pid, &status, 0) == pid &&
                                  WIFEXITED(status) &&
                                  WEXITSTATUS(status) == EXIT_SUCCESS &&
                                  r->state == 3,
                              "_POSIX_THREAD_PROCESS_SHARED condvar",
                              "cross-process condvar must wake a waiter");
                    }
                    (void) pthread_cond_destroy(&r->cv);
                    (void) pthread_mutex_destroy(&r->mtx);
                    (void) munmap(r, sizeof *r);
                    (void) shm_unlink(name);
                }
            }
        }
    }

#endif /* _POSIX_THREAD_PROCESS_SHARED */
#if defined(_POSIX_MEMORY_PROTECTION)
    /* _POSIX_MEMORY_PROTECTION — an anonymous mapping can be created, written,
     * reprotected read-only, and unmapped. (_POSIX_MAPPED_FILES stays
     * unverified: it means mmap of a *file* descriptor, which an anonymous
     * mapping does not exercise and the probe environment can't yet set up.)
     */
    check(sysconf(_SC_MEMORY_PROTECTION) == _POSIX_MEMORY_PROTECTION,
          "_SC_MEMORY_PROTECTION honored", "value");
    {
        size_t len = 4096;
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            test_fail("_POSIX_MEMORY_PROTECTION mmap", strerror(errno));
        } else {
            *(volatile char *) p = 42; /* mapping is writable */
            int prot = mprotect(p, len, PROT_READ);
            int un = munmap(p, len);
            check(prot == 0 && un == 0,
                  "_POSIX_MEMORY_PROTECTION mmap/mprotect",
                  "anonymous mapping must reprotect and unmap");
        }
    }

#endif /* _POSIX_MEMORY_PROTECTION */
#if defined(_POSIX_MAPPED_FILES)
    /* _POSIX_MAPPED_FILES — a sized descriptor (a shm object) maps fd-backed.
     * This is the file/fd-backed mmap the anonymous MEMORY_PROTECTION probe
     * above does not exercise. (_POSIX_FSYNC / _POSIX_SYNCHRONIZED_IO stay
     * unverified: fsync is a no-op-error on a shm object, and the probe
     * environment has no regular file to flush.)
     */
    check(sysconf(_SC_MAPPED_FILES) == _POSIX_MAPPED_FILES,
          "_SC_MAPPED_FILES honored", "value");
    {
        const char *name = "/posix-opt-mapfile";
        (void) shm_unlink(name); /* clear any stale object */
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd == -1) {
            test_fail("_POSIX_MAPPED_FILES setup", strerror(errno));
        } else {
            flock_t sp;
            memset(&sp, 0, sizeof sp);
            sp.l_whence = SEEK_SET;
            sp.l_len = 4096;
            if (fcntl(fd, F_ALLOCSP, &sp) != 0) {
                test_fail("_POSIX_MAPPED_FILES F_ALLOCSP", strerror(errno));
            } else {
                void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, 0);
                if (p == MAP_FAILED) {
                    test_fail("_POSIX_MAPPED_FILES fd mmap", strerror(errno));
                } else {
                    *(volatile char *) p = 7; /* fd-backed mapping is writable */
                    check(munmap(p, 4096) == 0, "_POSIX_MAPPED_FILES fd mmap",
                          "fd-backed mmap must work");
                }
            }
            (void) close(fd);
            (void) shm_unlink(name);
        }
    }

#endif /* _POSIX_MAPPED_FILES */
#if defined(_POSIX_SPAWN)
    /* _POSIX_SPAWN — spawn a child (/bin/true) and reap a clean exit. */
    check(sysconf(_SC_SPAWN) == _POSIX_SPAWN, "_SC_SPAWN honored", "value");
    {
        char *argv[] = {"true", NULL};
        pid_t pid;
        int status;
        int rc = posix_spawn(&pid, "/bin/true", NULL, NULL, argv, NULL);
        if (rc != 0) {
            test_fail("_POSIX_SPAWN posix_spawn", strerror(rc));
        } else {
            pid_t got;
            do { /* a blocked wait can surface EINTR; retry so the child reaps */
                got = waitpid(pid, &status, 0);
            } while (got == -1 && errno == EINTR);
            check(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "_POSIX_SPAWN posix_spawn",
                  "spawning /bin/true must succeed and exit 0");
        }

        /* wait() reaps any child, so in the shared suite it may first collect
         * an unrelated zombie; drain until our own pid comes back (a zombie is
         * already dead, so reaping it here is harmless).
         */
        if (posix_spawn(&pid, "/bin/true", NULL, NULL, argv, NULL) == 0) {
            pid_t got;
            do {
                got = wait(&status);
            } while (got != pid && (got > 0 || (got == -1 && errno == EINTR)));
            check(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "wait reaps a child",
                  "wait() must reap the spawned child and report its exit");
        }

        /* waitid() reaps a specific child and fills the siginfo directly. */
        if (posix_spawn(&pid, "/bin/true", NULL, NULL, argv, NULL) == 0) {
            siginfo_t info;
            int wr;
            do {
                wr = waitid(P_PID, (id_t) pid, &info, WEXITED);
            } while (wr == -1 && errno == EINTR);
            check(wr == 0 && info.si_pid == pid && info.si_code == CLD_EXITED &&
                      info.si_status == 0,
                  "waitid reaps a child",
                  "waitid() must reap the child and report CLD_EXITED status 0");
        }
    }

#endif /* _POSIX_SPAWN */
#if defined(_POSIX_MESSAGE_PASSING)
    /* _POSIX_MESSAGE_PASSING — advertised value matches, and a queue carries
     * one message (with its priority) from send to receive.
     */
    check(sysconf(_SC_MESSAGE_PASSING) == _POSIX_MESSAGE_PASSING,
          "_SC_MESSAGE_PASSING honored", "value");
    {
        struct mq_attr attr = {.mq_maxmsg = 4, .mq_msgsize = 32};
        mqd_t mq;

        (void) mq_unlink("/opt_mq"); /* clear any stale queue from a prior run */
        mq = mq_open("/opt_mq", O_CREAT | O_RDWR, 0600, &attr);
        if (mq == (mqd_t) -1) {
            test_fail("_POSIX_MESSAGE_PASSING mq_open", strerror(errno));
        } else {
            char in[32];
            unsigned prio = 0;

            check(mq_send(mq, "ping", 4, 3) == 0,
                  "_POSIX_MESSAGE_PASSING mq_send", "send must succeed");
            check(mq_receive(mq, in, sizeof in, &prio) == 4 && prio == 3 &&
                      memcmp(in, "ping", 4) == 0,
                  "_POSIX_MESSAGE_PASSING mq_receive",
                  "receive must return the sent message and priority");
            check(mq_close(mq) == 0, "_POSIX_MESSAGE_PASSING mq_close",
                  "close must succeed");
            check(mq_unlink("/opt_mq") == 0, "_POSIX_MESSAGE_PASSING mq_unlink",
                  "unlink must succeed");
        }
    }

#endif /* _POSIX_MESSAGE_PASSING */
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    /* _POSIX_THREAD_SAFE_FUNCTIONS — advertised value matches and the reentrant
     * _r variants work against caller-supplied buffers (no shared static
     * state). strerror_r/gmtime_r/asctime_r are pure — no filesystem — so they
     * exercise the option entirely in-process.
     */
    check(sysconf(_SC_THREAD_SAFE_FUNCTIONS) == _POSIX_THREAD_SAFE_FUNCTIONS,
          "_SC_THREAD_SAFE_FUNCTIONS honored", "value");
    {
        char ebuf[64];
        char abuf[32];
        time_t t = 0;
        struct tm tm;

        check(strerror_r(EINVAL, ebuf, sizeof ebuf) == 0 && ebuf[0] != '\0',
              "_POSIX_THREAD_SAFE_FUNCTIONS strerror_r",
              "strerror_r must fill the caller buffer");
        check(gmtime_r(&t, &tm) == &tm && tm.tm_year == 70 &&
                  asctime_r(&tm, abuf) == abuf && abuf[0] != '\0',
              "_POSIX_THREAD_SAFE_FUNCTIONS gmtime_r/asctime_r",
              "reentrant time functions must decode the epoch into the buffer");
    }

#endif /* _POSIX_THREAD_SAFE_FUNCTIONS */
#if defined(_POSIX_TYPED_MEMORY_OBJECTS)
    /* _POSIX_TYPED_MEMORY_OBJECTS — advertised value matches and a typed-memory
     * object (/sysram) opens, reports a non-zero extent, and closes. This is a
     * memory object like shm, not a filesystem path.
     */
    check(sysconf(_SC_TYPED_MEMORY_OBJECTS) == _POSIX_TYPED_MEMORY_OBJECTS,
          "_SC_TYPED_MEMORY_OBJECTS honored", "value");
    {
        int fd =
            posix_typed_mem_open("/sysram", O_RDWR, POSIX_TYPED_MEM_ALLOCATE);

        if (fd == -1) {
            test_fail("_POSIX_TYPED_MEMORY_OBJECTS open", strerror(errno));
        } else {
            struct posix_typed_mem_info info;

            check(posix_typed_mem_get_info(fd, &info) == 0 &&
                      info.posix_tmi_length != 0,
                  "_POSIX_TYPED_MEMORY_OBJECTS get_info",
                  "typed-mem info must report a non-zero extent");
            (void) close(fd);
        }
    }

#endif /* _POSIX_TYPED_MEMORY_OBJECTS */
    /* _POSIX_SPORADIC_SERVER — advertised value matches, and the process can
     * switch its own scheduler to SCHED_SPORADIC and back. Restoring the
     * original policy is clean: the kernel runs sched_ss_cleanup on any policy
     * change away from SCHED_SPORADIC, dropping the replenishment entry and
     * freeing the sporadic info, so the test process is left as it was found.
     * The budget is generous (10 ms) and the window is two syscalls, so the
     * process never falls to the low priority mid-probe.
     */
    check(sysconf(_SC_SPORADIC_SERVER) == _POSIX_SPORADIC_SERVER,
          "_SC_SPORADIC_SERVER honored", "value");
    {
        struct sched_param old;
        int old_policy = sched_getscheduler(0);

        if (old_policy == -1 || sched_getparam(0, &old) == -1) {
            test_fail("_POSIX_SPORADIC_SERVER save", strerror(errno));
        } else if (old.sched_priority <= 0) {
            /* Sporadic needs ss_low_priority in (0, sched_priority]; a base
             * priority of 0 cannot host it, so there is nothing to assert.
             */
        } else {
            struct sched_param sp = old;

            sp.ss_low_priority = 1;
            sp.ss_max_repl = 1;
            sp.ss_init_budget.tv_sec = 0;
            sp.ss_init_budget.tv_nsec = 10 * 1000 * 1000; /* 10 ms */
            sp.ss_repl_period.tv_sec = 0;
            sp.ss_repl_period.tv_nsec = 100 * 1000 * 1000; /* 100 ms */

            check(sched_setscheduler(0, SCHED_SPORADIC, &sp) == 0 &&
                      sched_getscheduler(0) == SCHED_SPORADIC,
                  "_POSIX_SPORADIC_SERVER set",
                  "the process must accept SCHED_SPORADIC");
            /* Block while some, but not all, of the budget is consumed. With
             * ss_max_repl 1 that drops the thread to its low priority while the
             * scheduler lock is held, which used to re-enter the lock and hang
             * the hart. The tick that consumes budget is not synchronous with
             * the burn, so repeat short burn-then-block rounds (a 10 ms budget
             * against 6 ms bursts) until the case is certain to have occurred;
             * the thread must come back from every block, including a MsgSend.
             */
            {
                struct timespec t0, t1, nap = {.tv_nsec = 1000000L};
                int ok = 1;

                for (int r = 0; r < 30 && ok; r++) {
                    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
                    do {
                        (void) clock_gettime(CLOCK_MONOTONIC, &t1);
                    } while ((t1.tv_sec - t0.tv_sec) * 1000000000L +
                                 (t1.tv_nsec - t0.tv_nsec) <
                             6 * 1000 * 1000L);
                    ok = nanosleep(&nap, NULL) == 0 &&   /* unready path */
                         access("/", F_OK) == 0;         /* MsgSend block */
                }
                check(ok, "sporadic_block_after_budget thread survives its blocks",
                      "blocking with budget consumed must not hang or fail");
            }
            /* Restore unconditionally, even if the assertion above failed. */
            check(sched_setscheduler(0, old_policy, &old) == 0,
                  "_POSIX_SPORADIC_SERVER restore",
                  "the original scheduling policy must be restorable");
        }
    }

    /* _POSIX_THREAD_SPORADIC_SERVER — advertised value matches, and a thread
     * created with an explicit SCHED_SPORADIC attribute actually runs under
     * that policy. This stays isolated to the child (the main thread's
     * scheduling is untouched), and the child reports its own policy back so
     * the check is real evidence the sporadic attribute took effect, not just
     * that pthread_create returned.
     */
    check(sysconf(_SC_THREAD_SPORADIC_SERVER) == _POSIX_THREAD_SPORADIC_SERVER,
          "_SC_THREAD_SPORADIC_SERVER honored", "value");
    {
        struct sched_param cur;
        pthread_attr_t attr;
        int base = sched_getscheduler(0);

        if (base == -1 || sched_getparam(0, &cur) == -1) {
            test_fail("_POSIX_THREAD_SPORADIC_SERVER base", strerror(errno));
        } else if (cur.sched_priority <= 0) {
            /* No base priority to host a sporadic low priority; nothing to do.
             */
        } else if (pthread_attr_init(&attr) != 0) {
            test_fail("_POSIX_THREAD_SPORADIC_SERVER attr_init", "attr init");
        } else {
            struct sched_param sp = cur;
            int set;

            sp.ss_low_priority = 1;
            sp.ss_max_repl = 1;
            sp.ss_init_budget.tv_sec = 0;
            sp.ss_init_budget.tv_nsec = 10 * 1000 * 1000; /* 10 ms */
            sp.ss_repl_period.tv_sec = 0;
            sp.ss_repl_period.tv_nsec = 100 * 1000 * 1000; /* 100 ms */

            set = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) ==
                      0 &&
                  pthread_attr_setschedpolicy(&attr, SCHED_SPORADIC) == 0 &&
                  pthread_attr_setschedparam(&attr, &sp) == 0;
            check(set, "_POSIX_THREAD_SPORADIC_SERVER attr",
                  "the attribute must accept SCHED_SPORADIC and its parameters");
            if (set) {
                pthread_t th;
                int child_policy = -1;
                int created =
                    pthread_create(&th, &attr, opt_thread_report_policy,
                                   &child_policy);
                /* Always join a thread we created, so none is left detached. */
                int joined = created == 0 ? pthread_join(th, NULL) : -1;

                check(created == 0 && joined == 0 &&
                          child_policy == SCHED_SPORADIC,
                      "_POSIX_THREAD_SPORADIC_SERVER thread",
                      "a thread created SCHED_SPORADIC must run under that policy");
            }
            (void) pthread_attr_destroy(&attr);
        }
    }

    /* Un-advertised options must fail closed: aio is absent, so its option and
     * limit report -1 (unsupported) rather than pretending to pass.
     */
    check(sysconf(_SC_ASYNCHRONOUS_IO) == -1, "aio option unsupported",
          "_SC_ASYNCHRONOUS_IO must report -1");
    check(sysconf(_SC_PRIORITIZED_IO) == -1, "prioritized-io option unsupported",
          "_SC_PRIORITIZED_IO must report -1");
    check(sysconf(_SC_AIO_PRIO_DELTA_MAX) == -1, "aio limit unsupported",
          "_SC_AIO_PRIO_DELTA_MAX must report -1");
}
