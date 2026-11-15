/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/iomsg.h>
#include <sys/siginfo.h>
#include <sys/rtmux-core.h>

#include <posix.h>
#include "signal-state.h"

/* Ask a descriptor's I/O service which of the requested conditions are
 * currently satisfied, using the _IO_NOTIFY "poll and disarm" action, and map
 * the result to revents. A service with no notify handler answers ENOSYS; a
 * regular file or directory is genuinely always ready, so it satisfies the
 * requested read/write events at once. Any other ENOSYS descriptor is not
 * pollable, so report POLLERR rather than a false always-ready that would
 * provoke a blocking read. A bad descriptor is POLLNVAL. POLLERR/POLLNVAL are
 * reported regardless of the requested events. The POLL action also disarms any
 * notification a prior _NOTIFY_ACTION_CONDARM left on the descriptor, so this
 * doubles as the disarm pass after a blocking wait. A service reports the
 * output-only POLLHUP/POLLERR conditions (e.g. a pipe read end whose writer
 * closed) through the extended flags field, folded into revents here. A service
 * that runs out of memory answering is neither ready nor in error: that returns
 * -1, and poll() fails with EAGAIN rather than report a false POLLERR.
 */
static short poll_query(int fd, short events)
{
    io_notify_t msg = {0};
    unsigned cond = 0;
    unsigned sat;
    short revents = 0;

    if (events & POLLIN)
        cond |= (unsigned) _NOTIFY_COND_INPUT;
    if (events & POLLOUT)
        cond |= (unsigned) _NOTIFY_COND_OUTPUT;

    msg.i.type = _IO_NOTIFY;
    msg.i.combine_len = sizeof msg.i;
    msg.i.action = _NOTIFY_ACTION_POLL;
    msg.i.flags = cond;
    SIGEV_NONE_INIT(&msg.i.event);

    if (msg_sendnc(fd, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) == -1) {
        struct stat st;

        if (errno == EBADF)
            return POLLNVAL;
        if (errno == ENOMEM)
            return -1;
        if (errno != ENOSYS)
            return POLLERR;

        /* No readiness notification. Only a regular file or directory is
         * genuinely always ready; other descriptors are not pollable yet.
         */
        if (fstat(fd, &st) != 0 ||
            (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)))
            return POLLERR;
        if (events & POLLIN)
            revents |= POLLRDNORM;
        if (events & POLLOUT)
            revents |= POLLOUT;
        return revents;
    }

    sat = msg.o.flags;
    if ((events & POLLIN) && (sat & (unsigned) _NOTIFY_COND_INPUT))
        revents |= POLLRDNORM;
    if ((events & POLLOUT) && (sat & (unsigned) _NOTIFY_COND_OUTPUT))
        revents |= POLLOUT;
    /* Output-only conditions the service reports regardless of the request. */
    revents |= (short) (msg.o.flags_exten & (unsigned) (POLLHUP | POLLERR));
    return revents;
}

/* Non-blocking pass: set every descriptor's revents and return the ready count.
 * A negative descriptor is ignored. Because poll_query issues the POLL action,
 * this also disarms any notification still armed from a prior wait. Every
 * descriptor is visited even after a service runs out of memory, so none is
 * left armed; the pass then returns -1 with errno set to EAGAIN.
 */
static int poll_scan(struct pollfd *fds, nfds_t nfds)
{
    int ready = 0, nomem = 0;
    nfds_t i;

    for (i = 0; i < nfds; i++) {
        short revents;

        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        revents = poll_query(fds[i].fd, fds[i].events);
        if (revents == -1) {
            nomem = 1;
            continue;
        }
        fds[i].revents = revents;
        if (revents)
            ready++;
    }
    if (nomem) {
        errno = EAGAIN; /* POSIX: internal allocation failed */
        return -1;
    }
    return ready;
}

/* Cancellation cleanup for a wait: poll is a cancellation point, and a thread
 * cancelled while blocked never reaches the disarm pass, so its arms would stay
 * on the descriptors and later fire at a connection the process has reused.
 */
struct poll_set {
    struct pollfd *fds;
    nfds_t nfds;
};

static void poll_disarm(void *arg)
{
    const struct poll_set *set = arg;
    int saved = errno;

    (void) poll_scan(set->fds, set->nfds);
    errno = saved;
}

/* The private channel a wait receives readiness pulses on. */
struct poll_chan {
    int chid;
    int coid;
};

/* Leaves errno as it was, so a caller's result survives the release. */
static void poll_chan_release(void *arg)
{
    const struct poll_chan *ch = arg;
    int saved = errno;

    if (ch->coid != -1)
        __connect_detach((uint64_t) ch->coid);
    if (ch->chid != -1)
        (void) channel_destroy(ch->chid);
    errno = saved;
}

