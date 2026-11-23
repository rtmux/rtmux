/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-mutex.h"
#include "root-process.h"
#include <sys/mman.h> /* PROT_* for roottask_mmap_anon_user */
#include <sys/spawn-limits.h>

/* Loader contexts come from the roottask_mmap_anon_user window, which only
 * grows (kernel/ext/query.c, root_phys_map_va), so one can never be returned:
 * each is kept for reuse by its size class. Each is also one contiguous run of
 * physical memory. So that clients cannot make roottask keep ever more of it
 *, a class has a limit on how many contexts it may ever have, and a
 * spawn that finds its class used up waits for one to come back.
 */
static pthread_mutex_t loader_context_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Largest loader context: a spawn needing more is refused rather than kept.
 * No request that passes the checks of proc_pspawn() and
 * proc_spawn_with_actions() needs more. A native request's context holds the
 * head (descriptor map, search path, cwd and path) without the arguments,
 * unless the whole request fit the receive buffer, which is far smaller.
 * A message rebuilt from a posix_spawn request is kept whole: with legacy file
 * actions it has a full descriptor map, a cwd, a path and ARG_MAX of
 * arguments; with packed ones the path, the arguments and the action records
 * all come out of one PSPAWN_MSG_MAX, and a bitmap of source descriptors
 * follows.
 */
#define LOADER_CLASS_MAX (512u * 1024u)
#define LOADER_CONTEXT_HEAD \
    (offsetof(struct loader_context, msg) + sizeof(struct _proc_spawn))
_Static_assert(LOADER_CONTEXT_HEAD + UINT16_MAX * sizeof(int32_t) +
                       2u * UINT16_MAX + ARG_MAX <=
                   LOADER_CLASS_MAX,
               "a rebuilt spawn message with a full fd map must fit the "
               "largest loader context");
_Static_assert(LOADER_CONTEXT_HEAD + UINT16_MAX * sizeof(int32_t) +
                       3u * UINT16_MAX <=
                   LOADER_CLASS_MAX,
               "the head of a native spawn request must fit the largest "
               "loader context");
_Static_assert(LOADER_CONTEXT_HEAD + PSPAWN_MSG_MAX + UINT16_MAX / 8u + 1u <=
                   LOADER_CLASS_MAX,
               "a spawn with packed file actions must fit the largest loader "
               "context");

/* Size classes. The client sets the message size, so exact sizes would let a
 * stream of ever-larger spawns map a new context each time. The standard
 * context (most spawns) is class 0 and each class doubles the one before, the
 * last being LOADER_CLASS_MAX itself. A request takes a context of its own
 * class only, so small spawns cannot use up the few large ones.
 *
 * `limit` is the most contexts the class may ever have: enough standard ones
 * for a storm of concurrent spawns, and a few of each larger class, whose
 * users (large argument lists, long descriptor maps) are rare and can queue.
 * All classes together can come to 4,653,056 bytes (docs/kstats.md,
 * roottask.loader_context_bytes).
 */
#define LOADER_CLASSES 6
#define LOADER_LIMIT_0 64
#define LOADER_LIMIT_1 8
#define LOADER_LIMIT_2 8
#define LOADER_LIMIT_3 4
#define LOADER_LIMIT_4 2
#define LOADER_LIMIT_5 2
#define LOADER_CONTEXTS_MAX                                         \
    (LOADER_LIMIT_0 + LOADER_LIMIT_1 + LOADER_LIMIT_2 + LOADER_LIMIT_3 + \
     LOADER_LIMIT_4 + LOADER_LIMIT_5)
static struct {
    struct loader_context *free;
    unsigned made;        /* contexts mapped so far, free or in use */
    const unsigned limit; /* the most `made` may reach */
} loader_class[LOADER_CLASSES] = {
    {.limit = LOADER_LIMIT_0}, {.limit = LOADER_LIMIT_1},
    {.limit = LOADER_LIMIT_2}, {.limit = LOADER_LIMIT_3},
    {.limit = LOADER_LIMIT_4}, {.limit = LOADER_LIMIT_5},
};

