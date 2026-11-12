/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Malformed user-pointer conformance for kernel calls.
 *
 * The kernel validates a user pointer with RD_VERIFY_PTR / WR_VERIFY_PTR (a
 * range check) followed by RD_PROBE_INT / WR_PROBE_INT (an access probe). The
 * range check only bounds the address from above -- WITHIN_BOUNDARY is `first
 * <= boundary && last <= boundary` -- so NULL and any unmapped address below
 * the boundary pass it. Whether the probe that follows is then safe depends on
 * a fault handler being armed, and most kcalls arm none.
 *
 * These cases go through the raw __* kcall stubs on purpose: the libc wrappers
 * validate their arguments, so only the stubs reach the kernel's own checks. A
 * refusal is a pass. A kernel that faults here does not merely fail this test,
 * it takes the machine down, so the rest of the suite not running is itself the
 * signal.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <time.h>
#include <unistd.h>

/* Private kcall-slot ABI. channel_create's event/cred slots have no libc
 * wrapper (every in-tree caller passes them as NULL), so the only way to
 * reach the kernel's checks on them is a raw trap. Same relative-include
 * convention capgate.c uses for the priv sub-namespace.
 */
#include "../../internal/include/sys/calls.h"

#include "test.h"

#define PAGE 4096

/* Raw six-argument kcall trap, bypassing libc entirely. Returns the kernel's
 * negated errno rather than setting errno, matching the *_r wrapper form.
 */
static long test_kcall6(long n,
                        long arg0,
                        long arg1,
                        long arg2,
                        long arg3,
                        long arg4,
                        long arg5)
{
#if defined(__aarch64__)
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;
    register long x5 __asm__("x5") = arg5;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
#else
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;

    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    return a0;
#endif
}

static void expect_refused(int rc, const char *name)
{
    if (rc != -1) {
        test_fail(name, "kernel accepted a malformed user pointer");
        return;
    }
    if (errno == EFAULT || errno == EINVAL || errno == ENOSYS ||
        errno == ENOTSUP) {
        test_ok(name);
        return;
    }
    test_fail(name, strerror(errno));
}

/* Return the first address of an unmapped page whose preceding page is mapped.
 *
 * Reserve two pages and make the second inaccessible (PROT_NONE). The returned address is inside the
 * user half, so a kernel range check accepts it, but it is unmapped, so only an
 * access probe can catch it. The page below it stays mapped and writable on
 * purpose: a caller testing a multi-word read needs to place its first word in
 * readable memory with the tail falling off the edge.
 *
 * Returns NULL if the mapping could not be arranged, which callers treat as
 * "skip".
 *
 * The kept page is deliberately never freed. These suites call this a handful
 * of times per boot and the process exits straight after; reclaiming it would
 * invalidate the address the caller is still holding.
 */
void *test_unmapped_user_addr(void)
{
    char *p = mmap(NULL, (size_t) 2 * PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, NOFD, 0);

    if (p == MAP_FAILED)
        return NULL;
    /* PROT_NONE rather than munmap: every access still faults, but the address
     * stays reserved. A truly unmapped hole is a free slot for the next mmap, and
     * thread stacks, the allocator, or a later test would be handed this very
     * page, turning a "refused" pointer into a valid one and making the case
     * flaky (thread_join consumed its target on the first, "refused", call).
     */
    if (mprotect(p + PAGE, (size_t) PAGE, PROT_NONE) == -1) {
        (void) munmap(p, (size_t) 2 * PAGE);
        return NULL;
    }

    return p + PAGE;
}

/* The WR_PROBE_OPT family: kcalls that paired a real WR_VERIFY_PTR with a
 * macro that expanded to nothing, so they read as range-checked and probed
 * while being range-checked only. NULL and every in-range unmapped address
 * passed straight through to a raw store with no fault handler armed. The
 * macro is gone and all nine sites now probe for real; one case per
 * user-reachable site.
 *
 * Every case here passes an in-range unmapped pointer rather than a
 * supervisor-half one on purpose: the range check always caught the latter,
 * which is exactly why the missing probe went unnoticed. `gone` is the only
 * input that tells the two apart.
 */
