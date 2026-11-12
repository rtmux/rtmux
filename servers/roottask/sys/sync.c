/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <pthread.h>
#include <stdalign.h>
#include <time.h>
#include <devctl/common.h>
#include <sys/iomsg.h>
#include "root-log.h"

/* Budget for one mount: its connect, DEVCTL_FS_SYNC and close share it. */
#define SYNC_TIMEOUT_MS 2000

/* Cap on the whole sweep, so several wedged mounts cannot stall shutdown for
 * SYNC_MAX_TARGETS * SYNC_TIMEOUT_MS. Mounts not yet visited at the cap are
 * logged and skipped.
 */
#define SYNC_SWEEP_MAX_MS 8000

static uint64_t sync_now_ns(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Bound the next send/reply block of this thread by the sweep deadline. */
static void sync_arm(uint64_t deadline)
{
    (void) __timer_timeout(
        CLOCK_MONOTONIC,
        TIMER_ABSTIME | RTMUX_TIMEOUT_SEND | RTMUX_TIMEOUT_REPLY, NULL,
        &deadline, NULL);
}

static void sync_disarm(void)
{
    (void) __timer_timeout(CLOCK_MONOTONIC, 0, NULL, NULL, NULL);
}

static int sync_timed_out(uint64_t deadline, int err)
{
    return err == ETIMEDOUT || (err == EINTR && sync_now_ns() >= deadline);
}

/* Flush one mount: connect, open its root read-only, DEVCTL_FS_SYNC, close.
 * Return value: EOK or ENOSYS (counted as success), ETIMEDOUT when the sweep
 * deadline passed, otherwise the errno of the failing step.
 */
static int sync_one(const struct sync_target *t, uint64_t deadline)
{
    struct {
        struct _io_connect c;
        char reply[256];
    } m;
    int fd, ret, err;

    fd = connect_attach(t->nd, t->pid, t->chid, 0, 0);
    if (fd == -1)
        return errno;

    memset(&m, 0, sizeof m);
    m.c.type = _IO_CONNECT;
    m.c.subtype = _IO_CONNECT_OPEN;
    m.c.file_type = _FTYPE_ANY;
    m.c.reply_max = sizeof m.reply;
    m.c.entry_max = 1;
    m.c.ioflag = _IO_FLAG_RD;
    m.c.access = _IO_FLAG_RD | _IO_FLAG_WR;
    m.c.path_len = 1; /* the root of the mount: "" + NUL */

    sync_arm(deadline);
    /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
    ret = msg_sendnc(fd, &m.c, sizeof m.c, m.reply, sizeof m.reply);
    err = errno;
    sync_disarm();
    if (ret == -1 || ((unsigned) ret & _IO_CONNECT_RET_FLAG)) {
        connect_detach_r((uint64_t) fd);
        if (ret != -1)
            return ENOSYS;
        return sync_timed_out(deadline, err) ? ETIMEDOUT : err;
    }

    sync_arm(deadline);
    ret = _devctl(fd, DEVCTL_FS_SYNC, NULL, 0, 0);
    err = ret == -1 ? errno : EOK;
    sync_disarm();
    if (ret == -1 && sync_timed_out(deadline, err)) {
        /* close() would wait on the same unresponsive server. */
        connect_detach_r((uint64_t) fd);
        return ETIMEDOUT;
    }

    sync_arm(deadline);
    (void) close(fd);
    sync_disarm();
    return err;
}

/* sync_one() bounds itself with kernel send/reply timeouts, but the kernel
 * keeps a reply-blocked client until the server has seen the unblock request,
 * so a server with no free dispatch thread holds it past the deadline. Run each
 * mount from a short-lived detached worker and wait for it with a timed wait;
 * past the deadline plus SYNC_GRACE_MS the worker is abandoned (it frees itself
 * if it ever returns) and the sweep moves on.
 */
#define SYNC_GRACE_MS 250
#define SYNC_WORKER_STACK 16384

/* pthread_create() cannot get a stack from inside a roottask worker, so the
 * workers run on static (BSS) stacks as the heartbeat thread does. A slot is
 * handed out round robin and held until its worker returns, so a wedged worker
 * keeps its slot and an exiting one is not handed out again straight away.
 */
static alignas(16) char worker_stack[SYNC_MAX_TARGETS][SYNC_WORKER_STACK];
static unsigned char slot_busy[SYNC_MAX_TARGETS];
static unsigned slot_next;

struct sync_job {
    struct sync_target target;
    uint64_t deadline;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int state; /* 0 running, 1 done, 2 abandoned */
    int err;
    unsigned slot;
};

/* Targets with an abandoned worker still blocked; each sweep skips them rather
 * than parking another thread behind the same wedged server.
 */
static pthread_mutex_t stuck_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    pid_t pid;
    int chid;
    uint32_t nd;
} stuck[SYNC_MAX_TARGETS];
static size_t nstuck;

