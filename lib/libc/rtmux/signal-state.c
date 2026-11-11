/*
 * RTMux libc — process-wide signal table and dispatcher.
 *
 * Implements the user-space half of the two-thread signal model. All state
 * lives in this translation unit; the only outside hooks are the functions
 * declared in <signal-state.h>.
 *
 * Concurrency model
 *
 * One libc-private mutex ("signal_lock") protects the entire table. Two parties
 * contend for it:
 *   - the system thread, calling __libc_signal_notification_recv() when a
 *     notification arrives on the per-process system channel;
 *   - any application thread, calling sigaction / sigprocmask /
 *     raise / kill.
 * Handlers are *always* invoked with the lock released, so they can re-enter
 * the public sig* API without deadlocking.
 *
 * Interrupting blocked calls
 *
 * A blocking libc wrapper registers its thread (__libc_wait_enter) for the
 * duration of the kernel call. After a handler installed without SA_RESTART
 * returns, deliver_one picks ONE registered waiter, the lowest tid that has not
 * already been chosen, and breaks its wait with thread_ctl(RTMUX_TCTL_INTERRUPT),
 * so the call returns EINTR. A handler with SA_RESTART interrupts nothing: the
 * call just keeps blocking. The mask is process-wide, so a blocked signal never
 * reaches deliver_one and interrupts nobody; it does so when unblocked. Only
 * threads already inside a wrapper when the signal is delivered are eligible: a
 * handler installed while a call is already blocked does not interrupt it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "signal-state.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

#define LIBC_NSIG MAXSIG /* table size — covers all of [1, MAXSIG] */

/* sigset_t access helpers. RTMux's sigset_t is { uint32_t bits[2]; }, 1-indexed
 * (signo 1 lives in bit 0 of bits[0]).
 */
#define SIGSET_BITS ((unsigned) sizeof(((sigset_t *) 0)->bits[0]) * 8)

static inline int sigset_test(const sigset_t *s, int signo)
{
    unsigned i = (unsigned) (signo - 1);
    return (s->bits[i / SIGSET_BITS] >> (i % SIGSET_BITS)) & 1U;
}

static inline void sigset_set(sigset_t *s, int signo)
{
    unsigned i = (unsigned) (signo - 1);
    s->bits[i / SIGSET_BITS] |= 1U << (i % SIGSET_BITS);
}

static inline void sigset_clr(sigset_t *s, int signo)
{
    unsigned i = (unsigned) (signo - 1);
    s->bits[i / SIGSET_BITS] &= ~(1U << (i % SIGSET_BITS));
}

static inline int sigset_empty(const sigset_t *s)
{
    return s->bits[0] == 0 && s->bits[1] == 0;
}

/* The table. Static-initialized so that any sigaction() call before
 * pthread_mutex_init runs (impossible in practice — but principled) finds a
 * usable mutex.
 */
static struct {
    struct sigaction handlers[LIBC_NSIG + 1]; /* index 1..LIBC_NSIG */
    sigset_t mask;
    sigset_t pending;
    /* Sender of the pending (or last coalesced) instance of each signal; for a
     * coalesced standard signal the last sender wins. Guarded by lock.
     */
    pid_t pending_pid[LIBC_NSIG + 1];
    pthread_mutex_t lock;
    pthread_cond_t wait;
    int pselect_coid;
    unsigned delivered;
    /* Interruptible-wait registry (see signal-state.h). `waiters` and
     * `wait_seq` are guarded by lock. `intr_armed` counts the handlers that can
     * interrupt a blocked call (real handler, no SA_RESTART); it is written
     * under lock and read lock-free by __libc_wait_enter so that I/O costs one
     * atomic load while nothing could interrupt it.
     */
    libc_waiter_t *waiters;
    unsigned wait_seq;
    int intr_armed;
    /* Job control. `stopped` is written only by the system thread (stop and
     * continue are both done there). While it is set the system thread keeps
     * `lock` locked: the other threads are held at arbitrary points, and one
     * stopped inside the table would otherwise keep the system thread from ever
     * taking SIGCONT. `tty_hold` is the _JOBCTL_HOLD_* bits last reported to
     * roottask, guarded by lock.
     */
    int stopped;
    int stop_req; /* a default stop picked by drain() on the system thread */
    unsigned tty_hold;
    pthread_t sys_tid; /* the system thread, once it has received a signal */
} state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .wait = PTHREAD_COND_INITIALIZER,
    .pselect_coid = -1,
};