/* Arm a readiness notification on fd that delivers a pulse to coid when a
 * requested condition later becomes satisfied. _NOTIFY_ACTION_CONDARM arms only
 * if nothing is satisfied yet: the service answers EAGAIN once armed, or
 * replies with the already-satisfied conditions (no arm, no pulse) if the
 * descriptor is ready now.
 *
 * Returns the satisfied revents (non-zero, fd not armed), 0 (armed, a pulse
 * will follow), or POLLNVAL/POLLERR on error. A service that could not record
 * the arm (ENOMEM) returns -1: the descriptor is neither ready nor in error, so
 * poll() must fail with EAGAIN rather than report a condition.
 */
static short poll_arm(int fd, short events, int coid)
{
    io_notify_t msg = {0};
    unsigned cond = 0;
    unsigned sat;
    short revents = 0;

    if (events & POLLIN)
        cond |= (unsigned) _NOTIFY_COND_INPUT;
    if (events & POLLOUT)
        cond |= (unsigned) _NOTIFY_COND_OUTPUT;

    msg.i.type = _IO_NOTIFY;
    msg.i.combine_len = sizeof msg.i;
    msg.i.action = _NOTIFY_ACTION_CONDARM;
    msg.i.flags = cond;
    /* Priority -1: the pulse inherits the receiver's priority. */
    SIGEV_NOTIFICATION_INIT(&msg.i.event, coid, -1, 0, 0);

    if (msg_sendnc(fd, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) == -1) {
        if (errno == EAGAIN) /* armed; a pulse will wake the poll */
            return 0;
        if (errno == EBADF)
            return POLLNVAL;
        if (errno == ENOMEM)
            return -1;
        return POLLERR;
    }

    sat = msg.o.flags;
    if ((events & POLLIN) && (sat & (unsigned) _NOTIFY_COND_INPUT))
        revents |= POLLRDNORM;
    if ((events & POLLOUT) && (sat & (unsigned) _NOTIFY_COND_OUTPUT))
        revents |= POLLOUT;
    /* Output-only conditions the service reports regardless of the request. */
    revents |= (short) (msg.o.flags_exten & (unsigned) (POLLHUP | POLLERR));
    return revents;
}

/* Arm every waitable descriptor, filling revents for any already ready.
 * Returns the ready count, or -1 when a service could not record an arm.
 */
static int poll_arm_all(struct pollfd *fds, nfds_t nfds, int coid)
{
    int ready = 0;
    nfds_t i;

    for (i = 0; i < nfds; i++) {
        short revents;

        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        revents = poll_arm(fds[i].fd, fds[i].events, coid);
        if (revents == -1)
            return -1;
        fds[i].revents = revents;
        if (revents)
            ready++;
    }
    return ready;
}

/* True once the monotonic clock has reached the deadline. */
static int poll_deadline_passed(const struct timespec *deadline)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

/* Block until a descriptor becomes ready or the timeout elapses. Arms a
 * SIGEV_NOTIFICATION on each not-ready descriptor pointing at a private
 * channel, then waits on that channel with a receive timeout. A pulse re-runs
 * the level-triggered scan; a timeout returns 0; a signal returns -1/EINTR.
 * timeout_ms == -1 blocks indefinitely. With no waitable descriptor (nfds 0 or
 * all negative) it degenerates to a pure timed/indefinite wait.
 */
static int poll_wait_channel(struct pollfd *fds,
                             nfds_t nfds,
                             int timeout_ms,
                             int chid,
                             int coid)
{
    struct poll_set set = {.fds = fds, .nfds = nfds};
    struct timespec deadline;
    int ready, err = 0;

    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_cleanup_push(poll_disarm, &set);
    for (;;) {
        struct _notification pulse;
        int rc;

        ready = poll_arm_all(fds, nfds, coid);
        if (ready < 0) {
            err = EAGAIN; /* POSIX: internal allocation failed */
            break;
        }
        if (ready > 0)
            break;
        if (timeout_ms >= 0 && poll_deadline_passed(&deadline))
            break;

        if (timeout_ms >= 0) {
            uint64_t t = (uint64_t) timespec2nsec(&deadline);
            int trc = __timer_timeout_r(CLOCK_MONOTONIC,
                                        TIMER_ABSTIME | RTMUX_TIMEOUT_RECEIVE,
                                        NULL, &t, NULL);
            if (trc != EOK) {
                /* Arming the receive timeout failed: clear any half-set
                 * per-thread timeout state and report the error rather than
                 * masking it as a spurious timeout.
                 */
                (void) __timer_timeout_r(CLOCK_MONOTONIC, 0, NULL, NULL, NULL);
                err = trc < 0 ? -trc : trc;
                ready = -1;
                break;
            }
        }
        rc = msg_receive(chid, &pulse, sizeof pulse, NULL);
        (void) __timer_timeout_r(CLOCK_MONOTONIC, 0, NULL, NULL, NULL);

        if (rc == -1) {
            /* Unblocked without a pulse: the receive timeout fired or a signal
             * arrived. Treat it as a timeout when the timer reports ETIMEDOUT
             * or the monotonic deadline has passed; otherwise it is a signal
             * and poll must fail with EINTR.
             */
            if (timeout_ms >= 0 &&
                (errno == ETIMEDOUT || poll_deadline_passed(&deadline)))
                break;
            err = EINTR;
            ready = -1;
            break;
        }
        if (pulse.code == __LIBC_PSELECT_WAKE_CODE) {
            err = EINTR;
            ready = -1;
            break;
        }

        /* A readiness pulse arrived; loop to re-scan (level-triggered) and, if
         * a racer already consumed the readiness, re-arm and keep waiting.
         */
    }
    pthread_cleanup_pop(0);

    /* Disarm every armed descriptor. On success poll_scan also collects the
     * authoritative ready set; on an error exit its result is discarded (and it
     * may clobber errno, so the saved code is applied last).
     */
    if (ready < 0)
        (void) poll_scan(fds, nfds);
    else if ((ready = poll_scan(fds, nfds)) < 0)
        err = EAGAIN;
    if (err)
        errno = err;
    return ready;
}