/* Exits immediately so the join below has a dead, unreaped target. */
static void *join_victim(void *arg)
{
    (void) arg;
    return NULL;
}

/* Stays alive until join_hold is cleared, so a join can be refused repeatedly
 * against a target that cannot have exited or been reaped meanwhile.
 */
static volatile int join_hold;

static void *join_holder(void *arg)
{
    (void) arg;
    while (join_hold)
        sched_yield();
    return NULL;
}

static void probe_opt_cases(void *gone)
{
    if (!gone)
        return;

    /* sched_get and sched_info: plain POSIX readers, no capability needed. */
    errno = 0;
    expect_refused(__sched_get(0, 0, gone), "sched_get unmapped param");
    errno = 0;
    expect_refused(__sched_info(0, 0, gone), "sched_info unmapped info");

    /* clock_time's per-thread running-time branch, reached through the
     * SYNC-encoded clock id that __clock_id returns. Its output pointer is the
     * one the RCU-section banner used to argue about.
     */
    errno = 0;
    expect_refused(
        __clock_time((clockid_t) (unsigned) __clock_id(0, 1), NULL, gone),
        "clock_time unmapped running-time out");

    /* msg_info with an invalid rcvid still checks the buffer first. */
    errno = 0;
    expect_refused(msg_info(0, gone), "msg_info unmapped info");

    /* connect_server_info / connect_client_info write a _server_info back
     * through the caller's pointer after dropping vec_slock.
     */
    errno = 0;
    expect_refused(connect_server_info(0, 0, gone),
                   "connect_server_info unmapped info");
    errno = 0;
    expect_refused(connect_client_info(0, gone, 0),
                   "connect_client_info unmapped info");

    /* thread_join's status pointer needs a genuinely joinable target. The
     * probe sits after every predicate that can refuse the join and before
     * the target is reserved for destruction, so a bogus tid returns ESRCH
     * without ever reaching it: only a real dead-but-unreaped thread gets
     * there. Joining one with an unmapped status pointer must fail closed and
     * leave the thread joinable, which the successful second join proves.
     *
     * The victim runs on a caller-supplied stack, which is a workaround, not
     * the contract. A thread on a stack the kernel allocated parks at exit
     * while roottask frees that stack (thread_destroy's stack handshake in
     * kernel/thread/lifecycle.c), and the KILLSELF specret that resumes it
     * (kernel/thread/special-return.c) forces DETACHED, so it is reaped as
     * soon as that finishes. A pthread_join that arrives later then returns
     * ESRCH, which POSIX does not allow for a terminated, unjoined thread. This
     * test cannot depend on that timing, so it avoids the handshake; a joinable
     * thread on a kernel-allocated stack is not covered here.
     */
    {
        enum { VICTIM_STACK = 64 * 1024 };
        pthread_attr_t attr;
        pthread_t victim;
        int rc;
        void *stack = mmap(NULL, VICTIM_STACK, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, NOFD, 0);

        if (stack == MAP_FAILED) {
            test_fail("thread_join unmapped status", "victim stack mmap");
        } else if ((rc = pthread_attr_init(&attr)) != EOK ||
                   (rc = pthread_attr_setstack(&attr, stack, VICTIM_STACK)) !=
                       EOK ||
                   (rc = pthread_create(&victim, &attr, join_victim, NULL)) !=
                       EOK) {
            test_fail("thread_join unmapped status", strerror(rc));
        } else {
            void *status = NULL;

            errno = 0;
            expect_refused((int) thread_join((int) victim, gone),
                           "thread_join unmapped status");
            if (thread_join((int) victim, &status) == 0) {
                test_ok("thread_join still joinable after refused status");
            } else {
                test_fail("thread_join still joinable after refused status",
                          strerror(errno));
            }
        }
        if (stack != MAP_FAILED)
            (void) munmap(stack, VICTIM_STACK);
    }
}

