/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * stall-detector.c — kernel-side watchdog that fires when the scheduler stops
 * making forward progress. Dumps per-CPU active threads and every
 * process/thread state, so a silent deadlock can be localized to a specific
 * pid/tid/blocked-on pair.
 *
 * Progress is measured directly, not inferred from console output. Roottask's
 * heartbeat thread bumps heartbeat_seq on a fixed interval, and it can only do
 * so if the scheduler dispatches it. This watchdog runs from the timer IRQ —
 * independent of the scheduler — so when dispatch wedges and heartbeat_seq
 * stops advancing, the watchdog sees it and dumps. Detecting a stall and
 * printing one are deliberately decoupled: the detector keys on the heartbeat
 * counter, never on kprintf output.
 *
 * Arming: the watch stays disabled until the first heartbeat is seen,
 *          so early boot — before roottask starts beating — never trips.
 * Firing: once armed, if heartbeat_seq does not advance for
 *          HEARTBEAT_STALL_NSEC of wall-clock time, fire the dump once.
 *
 * Runs from every CPU's clock_handler, before the in_kernel_context() bailout,
 * so the watchdog still fires even when the current CPU is stuck in a kernel
 * critical section.
 *
 * stall_detector_tick() is LOCK-FREE: a volatile read of heartbeat_seq and an
 * atomic CAS on the once-per-wedge "fired" flag. No spinlock is acquired in the
 * polling path. Exactly one CPU wins the dump (CAS on "fired") and runs
 * stall_dump() — the only place a kprintf burst (and its kprintf_slock)
 * happens, gated to fire at most once per wedge.
 *
 * Roottask wait scan: a roottask pool thread that sends to a wedged user server
 * (lkl-fsd, a slow mount) does not stop the heartbeat thread, so the check
 * above never fires. Once per STALL_SCAN_NSEC the boot hart's tick therefore
 * walks pid 1's thread vector and prints one "liveness:" line for every thread
 * that has been blocked on another party longer than STALL_WAIT_TIMEOUT_NSEC;
 * see stall_scan_roottask_waiters().
 *
 * License: Apache-2.0
 */

#include <core/globals.h>
#include <stdatomic.h>

#ifdef CONFIG_STALL_DETECTOR

/* Fire after the heartbeat has been silent this long, measured in WALL-CLOCK
 * time (not clock ticks). The roottask heartbeat interval is 5 s, so 15 s = ~3
 * missed beats — a real wedge, with margin for benign scheduling jitter.
 *
 * Why wall-clock, not a tick count: stall_detector_tick() runs from EVERY
 * hart's clock_handler, so a single per-tick counter was bumped by all N
 * ticking harts at once and reached a "15 s" tick threshold in 15 s / N —
 * firing spuriously between two healthy 5 s-apart beats on SMP (e.g. ~1.9 s at
 * N=8). qtimeptr->nsec is a monotonic since-boot nanosecond counter advanced by
 * the boot CPU ALONE (clock-core.c), so it tracks real elapsed time regardless
 * of how many harts tick.
 *
 * Caveat: if the boot CPU itself wedges, qtimeptr->nsec freezes and this
 * watchdog goes blind — that catastrophic case needs a separate mechanism. The
 * common wedge (a non-boot hart, including the one running the heartbeat
 * thread, stuck) keeps nsec advancing and is still caught from any hart.
 */
#define HEARTBEAT_STALL_NSEC (15ULL * 1000000000ULL)

/* A roottask thread blocked on another party (send, reply, mutex, condvar,
 * semaphore, join) this long is named. The roottask heartbeat period is 5 s and
 * a healthy server answers a roottask request in milliseconds, so 5 s is a
 * wedge, not jitter.
 */
#define STALL_WAIT_TIMEOUT_NSEC (5ULL * 1000000000ULL)

/* Period of the roottask wait scan. A named thread is reported between
 * STALL_WAIT_TIMEOUT_NSEC and STALL_WAIT_TIMEOUT_NSEC + STALL_SCAN_NSEC after
 * it blocked.
 */
#define STALL_SCAN_NSEC (1ULL * 1000000000ULL)

/* Most waiters one scan records and prints. Records live on the timer-trap
 * stack; a thread past the cap stays unlatched and is named by the next scan.
 */
#define STALL_WAIT_MAX 8U