static int stuck_find(const struct sync_target *t)
{
    size_t i;

    for (i = 0; i < nstuck; i++)
        if (stuck[i].pid == t->pid && stuck[i].chid == t->chid &&
            stuck[i].nd == t->nd)
            return (int) i;
    return -1;
}

static void stuck_clear(const struct sync_target *t)
{
    int i;

    pthread_mutex_lock(&stuck_mutex);
    i = stuck_find(t);
    if (i >= 0)
        stuck[i] = stuck[--nstuck];
    pthread_mutex_unlock(&stuck_mutex);
}

static void slot_release(unsigned slot)
{
    pthread_mutex_lock(&stuck_mutex);
    slot_busy[slot] = 0;
    pthread_mutex_unlock(&stuck_mutex);
}

static int slot_take(unsigned *slot)
{
    unsigned n;
    int ok = 0;

    pthread_mutex_lock(&stuck_mutex);
    for (n = 0; n < SYNC_MAX_TARGETS; n++) {
        unsigned s = (slot_next + n) % SYNC_MAX_TARGETS;

        if (!slot_busy[s]) {
            slot_busy[s] = 1;
            slot_next = s + 1;
            *slot = s;
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&stuck_mutex);
    return ok;
}

static void *sync_worker(void *arg)
{
    struct sync_job *j = arg;
    unsigned slot = j->slot;
    int err = sync_one(&j->target, j->deadline), abandoned;

    pthread_mutex_lock(&j->lock);
    abandoned = j->state == 2;
    j->err = err;
    if (!abandoned) {
        j->state = 1;
        pthread_cond_signal(&j->cond);
    }
    pthread_mutex_unlock(&j->lock);
    if (abandoned) {
        stuck_clear(&j->target);
        free(j);
    }
    slot_release(slot);
    /* A __thread_create thread has no return address: end it explicitly. */
    (void) thread_destroy_r(0, -1, NULL);
    return NULL;
}

/* Like sync_one(), but returns ETIMEDOUT by the deadline plus grace even when
 * the server never lets the blocked send go.
 */
static int sync_one_bounded(const struct sync_target *t, uint64_t deadline)
{
    struct sync_job *j;
    struct _thread_attr attr;
    pthread_condattr_t cattr;
    struct timespec ts;
    uint64_t until = deadline + (uint64_t) SYNC_GRACE_MS * 1000000ULL;
    int err, rc;

    pthread_mutex_lock(&stuck_mutex);
    rc = stuck_find(t);
    pthread_mutex_unlock(&stuck_mutex);
    if (rc >= 0)
        return ETIMEDOUT; /* an earlier worker is still wedged on this server */

    j = calloc(1, sizeof *j);
    if (!j)
        return sync_one(t, deadline);
    if (!slot_take(&j->slot)) {
        free(j);
        return sync_one(t, deadline);
    }
    j->target = *t;
    j->deadline = deadline;
    pthread_mutex_init(&j->lock, NULL);
    pthread_condattr_init(&cattr);
    (void) pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&j->cond, &cattr);
    memset(&attr, 0, sizeof attr);
    attr.stackaddr = worker_stack[j->slot];
    attr.stacksize = SYNC_WORKER_STACK;
    attr.flags = PTHREAD_CREATE_DETACHED;
    if (__thread_create(0, sync_worker, j, &attr) < 0) {
        slot_release(j->slot);
        free(j);
        return sync_one(t, deadline);
    }

    ts.tv_sec = (time_t) (until / 1000000000ULL);
    ts.tv_nsec = (long) (until % 1000000000ULL);
    pthread_mutex_lock(&j->lock);
    while (j->state == 0 && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&j->cond, &j->lock, &ts);
    if (j->state == 1) {
        err = j->err;
        pthread_mutex_unlock(&j->lock);
        free(j);
        return err;
    }
    j->state = 2;
    pthread_mutex_unlock(&j->lock);
    pthread_mutex_lock(&stuck_mutex);
    if (nstuck < SYNC_MAX_TARGETS) {
        stuck[nstuck].pid = t->pid;
        stuck[nstuck].chid = t->chid;
        stuck[nstuck].nd = t->nd;
        nstuck++;
    }
    pthread_mutex_unlock(&stuck_mutex);
    return ETIMEDOUT;
}