/* A spawner may hold at most half of a class's contexts, rounded up. A
 * loader can block for as long as some server does not answer it, which may be
 * the spawner itself; without a share, one client whose loaders block on its
 * own server held every standard context, and every other spawn in the system
 * waited out LOADER_CONTEXT_WAIT_S and failed. A spawn over its share waits as
 * one over the class limit does. Each in-use context is counted in the entry
 * of its owner, so LOADER_CONTEXTS_MAX entries always suffice; an entry with
 * nothing held is free.
 */
static struct loader_owner {
    pid_t pid;
    unsigned char held[LOADER_CLASSES];
} loader_owner[LOADER_CONTEXTS_MAX];

static unsigned loader_class_share(unsigned k)
{
    return (loader_class[k].limit + 1) / 2;
}

static bool loader_owner_idle(const struct loader_owner *o)
{
    for (unsigned k = 0; k < LOADER_CLASSES; k++)
        if (o->held[k])
            return false;
    return true;
}

/* The entry of `pid`, or with `make` a free one for it; NULL if there is
 * none. Called with loader_context_mutex held.
 */
static struct loader_owner *loader_owner_of(pid_t pid, bool make)
{
    struct loader_owner *idle = NULL;

    for (unsigned i = 0; i < LOADER_CONTEXTS_MAX; i++) {
        struct loader_owner *o = &loader_owner[i];

        if (loader_owner_idle(o)) {
            if (!idle)
                idle = o;
        } else if (o->pid == pid) {
            return o;
        }
    }
    if (!make)
        return NULL;
    if (idle)
        idle->pid = pid;
    return idle;
}

/* Give back one context of class k held by `pid`. Called with
 * loader_context_mutex held.
 */
static void loader_owner_put(pid_t pid, unsigned k)
{
    struct loader_owner *o = loader_owner_of(pid, false);

    CRASHCHECK(!o || !o->held[k]);
    o->held[k]--;
}
_Static_assert((sizeof(struct loader_context) << (LOADER_CLASSES - 2)) <
                       LOADER_CLASS_MAX &&
                   (sizeof(struct loader_context) << (LOADER_CLASSES - 1)) >=
                       LOADER_CLASS_MAX,
               "LOADER_CLASSES must end exactly at LOADER_CLASS_MAX");

/* A spawn may wait for a context of its class, on a pool thread, while those
 * in use are given back by the loaders that hold them, in arrival order among
 * the waiters of its class (a root_ticket keyed by the class; one that finds
 * no wait left takes a free context rather than fail). No more than
 * LOADER_CONTEXT_WAITERS wait at once (see PSPAWN_GATE_WAITERS for the share
 * of the pool) and none for longer than LOADER_CONTEXT_WAIT_S seconds; beyond
 * either the spawn fails with EAGAIN. All classes share the wait: a context
 * given back wakes every waiter, each to look at its own class.
 */
#define LOADER_CONTEXT_WAITERS 16
#define LOADER_CONTEXT_WAIT_S 30
static struct root_wait loader_context_wait =
    ROOT_WAIT_INITIALIZER(LOADER_CONTEXT_WAITERS);

/* Spawns over their spawner's share wait apart, LOADER_SHARE_WAITERS at most,
 * so that one spawner's backlog cannot take the waits of the others.
 */
#define LOADER_SHARE_WAITERS 8
static struct root_wait loader_share_wait =
    ROOT_WAIT_INITIALIZER(LOADER_SHARE_WAITERS);

/* posix_spawn requests that left the heap gate to wait for their share
 * (loader_share_await) wait on their own, LOADER_SHARE_WAITERS at most too:
 * otherwise they would take the waits of the requests the gate let in, which
 * then failed with EAGAIN at once (one spawner's ARG_MAX burst did).
 */
