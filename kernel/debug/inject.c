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
static thread_t * _Atomic sched_failed_thr; /* the caller just failed */

static inline bool inject_op_is_status(unsigned op)
{
    return op == INJECT_OP_SYNC_WAKE_CLAIMS || op == INJECT_OP_SITE_STATUS ||
           op == INJECT_OP_ALLOC_STATUS ||
           op == INJECT_OP_SCHED_SET_STATUS ||
           op == INJECT_OP_SCHED_SET_DISPLACED || op == INJECT_OP_PTE_PROBE ||
           op == INJECT_OP_MANIP_PARTIAL || op == INJECT_OP_PRIO_SETTLE_STATS;
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
};

static _Atomic(thread_t *) stalled_waiter;
static atomic_int wake_claims;
static atomic_int intr_before_switch_pid;

void inject_intr_before_switch(thread_t *thp)
{
    if (thp->state != STATE_INTR)
        return;
    int pid = thp->process->pid;
    if (atomic_compare_exchange_strong_explicit(&intr_before_switch_pid, &pid,
                                                0, memory_order_relaxed,
                                                memory_order_relaxed))
        atomic_or(&thp->flags, RTMUX_FLG_THR_INTR_PENDING);
}

/* Claim a one-shot stall armed for the site's pid and spin for its time. */
static void inject_stall_once(enum inject_site site, const process_t *prp, thread_t *waiter)
{
    int pid = atomic_load_explicit(&stall_site[site].pid, memory_order_acquire);
    uint64_t ms;
    uint64_t end;

    if (pid < 0 || !prp || prp->pid != pid)
        return;
    /* Read the duration the arm published before claiming it. */
    ms = atomic_load_explicit(&stall_site[site].ms, memory_order_relaxed);
    if (!atomic_compare_exchange_strong_explicit(&stall_site[site].pid, &pid,
                                                 -1, memory_order_acq_rel,
                                                 memory_order_relaxed))
        return;
    end = r_time() + ms * (qtimeptr->cycles_per_sec / 1000);
    if (waiter) {
        atomic_store_explicit(&wake_claims, 0, memory_order_relaxed);
        atomic_store_explicit(&stalled_waiter, waiter, memory_order_release);
    }
    atomic_store_explicit(&stall_site[site].taken, true, memory_order_release);
    inject_spin_begin();
    while (!atomic_load_explicit(&stall_site[site].release,
                                 memory_order_acquire) &&
           r_time() < end)
        ;
    inject_spin_end();
    if (waiter)
        atomic_store_explicit(&stalled_waiter, NULL, memory_order_release);
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

/* Called from sync_chain_force_ready after it read the waiter's key, before it
 * takes the chain lock.
 */
void inject_force_ready_stall(thread_t *thp)
{
    if (atomic_load_explicit(&stalled_waiter, memory_order_acquire) == thp)
        atomic_fetch_add_explicit(&wake_claims, 1, memory_order_relaxed);
    inject_stall_once(STALL_FORCE_READY, thp->process, NULL);
}

void inject_sync_waker_stall(thread_t *thp)
{
    inject_stall_once(STALL_SYNC_WAKER, thp->process, thp);
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
    case INJECT_OP_INTR_BEFORE_SWITCH:
        if (!kap->arg0 || kap->arg0 > INT_MAX)
            ret = EINVAL;
        else {
            atomic_store_explicit(&intr_before_switch_pid, (int) kap->arg0,
                                  memory_order_relaxed);
            ret = EOK;
        }
        break;
    case INJECT_OP_STALL_SYNC_WAKER:
        ret = inject_stall_arm(STALL_SYNC_WAKER, kap->arg0, kap->arg1);
        break;
    case INJECT_OP_SYNC_WAKE_CLAIMS:
        ret = atomic_load_explicit(&wake_claims, memory_order_acquire);
        break;
    case INJECT_OP_SITE_STATUS:
        if (kap->arg0 == INJECT_SITE_TURNSTILE)
            ret = atomic_load_explicit(&ts_consumed, memory_order_acquire);
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
            ret = (int) atomic_load_explicit(&prio_idle_runs, memory_order_relaxed);
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