/* Default-action policy. POSIX defines five categories; we collapse them into
 * terminate / ignore / stop (no core dumps). SIGKILL and SIGSTOP are
 * uncatchable by definition and never reach this table; SIGSTOP and SIGCONT act
 * before it (see __libc_signal_notification_recv).
 */
/* Whether a signal's default action is to ignore it.
 *
 * Parameters:
 *   signo : signal number
 * Return value: 1 if the default action is ignore, 0 if terminate.
 */
static int default_is_ignore(int signo)
{
    switch (signo) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    case SIGCONT: /* continues the process first, see jobctl_cont_locked */
        return 1;
    default:
        return 0;
    }
}

/* Whether a signal's default action is to stop the process. */
static int default_is_stop(int signo)
{
    return signo == SIGSTOP || signo == SIGTSTP || signo == SIGTTIN ||
           signo == SIGTTOU;
}

/* Tell roottask about this process's job control state. Roottask records it for
 * waitpid() and for the terminal servers; there is nothing to do when it is
 * unreachable, and roottask itself has no one to tell.
 */
static void jobctl_report(int event, int arg2)
{
    struct _proc_jobctl msg;

    if (getpid() == ROOTTASK_PID)
        return;
    memset(&msg, 0, sizeof msg);
    msg.type = _PROC_JOBCTL;
    msg.subtype = _PROC_JOBCTL_STATE;
    msg.arg = event;
    msg.arg2 = arg2;
    (void) msg_sendnc(ROOTTASK_COID, &msg, sizeof msg, 0, 0);
}

/* Hand a stop or continue request that came in on an application thread (raise,
 * kill of oneself, a sigprocmask that unblocked a stop signal) to the system
 * thread through roottask: only it may hold the others, as it is the one thread
 * that must stay runnable to take SIGCONT. Returns 0 when posted.
 */
static int jobctl_post(int signo)
{
    proc_kill_t msg;

    msg.i.type = _PROC_KILL;
    msg.i.zero = 0;
    msg.i.pid = (int32_t) getpid();
    msg.i.signo = (int32_t) signo;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0);
}

/* Stop the process: hold every other thread (this one, the system thread, stays
 * runnable to receive SIGCONT and SIGKILL), then report it so waitpid(WUNTRACED)
 * sees the stop only once it has taken effect. Runs on the system thread with
 * state.lock held, which it keeps until the process is continued (see the
 * comment on `stopped`): no other thread can be inside the table when held, and
 * those waiting for the lock are stopped waiting.
 */
static void jobctl_stop_locked(int signo)
{
    state.stopped = 1;
    (void) thread_ctl_r(RTMUX_TCTL_THREADS_HOLD, NULL);
    jobctl_report(_JOBCTL_STOPPED, signo);
}

/* Continue a stopped process and release state.lock, which the stop kept.
 * Roottask is told first: it clears the stopped flag, which the kernel insists
 * on before it lets the threads go. System thread only.
 */
static void jobctl_cont_locked(void)
{
    jobctl_report(_JOBCTL_CONTINUED, SIGCONT);
    (void) thread_ctl_r(RTMUX_TCTL_THREADS_CONT, NULL);
    state.stopped = 0;
    pthread_mutex_unlock(&state.lock);
}

/* Whether a signal can be caught, blocked, or ignored.
 *
 * Parameters:
 *   signo : signal number
 * Return value: 1 if signo is SIGKILL or SIGSTOP (uncatchable), else 0.
 */
static int signo_uncatchable(int signo)
{
    /* SIGKILL and SIGSTOP cannot be caught, blocked, or ignored. */
    return signo == SIGKILL || signo == SIGSTOP;
}

/* How many times, and how often, a delivery re-offers the interrupt to a chosen
 * waiter that has registered but not yet blocked in the kernel (the kernel
 * answers EAGAIN until it does). Tuning: the window is the few instructions
 * between __libc_wait_enter() and the blocking kcall, so one retry normally
 * suffices; 16 x 100 us rides out a preemption of the waiter by a higher
 * priority thread. Past it the waiter simply keeps waiting (a handler ran, the
 * call was not interrupted). The kernel also answers EAGAIN for a stable
 * uninterruptible state (a mutex wait), so the bound doubles as the limit on how
 * long the signal thread spends on such a target.
 */