static struct root_wait loader_defer_wait =
    ROOT_WAIT_INITIALIZER(LOADER_SHARE_WAITERS);

static unsigned loader_class_of(size_t size);

/* Every context ever mapped, for the hold bound below. */
static struct loader_context *loader_all[LOADER_CONTEXTS_MAX];
static unsigned loader_all_n;

/* A loader may block for as long as a server does not answer it, holding its
 * context all the while. A context held longer than LOADER_HOLD_MAX_S while
 * another spawn waits for one of its class has its loader interrupted:
 * the call it is blocked in fails with EINTR, the loader gives up, and the
 * context comes back. Loading takes milliseconds; a loader that has held one
 * this long is stuck, most likely on a server that does not answer. The
 * interrupt breaks a send, a receive and a reply wait (STATE_INTERRUPT_BITS),
 * but a server that holds a reply and ignores the unblock still holds it.
 */
#define LOADER_HOLD_MAX_S 10

/* Interrupt the loader of pid, thread tid, if pid is still loading with
 * the context lcp.
 */
static void loader_interrupt(pid_t pid, int tid, const struct loader_context *lcp)
{
    root_proc_info_t qinfo;
    process_t *prp;
    static atomic_uint nnote;
    unsigned n;
    int rc = -1;

    if (!(prp = root_proc_lock(pid)))
        return;
    if (process_query_info(pid, &qinfo) == EOK &&
        (qinfo.flags & RTMUX_FLG_PROC_LOADING) && qinfo.lcp == lcp) {
        rc = kext_thread_ctl_call(prp, tid, RTMUX_TCTL_INTERRUPT,
                                  (void *) (intptr_t) tid);
        if ((n = root_warn_due(&nnote)))
            root_warn("spawn: loader of pid %d held its context over %d s; "
                      "interrupted (%u, status %d)\n",
                      (int) pid, LOADER_HOLD_MAX_S, n, rc);
    }
    root_proc_unlock(pid);
}

/* Wait on w for a context of class k, until *until (ns, set on the first
 * call), holding loader_context_mutex. Before waiting, interrupt the loaders
 * that have held a context of class k past LOADER_HOLD_MAX_S (only those
 * counted against `owner` when it is not 0: a wait for a share ends only when
 * one of its own comes back), and wait no longer than until the next one gets
 * there. Returns 0 to look again, ETIMEDOUT at the deadline, or EAGAIN with
 * the wait full.
 */
static int loader_wait(struct root_wait *w, unsigned k, pid_t owner,
                       uint64_t *until)
{
    const uint64_t hold = (uint64_t) LOADER_HOLD_MAX_S * 1000000000u;
    struct {
        struct loader_context *lcp;
        pid_t pid;
        int tid;
    } stuck[LOADER_CONTEXTS_MAX];
    uint64_t now = root_now_ns(), slice;
    unsigned nstuck = 0;
    int r;

    if (root_deadline_ns(until, LOADER_CONTEXT_WAIT_S))
        return EAGAIN;
    slice = *until;
    for (unsigned i = 0; i < loader_all_n; i++) {
        struct loader_context *lcp = loader_all[i];
        uint64_t since = __atomic_load_n(&lcp->ctx.held_since, __ATOMIC_ACQUIRE);
        /* Written by the spawner and the loader without this mutex: a value
         * not there yet is seen at the next look.
         */
        pid_t pid = __atomic_load_n(&lcp->ctx.pid, __ATOMIC_RELAXED);
        int tid = __atomic_load_n(&lcp->ctx.tid, __ATOMIC_RELAXED);
        uint64_t due;

        if (!since || loader_class_of(lcp->ctx.size) != k || pid <= 0 || tid <= 0)
            continue;
        if (owner && lcp->ctx.owner != owner)
            continue;
        due = since + hold;
        if (lcp->ctx.interrupt_after > due)
            due = lcp->ctx.interrupt_after;
        if (now >= due) {
            /* Not again within a second (the interrupt may also have found
             * it between waits): one that still holds it then is tried
             * again.
             */
            lcp->ctx.interrupt_after = now + 1000000000u;
            stuck[nstuck].lcp = lcp;
            stuck[nstuck].pid = pid;
            stuck[nstuck++].tid = tid;
        } else if (due < slice) {
            slice = due;
        }
    }
    if (nstuck) {
        /* root_proc_lock is not taken under loader_context_mutex. */
        pthread_mutex_unlock(&loader_context_mutex);
        for (unsigned i = 0; i < nstuck; i++)
            loader_interrupt(stuck[i].pid, stuck[i].tid, stuck[i].lcp);
        root_mutex_lock(&loader_context_mutex);
        return 0;
    }
    r = root_wait_until_ns(w, &loader_context_mutex, slice);
    if (r == ETIMEDOUT)
        return slice == *until ? ETIMEDOUT : 0;
    return r;
}