/* One sweep over the mounts. Each mount gets its own SYNC_TIMEOUT_MS budget
 * (never more than what is left of SYNC_SWEEP_MAX_MS), and a mount that times
 * out is logged once and the sweep carries on, so a wedged mount early in the
 * walk cannot starve a healthy one after it. A server that never takes the
 * kernel's unblock request cannot hold the sweep either: sync_one_bounded()
 * abandons it.
 */
static void sync_sweep(void)
{
    struct sync_target targets[SYNC_MAX_TARGETS];
    uint64_t start, cap, end;
    size_t n, i;
    int reserved = 1;

    if (proc_thread_pool_reserve() != 0) {
        /* Retry once, then sweep on this worker: a flush without the spare only
         * risks a server that calls back into roottask, and every send below is
         * still timed.
         */
        struct timespec ts = {0, 10000000};

        (void) nanosleep(&ts, NULL);
        if (proc_thread_pool_reserve() != 0) {
            root_log("sync: no spare worker, sweeping inline\n");
            reserved = 0;
        }
    }
    root_log("sync: start\n");
    start = sync_now_ns();
    cap = start + (uint64_t) SYNC_SWEEP_MAX_MS * 1000000ULL;
    n = namespace_sync_snapshot(targets, SYNC_MAX_TARGETS);
    for (i = 0; i < n; i++) {
        uint64_t now = sync_now_ns(), deadline;
        int err;

        if (now >= cap) {
            root_log("sync: budget exhausted, %d mount(s) skipped\n",
                     (int) (n - i));
            break;
        }
        deadline = now + (uint64_t) SYNC_TIMEOUT_MS * 1000000ULL;
        if (deadline > cap)
            deadline = cap;
        err = sync_one_bounded(&targets[i], deadline);
        if (err == ETIMEDOUT)
            root_log("sync timeout: %s\n", targets[i].label);
        else if (err != EOK && err != ENOSYS)
            root_log("sync: %s: error %d\n", targets[i].label, err);
    }
    end = sync_now_ns();
    root_log("sync: done t=%d\n", (int) ((end - start) / 1000000ULL));
    if (reserved)
        proc_thread_pool_reserve_done();
}

/* Callers of an unprivileged sync() allowed to block behind a sweep. Each one
 * holds a roottask worker for up to a sweep plus the next one (2 * 8 s), so the
 * cap keeps a sync() loop from occupying every worker. Waiters all share the
 * same next sweep, so more than this adds no coverage.
 */
#define SYNC_MAX_WAITERS 2

static pthread_mutex_t sweep_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sweep_cond = PTHREAD_COND_INITIALIZER;
static unsigned sweeps_started, sweeps_done;
static unsigned sync_waiters;
static int sweep_running;

/* Ask every mounted server to flush to stable storage.
 *
 * One sweep runs at a time. A caller that arrives during a sweep waits and
 * shares the next one (it must start after the caller's own writes, so the
 * sweep in flight does not count), so a sync() loop costs one reserved worker
 * and at most one extra sweep, not a thread per call.
 *
 * bounded is set for the unprivileged _SYS_CMD_SYNC: once SYNC_MAX_WAITERS
 * callers already wait on the in-flight sweep, a further caller gets EBUSY at
 * once instead of taking another worker. sync() has no failure indication, so
 * the refused caller simply is not covered by a sweep; a repeat call is. The
 * privileged reboot/shutdown path passes 0 and is never refused.
 * Return value: EOK once a sweep that started after the call has finished,
 * EBUSY when a bounded caller was turned away.
 */
int root_sync_all(int bounded)
{
    unsigned want;
    int waited = 0;

    pthread_mutex_lock(&sweep_mutex);
    want = sweeps_started + 1;
    while (sweeps_done < want) {
        if (!sweep_running) {
            sweep_running = 1;
            sweeps_started++;
            pthread_mutex_unlock(&sweep_mutex);
            sync_sweep();
            pthread_mutex_lock(&sweep_mutex);
            sweeps_done++;
            sweep_running = 0;
            pthread_cond_broadcast(&sweep_cond);
        } else {
            /* A caller that already waited keeps its place when woken. */
            if (bounded && !waited && sync_waiters >= SYNC_MAX_WAITERS) {
                pthread_mutex_unlock(&sweep_mutex);
                return EBUSY;
            }
            waited = 1;
            sync_waiters++;
            pthread_cond_wait(&sweep_cond, &sweep_mutex);
            sync_waiters--;
        }
    }
    pthread_mutex_unlock(&sweep_mutex);
    return EOK;
}