/* The rest of the user-pointer idiom: kcalls that range-checked a caller
 * pointer and then read or wrote through it with no access probe, or read it
 * before the check ran at all. Every one of these was an unprivileged kernel
 * fault of the same class as timer_settime's, and channel_create's was a
 * disclosure on top: it probed the pointer without range-checking it, so a
 * mapped supervisor address was copied into a channel the caller reads back.
 *
 * gone may be NULL (the mapping could not be arranged); the unmapped cases are
 * then skipped and the kernel-range ones still run.
 */
static void other_user_ptr_cases(void *gone)
{
    /* sigevent readers. Both dereference kap->event before doing anything with
     * the rcvid, so the rcvid here only has to be syntactically acceptable.
     */
    errno = 0;
    expect_refused(msg_verify_event(0, NULL), "msg_verify_event null event");
    errno = 0;
    expect_refused(
        msg_verify_event(0, (const struct sigevent *) TEST_KERNEL_ADDR),
        "msg_verify_event kernel-range event");
    errno = 0;
    expect_refused(msg_deliver_event(0, NULL), "msg_deliver_event null event");
    if (gone) {
        errno = 0;
        expect_refused(msg_verify_event(0, (const struct sigevent *) gone),
                       "msg_verify_event unmapped event");
        errno = 0;
        expect_refused(msg_deliver_event(0, (const struct sigevent *) gone),
                       "msg_deliver_event unmapped event");
    }

    /* Clock setters. The RTMUX_PRIV_CAP_TIME check runs after the read, so an
     * unprivileged caller that will be refused EPERM still gets its pointer
     * dereferenced first, which is exactly why the probe has to be there.
     */
    if (gone) {
        errno = 0;
        expect_refused(__clock_time(CLOCK_REALTIME, gone, NULL),
                       "clock_time unmapped new-value");
        errno = 0;
        expect_refused(__clock_period(CLOCK_REALTIME, gone, NULL, 0),
                       "clock_period unmapped new-value");
    }

    /* thread_ctl's runmask get-and-set reads tr->size straight after the range
     * check.
     */
    errno = 0;
    expect_refused(thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, NULL),
                   "thread_ctl runmask null data");
    if (gone) {
        /* Place the struct so its header is readable and its trailing mask
         * words fall in the unmapped page, which is the extent the kernel
         * probes.
         */
        struct _thread_runmask *rm =
            (struct _thread_runmask *) ((char *) gone - sizeof(*rm));

        errno = 0;
        expect_refused(
            thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, gone),
            "thread_ctl runmask unmapped data");
        errno = 0;
        expect_refused(thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, rm),
                       "thread_ctl runmask unmapped mask words");
    }

    /* channel_create's global path. No libc wrapper passes the event/cred
     * slots, so trap directly: a0 flags, a1 cap_out, a2 bufsize, a3 maxbuf,
     * a4 event, a5 cred. A supervisor address in either slot must be refused
     * before it is copied into the channel object, and the channel must not
     * be created, since a success here would both leak kernel memory and
     * leave a global channel behind.
     */
    if (test_kcall6(__KSYS_CHANNEL_CREATE, (long) RTMUX_FLG_CHF_GLOBAL, 0, 0, 0,
                    (long) TEST_KERNEL_ADDR, 0) < 0) {
        test_ok("channel_create kernel-range event");
    } else {
        test_fail("channel_create kernel-range event",
                  "kernel accepted a supervisor-half sigevent");
    }
    if (test_kcall6(__KSYS_CHANNEL_CREATE, (long) RTMUX_FLG_CHF_GLOBAL, 0, 0, 0,
                    0, (long) TEST_KERNEL_ADDR) < 0) {
        test_ok("channel_create kernel-range cred");
    } else {
        test_fail("channel_create kernel-range cred",
                  "kernel accepted a supervisor-half cred");
    }
    if (gone) {
        if (test_kcall6(__KSYS_CHANNEL_CREATE, (long) RTMUX_FLG_CHF_GLOBAL, 0,
                        0, 0, (long) (uintptr_t) gone, 0) < 0) {
            test_ok("channel_create unmapped event");
        } else {
            test_fail("channel_create unmapped event",
                      "kernel accepted an unmapped sigevent");
        }
    }
}

