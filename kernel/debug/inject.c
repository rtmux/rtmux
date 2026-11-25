/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Debug-only fault injection; built only with CONFIG_DEBUG_INJECT. */

#include <core/globals.h>

#ifdef CONFIG_DEBUG_INJECT
#include "as.h"
#include <sv39-lookup.h>
#include <private/inject.h>
#include <private/kstats.h>

/* The armed one-shot allocation failure. Two words, both written only here and
 * consumed with an atomic exchange of `armed` in inject_should_fail_alloc, so a
 * failure is delivered to exactly one caller.
 */
static atomic_uint armed_soul = KSTAT_SOUL_COUNT; /* KSTAT_SOUL_COUNT: none */
static atomic_int armed_pid;
static atomic_int consumed;

/* The armed SchedSet failures: the pid they are armed for, how many are left,
 * and how many were delivered. A failure goes to exactly one caller by a
 * compare-and-swap of `sched_left`.
 */
static atomic_int sched_armed_pid = -1;
static atomic_int sched_left;
static atomic_int sched_consumed;
static atomic_int sched_displaced;
static thread_t *_Atomic sched_failed_thr; /* the caller just failed */

static inline bool inject_op_is_status(unsigned op)
{
    return op == INJECT_OP_SCHED_STATS || op == INJECT_OP_SYNC_WAKE_CLAIMS ||
           op == INJECT_OP_SITE_STATUS || op == INJECT_OP_INTR_SELECT_SEEN ||
           op == INJECT_OP_ALLOC_STATUS || op == INJECT_OP_SCHED_SET_STATUS ||
           op == INJECT_OP_SCHED_SET_DISPLACED || op == INJECT_OP_PTE_PROBE ||
           op == INJECT_OP_MANIP_PARTIAL || op == INJECT_OP_PRIO_SETTLE_STATS ||
           op == INJECT_OP_XFER_STATS || op == INJECT_OP_RCU_STALL_WAITS;
}

atomic_ullong inject_spin_state;

/* How long an injected SchedSet failure holds the caller in the kcall, after
 * the change is applied, with the other harts kicked (microseconds).
 */
#define SCHED_WINDOW_US 500

/* Leave the other harts a window to take a caller that the change exposed. */
static void inject_sched_window(void)
{
    unsigned me = get_cpunum();
    uint64_t live = cpu_online_snapshot();
    uint64_t end =
        r_time() + SCHED_WINDOW_US * (qtimeptr->cycles_per_sec / 1000000);

    for (unsigned h = 0; h < num_processors; h++)
        if (h != me && cpu_in_mask(live, h))
            SENDIPI(h, IPI_RESCHED);
    while (r_time() < end)
        ;
}

/* Called from ksys_sched_set once the change is applied; `lowered` says the
 * caller's priority went down. Only a lowering call fails, so the caller can
 * raise itself again with the failure still armed.
 */
bool inject_should_fail_sched_set(thread_t *act, bool lowered)
{
    int left;

    if (!lowered || !act || !act->process ||
        act->process->pid !=
            atomic_load_explicit(&sched_armed_pid, memory_order_acquire))
        return false;
    left = atomic_load_explicit(&sched_left, memory_order_relaxed);
    do {
        if (left <= 0)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &sched_left, &left, left - 1, memory_order_acq_rel,
        memory_order_relaxed));

    /* One pending mark: failures injected from two threads at once overwrite
     * it, and the displaced count then falls short. The tests that count
     * displaced switches inject from one thread.
     */
    atomic_store_explicit(&sched_failed_thr, act, memory_order_release);
    atomic_fetch_add_explicit(&sched_consumed, 1, memory_order_release);
    inject_sched_window();
    return true;
}

/* Called from the trap handler on every error return. If that error is the
 * SchedSet failure just injected for act, consume the mark, and count it when
 * the caller was already switched out or its switch was still owed to
 * kernel_exit, so a test can tell a status that met a switch from one that
 * never did. Consuming the mark on any other return keeps it from matching a
 * later, unrelated error.
 */
void inject_note_sched_return(thread_t *act, bool switched)
{
    thread_t *expect = act;

    if (atomic_compare_exchange_strong_explicit(&sched_failed_thr, &expect,
                                                NULL, memory_order_acq_rel,
                                                memory_order_relaxed) &&
        switched)
        atomic_fetch_add_explicit(&sched_displaced, 1, memory_order_release);
}

/* Where a client's departure settles the server's priority (K29). The settle is
 * deferred to thread context, so none may run inside force_ready's departure
 * (in_departure, per hart); the count of deferred ones that ran lets a test see
 * that the deferral happened at all.
 */
static atomic_int in_departure_hart[CONFIG_PROCESSORS_MAX];
static atomic_ullong prio_thread_runs;
static atomic_ullong prio_idle_runs;
static atomic_ullong prio_departure_work;

void inject_in_departure(int on)
{
    atomic_store_explicit(&in_departure_hart[get_cpunum()], on,
                          memory_order_relaxed);
}

/* A priority recomputation is about to run (thread_prio_update_dpp, settle). */
void inject_note_prio_work(void)
{
    if (atomic_load_explicit(&in_departure_hart[get_cpunum()],
                             memory_order_relaxed))
        atomic_fetch_add_explicit(&prio_departure_work, 1,
                                  memory_order_relaxed);
}