#define LIBC_INTR_RETRY_MAX 16
#define LIBC_INTR_RETRY_NS 100000ULL

/* Whether a handler installed with these flags can interrupt a blocked call. */
static int handler_interrupts(const struct sigaction *sa)
{
    return sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN &&
           !(sa->sa_flags & SA_RESTART);
}

/* Recompute intr_armed from the table. Caller holds state.lock. */
static void intr_rearm(void)
{
    int i, n = 0;

    for (i = 1; i <= LIBC_NSIG; i++)
        n += handler_interrupts(&state.handlers[i]);
    __atomic_store_n(&state.intr_armed, n, __ATOMIC_RELEASE);
}

/* Register the calling thread as interruptibly blocked (see signal-state.h). */
void __libc_wait_enter(libc_waiter_t *w)
{
    w->tid = 0;
    if (!__atomic_load_n(&state.intr_armed, __ATOMIC_ACQUIRE))
        return;

    w->hit = 0;
    w->tid = (int) pthread_self();
    pthread_mutex_lock(&state.lock);
    w->seq = ++state.wait_seq;
    w->next = state.waiters;
    state.waiters = w;
    pthread_mutex_unlock(&state.lock);
}

/* Unregister; after this returns no delivery will touch w. */
void __libc_wait_exit(libc_waiter_t *w)
{
    libc_waiter_t **pp;
    int saved_errno;

    if (!w->tid)
        return;

    saved_errno = errno; /* the wrapped call's errno must survive */

    pthread_mutex_lock(&state.lock);
    for (pp = &state.waiters; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    }
    pthread_mutex_unlock(&state.lock);
    w->tid = 0;
    errno = saved_errno;
}

/* Drop every registration of a thread that is exiting or was cancelled. tid
 * must be the calling thread's own (the lock-free early return relies on it).
 */