/* user_slice rejection rules on converted calls (make check-user-slice models
 * the same rules). The user half ends at GIG(128) - 1 on Sv39: a buffer that
 * starts below it and ends above it, and one whose end wraps past UINTPTR_MAX,
 * must each be refused by the range rule alone, before any access.
 */
static void user_slice_cases(void *gone)
{
    void *straddle = (void *) (uintptr_t) ((1UL << 37) - 4);
    void *wrap = (void *) (uintptr_t) -4L;
    struct _clockperiod cp;

    errno = 0;
    expect_refused(__sched_info(0, 0, straddle),
        "sched_info boundary-straddling info");
    errno = 0;
    expect_refused(__sched_info(0, 0, wrap), "sched_info wrapping info");
    errno = 0;
    expect_refused(__clock_time(CLOCK_REALTIME, wrap, NULL),
        "clock_time wrapping new-value");
    errno = 0;
    expect_refused(__clock_time(CLOCK_REALTIME, NULL, straddle),
        "clock_time boundary-straddling old-value");
    errno = 0;
    expect_refused(__clock_period(CLOCK_REALTIME, straddle, NULL, 0),
        "clock_period boundary-straddling new-value");
    if (gone) {
        errno = 0;
        expect_refused(__clock_period(CLOCK_REALTIME, NULL, gone, 0),
            "clock_period unmapped old-value");
        errno = 0;
        expect_refused(__clock_time(CLOCK_REALTIME, NULL, gone),
            "clock_time unmapped old-value");
    }

    /* A valid buffer must still work: the copy-out lands. */
    memset(&cp, 0xa5, sizeof cp);
    if (__clock_period(CLOCK_REALTIME, NULL, &cp, 0) == 0 && cp.nsec != 0)
        test_ok("clock_period valid old-value copied out");
    else
        test_fail("clock_period valid old-value copied out", strerror(errno));
}

/* The thread calls converted to user_slice (thread_create attr, thread_join
 * status, thread_ctl runmask / data words / name / destroy stats). Each is
 * refused on range alone (wrap, boundary straddle, supervisor half) or on an
 * unmapped page, and the refused name write must leave the thread name alone.
 */