/* A context came back or a claim was given up: both kinds of waiter look
 * again. Called with loader_context_mutex held.
 */
static void loader_context_wake(void)
{
    root_wake(&loader_context_wait);
    root_wake(&loader_share_wait);
    root_wake(&loader_defer_wait);
}

/* Bytes of loader contexts mapped so far (kstat roottask.loader_context_bytes).
 */
static atomic_size_t loader_context_mapped;

size_t loader_context_bytes(void)
{
    return atomic_load_explicit(&loader_context_mapped, memory_order_relaxed);
}

static size_t loader_class_size(unsigned k)
{
    size_t size = sizeof(struct loader_context) << k;

    return size > LOADER_CLASS_MAX ? LOADER_CLASS_MAX : size;
}

/* The class of a context of `size` bytes, which is at most LOADER_CLASS_MAX.
 * Bounded by the last class, so a bad size cannot index past loader_class.
 */
static unsigned loader_class_of(size_t size)
{
    unsigned k = 0;

    while (k < LOADER_CLASSES - 1 && loader_class_size(k) < size)
        k++;
    return k;
}


/* A spawn is about to fail with EAGAIN for want of a loader context, which is
 * not roottask's heap: the context is contiguous physical memory mapped by the
 * kernel (kext_root_mmap_anon_user). `why` names the cause: the error of that
 * mapping (ENOMEM when no contiguous run of `size` bytes is free; kstat
 * pa.largest_free_run), or a class whose contexts stayed in use. Rate limited.
 */
static void loader_context_note(size_t size, const char *why)
{
    static atomic_uint nfail;
    unsigned n = root_warn_due(&nfail);

    if (n)
        root_warn("spawn: no loader context of %zu bytes (failure %u): %s\n",
                  size, n, why);
}

/* loader_context_alloc — obtain a loader context for a spawn.
 *
 * Takes a free context of the request's size class, maps a new one while the
 * class is under its limit (via roottask_mmap_anon_user, so that it lands
 * inside the child-visible L2[3] view where the loader thread, running in the
 * child's pgdir, can dereference it), and otherwise waits for one. It also
 * waits while `owner` holds its share of the class.
 *
 * Parameters:
 *   owner   : the process asking for the spawn
 *   msgsize : message-buffer size the context must hold
 *   state   : initial loader-context state
 *   lcpp    : out — the context
 * Return value: EOK; E2BIG for a message no class holds (the request checks
 * rule that out); EAGAIN when the class, or the owner's share of it, is used
 * up and stays so, or the physical memory for a new context could not be had.
 */
int loader_context_alloc(pid_t owner, unsigned msgsize, int state,
                         struct loader_context **lcpp, int *share_class)
{
    struct loader_context *lcp = NULL;
    struct loader_owner *o;
    struct root_ticket t;
    uint64_t until = 0;
    bool queued = false, fresh = false;
    size_t size;
    unsigned k;
    int r = EOK;