void __libc_wait_purge(int tid)
{
    libc_waiter_t **pp;

    /* This thread's own records were linked under the lock, so an empty list
     * seen here cannot hide one of them.
     */
    if (!__atomic_load_n(&state.waiters, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&state.lock);
    for (pp = &state.waiters; *pp;) {
        if ((*pp)->tid == tid)
            *pp = (*pp)->next;
        else
            pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&state.lock);
}

/* Whether the registration (w, seq) is still in the list. Lock held. */
static int waiter_present(const libc_waiter_t *w, unsigned seq)
{
    const libc_waiter_t *p;

    for (p = state.waiters; p; p = p->next)
        if (p == w)
            return p->seq == seq;
    return 0;
}

/* After a handler without SA_RESTART ran: break the lowest-tid registered
 * waiter out of its blocked call with EINTR. One waiter per signal. Caller
 * holds state.lock; it is dropped only for the retry pauses and is held on
 * return.
 *
 * The kernel call is made with state.lock held and only while the waiter is
 * still registered. A waiter unregisters under the same lock, so the interrupt
 * can never land after it left the wrapped call and blocked somewhere that does
 * not expect EINTR. thread_ctl never blocks, so holding the lock is bounded.
 */
static void interrupt_one_waiter(void)
{
    libc_waiter_t *p, *w = NULL;
    unsigned seq;
    int tid, i;

    for (p = state.waiters; p; p = p->next)
        if (!p->hit && (!w || p->tid < w->tid))
            w = p;
    if (!w)
        return;
    w->hit = 1;
    tid = w->tid;
    seq = w->seq;

    for (i = 0; i < LIBC_INTR_RETRY_MAX; i++) {
        uint64_t ns = LIBC_INTR_RETRY_NS;
        int r = thread_ctl_r(RTMUX_TCTL_INTERRUPT, (void *) (intptr_t) tid);

        if ((r < 0 ? -r : r) != EAGAIN)
            return; /* EOK: interrupted; ESRCH: the thread is gone */

        /* Registered but not blocked in the kernel yet: pause and re-offer. */
        pthread_mutex_unlock(&state.lock);
        /* Raw timer, not nanosleep(): that wrapper would register the signal
         * thread itself as a waiter.
         */
        (void) __timer_timeout_r(CLOCK_MONOTONIC, RTMUX_TIMEOUT_NANOSLEEP, NULL,
                                 &ns, NULL);
        pthread_mutex_lock(&state.lock);
        if (!waiter_present(w, seq))
            return; /* the call ended on its own */
    }
    /* Gave up: let a later signal choose this waiter again. */
    w->hit = 0;
}

/* Reset the process-wide signal table to its initial state: every handler
 * SIG_DFL, empty mask, no pending signals.
 */
void __libc_signal_init(void)
{
    int i;

    /* sigaction[0] is unused (no signal 0).  All others SIG_DFL. */
    for (i = 0; i <= LIBC_NSIG; i++) {
        state.handlers[i].sa_handler = SIG_DFL;
        state.handlers[i].sa_flags = 0;
        sigemptyset(&state.handlers[i].sa_mask);
    }
    sigemptyset(&state.mask);
    sigemptyset(&state.pending);
    memset(state.pending_pid, 0, sizeof state.pending_pid);
    intr_rearm();
}

/* deliver_one — run the handler for "signo" if installed and not blocked.
 * Caller MUST hold state.lock; this function drops the lock around the actual
 * handler call and re-acquires before returning.
 *
 * Returns 1 if a handler ran (or default action terminated us, in which case we
 * don't return), 0 if the signal was blocked or ignored.
 */
static int deliver_one(int signo)
{
    struct sigaction sa;
    sigset_t saved_mask;
    pid_t sender;

    if (sigset_test(&state.mask, signo)) {
        /* blocked — leave it pending */
        sigset_set(&state.pending, signo);
        return 0;
    }
    sigset_clr(&state.pending, signo);

    sa = state.handlers[signo];
    sender = state.pending_pid[signo];

    if (sa.sa_handler == SIG_IGN)
        return 0;

    if (sa.sa_handler == SIG_DFL) {
        if (default_is_ignore(signo))
            return 0;
        if (default_is_stop(signo)) {
            /* No handler ran. Only the system thread stops the process: it does
             * so once drain() is done (stop_req); any other thread hands the
             * signal over to it.
             */
            if (getpid() == ROOTTASK_PID)
                return 0;
            if (pthread_self() == state.sys_tid) {
                state.stop_req = signo;
            } else {
                pthread_mutex_unlock(&state.lock);
                (void) jobctl_post(signo);
                pthread_mutex_lock(&state.lock);
            }
            return 0;
        }

        /* Default action = terminate. Drop the lock first so we don't carry it
         * into _exit.
         */
        pthread_mutex_unlock(&state.lock);
        _exit(128 + signo);
    }

    /* Real handler. POSIX: while running, the signal itself is blocked (unless
     * SA_NODEFER), and any signals listed in sa_mask are also blocked. Save the
     * previous mask so we can restore it after the handler returns.
     */
    saved_mask = state.mask;
    {
        sigset_t hmask = sa.sa_mask;
        if (!(sa.sa_flags & SA_NODEFER))
            sigset_set(&hmask, signo);
        /* hmask | mask */
        state.mask.bits[0] |= hmask.bits[0];
        state.mask.bits[1] |= hmask.bits[1];
    }

    /* SA_RESETHAND — reset to SIG_DFL before the handler runs */
    if (sa.sa_flags & SA_RESETHAND) {
        state.handlers[signo].sa_handler = SIG_DFL;
        state.handlers[signo].sa_flags = 0;
        sigemptyset(&state.handlers[signo].sa_mask);
        intr_rearm();
    }

    pthread_mutex_unlock(&state.lock);
    if (sa.sa_flags & SA_SIGINFO) {
        /* No interrupted-thread context exists (handlers run on the system
         * thread), so the ucontext argument is NULL.
         */
        siginfo_t si;

        memset(&si, 0, sizeof si);
        si.si_signo = signo;
        si.si_code = SI_USER;
        si.si_pid = sender;
        sa.sa_sigaction(signo, &si, NULL);
    } else {
        sa.sa_handler(signo);
    }
    pthread_mutex_lock(&state.lock);

    /* Restore mask. If anything became pending and is now unblocked, the
     * caller's drain loop will pick it up.
     */
    state.mask = saved_mask;
    state.delivered++;
    if (handler_interrupts(&sa))
        interrupt_one_waiter();
    pthread_cond_signal(&state.wait);
    if (state.pselect_coid != -1)
        (void) msg_send_notification(state.pselect_coid, -1,
                                     __LIBC_PSELECT_WAKE_CODE, 0);
    return 1;
}

/* drain — run any pending signals that aren't blocked. Caller holds the lock.
 * Loops because each delivery might unblock additional signals (mask is
 * restored after handler returns).
 */
static void drain(void)
{
    int progress;
    int signo;

    do {
        progress = 0;
        for (signo = 1; signo <= LIBC_NSIG; signo++) {
            if (!sigset_test(&state.pending, signo))
                continue;
            if (sigset_test(&state.mask, signo))
                continue;
            if (deliver_one(signo))
                progress = 1;
            else
                /* blocked or ignored — make sure it doesn't loop */
                sigset_clr(&state.pending, signo);
        }
    } while (progress);
}

/* Notification from the system thread. Marks the signal pending and either
 * delivers immediately (unblocked) or leaves it for a future sigprocmask()
 * unblock.
 *
 * Note: handlers invoked from here run on the system thread. Application code
 * that wants in-thread delivery should use raise() (synchronous) or wrap the
 * work in a sigprocmask unblock from the main thread.
 */
void __libc_signal_notification_recv(int signo, pid_t sender)
{
    if (signo < 1 || signo > LIBC_NSIG)
        return;

    state.sys_tid = pthread_self();

    if (getpid() == ROOTTASK_PID && (signo == SIGSTOP || signo == SIGCONT))
        return;

    if (state.stopped) {
        /* We hold state.lock on behalf of the stopped process. Only SIGCONT
         * wakes it and SIGKILL ends it; the rest wait as pending.
         */
        if (signo == SIGKILL)
            _exit(128 + signo);
        if (signo != SIGCONT) {
            if (signo != SIGSTOP) {
                sigset_set(&state.pending, signo);
                state.pending_pid[signo] = sender;
            }
            return;
        }
        jobctl_cont_locked();
    } else if (signo == SIGSTOP) {
        /* SIGSTOP acts whatever the mask and dispositions say. */
        pthread_mutex_lock(&state.lock);
        jobctl_stop_locked(signo);
        return;
    }

    pthread_mutex_lock(&state.lock);
    /* A stop signal discards a pending SIGCONT and the reverse (POSIX 2.4.3). */
    if (signo == SIGCONT) {
        sigset_clr(&state.pending, SIGTSTP);
        sigset_clr(&state.pending, SIGTTIN);
        sigset_clr(&state.pending, SIGTTOU);
    } else if (default_is_stop(signo)) {
        sigset_clr(&state.pending, SIGCONT);
    }
    sigset_set(&state.pending, signo);
    state.pending_pid[signo] = sender;
    drain();
    if (state.stop_req) {
        int sig = state.stop_req;

        state.stop_req = 0;
        jobctl_stop_locked(sig); /* keeps the lock until SIGCONT */
        return;
    }
    pthread_mutex_unlock(&state.lock);
}

/* raise() — synchronous in-thread dispatch. No notification needed; we already
 * are the right thread.
 */
int __libc_signal_raise(int signo)
{
    if (signo < 1 || signo > LIBC_NSIG) {
        errno = EINVAL;
        return -1;
    }

    /* These act on the whole process whatever the mask says, so the system
     * thread takes them like any other process's.
     */
    if ((signo == SIGSTOP || signo == SIGCONT) && getpid() != ROOTTASK_PID)
        return jobctl_post(signo);

    pthread_mutex_lock(&state.lock);
    sigset_set(&state.pending, signo);
    state.pending_pid[signo] = getpid();
    drain();
    pthread_mutex_unlock(&state.lock);
    return 0;
}

int __libc_signal_pause(void)
{
    unsigned delivered;

    pthread_mutex_lock(&state.lock);
    delivered = state.delivered;
    if (!sigset_empty(&state.pending))
        drain();
    /* Raw condvar wait, not pthread_cond_wait(): that wrapper registers the
     * caller in the waiter list under state.lock, which pause holds here. A
     * caught signal still ends this wait through the cond_signal in
     * deliver_one.
     */
    while (delivered == state.delivered)
        (void) sync_condvar_wait_r((sync_t *) &state.wait,
                                   (sync_t *) &state.lock);
    pthread_mutex_unlock(&state.lock);

    errno = EINTR;
    return -1;
}

/* Which of SIGTTIN/SIGTTOU the process blocks or ignores, as _JOBCTL_HOLD_*
 * bits. Caller holds state.lock.
 */
static unsigned tty_hold_bits(void)
{
    unsigned bits = 0;

    if (sigset_test(&state.mask, SIGTTIN) ||
        state.handlers[SIGTTIN].sa_handler == SIG_IGN)
        bits |= _JOBCTL_HOLD_TTIN;
    if (sigset_test(&state.mask, SIGTTOU) ||
        state.handlers[SIGTTOU].sa_handler == SIG_IGN)
        bits |= _JOBCTL_HOLD_TTOU;
    return bits;
}

/* Report a change of the SIGTTIN/SIGTTOU state to roottask, whose terminal
 * servers choose between EIO and a stop signal by it. Caller holds state.lock.
 * The call does not wait on anything this process holds.
 */
static void tty_hold_report(void)
{
    unsigned bits = tty_hold_bits();

    if (bits == state.tty_hold)
        return;
    state.tty_hold = bits;
    jobctl_report(_JOBCTL_TTYMASK, (int) bits);
}

/* sigaction() — real impl. Reject SIGKILL/SIGSTOP per POSIX. */
int __libc_sigaction(int signo,
                     const struct sigaction *act,
                     struct sigaction *oact)
{
    if (signo < 1 || signo > LIBC_NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (act && signo_uncatchable(signo)) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&state.lock);
    if (oact)
        *oact = state.handlers[signo];
    if (act) {
        state.handlers[signo] = *act;
        intr_rearm();
        if (signo == SIGTTIN || signo == SIGTTOU)
            tty_hold_report();

        /* SIGKILL / SIGSTOP can't even be ignored — but the test above already
         * guards them.
         */
    }
    pthread_mutex_unlock(&state.lock);
    return 0;
}

/* sigprocmask() — real impl. After mutating, drain any newly unblocked pending
 * signals from the calling thread.
 */
int __libc_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    pthread_mutex_lock(&state.lock);
    if (oset)
        *oset = state.mask;
    if (set) {
        sigset_t nset = *set;
        /* SIGKILL / SIGSTOP can never be blocked. */
        sigset_clr(&nset, SIGKILL);
        sigset_clr(&nset, SIGSTOP);

        switch (how) {
        case SIG_BLOCK:
            state.mask.bits[0] |= nset.bits[0];
            state.mask.bits[1] |= nset.bits[1];
            break;
        case SIG_UNBLOCK:
            state.mask.bits[0] &= ~nset.bits[0];
            state.mask.bits[1] &= ~nset.bits[1];
            break;
        case SIG_SETMASK:
            state.mask = nset;
            break;
        default:
            pthread_mutex_unlock(&state.lock);
            errno = EINVAL;
            return -1;
        }
    }
    if (set)
        tty_hold_report();
    /* drain anything that's now unblocked */
    if (!sigset_empty(&state.pending))
        drain();
    pthread_mutex_unlock(&state.lock);
    return 0;
}

int __libc_signal_pselect_begin(const sigset_t *set,
                                int coid,
                                sigset_t *oset,
                                unsigned *seq)
{
    sigset_t nset = *set;

    sigset_clr(&nset, SIGKILL);
    sigset_clr(&nset, SIGSTOP);

    pthread_mutex_lock(&state.lock);
    if (coid != -1 && state.pselect_coid != -1) {
        pthread_mutex_unlock(&state.lock);
        errno = EBUSY;
        return -1;
    }

    *oset = state.mask;
    *seq = state.delivered;
    state.mask = nset;
    if (coid != -1)
        state.pselect_coid = coid;
    if (!sigset_empty(&state.pending))
        drain();
    pthread_mutex_unlock(&state.lock);
    return 0;
}

int __libc_signal_pselect_end(int coid, const sigset_t *oset, unsigned seq)
{
    int interrupted;

    pthread_mutex_lock(&state.lock);
    interrupted = state.delivered != seq;
    if (coid != -1 && state.pselect_coid == coid)
        state.pselect_coid = -1;
    state.mask = *oset;
    if (!sigset_empty(&state.pending))
        drain();
    pthread_mutex_unlock(&state.lock);
    return interrupted;
}