static void thread_slice_cases(void *gone)
{
    void *straddle = (void *) (uintptr_t) ((1UL << 37) - 4);
    void *wrap = (void *) (uintptr_t) -4L;
    void *bad[] = {wrap, straddle, (void *) TEST_KERNEL_ADDR, gone};
    static const char *const label[] = {"wrapping", "boundary-straddling",
                                        "kernel-range", "unmapped"};
    char msg[96];
    pthread_t victim;

    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (!bad[i])
            continue;
        errno = 0;
        snprintf(msg, sizeof msg, "thread_create %s attr", label[i]);
        expect_refused(__thread_create(0, NULL, NULL, bad[i]), msg);

        errno = 0;
        snprintf(msg, sizeof msg, "thread_ctl runmask-get-and-set %s data",
                 label[i]);
        expect_refused(thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET, bad[i]), msg);

        errno = 0;
        snprintf(msg, sizeof msg, "thread_ctl runmask-inherit %s data",
                 label[i]);
        expect_refused(
            thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, bad[i]), msg);

        errno = 0;
        snprintf(msg, sizeof msg, "thread_ctl align_fault %s data", label[i]);
        expect_refused(thread_ctl(RTMUX_TCTL_ALIGN_FAULT, bad[i]), msg);

        errno = 0;
        snprintf(msg, sizeof msg, "thread_ctl name %s data", label[i]);
        expect_refused(thread_ctl(RTMUX_TCTL_NAME, bad[i]), msg);

        /* Needs euid 0; an unprivileged caller is refused EPERM, which the
         * check below does not accept, so only count it when root.
         */
        if (getuid() == 0) {
            errno = 0;
            snprintf(msg, sizeof msg, "thread_ctl destroy_stats %s data",
                     label[i]);
            expect_refused(thread_ctl(RTMUX_TCTL_THREAD_DESTROY_STATS, bad[i]),
                           msg);
        }
    }

    /* thread_join status: a live target, refused three ways, then released and
     * joined with a valid pointer. The target is held alive on purpose: an
     * unjoined zombie can disappear between calls (Q40(a)), which would fail
     * the final join for a reason unrelated to the status checks. The status
     * check runs before the target's state is consulted, so the live path
     * exercises it the same way.
     */
    join_hold = 1;
    if (pthread_create(&victim, NULL, join_holder, NULL) != EOK) {
        test_fail("thread_join slice victim", strerror(errno));
    } else {
        void *status = NULL;

        for (unsigned i = 0; i < 3; i++) {
            errno = 0;
            snprintf(msg, sizeof msg, "thread_join %s status", label[i]);
            expect_refused((int) thread_join((int) victim, bad[i]), msg);
        }
        join_hold = 0;
        if (thread_join((int) victim, &status) == 0)
            test_ok("thread_join joinable after slice-refused status");
        else
            test_fail("thread_join joinable after slice-refused status",
                      strerror(errno));
    }

    /* thread_ctl name. A valid set and get round-trip first, then sets whose
     * output extent reaches into the unmapped page: they must be refused
     * before the name is replaced, so a get still returns the earlier name.
     */
    {
        union {
            struct _thread_name hdr;
            char raw[sizeof(struct _thread_name) + 16];
        } nb;
        const size_t off = offsetof(struct _thread_name, name_buf);
        char prev[8];

        memset(&nb, 0, sizeof nb);
        nb.hdr.new_name_len = 4;
        nb.hdr.name_buf_len = 8;
        memcpy(nb.raw + off, "abcd", 4);
        errno = 0;
        if (thread_ctl(RTMUX_TCTL_NAME, &nb) == 0)
            test_ok("thread_ctl name valid set");
        else
            test_fail("thread_ctl name valid set", strerror(errno));
        memcpy(prev, nb.raw + off, 8); /* the name this call replaced */
        prev[7] = '\0';

        if (gone) {
            /* Header and the four new-name bytes readable; the eight-byte
             * output extent ends in the unmapped page.
             */
            struct _thread_name *edge =
                (struct _thread_name *) ((char *) gone - off - 4);

            edge->new_name_len = 4;
            edge->name_buf_len = 8;
            memcpy((char *) edge + off, "zzzz", 4);
            errno = 0;
            expect_refused(thread_ctl(RTMUX_TCTL_NAME, edge),
                           "thread_ctl name output tail unmapped");
        }

        if (gone) {
            memset(&nb, 0, sizeof nb);
            nb.hdr.new_name_len = -1;
            nb.hdr.name_buf_len = 8;
            errno = 0;
            if (thread_ctl(RTMUX_TCTL_NAME, &nb) == 0 &&
                memcmp(nb.raw + off, "abcd", 5) == 0)
                test_ok("thread_ctl name unchanged after refused set");
            else
                test_fail("thread_ctl name unchanged after refused set",
                          strerror(errno));
        }

        /* Put the thread's earlier name back (the first set returned it). */
        nb.hdr.new_name_len = (int) strlen(prev);
        nb.hdr.name_buf_len = 0;
        memcpy(nb.raw + off, prev, sizeof prev);
        (void) thread_ctl(RTMUX_TCTL_NAME, &nb);
    }
}