/* Make the private channel. A fixed-priority channel: a delivered readiness
 * pulse must not drag the polling thread down to the pulse's (process) priority
 * on wake.
 *
 * Returns 0, or -1 with errno set.
 */
static int poll_chan_create(struct poll_chan *ch)
{
    ch->coid = -1;
    if ((ch->chid = channel_create(RTMUX_FLG_CHF_FIXED_PRIORITY)) == -1)
        return -1;
    ch->coid = connect_attach(0, 0, ch->chid, RTMUX_SIDE_CHANNEL, 0);
    if (ch->coid == -1) {
        int saved = errno;

        (void) channel_destroy(ch->chid);
        errno = saved;
        return -1;
    }
    return 0;
}

static int poll_block(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    struct poll_chan ch;
    int ready;

    if (poll_chan_create(&ch) == -1)
        return -1;
    pthread_cleanup_push(poll_chan_release, &ch);
    ready = poll_wait_channel(fds, nfds, timeout_ms, ch.chid, ch.coid);
    pthread_cleanup_pop(1);
    return ready;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    int ready, waitable = 0;
    nfds_t i;

    if (timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }

    ready = poll_scan(fds, nfds);
    if (ready)
        return ready;
    if (timeout_ms == 0)
        return 0;

    for (i = 0; i < nfds; i++)
        if (fds[i].fd >= 0)
            waitable++;

    /* No descriptor to watch: a finite timeout is a plain sleep; an infinite
     * one blocks until a signal (handled by poll_block's degenerate wait).
     */
    if (waitable == 0 && timeout_ms >= 0) {
        struct timespec ts = {
            .tv_sec = timeout_ms / 1000,
            .tv_nsec = (long) (timeout_ms % 1000) * 1000000L,
        };

        if (nanosleep(&ts, NULL) == -1)
            return -1;
        return 0;
    }

    return poll_block(fds, nfds, timeout_ms);
}

/* A pselect wait: its channel and the signal mask it replaced. */
struct poll_psel {
    struct poll_chan ch;
    sigset_t oldmask;
    unsigned seq;
};

/* Cancellation cleanup: give the process back its signal mask and the pselect
 * wake slot, which would otherwise stay claimed by a dead thread.
 */
static void poll_psel_cancel(void *arg)
{
    struct poll_psel *ps = arg;

    (void) __libc_signal_pselect_end(ps->ch.coid, &ps->oldmask, ps->seq);
    poll_chan_release(&ps->ch);
}

int __poll_pselect(struct pollfd *fds,
                   nfds_t nfds,
                   int timeout_ms,
                   const sigset_t *sigmask)
{
    struct poll_psel ps = {.ch = {.chid = -1, .coid = -1}};
    int ready, saved;

    if (!sigmask)
        return poll(fds, nfds, timeout_ms);
    if (timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }

    if (timeout_ms != 0 && poll_chan_create(&ps.ch) == -1)
        return -1;

    if (__libc_signal_pselect_begin(sigmask, ps.ch.coid, &ps.oldmask,
                                    &ps.seq) == -1) {
        poll_chan_release(&ps.ch);
        return -1;
    }

    pthread_cleanup_push(poll_psel_cancel, &ps);
    ready = poll_scan(fds, nfds);
    if (ready == 0 && timeout_ms != 0)
        ready =
            poll_wait_channel(fds, nfds, timeout_ms, ps.ch.chid, ps.ch.coid);
    pthread_cleanup_pop(0);
    saved = errno;
    if (__libc_signal_pselect_end(ps.ch.coid, &ps.oldmask, ps.seq)) {
        saved = EINTR;
        ready = -1;
    }
    errno = saved;
    poll_chan_release(&ps.ch);
    return ready;
}