/* Console-lock attempts before a "liveness:" line is parked in the hart's crash
 * buffer, same bounded print as the neighbour watchdog.
 */
#define STALL_WAIT_LOCK_RETRIES 20000U

/* Heartbeat liveness counter, bumped by kext_heartbeat (roottask's heartbeat
 * thread). It advances only when the scheduler dispatches that thread, so a
 * stale value is a direct, kprintf-independent signal that forward progress has
 * stopped.
 */
extern volatile unsigned heartbeat_seq;

static unsigned prev_hb_seq;  /* last heartbeat_seq we observed       */
static int hb_seen;           /* set once the heartbeat first beats   */
static uint64_t last_hb_nsec; /* qtimeptr->nsec when it last advanced */
static atomic_int fired;      /* dump claimed (one CPU wins)        */

/* Map a thread state value to its short display name. Parameters:
 *   s : thread state.
 * Return value: a static name string ("???" for unknown states).
 */
static const char *state_name(unsigned s)
{
    switch (s) {
    case STATE_DEAD:
        return "DEAD";
    case STATE_RUNNING:
        return "RUN";
    case STATE_READY:
        return "RDY";
    case STATE_STOPPED:
        return "STOP";
    case STATE_SEND:
        return "SEND";
    case STATE_RECEIVE:
        return "RECV";
    case STATE_REPLY:
        return "REPLY";
    case STATE_STACK:
        return "STACK";
    case STATE_WAITTHREAD:
        return "WAITTHR";
    case STATE_WAITPAGE:
        return "WAITPG";
    case STATE_SIGSUSPEND:
        return "SIGSUSP";
    case STATE_SIGWAITINFO:
        return "SIGWAIT";
    case STATE_NANOSLEEP:
        return "NSLEEP";
    case STATE_MUTEX:
        return "MUTEX";
    case STATE_CONDVAR:
        return "CONDVAR";
    case STATE_JOIN:
        return "JOIN";
    case STATE_INTR:
        return "INTR";
    case STATE_SEM:
        return "SEM";
    case STATE_WAITCTX:
        return "WAITCTX";
    default:
        return "???";
    }
}

/* One waiter copied out of the thread under the RCU read sections. */
struct stall_wait {
    int pid;
    int tid;
    unsigned state;
    uint64_t dur_ns;
    int chid;  /* SEND/REPLY: channel being waited on, else -1 */
    void *obj; /* other states: the wait object, for the log only */
};

static uint64_t next_scan_nsec; /* boot hart only */

/* States in which a thread waits on another party. RECEIVE (a server waiting
 * for work) and the sleeps are idle by design and never reported.
 */
static int waits_on_party(unsigned s)
{
    return s == STATE_SEND || s == STATE_REPLY || s == STATE_MUTEX ||
           s == STATE_CONDVAR || s == STATE_SEM || s == STATE_JOIN;
}

/* Name roottask threads blocked on another party for more than
 * STALL_WAIT_TIMEOUT_NSEC. Called from the boot hart's timer tick: interrupt
 * context, lock-free, one hart only.
 *
 * Concurrency rule. The scan holds tthread_rcu and process_rcu read sections
 * (RCU-sched: the hart cannot pass a quiescent state inside the trap) so pid
 * 1's process_t and its thread_t objects are not freed under it, and
 * connobj_rcu for the connect_t/channel_t reached through blocked_on. No lock
 * is taken: vec_slock may be held by the code this tick interrupted. The thread
 * vector is read lock-free in the order vector_add() publishes it (new array
 * first, nentries after, old array never reused for a larger count), so
 * nentries is read before the array pointer and no index past the array is
 * formed. The old array of a growing vector is freed at once, not after a grace
 * period (vector_add), so a scan that straddles a growth of pid 1's thread
 * vector may read the freed copy; the heap stays mapped and the copy holds the
 * same thread pointers, the same assumption the timer-wheel IRQ code makes
 * about vectors. Fields the owner updates without a lock (state,
 * timestamp_last_block) are re-read before blocked_on is typed and again after
 * the copy, and a thread that woke or re-blocked meanwhile is skipped until the
 * next scan. stall_reported_ts is written only here. Output goes out after the
 * read sections, through kprintf_bounded(). The period is kept in
 * arch_timer_now_ns() time, the same clock stall_next_deadline() hands the idle
 * timer, so the boot hart wakes for each scan even when it would otherwise
 * sleep until the heartbeat deadline.
 */