/* sync_t user pointers. The kernel probes the caller's sync_t with an atomic
 * RMW (a plain read-back-write would clobber a concurrent owner update), and
 * that probe used to run with no fault handler, so a sync object whose page was
 * unmapped after init was an unprivileged kernel panic on the mutex, condvar,
 * and semaphore paths. Each object is initialized in its own mmap page, the page
 * is unmapped, and the raw kcall must come back EFAULT.
 */
static void sync_unmapped_cases(void)
{
    struct {
        const char *name;
        long call;
        long arg1;
    } cases[] = {
        {"mutex_unmapped_after_init", __KSYS_SYNC_MUTEX_LOCK, 0},
        {"condvar_unmapped_after_init", __KSYS_SYNC_CONDVAR_WAIT, 0},
        {"sem_unmapped_after_init", __KSYS_SYNC_SEM_WAIT, 1},
    };

    /* A misaligned sync_t inside a mapped page: an AMO there is not recovered by
     * the xfer path, so the kernel must refuse it up front.
     */
    {
        char *pg = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, NOFD, 0);

        if (pg != MAP_FAILED) {
            check(test_kcall6(__KSYS_SYNC_MUTEX_LOCK, (long) (pg + 1), 0, 0, 0,
                              0, 0) == -EFAULT,
                  "mutex_misaligned", "expected EFAULT, no kernel fault");
            (void) munmap(pg, PAGE);
        }
    }

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        void *pg = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, NOFD, 0);
        long rc;

        if (pg == MAP_FAILED) {
            test_fail(cases[i].name, "mmap failed");
            continue;
        }
        switch (i) {
        case 0:
            (void) pthread_mutex_init(pg, NULL);
            break;
        case 1:
            (void) pthread_cond_init(pg, NULL);
            break;
        default:
            (void) sem_init(pg, 0, 0);
            break;
        }
        if (munmap(pg, PAGE) == -1) {
            test_fail(cases[i].name, "munmap failed");
            continue;
        }
        /* A condvar wait names a mutex too; pass the same unmapped address so
         * either probe can be the one that fires.
         */
        rc = test_kcall6(cases[i].call, (long) pg,
                         i == 1 ? (long) pg : cases[i].arg1, 0, 0, 0, 0);
        check(rc == -EFAULT, cases[i].name, "expected EFAULT, no kernel fault");
    }
}

/* A thread whose stack and TLS live in a region another thread unmaps. The
 * kernel's pthread_cancel path reads and publishes the target's TLS flags; with
 * the region gone that must come back EFAULT, not fault the kernel. Child half:
 * returns 0 only if pthread_cancel failed with EFAULT; the parked thread is then
 * released onto its unmapped stack, so the process is expected to die with
 * SIGSEGV (checked by the parent).
 */
static sem_t cancel_tls_parked, cancel_tls_go;

static void *cancel_tls_body(void *arg)
{
    (void) arg;
    (void) sem_post(&cancel_tls_parked);
    (void) sem_wait(&cancel_tls_go);
    return NULL;
}

int cancel_tls_child(void)
{
    enum { REGION = 256 * 1024 };
    pthread_attr_t attr;
    pthread_t th;
    void *region;
    int rc;

    region = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, NOFD, 0);
    if (region == MAP_FAILED || sem_init(&cancel_tls_parked, 0, 0) != 0 ||
        sem_init(&cancel_tls_go, 0, 0) != 0)
        return 2;
    if (pthread_attr_init(&attr) != 0 ||
        pthread_attr_setstack(&attr, region, REGION) != 0 ||
        pthread_create(&th, &attr, cancel_tls_body, NULL) != 0)
        return 3;
    (void) sem_wait(&cancel_tls_parked);
    (void) munmap(region, REGION);
    rc = pthread_cancel(th);
    if (rc != EFAULT)
        return 4;
    /* Wake the thread onto its now-unmapped stack: the process must die. */
    (void) sem_post(&cancel_tls_go);
    for (int i = 0; i < 500; i++) {
        struct timespec ts = {.tv_nsec = 10000000L};

        (void) nanosleep(&ts, NULL);
    }
    return 5; /* still alive: the unmapped stack did not fault */
}