    if (msgsize < sizeof(union proc_msg_union)) {
        msgsize = sizeof(union proc_msg_union);
    }
    size = offsetof(struct loader_context, msg) + msgsize;
    if (size > LOADER_CLASS_MAX) {
        loader_context_note(size, "over the largest class");
        return E2BIG;
    }
    k = loader_class_of(size);
    size = loader_class_size(k);

    root_mutex_lock(&loader_context_mutex);
    while (r == EOK) {
        struct root_wait *w = &loader_context_wait;

        o = loader_owner_of(owner, false);
        if (o && o->held[k] >= loader_class_share(k)) {
            /* Over its share: the class queue is not where it waits. A caller
             * that holds something others need (the heap gate) is told the
             * class instead, to wait without it (loader_share_await).
             */
            if (queued) {
                root_ticket_leave(&loader_context_wait, &t);
                queued = false;
            }
            if (share_class) {
                *share_class = (int) k;
                r = EAGAIN;
                break;
            }
            w = &loader_share_wait;
        } else if (queued ? !root_ticket_first(&loader_context_wait, &t)
                          : root_ticket_queued(&loader_context_wait, k) &&
                                loader_context_wait.tickets <
                                    loader_context_wait.max) {
            /* behind older waiters of its class */
        } else if ((lcp = loader_class[k].free)) {
            loader_class[k].free = lcp->ctx.next;
            break;
        } else if (loader_class[k].made < loader_class[k].limit) {
            loader_class[k].made++; /* claimed; mapped below, unlocked */
            break;
        }
        if (w == &loader_context_wait && !queued) {
            /* Every waiter here holds a ticket, so bounding the tickets by
             * `max` keeps a queued waiter from finding the wait full when it
             * goes back to it after a wake.
             */
            if (loader_context_wait.tickets >= loader_context_wait.max) {
                r = EAGAIN;
                break;
            }
            root_ticket_join(&loader_context_wait, &t, k);
            queued = true;
        }
        if ((r = loader_wait(w, k, w == &loader_share_wait ? owner : 0, &until)))
            r = EAGAIN;
    }
    if (queued)
        root_ticket_leave(&loader_context_wait, &t);
    if (r == EOK) {
        /* At most one entry per context in use, this one included. */
        o = loader_owner_of(owner, true);
        CRASHCHECK(!o);
        o->held[k]++;
    }
    pthread_mutex_unlock(&loader_context_mutex);
    if (r != EOK && share_class && *share_class >= 0)
        return r; /* the caller waits for the share, and asks again */
    if (r != EOK) {
        loader_context_note(size, "its class, or the spawner's share of it, "
                                  "stayed used up, or too many spawns waited "
                                  "already");
        return r;
    }

    if (!lcp) {
        uintptr_t va;

        /* lcp must live at a VA reachable from a CHILD's pgdir during the
         * loader-thread phase. The loader thread runs in the child's pgdir with
         * an L2[3] view of roottask's image (mm_roottask_view_install in
         * kernel/mm/roottask-view.c) — that view only covers VA
         * 0xC0000000–0xFFFFFFFF. libc mmap() can land anywhere in roottask's
         * user-VA range, including low addresses outside L2[3]; if it does, the
         * loader thread's first deref of "lcp" (via lcp->stack[]) faults.
         *
         * roottask_mmap_anon_user allocates from root_phys_map_va (≥
         * 0xc1000000) — squarely inside L2[3] — and skips the I/O service
         * message round-trip entirely (it's a single kernel extension).
         */
        r = roottask_mmap_anon_user(size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    &va);
        if (r != EOK) {
            /* Give the claim back: a waiter may have better luck later. */
            root_mutex_lock(&loader_context_mutex);
            loader_class[k].made--;
            loader_owner_put(owner, k);
            loader_context_wake();
            pthread_mutex_unlock(&loader_context_mutex);
            loader_context_note(size, strerror(r));
            return EAGAIN;
        }
        atomic_fetch_add_explicit(&loader_context_mapped, size,
                                  memory_order_relaxed);
        lcp = (void *) va;
        fresh = true;
    }
    /* We don't have to zero the whole thing - just the first part. */
    memset(lcp, 0, sizeof(struct loader_context_base));
#ifndef NDEBUG
    /* Ensure no code assumes this is zeroed */
    memset(&lcp->msg, 0xee, sizeof(lcp->msg));
#endif
    lcp->ctx.size = size;
    lcp->ctx.owner = owner;
    lcp->ctx.state = state;
    lcp->ctx.msgsize = msgsize;
    __atomic_store_n(&lcp->ctx.held_since, root_now_ns(), __ATOMIC_RELEASE);
    if (fresh) {
        /* A new context is listed for the hold scan once it is set up. */
        root_mutex_lock(&loader_context_mutex);
        CRASHCHECK(loader_all_n >= LOADER_CONTEXTS_MAX);
        loader_all[loader_all_n++] = lcp;
        pthread_mutex_unlock(&loader_context_mutex);
    }
    *lcpp = lcp;
    return EOK;
}