static void stall_scan_roottask_waiters(void)
{
    struct stall_wait rec[STALL_WAIT_MAX];
    unsigned nrec = 0;
    unsigned nthreads;
    unsigned i;
    uint64_t now;
    process_t *prp;
    void **tvec;

    now = arch_timer_now_ns(qtimeptr);
    if (now == (uint64_t) -1 || now < next_scan_nsec)
        return;
    next_scan_nsec = now + STALL_SCAN_NSEC;

    rcu_read_lock(&tthread_rcu);
    rcu_read_lock(&connobj_rcu);
    prp = lookup_pid_rcu(1); /* opens the process_rcu read section */
    if (prp) {
        nthreads = __atomic_load_n(&prp->threads.nentries, __ATOMIC_ACQUIRE);
        tvec = rcu_dereference(prp->threads.vector);
        for (i = 0; tvec && i < nthreads && nrec < STALL_WAIT_MAX; i++) {
            thread_t *thp = tvec[i];
            unsigned state;
            uint64_t ts;
            struct stall_wait *w = &rec[nrec];

            if (!thp || ((uintptr_t) thp & 3))
                continue; /* free slot, or destroyed and not yet joined */
            state = thp->state;
            ts = thp->timestamp_last_block;
            if (!waits_on_party(state) || !ts || now <= ts ||
                now - ts <= STALL_WAIT_TIMEOUT_NSEC ||
                ts == thp->stall_reported_ts)
                continue;
            w->pid = prp->pid;
            w->tid = (int) thp->tid;
            w->state = state;
            w->dur_ns = now - ts;
            w->chid = -1;
            w->obj = thp->blocked_on;

            /* blocked_on is only a connect_t while the thread is in SEND or
             * REPLY, and the owner changes both without a lock. Confirm the
             * wait is still the one read before the pointer is typed, and again
             * after; a thread that moved on is skipped until the next scan.
             */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (thp->state != state || thp->timestamp_last_block != ts)
                continue;
            if ((state == STATE_SEND || state == STATE_REPLY) && w->obj) {
                channel_t *chp = ((connect_t *) w->obj)->channel;

                if (chp)
                    w->chid = (int) chp->chid;
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (thp->state != state || thp->timestamp_last_block != ts)
                    continue;
            }
            thp->stall_reported_ts = ts;
            nrec++;
        }
    }
    rcu_read_unlock(&process_rcu);
    rcu_read_unlock(&connobj_rcu);
    rcu_read_unlock(&tthread_rcu);

    for (i = 0; i < nrec; i++) {
        unsigned long ms = (unsigned long) (rec[i].dur_ns / 1000000ULL);

        if (rec[i].chid >= 0)
            kprintf_bounded(STALL_WAIT_LOCK_RETRIES,
                            "liveness: pid %d tid %d %s %lums chid=%d\n",
                            rec[i].pid, rec[i].tid, state_name(rec[i].state),
                            ms, rec[i].chid);
        else
            kprintf_bounded(STALL_WAIT_LOCK_RETRIES,
                            "liveness: pid %d tid %d %s %lums obj=%p\n",
                            rec[i].pid, rec[i].tid, state_name(rec[i].state),
                            ms, rec[i].obj);
    }
}

/* Print the full stall diagnostic: the inkernel state word, each CPU's active
 * thread, and every process/thread's state, so a wedge can be pinned to a
 * specific pid/tid/blocked-on. Fired at most once per wedge (see
 * stall_detector_tick).
 */