void inject_note_prio_deferred_run(void)
{
    if (!get_active_thread(get_cpunum())) {
        if (intr_get())
            crash("idle priority settle with interrupts enabled");
        atomic_fetch_add_explicit(&prio_idle_runs, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&prio_thread_runs, 1, memory_order_relaxed);
}

/* The pid whose next turnstile allocation fails, or -1: consumed like
 * armed_soul so exactly one allocation takes the failure.
 */
static atomic_int armed_ts_pid = -1;
static atomic_bool ts_consumed;

int inject_should_fail_turnstile(process_t *prp)
{
    int pid = atomic_load_explicit(&armed_ts_pid, memory_order_acquire);

    if (pid < 0 || !prp || prp->pid != pid ||
        !atomic_compare_exchange_strong_explicit(&armed_ts_pid, &pid, -1,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed))
        return 0;
    atomic_store_explicit(&ts_consumed, true, memory_order_release);
    return 1;
}

/* One-shot stalls, one per site: the pid whose next pass through the site
 * stalls, or -1, and for how long. Consumed like armed_ts_pid. The sites are
 * the windows of a contended PI mutex lock (published but not queued, before
 * WAITING is published, after the priority walk) and force_ready's wait-chain
 * window.
 */
static struct {
    atomic_int pid;
    atomic_uint ms;
    atomic_bool taken, release;
} stall_site[STALL_SITES] = {
    [STALL_LOCK] = {.pid = -1},
    [STALL_LOCK_EARLY] = {.pid = -1},
    [STALL_LOCK_PROPAGATED] = {.pid = -1},
    [STALL_FORCE_READY] = {.pid = -1},
    [STALL_SYNC_WAKER] = {.pid = -1},
    [STALL_BLOCK] = {.pid = -1},
    [STALL_STEAL] = {.pid = -1},
    [STALL_CANCEL] = {.pid = -1},
    [STALL_REPRIO] = {.pid = -1},
    [STALL_REPLY_CLAIM] = {.pid = -1},
    [STALL_REPLY_UNBLOCK] = {.pid = -1},
    [STALL_CLAIM_HELD] = {.pid = -1},
    [STALL_FORCE_CLAIMED] = {.pid = -1},
    [STALL_CAP_PEEK] = {.pid = -1},
    [STALL_READY] = {.pid = -1},
    [STALL_SWITCH] = {.pid = -1},
    [STALL_SEND_PICK] = {.pid = -1},
    [STALL_UNBLOCK_FORCE] = {.pid = -1},
    [STALL_UNBLOCK_REPLY] = {.pid = -1},
    [STALL_KPAGE_ZERO] = {.pid = -1},
    [STALL_THREAD_CREATE] = {.pid = -1},
    [STALL_TIMER_FIRE] = {.pid = -1},
};

static _Atomic(thread_t *) stalled_waiter;
static atomic_int wake_claims;
static atomic_int intr_before_switch_pid;
static atomic_int intr_after_select_pid;
static atomic_int intr_select_seen;
static atomic_uint
    stall_block_tid; /* pthread_t the STALL_BLOCK site waits for */
static atomic_uint stall_steal_tid;   /* and the STALL_STEAL site */
static atomic_uint stall_reprio_tid;  /* and the STALL_REPRIO site */
atomic_uint inject_empty_steal_locks; /* SCHED_STATS 0 */
atomic_uint inject_as_slock_takes;    /* SCHED_STATS 1 */

/* POKE: find thread tid (pthread_t) of pid, pinned, and wake or stop it. */
static int inject_poke(uint64_t pid, uint64_t arg)
{
    unsigned tid = (unsigned) (arg >> 8), action = (unsigned) (arg & 0xff);
    process_t *prp;
    thread_t *thp = NULL;

    if (pid > INT32_MAX || tid == 0)
        return EINVAL;
    prp = lookup_pid_rcu((pid_t) pid);
    if (prp) {
        thp = vector_lookup(&prp->threads, tid - 1);
        if (thp && (thp->tid != tid - 1 || tthread_pin(thp) != EOK))
            thp = NULL;
    }
    rcu_sched_read_unlock();
    if (!thp)
        return ESRCH;
    if (action == INJECT_POKE_STOP)
        unready(thp, STATE_STOPPED);
    else
        ready(thp);
    tthread_unpin(thp);
    return EOK;
}
static atomic_uint stall_ready_tid;  /* and the STALL_READY site */
static atomic_uint stall_switch_tid; /* and the STALL_SWITCH site */
static atomic_uint stall_timer_tid;  /* and the STALL_TIMER_FIRE site */

/* Stand in for the PLIC handler finding thp as its hart's active thread: set
 * PENDING once for the process armed in *armed.
 *
 * Returns whether it did.
 */
static bool inject_intr_pending(atomic_int *armed, thread_t *thp)
{
    if (thp->state != STATE_INTR)
        return false;
    int pid = thp->process->pid;
    if (!atomic_compare_exchange_strong_explicit(
            armed, &pid, 0, memory_order_relaxed, memory_order_relaxed))
        return false;
    atomic_or(&thp->flags, RTMUX_FLG_THR_INTR_PENDING);
    return true;
}

void inject_intr_before_switch(thread_t *thp)
{
    (void) inject_intr_pending(&intr_before_switch_pid, thp);
}

/* Called with the dispatch lock held again after the next thread was chosen. */
void inject_intr_after_select(thread_t *thp, const thread_t *chosen)
{
    if (inject_intr_pending(&intr_after_select_pid, thp))
        atomic_store_explicit(&intr_select_seen,
                              chosen ? 2 | (int) ((chosen->tid + 1) << 8) : 1,
                              memory_order_release);
}

/* Claim a one-shot stall armed for the site's pid and spin for its time, or
 * until the site is released or, with a watch, until *watch is cleared.
 *
 * Returns whether the stall was taken.
 */
static bool inject_stall_claim(enum inject_site site,
                               const process_t *prp,
                               uint64_t *end)
{
    int pid = atomic_load_explicit(&stall_site[site].pid, memory_order_acquire);
    uint64_t ms;

    if (pid < 0 || !prp || prp->pid != pid)
        return false;
    /* Read the duration the arm published before claiming it. */
    ms = atomic_load_explicit(&stall_site[site].ms, memory_order_relaxed);
    if (!atomic_compare_exchange_strong_explicit(&stall_site[site].pid, &pid,
                                                 -1, memory_order_acq_rel,
                                                 memory_order_relaxed))
        return false;
    *end = r_time() + ms * (qtimeptr->cycles_per_sec / 1000);
    return true;
}

/* Harts now spinning in inject_stall_until, and for each hart the grace-period
 * waits on it that found one of them (INJECT_OP_RCU_STALL_WAITS).
 */
static atomic_ullong stall_harts;
static atomic_uint rcu_stall_waits[CONFIG_PROCESSORS_MAX];

/* Called by rcu_sched_wait() on each pass while it waits for hart cpu: counts
 * the wait for the waiting hart once cpu is seen in a stall.
 *
 * Returns whether it was, so the caller stops asking.
 */
bool inject_note_rcu_wait(unsigned cpu)
{
    if (!(atomic_load_explicit(&stall_harts, memory_order_acquire) &
          (1ULL << cpu)))
        return false;
    atomic_fetch_add_explicit(&rcu_stall_waits[get_cpunum()], 1,
                              memory_order_relaxed);
    return true;
}

static bool inject_stall_until(enum inject_site site,
                               const process_t *prp,
                               thread_t *waiter,
                               void *const volatile *watch)
{
    uint64_t end;

    if (!inject_stall_claim(site, prp, &end))
        return false;
    if (waiter) {
        atomic_store_explicit(&wake_claims, 0, memory_order_relaxed);
        atomic_store_explicit(&stalled_waiter, waiter, memory_order_release);
    }

    /* Marked before it is seen taken: a grace-period wait the test starts once
     * it sees the stall must find the mark.
     */
    atomic_fetch_or_explicit(&stall_harts, 1ULL << get_cpunum(),
                             memory_order_release);
    atomic_store_explicit(&stall_site[site].taken, true, memory_order_release);
    inject_spin_begin();
    while (!atomic_load_explicit(&stall_site[site].release,
                                 memory_order_acquire) &&
           !(watch && !__atomic_load_n(watch, __ATOMIC_ACQUIRE)) &&
           r_time() < end)
        ;
    inject_spin_end();
    atomic_fetch_and_explicit(&stall_harts, ~(1ULL << get_cpunum()),
                              memory_order_release);
    if (waiter)
        atomic_store_explicit(&stalled_waiter, NULL, memory_order_release);
    return true;
}

static void inject_stall_once(enum inject_site site,
                              const process_t *prp,
                              thread_t *waiter)
{
    (void) inject_stall_until(site, prp, waiter, NULL);
}

/* Arm a stall at site for pid and ms milliseconds; an errno. */
static int inject_stall_arm(enum inject_site site, uint64_t pid, uint64_t ms)
{
    /* A pid past INT32_MAX would truncate to another process's pid below. */
    if (pid > INT32_MAX || ms == 0 || ms > INJECT_SPIN_MAX_MS)
        return EINVAL;
    atomic_store_explicit(&stall_site[site].taken, false, memory_order_relaxed);
    atomic_store_explicit(&stall_site[site].release, false,
                          memory_order_relaxed);
    atomic_store_explicit(&stall_site[site].ms, (unsigned) ms,
                          memory_order_relaxed);
    atomic_store_explicit(&stall_site[site].pid, (int) pid,
                          memory_order_release);
    return EOK;
}

/* Called from the contended lock path with the chain lock dropped, after the
 * waiter published itself and before it queues.
 */
void inject_mutex_lock_stall(process_t *prp)
{
    inject_stall_once(STALL_LOCK, prp, NULL);
}

/* Called with the chain lock held, before WAITING is published. */
void inject_mutex_lock_early(process_t *prp)
{
    inject_stall_once(STALL_LOCK_EARLY, prp, NULL);
}

/* Called with the chain lock dropped, after the priority walk. */
void inject_mutex_lock_propagated(process_t *prp)
{
    inject_stall_once(STALL_LOCK_PROPAGATED, prp, NULL);
}

/* Called from force_ready before it takes a sync waiter's chain lock or a
 * receiver's channel lock.
 */
void inject_force_ready_stall(thread_t *thp)
{
    if (atomic_load_explicit(&stalled_waiter, memory_order_acquire) == thp)
        atomic_fetch_add_explicit(&wake_claims, 1, memory_order_relaxed);
    inject_stall_once(STALL_FORCE_READY, thp->process, NULL);
}

/* Called from ksys_thread_cancel between its read of a foreign target's wait
 * and the cancel it posts.
 */
void inject_cancel_stall(thread_t *thp)
{
    inject_stall_once(STALL_CANCEL, thp->process, NULL);
}

void inject_sync_waker_stall(thread_t *thp)
{
    inject_stall_once(STALL_SYNC_WAKER, thp->process, thp);
}

/* Called by a thread about to publish a blocked state, after the pending checks
 * of its kernel call. Callers may hold a sync chain lock. Called by a hart that
 * has taken thp off another hart's run queue, between the two dispatch locks.
 * Holds no lock.
 */
void inject_steal_stall(thread_t *thp)
{
    if (thp->tid + 1 ==
        atomic_load_explicit(&stall_steal_tid, memory_order_acquire))
        inject_stall_once(STALL_STEAL, thp->process, NULL);
}

/* Called by adjust_priority with a READY thp requeued at its new priority and
 * that run queue locked.
 */
void inject_reprio_stall(thread_t *thp)
{
    if (thp->tid + 1 ==
        atomic_load_explicit(&stall_reprio_tid, memory_order_acquire))
        inject_stall_once(STALL_REPRIO, thp->process, NULL);
}

void inject_block_stall(thread_t *thp)
{
    if (thp->tid + 1 ==
        atomic_load_explicit(&stall_block_tid, memory_order_acquire))
        inject_stall_once(STALL_BLOCK, thp->process, NULL);
}

/* Called by a reply, an error or a follow-up copy after the rcvid lookup,
 * before it reads received->channel: hold the caller there until a channel
 * destroy on another hart has detached the connection (or the armed time
 * passes), so the caller reads the cleared field every time, not once in a long
 * soak. The caller's RCU-sched section keeps the destroyer from freeing the
 * channel meanwhile, as it does without the stall.
 */
static atomic_bool reply_claim_detached; /* the last stall ended on a detach */

void inject_reply_claim_stall(const connect_t *received)
{
    thread_t *act = get_active_thread(get_cpunum());

    if (act && inject_stall_until(STALL_REPLY_CLAIM, act->process, NULL,
                                  (void *const volatile *) &received->channel))
        atomic_store_explicit(
            &reply_claim_detached,
            !__atomic_load_n(&received->channel, __ATOMIC_ACQUIRE),
            memory_order_release);
}

static atomic_uint
    stall_send_pick_tid; /* pthread_t STALL_SEND_PICK waits for */

/* Called by a send that has looked for a receiver and dropped the lock. */
void inject_send_pick_window(const thread_t *act)
{
    if (act->tid + 1 ==
        atomic_load_explicit(&stall_send_pick_tid, memory_order_acquire))
        (void) inject_stall_until(STALL_SEND_PICK, act->process, NULL, NULL);
}

static atomic_uint stall_cap_peek_tid; /* pthread_t STALL_CAP_PEEK waits for */

/* Called between the copy of a long message and the look at its head. */
void inject_cap_peek_window(const thread_t *act)
{
    if (act->tid + 1 ==
        atomic_load_explicit(&stall_cap_peek_tid, memory_order_acquire))
        (void) inject_stall_until(STALL_CAP_PEEK, act->process, NULL, NULL);
}

/* Called by msg_readv with its claim on the client taken: hold it. */
void inject_claim_held(void)
{
    thread_t *act = get_active_thread(get_cpunum());

    if (act)
        (void) inject_stall_until(STALL_CLAIM_HELD, act->process, NULL, NULL);
}

/* HOLD_EXIT_DESTROY: the process whose thread destruction is held, and until
 * when (r_time).
 */
static atomic_int exit_hold_pid = -1;
static _Atomic uint64_t exit_hold_end;

/* Whether kext_exit_destroy_threads must ask for another pass for prp. */
bool inject_exit_destroy_held(const process_t *prp)
{
    int pid = atomic_load_explicit(&exit_hold_pid, memory_order_acquire);

    if (pid < 0 || prp->pid != pid)
        return false;
    if (r_time() < atomic_load_explicit(&exit_hold_end, memory_order_relaxed))
        return true;
    (void) atomic_compare_exchange_strong_explicit(
        &exit_hold_pid, &pid, -1, memory_order_acq_rel, memory_order_relaxed);
    return false;
}

/* ABANDON_EXEC: the process whose next exec is given up, how far that exec has
 * got (0 armed, 1 its begin step has been asked to run again), and from stage 1
 * the shell that names it. An exec that fails on its own once at stage 1 (its
 * caller died) leaves the arm unmatched: a later exec has another shell.
 */
static atomic_int abandon_exec_pid = -1;
static atomic_int abandon_exec_stage;
static atomic_int abandon_exec_shell = -1;

/* Whether the begin step of an exec in prp (shell its loader shell's pid), its
 * work done, must ask to be made again: the first call of the exec ABANDON_EXEC
 * armed.
 */
bool inject_exec_begin_again(const process_t *prp, pid_t shell)
{
    int pid = atomic_load_explicit(&abandon_exec_pid, memory_order_acquire);
    int from = 0;

    if (pid < 0 || prp->pid != pid ||
        !atomic_compare_exchange_strong_explicit(&abandon_exec_stage, &from, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed))
        return false;
    atomic_store_explicit(&abandon_exec_shell, shell, memory_order_release);
    return true;
}

/* Whether this call of the begin step of an exec in prp must find the loader
 * shell gone: the call after the one inject_exec_begin_again held, of the same
 * exec. Disarms.
 */
bool inject_exec_begin_gone(const process_t *prp, pid_t shell)
{
    int pid = atomic_load_explicit(&abandon_exec_pid, memory_order_acquire);
    int from = 1;

    if (pid < 0 || prp->pid != pid ||
        atomic_load_explicit(&abandon_exec_shell, memory_order_acquire) !=
            shell ||
        !atomic_compare_exchange_strong_explicit(&abandon_exec_stage, &from, 2,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed))
        return false;
    atomic_store_explicit(&abandon_exec_pid, -1, memory_order_release);
    return true;
}

/* Called by force_ready when it finds thp claimed, before it hands its wake
 * over: let the claimer that inject_claim_held() holds go, and give it time to
 * release the claim.
 */
void inject_force_ready_claimed(thread_t *thp)
{
    int pid = atomic_load_explicit(&stall_site[STALL_FORCE_CLAIMED].pid,
                                   memory_order_acquire);
    uint64_t end;

    if (pid < 0 || thp->process->pid != pid)
        return;
    end = atomic_load_explicit(&stall_site[STALL_FORCE_CLAIMED].ms,
                               memory_order_relaxed);
    if (!atomic_compare_exchange_strong_explicit(
            &stall_site[STALL_FORCE_CLAIMED].pid, &pid, -1,
            memory_order_acq_rel, memory_order_relaxed))
        return;
    end = r_time() + end * (qtimeptr->cycles_per_sec / 1000);
    atomic_store_explicit(&stall_site[STALL_FORCE_CLAIMED].taken, true,
                          memory_order_release);
    atomic_store_explicit(&stall_site[STALL_CLAIM_HELD].release, true,
                          memory_order_release);
    inject_spin_begin();
    while ((thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) && r_time() < end)
        __asm__ volatile("" ::: "memory");
    inject_spin_end();
}

/* Claim the one-shot stall armed at site for prp and spin until thp has one of
 * the internal flags in bits set, the site is released, or its time passes.
 */
static void inject_stall_for_flags(enum inject_site site,
                                   const process_t *prp,
                                   const thread_t *thp,
                                   unsigned bits)
{
    uint64_t end;

    if (!inject_stall_claim(site, prp, &end))
        return;
    atomic_store_explicit(&stall_site[site].taken, true, memory_order_release);
    inject_spin_begin();
    while (!(__atomic_load_n(&thp->internal_flags, __ATOMIC_ACQUIRE) & bits) &&
           !atomic_load_explicit(&stall_site[site].release,
                                 memory_order_acquire) &&
           r_time() < end)
        __asm__ volatile("" ::: "memory");
    inject_spin_end();
}

/* Called by force_ready on a reply-blocked thp before it takes the channel lock
 * to test the claim and decide on thp's unblock notification: wait for a reply
 * to claim thp (at once, if one already has).
 */
void inject_unblock_force_window(thread_t *thp)
{
    inject_stall_for_flags(STALL_UNBLOCK_FORCE, thp->process, thp,
                           RTMUX_ITF_MSG_DELIVERY);
}

/* Called by a reply or an error that has claimed client and looked for its
 * unblock notification: wait for a timeout to mark one queued.
 */
void inject_unblock_reply_window(const thread_t *client)
{
    thread_t *act = get_active_thread(get_cpunum());

    if (act)
        inject_stall_for_flags(STALL_UNBLOCK_REPLY, act->process, client,
                               RTMUX_ITF_UNBLOCK_QUEUED);
}

/* SLOW_KPAGE_ZERO: the roottask thread it caught, its delay and the pages left.
 */
static _Atomic(const thread_t *) kpage_zero_thr;
static atomic_uint kpage_zero_us, kpage_zero_left;

/* Called by kext_kpage_zero, on the roottask thread that zeroes a page. */
void inject_kpage_zero_slow(const thread_t *act)
{
    const thread_t *thr =
        atomic_load_explicit(&kpage_zero_thr, memory_order_acquire);
    uint64_t end;

    if (!thr) {
        int pid = atomic_load_explicit(&stall_site[STALL_KPAGE_ZERO].pid,
                                       memory_order_acquire);
        const thread_t *client =
            __atomic_load_n(&act->client, __ATOMIC_ACQUIRE);

        if (pid < 0 || !client || client->process->pid != pid ||
            !atomic_compare_exchange_strong_explicit(
                &stall_site[STALL_KPAGE_ZERO].pid, &pid, -1,
                memory_order_acq_rel, memory_order_relaxed))
            return;
        atomic_store_explicit(&kpage_zero_thr, act, memory_order_release);
        atomic_store_explicit(&stall_site[STALL_KPAGE_ZERO].taken, true,
                              memory_order_release);
    } else if (thr != act) {
        return;
    }
    if (atomic_load_explicit(&stall_site[STALL_KPAGE_ZERO].release,
                             memory_order_acquire) ||
        !atomic_load_explicit(&kpage_zero_left, memory_order_relaxed)) {
        atomic_store_explicit(&kpage_zero_thr, NULL, memory_order_release);
        return;
    }
    atomic_fetch_sub_explicit(&kpage_zero_left, 1, memory_order_relaxed);
    end = r_time() + (uint64_t) atomic_load_explicit(&kpage_zero_us,
                                                     memory_order_relaxed) *
                         (qtimeptr->cycles_per_sec / 1000000);
    inject_spin_begin();
    while (r_time() < end)
        __asm__ volatile("" ::: "memory");
    inject_spin_end();
}

/* Called by thread_create once the new thread is published and counted active,
 * before it first runs: hold that window open for an armed process.
 */
void inject_thread_create_published(const process_t *prp)
{
    inject_stall_once(STALL_THREAD_CREATE, prp, NULL);
}

/* Whether the reply that just removed an unblock notification restarts as if a
 * preemption were pending: once, for the armed pid, after the 1 ms stall that
 * arming a stall site costs.
 */
bool inject_reply_unblock_preempt(void)
{
    thread_t *act = get_active_thread(get_cpunum());

    return act &&
           inject_stall_until(STALL_REPLY_UNBLOCK, act->process, NULL, NULL);
}

/* Stepped message copies of one process, counted (XFER_TRACE). */
static atomic_int xfer_trace_pid = -1;
static atomic_uint xfer_counter[INJECT_XFER_COUNTERS];

static void inject_xfer_note(const thread_t *act, unsigned what)
{
    if (act->process->pid ==
        atomic_load_explicit(&xfer_trace_pid, memory_order_relaxed))
        atomic_fetch_add_explicit(&xfer_counter[what], 1, memory_order_relaxed);
}

void inject_xfer_start(const thread_t *act)
{
    inject_xfer_note(act, INJECT_XFER_STARTS);
}

void inject_xfer_yield(const thread_t *act)
{
    inject_xfer_note(act, INJECT_XFER_YIELDS);
}

void inject_xfer_peek(const thread_t *act)
{
    inject_xfer_note(act, INJECT_XFER_PEEKS);
}

void inject_xfer_budget(const thread_t *act)
{
    inject_xfer_note(act, INJECT_XFER_BUDGETS);
}

/* Called at the start of ready_default, before it looks at the thread. */
void inject_ready_stall(thread_t *thp)
{
    if (thp->tid + 1 ==
        atomic_load_explicit(&stall_ready_tid, memory_order_acquire))
        inject_stall_once(STALL_READY, thp->process, NULL);
}

/* Called by timer_expiry, interrupts off and the wheel lock released, before it
 * delivers a recorded fire to thp; wait_seq is the wait the fire is bound to,
 * or NULL. The stall also ends once thp has left that wait (the one it is in
 * now, unbound) and sleeps again: the fire then belongs to a sleep that is
 * over, the case the window is held open for.
 */
void inject_timer_fire_stall(thread_t *thp, const uint32_t *wait_seq)
{
    uint64_t end;
    uint32_t seq;

    if (thp->tid + 1 !=
            atomic_load_explicit(&stall_timer_tid, memory_order_acquire) ||
        !inject_stall_claim(STALL_TIMER_FIRE, thp->process, &end))
        return;
    seq = wait_seq ? *wait_seq : thread_wake_seq(thp);
    atomic_fetch_or_explicit(&stall_harts, 1ULL << get_cpunum(),
                             memory_order_release);
    atomic_store_explicit(&stall_site[STALL_TIMER_FIRE].taken, true,
                          memory_order_release);
    inject_spin_begin();
    while (!atomic_load_explicit(&stall_site[STALL_TIMER_FIRE].release,
                                 memory_order_acquire) &&
           r_time() < end &&
           (thread_wake_seq(thp) == seq ||
            __atomic_load_n(&thp->state, __ATOMIC_ACQUIRE) != STATE_NANOSLEEP))
        ;
    inject_spin_end();
    atomic_fetch_and_explicit(&stall_harts, ~(1ULL << get_cpunum()),
                              memory_order_release);
}

/* Called on the hart of thp, still its current thread, before the hart switches
 * away from it. Callers may hold the dispatch lock; thp may be NULL.
 */
void inject_switch_stall(thread_t *thp)
{
    if (thp && thp->tid + 1 == atomic_load_explicit(&stall_switch_tid,
                                                    memory_order_acquire))
        inject_stall_once(STALL_SWITCH, thp->process, NULL);
}

/* Called from object_alloc before it touches the free list. Ordinary allocation
 * only: the reserved path (object_alloc_reserved) must never fail this way.
 */
int inject_should_fail_alloc(process_t *prp, soul_t *soulp)
{
    static soul_t *const pools[KSTAT_SOUL_COUNT] = {
#define KSTAT_SOUL_PTR(name, var) [KSTAT_SOUL_##name] = &var,
        KSTAT_SOUL_LIST(KSTAT_SOUL_PTR)
#undef KSTAT_SOUL_PTR
    };
    unsigned id = atomic_load_explicit(&armed_soul, memory_order_acquire);

    if (id >= KSTAT_SOUL_COUNT || pools[id] != soulp || !prp ||
        prp->pid != atomic_load_explicit(&armed_pid, memory_order_relaxed))
        return 0;
    /* Only the caller that swaps the arm away delivers the failure. */
    if (atomic_exchange_explicit(&armed_soul, KSTAT_SOUL_COUNT,
                                 memory_order_acq_rel) != id)
        return 0;
    atomic_store_explicit(&consumed, 1, memory_order_release);
    return 1;
}

/* Pages done by the most recent failed MAP, as cpu_pte_manipulate reported it.
 */
static atomic_uint manip_partial;

void inject_note_manip_failure(unsigned pages)
{
    atomic_store_explicit(&manip_partial, pages, memory_order_release);
}

/* pid's address space, or NULL. Opens the RCU read section, which the caller
 * closes either way.
 */
static address_t *inject_pid_memory(uint64_t pid)
{
    process_t *prp = lookup_pid_rcu((pid_t) pid);

    return prp ? prp->memory : NULL;
}

/* The armed FAIL_PTE_MAP: the process, and how many pages it may map. */
static atomic_int armed_map_pid = -1;
static atomic_uint armed_map_pages;
static _Atomic uintptr_t armed_map_va; /* 0: any start */

/* Called from cpu_pte_manipulate for a MAP of [start, *endp] in adp (*endp is
 * the last byte, as in struct mm_pte_manipulate): if a failure is armed for adp
 * and the range is longer than the armed pages, cut *endp there and take the
 * arm (once), and the caller reports ENOMEM.
 */
bool inject_pte_map_stop(const void *adp, uintptr_t start, uintptr_t *endp)
{
    int armed = atomic_load_explicit(&armed_map_pid, memory_order_acquire);
    process_t *prp;
    bool match;
    unsigned pages;
    uintptr_t va;

    if (!adp || armed < 0)
        return false;

    /* The pid, not the address space, stays armed: a pid carries a generation,
     * so an arm that outlives its process matches nothing. The process can go
     * away under us, so read it inside the RCU section lookup_pid_rcu opens.
     */
    prp = lookup_pid_rcu(armed);
    match = prp && prp->memory == adp;
    rcu_sched_read_unlock();
    if (!match)
        return false;
    pages = atomic_load_explicit(&armed_map_pages, memory_order_relaxed);
    va = atomic_load_explicit(&armed_map_va, memory_order_relaxed);
    if (va && va != start)
        return false;
    /* *endp is the last byte of the range, not one past it. */
    if (*endp < start || (*endp - start + 1) / __PAGESIZE <= pages)
        return false;

    /* Only the caller that swaps this arm away takes it; a newer arm for
     * another address space stays.
     */
    if (!atomic_compare_exchange_strong_explicit(&armed_map_pid, &armed, -1,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed))
        return false;
    atomic_store_explicit(&consumed, 1, memory_order_release);
    *endp = start + pages * __PAGESIZE;
    return true;
}

/* inject_pte — PTE_SUPERPAGE, PTE_CLEAR_SUPERPAGE and PTE_PROBE on the page
 * table of process pid.
 *
 * Returns an errno (EOK on success), or for PTE_PROBE 0 or 1 through *probedp.
 * The table is read and written under as_slock, the lock ksys_manipulate holds
 * for every other change to it.
 */
static int inject_pte(unsigned op, uint64_t pid, uint64_t va, int *probedp)
{
    address_t *adp;
    pagetable_t pgdir;
    int ret = EOK;
    bool flush = false;

    if (pid > INT32_MAX || va >= MAXVA ||
        (op != INJECT_OP_PTE_PROBE && (va & (sv39_leaf_size(1) - 1))))
        return EINVAL;
    adp = inject_pid_memory(pid);
    if (!adp) {
        rcu_sched_read_unlock();
        return ESRCH;
    }
    pgdir = adp->cpu.pgdir;
    kspinlock_lock(&as_slock);
    if (op == INJECT_OP_PTE_PROBE) {
        *probedp = sv39_lookup_leaf(pgdir, (uintptr_t) va, NULL, NULL) != NULL;
    } else {
        pte_t top = pgdir[PX(2, va)];

        if (!(top & PTE_V) || sv39_is_leaf(top)) {
            ret = EINVAL;
        } else {
            pte_t *slot = &((pagetable_t) PA_TO_VA(PTE2PA(top)))[PX(1, va)];

            if (op == INJECT_OP_PTE_SUPERPAGE && !(*slot & PTE_V)) {
                *slot = PA2PTE(0) | PTE_V | PTE_R | PTE_U | PTE_A;
                atomic_store_explicit(&manip_partial, 0, memory_order_relaxed);
                flush = true;
            } else if (op == INJECT_OP_PTE_CLEAR_SUPERPAGE && (*slot & PTE_V) &&
                       sv39_is_leaf(*slot)) {
                *slot = 0;
                flush = true;
            } else {
                ret = EBUSY;
            }
        }
    }
    kspinlock_unlock(&as_slock);
    rcu_sched_read_unlock();
    if (flush)
        cpu_tlb_flush_complete();
    return ret;
}

/* kext_debug_inject — the DEBUG_INJECT private call. */
void kext_debug_inject(void *data)
{
    struct kcall_args_debug_inject *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    unsigned self = get_cpunum();
    int ret = EOK;

    switch (kap->op) {
    case INJECT_OP_SPIN_IRQOFF:
    case INJECT_OP_SPIN_CONSOLE:
    case INJECT_OP_HANG_HART: {
        unsigned hart;
        unsigned ms;
        unsigned live = 0;

        /* Range-check the full 64-bit arguments before narrowing them. */
        if (kap->arg0 >= CONFIG_PROCESSORS_MAX ||
            (kap->op != INJECT_OP_HANG_HART &&
             (kap->arg1 == 0 || kap->arg1 > INJECT_SPIN_MAX_MS))) {
            ret = EINVAL;
            break;
        }
        hart = (unsigned) kap->arg0;
        ms = kap->op == INJECT_OP_HANG_HART ? UINT32_MAX : (unsigned) kap->arg1;

        uint64_t online = cpu_online_snapshot();
        for (unsigned c = 0; c < num_processors; c++)
            live += cpu_in_mask(online, c) ? 1u : 0u;

        /* Refuse the caller's own hart, a hart that is not alive, and the last
         * live hart: any of them would turn a test into an unrecoverable stop
         * rather than a stall to be diagnosed.
         */
        if (hart == self || !cpu_in_mask(online, hart) || live < 2) {
            ret = EINVAL;
            break;
        }
        pcpu_array[hart].pc_inject_spin_ms = ms;
        pcpu_array[hart].pc_inject_hold_console =
            kap->op == INJECT_OP_SPIN_CONSOLE;
        SENDIPI(hart, IPI_DEBUG_SPIN);
        break;
    }
    case INJECT_OP_FAIL_NEXT_ALLOC:
        if (kap->arg0 >= KSTAT_SOUL_COUNT) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&consumed, 0, memory_order_relaxed);
        atomic_store_explicit(&armed_pid, (int) kap->arg1,
                              memory_order_relaxed);
        atomic_store_explicit(&armed_soul, (unsigned) kap->arg0,
                              memory_order_release);
        break;
    case INJECT_OP_FAIL_NEXT_TURNSTILE:
        /* Same truncation as a stall arm; INT32_MAX stays valid, as the pid
         * tests use to disarm.
         */
        if (kap->arg0 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&ts_consumed, false, memory_order_relaxed);
        atomic_store_explicit(&armed_ts_pid, (int) kap->arg0,
                              memory_order_release);
        break;
    case INJECT_OP_STALL_MUTEX_LOCK:
        ret = inject_stall_arm(STALL_LOCK, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_STALL_MUTEX_EARLY:
        ret = inject_stall_arm(STALL_LOCK_EARLY, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_STALL_MUTEX_PROPAGATED:
        ret = inject_stall_arm(STALL_LOCK_PROPAGATED, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_STALL_FORCE_READY:
        ret = inject_stall_arm(STALL_FORCE_READY, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_STALL_CANCEL:
        ret = inject_stall_arm(STALL_CANCEL, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_INTR_SELECT_SEEN:
        ret = atomic_exchange_explicit(&intr_select_seen, 0,
                                       memory_order_acq_rel);
        break;
    case INJECT_OP_INTR_BEFORE_SWITCH:
    case INJECT_OP_INTR_AFTER_SELECT:
        if (!kap->arg0 || kap->arg0 > INT_MAX)
            ret = EINVAL;
        else {
            atomic_store_explicit(kap->op == INJECT_OP_INTR_BEFORE_SWITCH
                                      ? &intr_before_switch_pid
                                      : &intr_after_select_pid,
                                  (int) kap->arg0, memory_order_relaxed);
            ret = EOK;
        }
        break;
    case INJECT_OP_STALL_SYNC_WAKER:
        ret = inject_stall_arm(STALL_SYNC_WAKER, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_CRASH:
        if (kap->arg0 == INJECT_CRASH_SHUTDOWN) {
            /* The power-off a roottask crash ends in (root_crash, through
             * __KSYS_PRIV_REBOOT): report, then the abnormal reboot, which
             * shuts the machine down through the SBI.
             */
            kprintf(
                "FATAL: power-off crash requested by debug-inject on "
                "hart %u\n",
                self);
            system_reboot(1);
        } else if (kap->arg0 != 0) {
            ret = EINVAL;
            break;
        }
        kprintf("FATAL: crash requested by debug-inject on hart %u\n", self);
        crash();
        break;
    case INJECT_OP_POKE:
        ret = inject_poke(kap->arg0, kap->arg1);
        break;
    case INJECT_OP_SLOW_KPAGE_ZERO: {
        unsigned us = (unsigned) (kap->arg1 >> 32);
        unsigned count = (unsigned) (kap->arg1 & 0xffffffffU);

        if (kap->arg0 > INT32_MAX || us == 0 || us > 100000 || count == 0) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&kpage_zero_thr, NULL, memory_order_relaxed);
        atomic_store_explicit(&kpage_zero_us, us, memory_order_relaxed);
        atomic_store_explicit(&kpage_zero_left, count, memory_order_relaxed);
        ret = inject_stall_arm(STALL_KPAGE_ZERO, kap->arg0, 1);
        break;
    }
    case INJECT_OP_STALL_THREAD_CREATE:
        ret = inject_stall_arm(STALL_THREAD_CREATE, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_HOLD_EXIT_DESTROY:
        if (kap->arg0 > INT32_MAX || kap->arg1 == 0 ||
            kap->arg1 > INJECT_SPIN_MAX_MS) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(
            &exit_hold_end,
            r_time() + kap->arg1 * (qtimeptr->cycles_per_sec / 1000),
            memory_order_relaxed);
        atomic_store_explicit(&exit_hold_pid, (int) kap->arg0,
                              memory_order_release);
        ret = EOK;
        break;
    case INJECT_OP_ABANDON_EXEC:
        if (kap->arg0 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&abandon_exec_stage, 0, memory_order_relaxed);
        atomic_store_explicit(&abandon_exec_shell, -1, memory_order_relaxed);
        atomic_store_explicit(&abandon_exec_pid, (int) kap->arg0,
                              memory_order_release);
        ret = EOK;
        break;
    case INJECT_OP_SCHED_STATS:
        if (kap->arg0 == 0)
            ret = (int) atomic_load_explicit(&inject_empty_steal_locks,
                                             memory_order_relaxed);
        else if (kap->arg0 == 1)
            ret = (int) (atomic_load_explicit(&inject_as_slock_takes,
                                              memory_order_relaxed) &
                         INT32_MAX);
        else
            ret = -EINVAL;
        break;
    case INJECT_OP_STALL_STEAL:
        atomic_store_explicit(&stall_steal_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret = inject_stall_arm(STALL_STEAL, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_REPRIO:
        atomic_store_explicit(&stall_reprio_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret = inject_stall_arm(STALL_REPRIO, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_BLOCK:
        atomic_store_explicit(&stall_block_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret = inject_stall_arm(STALL_BLOCK, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_READY:
        atomic_store_explicit(&stall_ready_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret = inject_stall_arm(STALL_READY, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_SWITCH:
        atomic_store_explicit(&stall_switch_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret = inject_stall_arm(STALL_SWITCH, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_TIMER_FIRE:
        atomic_store_explicit(&stall_timer_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret =
            inject_stall_arm(STALL_TIMER_FIRE, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_SYNC_WAKE_CLAIMS:
        ret = atomic_load_explicit(&wake_claims, memory_order_acquire);
        break;
    case INJECT_OP_XFER_TRACE:
        if (kap->arg0 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&xfer_trace_pid, -1, memory_order_relaxed);
        for (unsigned i = 0; i < INJECT_XFER_COUNTERS; i++)
            atomic_store_explicit(&xfer_counter[i], 0, memory_order_relaxed);
        atomic_store_explicit(&xfer_trace_pid, (int) kap->arg0,
                              memory_order_release);
        ret = EOK;
        break;
    case INJECT_OP_STALL_CAP_PEEK:
        atomic_store_explicit(&stall_cap_peek_tid, (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret = inject_stall_arm(STALL_CAP_PEEK, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_SEND_PICK:
        atomic_store_explicit(&stall_send_pick_tid,
                              (unsigned) (kap->arg1 >> 32),
                              memory_order_release);
        ret =
            inject_stall_arm(STALL_SEND_PICK, kap->arg0, (uint32_t) kap->arg1);
        break;
    case INJECT_OP_STALL_CLAIM_HELD:
        ret = inject_stall_arm(STALL_CLAIM_HELD, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_STALL_FORCE_CLAIMED:
        ret = inject_stall_arm(STALL_FORCE_CLAIMED, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_XFER_STATS:
        ret = kap->arg0 < INJECT_XFER_COUNTERS
                  ? (int) atomic_load_explicit(&xfer_counter[kap->arg0],
                                               memory_order_relaxed)
                  : -EINVAL;
        break;
    case INJECT_OP_STALL_REPLY_CLAIM:
        atomic_store_explicit(&reply_claim_detached, false,
                              memory_order_relaxed);
        ret = inject_stall_arm(STALL_REPLY_CLAIM, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_STALL_UNBLOCK_RACE:
        ret = inject_stall_arm(STALL_UNBLOCK_FORCE, kap->arg0, kap->arg1);
        if (ret == EOK)
            ret = inject_stall_arm(STALL_UNBLOCK_REPLY, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_PREEMPT_REPLY_UNBLOCK:
        ret = inject_stall_arm(STALL_REPLY_UNBLOCK, kap->arg0, 1);
        break;
    case INJECT_OP_SITE_STATUS:
        if (kap->arg0 == INJECT_SITE_TURNSTILE)
            ret = atomic_load_explicit(&ts_consumed, memory_order_acquire);
        else if (kap->arg0 == INJECT_SITE_REPLY_DETACHED)
            ret = atomic_load_explicit(&reply_claim_detached,
                                       memory_order_acquire);
        else if (kap->arg0 < STALL_SITES)
            ret = atomic_load_explicit(&stall_site[kap->arg0].taken,
                                       memory_order_acquire);
        else
            ret = -EINVAL;
        break;
    case INJECT_OP_SITE_RELEASE:
        if (kap->arg0 >= STALL_SITES)
            ret = EINVAL;
        else
            atomic_store_explicit(&stall_site[kap->arg0].release, true,
                                  memory_order_release);
        break;
    case INJECT_OP_ALLOC_STATUS:
        ret = atomic_load_explicit(&consumed, memory_order_acquire);
        break;
    case INJECT_OP_RCU_STALL_WAITS:
        if (kap->arg0 >= CONFIG_PROCESSORS_MAX) {
            ret = -1;
            break;
        }
        ret = (int) (kap->arg1
                         ? atomic_exchange_explicit(&rcu_stall_waits[kap->arg0],
                                                    0, memory_order_relaxed)
                         : atomic_load_explicit(&rcu_stall_waits[kap->arg0],
                                                memory_order_relaxed));
        break;
    case INJECT_OP_FAIL_NEXT_SCHED_SET:
        if (kap->arg0 > INT32_MAX || kap->arg1 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&sched_consumed, 0, memory_order_relaxed);
        atomic_store_explicit(&sched_displaced, 0, memory_order_relaxed);
        atomic_store_explicit(&sched_left, (int) kap->arg1,
                              memory_order_relaxed);
        atomic_store_explicit(&sched_armed_pid, (int) kap->arg0,
                              memory_order_release);
        break;
    case INJECT_OP_SCHED_SET_STATUS:
        ret = atomic_load_explicit(&sched_consumed, memory_order_acquire);
        break;
    case INJECT_OP_SCHED_SET_DISPLACED:
        ret = atomic_load_explicit(&sched_displaced, memory_order_acquire);
        break;
    case INJECT_OP_PRIO_SETTLE_STATS:
        if (kap->arg0 == 2) {
            atomic_store_explicit(&prio_idle_runs, 0, memory_order_relaxed);
            atomic_store_explicit(&prio_thread_runs, 0, memory_order_relaxed);
            atomic_store_explicit(&prio_departure_work, 0,
                                  memory_order_relaxed);
        } else if (kap->arg0 == 3) {
            ret = (int) atomic_load_explicit(&prio_idle_runs,
                                             memory_order_relaxed);
        } else if (kap->arg0 <= 1) {
            ret = (int) atomic_load_explicit(
                kap->arg0 ? &prio_departure_work : &prio_thread_runs,
                memory_order_relaxed);
        } else {
            ret = EINVAL;
        }
        break;
    case INJECT_OP_PTE_SUPERPAGE:
    case INJECT_OP_PTE_CLEAR_SUPERPAGE:
        ret = inject_pte(kap->op, kap->arg0, kap->arg1, NULL);
        break;
    case INJECT_OP_PTE_PROBE: {
        int mapped = 0;

        ret = inject_pte(kap->op, kap->arg0, kap->arg1, &mapped);
        ret = ret == EOK ? mapped : -ret; /* a status: errors read negative */
        break;
    }
    case INJECT_OP_FAIL_PTE_MAP: {
        address_t *adp;

        if (kap->arg0 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        adp = inject_pid_memory(kap->arg0);
        if (!adp) {
            ret = ESRCH;
        } else {
            atomic_store_explicit(&consumed, 0, memory_order_relaxed);
            atomic_store_explicit(&manip_partial, 0, memory_order_relaxed);
            atomic_store_explicit(&armed_map_pages,
                                  (unsigned) (kap->arg1 & (__PAGESIZE - 1)),
                                  memory_order_relaxed);
            atomic_store_explicit(&armed_map_va,
                                  (uintptr_t) (kap->arg1 & ~(__PAGESIZE - 1)),
                                  memory_order_relaxed);
            atomic_store_explicit(&armed_map_pid, (int) kap->arg0,
                                  memory_order_release);
        }
        rcu_sched_read_unlock();
        break;
    }
    case INJECT_OP_GRANT_IPC_CAP: {
        process_t *prp;

        if (kap->arg0 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        prp = lookup_pid_rcu((pid_t) kap->arg0);
        if (!prp)
            ret = ESRCH;
        else if (prp->flags & RTMUX_FLG_PROC_LOADING)
            ret = EBUSY; /* its start sets the capabilities, over this one */
        else
            prp->priv_caps |= RTMUX_PRIV_CAP_IPC;
        rcu_sched_read_unlock();
        break;
    }
    case INJECT_OP_MANIP_PARTIAL:
        ret = (int) atomic_load_explicit(&manip_partial, memory_order_acquire);
        break;
    default:
        ret = EINVAL;
        break;
    }
    kap->ret = ret;

    /* The *_STATUS and *_DISPLACED ops answer through kap->ret; a nonzero
     * status would read as a refused call to syscall_priv callers.
     */
    SETKSTATUS(act, inject_op_is_status(kap->op) ? EOK : ret);
}

/* Trace of the writes to thread_t.blocked_on in the IPC paths, for a client
 * found in STATE_REPLY with a NULL blocked_on while still linked on a channel's
 * reply queue. Each IPC_BLK() site records who touched the thread last, so the
 * hang dump can name the writer. Lock-free and lossy by design: a slot being
 * rewritten when the dump reads it is just noise.
 */
struct ipc_blk_ev {
    uint64_t t;
    const char *file;
    thread_t *thp;
    void *blocked_on;
    int line;
    unsigned char cpu;
    unsigned char state;
    unsigned short itf;
};

#define IPC_BLK_RING 2048
struct ipc_blk_ev ipc_blk_ring[IPC_BLK_RING];
atomic_uint ipc_blk_idx;

void ipc_blk_trace(const char *file, int line, thread_t *thp)
{
    unsigned i =
        atomic_fetch_add_explicit(&ipc_blk_idx, 1, memory_order_relaxed);
    struct ipc_blk_ev *e = &ipc_blk_ring[i % IPC_BLK_RING];

    e->t = r_time();
    e->file = file;
    e->line = line;
    e->thp = thp;
    e->blocked_on = thp->blocked_on;
    e->cpu = (unsigned char) get_cpunum();
    e->state = (unsigned char) thp->state;
    e->itf = (unsigned short) thp->internal_flags;
}

/* The last arms and clears of the per-hart xfer fault handler. A handler still
 * armed when a thread leaves the kernel makes the next fault on that hart, from
 * any thread and any code, longjmp into a frame that no longer exists.
 */
#define XH_RING 16
struct xh_ev {
    uint64_t t;
    const char *file;
    const void *handler;
    int line;
};
static struct xh_ev xh_ring[CONFIG_PROCESSORS_MAX][XH_RING];
static unsigned xh_idx[CONFIG_PROCESSORS_MAX];

void xfer_handler_trace(const char *file, int line, const void *handler)
{
    unsigned cpu = get_cpunum();
    struct xh_ev *e = &xh_ring[cpu][xh_idx[cpu]++ % XH_RING];

    e->t = r_time();
    e->file = file;
    e->line = line;
    e->handler = handler;
}

void xfer_handler_check_exit(void)
{
    unsigned cpu = get_cpunum();
    thread_t *act = get_active_thread(cpu);

    if (!get_pcpu()->pc_xfer_handlers)
        return;
    kprintf(
        "xfer handler %p still armed on cpu %u at kcall exit, thread "
        "pid %d tid %u syscall %d; last sets, newest first:\n",
        (void *) get_pcpu()->pc_xfer_handlers, cpu,
        (act && act->process) ? act->process->pid : -1, act ? act->tid : 0U,
        act ? (int) act->syscall : -1);
    for (unsigned k = 0; k < XH_RING; k++) {
        struct xh_ev *e = &xh_ring[cpu][(xh_idx[cpu] - 1 - k) % XH_RING];

        if (e->file)
            kprintf("  t=%lu %s:%d handler %p\n", (unsigned long) e->t, e->file,
                    e->line, e->handler);
    }
    crash();
}

/* Lifecycle trace of thread_t objects: readies, dequeues, running, destroys and
 * frees, with the call site and its caller. A thread found zeroed on a ready
 * queue was freed while queued or readied after it was dead; the events for its
 * address say which. Lock-free and lossy, like the blocked_on trace.
 */
struct thr_ev {
    uint64_t t;
    const char *file;
    thread_t *thp;
    void *ra;
    int line;
    unsigned char cpu;
    unsigned char state;
    unsigned short itf;
};

#define THR_RING 4096
static struct thr_ev thr_ring[THR_RING];
static atomic_uint thr_idx;

void thr_trace(const char *file, int line, thread_t *thp, void *ra)
{
    unsigned i = atomic_fetch_add_explicit(&thr_idx, 1, memory_order_relaxed);
    struct thr_ev *e = &thr_ring[i % THR_RING];

    e->t = r_time();
    e->file = file;
    e->line = line;
    e->thp = thp;
    e->ra = ra;
    e->cpu = (unsigned char) get_cpunum();
    e->state = (unsigned char) thp->state;
    e->itf = (unsigned short) thp->internal_flags;
}

void thr_trace_dump(const thread_t *thp)
{
    unsigned end = atomic_load_explicit(&thr_idx, memory_order_relaxed);
    unsigned shown = 0;

    kprintf("thread %p events, newest first:\n", (const void *) thp);
    for (unsigned k = 1; k <= THR_RING && k <= end && shown < 32; k++) {
        struct thr_ev *e = &thr_ring[(end - k) % THR_RING];

        if (e->thp != thp)
            continue;
        kprintf("  t=%lu cpu%u %s:%d state %u itf %#x ra %p\n",
                (unsigned long) e->t, (unsigned) e->cpu, e->file, e->line,
                (unsigned) e->state, (unsigned) e->itf, e->ra);
        shown++;
    }
}

#endif /* CONFIG_DEBUG_INJECT */