static void test_cancel_unmapped_tls(void)
{
    char *argv[] = {"syscall_testing", "--cancel-tls-child", NULL};
    pid_t pid;
    int status = 0;

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0) {
        test_fail("cancel_unmapped_tls spawn", strerror(errno));
        return;
    }
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGSEGV,
          "cancel_unmapped_tls: cancel is refused, then the parked thread dies on its unmapped stack",
          "pthread_cancel on a thread with unmapped TLS must return EFAULT "
          "without a kernel fault");
}

void test_user_ptr_negative(void)
{
    struct sigevent sev;
    struct _itimer it;
    void *gone = test_unmapped_user_addr();
    timer_t id;

    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_NONE;
    id = (timer_t) __timer_create(CLOCK_MONOTONIC, &sev);
    if ((int) id == -1) {
        test_fail("timer_settime user-pointer cases", strerror(errno));
        return;
    }

    /* 1. NULL new-value. timer.c range-checks kap->itime and then probes it
     * with no NULL guard -- the `if (kap->oitime)` guard covers only the output
     * pointer -- and NULL is inside the range the check accepts.
     */
    errno = 0;
    expect_refused(__timer_settime(id, 0, NULL, NULL),
                   "timer_settime null new-value");

    /* 2. In-range but unmapped new-value: passes the range check, so only the
     * probe stands between it and a kernel fault.
     */
    if (gone) {
        errno = 0;
        expect_refused(__timer_settime(id, 0, gone, NULL),
                       "timer_settime unmapped new-value");
    }

    /* 3. Same for the output pointer, which is write-probed. */
    memset(&it, 0, sizeof it);
    if (gone) {
        errno = 0;
        expect_refused(__timer_settime(id, 0, &it, gone),
                       "timer_settime unmapped old-value");
    }

    /* 4. Misaligned but mapped new-value. The probe reads the extent as 32-bit
     * words, and the kernel's xfer recovery only covers page faults
     * (RISCV_EXC_IS_PAGE_FAULT), not RISCV_EXC_LOAD_MISALIGNED. Whether that is
     * reachable depends on the platform handling misaligned accesses in
     * hardware or M-mode: QEMU virt does, so this must simply be accepted or
     * rejected without taking the machine down. A target that traps misaligned
     * accesses to S-mode would need the recovery widened.
     */
    if (gone) {
        char *odd = (char *) gone - PAGE + 1;

        memset(odd, 0, sizeof it);

        /* Unconditional: accept or reject, either is fine. The assertion is
         * that control reaches the next line at all -- a kernel that trapped
         * the misaligned access would not. Written as a bare call plus test_ok
         * rather than a predicate, because any predicate over the return value
         * here is vacuously true and would read as a check that is not one.
         */
        (void) __timer_settime(id, 0, (const void *) odd, NULL);
        test_ok("timer_settime misaligned new-value survives");
    }

    /* 5. Supervisor-half pointer must lose on the range check alone. */
    errno = 0;
    expect_refused(
        __timer_settime(id, 0, (const void *) TEST_KERNEL_ADDR, NULL),
        "timer_settime kernel-range new-value");

    /* 6. timer_info's output buffer. The kernel used to probe only the otime
     * member and then store the whole struct _timer_info through the pointer,
     * and the store sits after the RCU unlock, far from the probe.
     */
    if (gone) {
        errno = 0;
        expect_refused(__timer_info(0, id, 0, gone),
                       "timer_info unmapped info");
    }
    errno = 0;
    expect_refused(__timer_info(0, id, 0, (void *) TEST_KERNEL_ADDR),
                   "timer_info kernel-range info");

    (void) __timer_destroy(id);

    other_user_ptr_cases(gone);
    probe_opt_cases(gone);
    user_slice_cases(gone);
    thread_slice_cases(gone);
    sync_unmapped_cases();
    test_cancel_unmapped_tls();
}