static void stall_dump(void)
{
    unsigned cpu;
    unsigned pidx;
    extern volatile unsigned inkernel;
    extern atomic_uint kprintf_slock;
    unsigned ik = inkernel;

    /* Force-release the kprintf spinlock. If some other CPU is stuck holding it
     * (a plausible explanation for a wedge that also stops the heartbeat is
     * someone wedged mid-print), our own kprintf below would spin forever
     * waiting for it — exactly the symptom we're trying to investigate. Release
     * first, print second.
     */
    atomic_store_explicit(&kprintf_slock, 0, memory_order_release);

    kprintf("\n!!! STALL DETECTED: roottask heartbeat silent for ~%u s !!!\n",
            (unsigned) (HEARTBEAT_STALL_NSEC / 1000000000ULL));
    kprintf("-- inkernel=%x (now=%u, lock=%u, owner_cpu=%u, intrmask=%u) --\n",
            ik, !!(ik & INKERNEL_NOW),
            !!(ik & (INKERNEL_NOW >> 1)), /* INKERNEL_LOCK adjacent bit */
            (ik >> 24) & 0xff, ik & 0xff);

    kprintf("-- Per-CPU active thread --\n");
    for (cpu = 0; cpu < num_processors; cpu++) {
        thread_t *thp = get_active_thread(cpu);
        if (!thp) {
            kprintf("  cpu%u: <none>\n", cpu);
            continue;
        }
        kprintf(
            "  cpu%u: pid=%d tid=%d state=%s(%d) pri=%d runcpu=%u "
            "flags=%x syscall=%d pc=%lx ra=%lx sp=%lx "
            "blocked_on=%p proc=%s\n",
            cpu, thp->process ? thp->process->pid : -1, thp->tid,
            state_name(thp->state), thp->state, thp->priority, thp->runcpu,
            thp->flags, thp->syscall, (unsigned long) thp->reg.pc,
            (unsigned long) thp->reg.gpr[1], /* x1 = ra */
            (unsigned long) thp->reg.gpr[2], /* x2 = sp */
            thp->blocked_on,
            /* debug_name moved to root_proc(pid)->debug_name (roottask) */
            "?");
    }

    kprintf("-- All processes & threads --\n");
    for (pidx = 1; pidx < process_vector.nentries; pidx++) {
        process_t *prp;
        unsigned tid;

        if (!VECP(prp, &process_vector, pidx))
            continue;

        kprintf(" pid=%d name=%s flags=%x nthreads=%u\n", prp->pid,
                /* debug_name moved to root_proc(pid)->debug_name (roottask) */
                "?", prp->flags, prp->threads.nentries);

        for (tid = 0; tid < prp->threads.nentries; tid++) {
            thread_t *thp;
            if (!VECP(thp, &prp->threads, tid))
                continue;
            kprintf(
                "   tid=%u state=%s(%d) pri=%d runcpu=%u "
                "flags=%x syscall=%d blocked_on=%p\n",
                thp->tid, state_name(thp->state), thp->state, thp->priority,
                thp->runcpu, thp->flags, thp->syscall, thp->blocked_on);
        }
    }
    kprintf("!!! END STALL DUMP !!!\n");
}

/* Absolute monotonic-ns deadline the idle timer must not sleep past on the boot
 * hart: the next roottask wait scan, at most STALL_SCAN_NSEC away. Every wake
 * also runs the heartbeat check below, which is why the old heartbeat deadline
 * (15 s out) no longer needs to be offered separately. Read-only; see
 * clock_next_deadline().
 */
uint64_t stall_next_deadline(void)
{
    return next_scan_nsec;
}

/* Called from any CPU's clock_handler, before the in_kernel_context() bailout,
 * so it fires even if the boot CPU is stuck inside the kernel. Only one CPU
 * performs the dump (winner of the atomic "fired" flag).
 *
 * The heartbeat advancing means the scheduler is still dispatching roottask's
 * heartbeat thread; once we have seen it beat at least once we watch for it
 * going stale. There is no benign reason for that thread to stop being
 * dispatched for HEARTBEAT_STALL_NSEC.
 */
void stall_detector_tick(void)
{
    unsigned hb;
    uint64_t now;

    now = qtimeptr->nsec; /* monotonic since-boot ns (boot CPU)  */
    if (get_cpunum() == boot_cpunum)
        stall_scan_roottask_waiters();

    if (fired)
        return;

    hb = heartbeat_seq;
    if (hb != prev_hb_seq) {
        prev_hb_seq = hb;
        hb_seen = 1;
        last_hb_nsec = now;
        return;
    }

    if (!hb_seen)
        return; /* roottask not beating yet (early boot) */

    if (now - last_hb_nsec < HEARTBEAT_STALL_NSEC)
        return;

    /* Claim the dump: only one CPU wins the CAS and runs stall_dump(). */
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &fired, &expected, 1, memory_order_acq_rel, memory_order_acquire))
        stall_dump();
}

#endif /* CONFIG_STALL_DETECTOR */