/* Wait, without anything else held, until `owner` is under its share of
 * class k (loader_context_alloc told the class). *until carries the whole
 * spawn's deadline across tries. Returns EOK to ask again, EAGAIN at the
 * deadline, or EBUSY when LOADER_SHARE_WAITERS wait already.
 */
int loader_share_await(pid_t owner, int k, uint64_t *until)
{
    struct loader_owner *o;
    int r = EOK;

    if (k < 0 || k >= LOADER_CLASSES)
        return EINVAL;
    /* The deadline is the whole spawn's: a share won back only to be lost
     * again cannot keep it trying for longer.
     */
    if (root_deadline_ns(until, LOADER_CONTEXT_WAIT_S))
        return EAGAIN;
    root_mutex_lock(&loader_context_mutex);
    while (r == EOK && (o = loader_owner_of(owner, false)) &&
           o->held[k] >= loader_class_share((unsigned) k))
        r = loader_wait(&loader_defer_wait, (unsigned) k, owner, until);
    pthread_mutex_unlock(&loader_context_mutex);
    if (r == EAGAIN)
        return EBUSY; /* no wait to be had: the caller waits as it may */
    if (r != EOK)
        loader_context_note(loader_class_size((unsigned) k),
                            "the spawner's share of its class stayed used up");
    return r ? EAGAIN : EOK;
}


/* loader_context_free — return a loader context to its class.
 *
 * Always kept for reuse: its memory came from the grow-only
 * roottask_mmap_anon_user window, which libc munmap cannot give back.
 *
 * Parameters:
 *   lcp : the loader context being released
 */
void loader_context_free(struct loader_context *lcp)
{
    unsigned k = loader_class_of(lcp->ctx.size);

    root_mutex_lock(&loader_context_mutex);
    loader_owner_put(lcp->ctx.owner, k);
    __atomic_store_n(&lcp->ctx.held_since, 0, __ATOMIC_RELEASE);
    lcp->ctx.next = loader_class[k].free;
    loader_class[k].free = lcp;
    loader_context_wake();
    pthread_mutex_unlock(&loader_context_mutex);
}


/* loader_thread_attr — build the thread attributes for a loader thread.
 *
 * Points the attributes at the loader context's embedded stack and, if a I/O
 * Loader threads use roottask's local scheduler context. Spawn attributes are
 * applied to the child when the loader commits the start state; inheriting a
 * client thread here would require an ambient cross-process scheduler lookup.
 *
 * Parameters:
 *   attr : thread-attribute structure to fill in
 *   lcp  : loader context providing the loader-thread stack
 *   ctp  : I/O service context to inherit scheduling from, or NULL
 */
void loader_thread_attr(struct _thread_attr *attr,
                        struct loader_context *lcp,
                        ioserv_context_t *ctp)
{
    memset(attr, 0x00, sizeof *attr);
    attr->stackaddr = lcp->stack;
    attr->stacksize = sizeof(lcp->stack);
    (void) ctp;
}
