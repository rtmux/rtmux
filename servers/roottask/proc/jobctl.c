/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Job control: stop/continue state and the terminal servers' view of sessions.
 *
 * A process stops and continues itself (its libc system thread holds and
 * releases the other threads) and reports each transition here, so roottask can
 * set RTMUX_FLG_PROC_STOPPED / RTMUX_FLG_PROC_CONTINUED and the siginfo that
 * waitpid() reports.
 *
 * A terminal server (/dev/text inside roottask, dev-serial as a process) asks
 * the group, session, and orphan questions here. Nothing is cached: every
 * answer is computed from the current process, group, and session state. A
 * server is trusted only for sessions whose controlling terminal it serves, so
 * a plain process cannot use these calls to signal an arbitrary group.
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <sys/procmsg.h>

#define JC_DEAD                                          \
    (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_ZOMBIE |    \
     RTMUX_FLG_PROC_WAITINFO | RTMUX_FLG_PROC_WAITDONE | \
     RTMUX_FLG_PROC_DESTROYALL)

/* A consistent copy of one process's group and session state. */
struct jc_snap {
    pid_t pid;
    pid_t ppid;
    pid_t pgrp;
    pid_t sid; /* session leader pid, 0 when there is no session */
    pid_t fg;  /* foreground group of the session's terminal */
    int fd;    /* roottask's descriptor to the terminal, or -1 */
    unsigned flags;
    channel_t *chp; /* the process's system channel */
};

static int jc_snapshot(pid_t pid, struct jc_snap *s)
{
    root_proc_info_t qinfo;

    if (!root_proc_lock(pid))
        return ESRCH;
    if (process_query_info(pid, &qinfo) != EOK) {
        root_proc_unlock(pid);
        return ESRCH;
    }
    s->pid = pid;
    s->ppid = qinfo.parent_pid;
    s->pgrp = qinfo.pgrp ? qinfo.pgrp : pid;
    s->sid = qinfo.session ? qinfo.sess_leader : 0;
    s->fg = qinfo.sess_pgrp;
    s->fd = qinfo.session ? qinfo.sess_fd : -1;
    s->flags = qinfo.flags;
    s->chp = qinfo.sys_chp;
    root_proc_unlock(pid);
    return EOK;
}

/* Pid of the server behind roottask's terminal descriptor fd, or 0. */
static pid_t jc_tty_server(int fd)
{
    struct _server_info info;

    if (fd < 0 || connect_server_info(0, (uint64_t) fd, &info) != fd)
        return 0;
    return info.pid;
}

/* Whether the group pgrp of session sid has no member whose parent is in a
 * different group of the same session (POSIX 3.299 "orphaned process group").
 * Dead members and parents do not count.
 */
static int jc_orphaned(pid_t pgrp, pid_t sid)
{
    for (unsigned i = 0; i < ROOT_PROCESS_MAX; i++) {
        pid_t pid = root_process[i].pid;
        struct jc_snap m, p;

        if (pid == 0 || jc_snapshot(pid, &m) != EOK)
            continue;
        if (m.pgrp != pgrp || m.sid != sid || (m.flags & JC_DEAD))
            continue;
        if (m.ppid > 0 && jc_snapshot(m.ppid, &p) == EOK && p.sid == sid &&
            p.pgrp != pgrp && !(p.flags & JC_DEAD))
            return 0;
    }
    return 1;
}

/* Post signo to every live member of group pgrp in session sid, except pid 1
 * and system servers: a terminal signal must not take them down along with the
 * jobs.
 *
 * Returns the number of members signalled.
 */
static int jc_signal_group(pid_t pgrp, pid_t sid, int signo, pid_t sender)
{
    int n = 0;

    /* POSIX 2.4.3: the stop signals never stop an orphaned group, whose members
     * nobody is left to continue. Say it was handled, as a deliverable signal
     * that is then ignored would be.
     */
    if ((signo == SIGTSTP || signo == SIGTTIN || signo == SIGTTOU) &&
        jc_orphaned(pgrp, sid))
        return 1;

    for (unsigned i = 0; i < ROOT_PROCESS_MAX; i++) {
        pid_t pid = root_process[i].pid;
        struct jc_snap m;

        if (pid == 0 || pid == ROOTTASK_PID || root_proc(pid)->system ||
            jc_snapshot(pid, &m) != EOK)
            continue;
        if (m.pgrp != pgrp || m.sid != sid || (m.flags & JC_DEAD) || !m.chp)
            continue;

        /* Post under the lock so the channel cannot go away with the process.
         */
        if (root_proc_lock(pid)) {
            root_proc_info_t qinfo;

            if (process_query_info(pid, &qinfo) == EOK && qinfo.sys_chp &&
                !(qinfo.flags & JC_DEAD)) {
                signal_kill(qinfo.sys_chp, signo, sender);
                n++;
            }
            root_proc_unlock(pid);
        }
    }
    return n;
}

/* proc_jobctl_query — describe client pid's standing on the terminal that
 * server serves.
 *
 * _JOBCTL_CTTY is set only when pid's session has that server's terminal as its
 * controlling terminal; the others are meaningful only then.
 *
 * Returns ESRCH when pid is gone.
 */
int proc_jobctl_query(pid_t server, pid_t pid, struct _proc_jobctl_reply *out)
{
    struct jc_snap c;
    int rc;

    if ((rc = jc_snapshot(pid, &c)) != EOK)
        return rc;
    out->pgrp = c.pgrp;
    out->sid = c.sid;
    out->fg_pgrp = c.sid && c.fd >= 0 ? c.fg : 0;
    out->flags = 0;
    if (!c.sid || server == 0 || jc_tty_server(c.fd) != server)
        return EOK;
    out->flags |= _JOBCTL_CTTY;
    if (c.pgrp == c.fg)
        out->flags |= _JOBCTL_FG;
    else if (jc_orphaned(c.pgrp, c.sid)) /* only a background group matters */
        out->flags |= _JOBCTL_ORPHAN;
    out->flags |= root_proc(pid)->tty_hold;
    return EOK;
}

/* proc_jobctl_signal_fg — signal the foreground group of the session led by
 * sid, for a terminal character (VINTR, VQUIT, VSUSP) typed on server's
 * terminal.
 *
 * Returns ESRCH when no such session is bound to that terminal.
 */
int proc_jobctl_signal_fg(pid_t server, pid_t sid, int signo)
{
    struct jc_snap l;
    int n;

    /* Only what the termios signal characters raise. */
    if ((signo != SIGINT && signo != SIGQUIT && signo != SIGTSTP) || sid <= 0)
        return EINVAL;
    /* Any live member names the session; the leader may be gone. */
    for (unsigned i = 0; i < ROOT_PROCESS_MAX; i++) {
        pid_t pid = root_process[i].pid;

        if (pid == 0 || jc_snapshot(pid, &l) != EOK || l.sid != sid ||
            (l.flags & JC_DEAD) || jc_tty_server(l.fd) != server)
            continue;

        /* The foreground group lives in the session, which every member shares,
         * so the first live member is enough.
         */
        n = jc_signal_group(l.fg, sid, signo, 0);
        return n ? EOK : ESRCH;
    }
    return ESRCH;
}

/* proc_jobctl_signal_bg — send SIGTTIN or SIGTTOU to the group of background
 * client pid. Refused for any other signal and for a client that is not a
 * background member of a session controlled by server's terminal.
 */
int proc_jobctl_signal_bg(pid_t server, pid_t pid, int signo)
{
    struct _proc_jobctl_reply q;
    int rc;

    if (signo != SIGTTIN && signo != SIGTTOU)
        return EINVAL;
    if ((rc = proc_jobctl_query(server, pid, &q)) != EOK)
        return rc;
    if (!(q.flags & _JOBCTL_CTTY) || (q.flags & _JOBCTL_FG))
        return EPERM;
    return jc_signal_group(q.pgrp, q.sid, signo, 0) ? EOK : ESRCH;
}

/* proc_jobctl_setpgrp — tcsetpgrp() as seen by the terminal server: record pgrp
 * as the foreground group of client pid's session.
 *
 * ENOTTY: the terminal is not the client's controlling terminal. EPERM: pgrp is
 * not a group of the client's session. A background client that neither blocks
 * nor ignores SIGTTOU gets EIO when its group is orphaned, else SIGTTOU and
 * EINTR; a process still loading (spawned to take the terminal) is exempt.
 */
int proc_jobctl_setpgrp(pid_t server, pid_t pid, pid_t pgrp)
{
    struct _proc_jobctl_reply q;
    struct jc_snap c;
    int rc;
    int found = 0;

    if (pgrp <= 0)
        return EINVAL;
    if ((rc = jc_snapshot(pid, &c)) != EOK)
        return rc;
    if ((rc = proc_jobctl_query(server, pid, &q)) != EOK)
        return rc;
    if (!(q.flags & _JOBCTL_CTTY))
        return ENOTTY;

    for (unsigned i = 0; i < ROOT_PROCESS_MAX && !found; i++) {
        pid_t m = root_process[i].pid;
        struct jc_snap g;

        if (m != 0 && jc_snapshot(m, &g) == EOK && g.pgrp == pgrp &&
            g.sid == q.sid && !(g.flags & JC_DEAD))
            found = 1;
    }
    if (!found)
        return EPERM;

    if (!(q.flags & _JOBCTL_FG) && !(c.flags & RTMUX_FLG_PROC_LOADING) &&
        !(q.flags & _JOBCTL_HOLD_TTOU)) {
        if (q.flags & _JOBCTL_ORPHAN)
            return EIO;
        (void) jc_signal_group(q.pgrp, q.sid, SIGTTOU, 0);
        return EINTR;
    }

    /* Record the new foreground group on the session; every member shares it.
     * Members read it under their own lock, so take the client's.
     */
    if (!root_proc_lock(pid))
        return ESRCH;
    {
        root_process_t *tmp = root_proc(pid);

        if (tmp->session && tmp->session->fd >= 0)
            tmp->session->pgrp = pgrp;
        else
            rc = ENOTTY;
    }
    root_proc_unlock(pid);
    return rc;
}

/* proc_jobctl_tty_busy — whether another live session already has the terminal
 * served by server for its controlling terminal. A terminal belongs to at most
 * one session; the leader of a session that lost every member is gone with it.
 */
int proc_jobctl_tty_busy(pid_t server, pid_t sid)
{
    for (unsigned i = 0; i < ROOT_PROCESS_MAX; i++) {
        pid_t pid = root_process[i].pid;
        struct jc_snap m;

        if (pid != 0 && jc_snapshot(pid, &m) == EOK && m.sid != 0 &&
            m.sid != sid && !(m.flags & JC_DEAD) && m.fd >= 0 &&
            jc_tty_server(m.fd) == server)
            return 1;
    }
    return 0;
}

/* Tell the parent a child stopped or continued: SIGCHLD, as for an exit. */
static void jc_notify_parent(pid_t pid)
{
    struct jc_snap c, p;

    if (jc_snapshot(pid, &c) != EOK || c.ppid <= 0 ||
        jc_snapshot(c.ppid, &p) != EOK || !p.chp || (p.flags & JC_DEAD))
        return;
    if (root_proc_lock(c.ppid)) {
        root_proc_info_t qinfo;

        if (process_query_info(c.ppid, &qinfo) == EOK && qinfo.sys_chp)
            signal_kill(qinfo.sys_chp, SIGCHLD, pid);
        root_proc_unlock(c.ppid);
    }
}

/* Record a stop or continue the process reported about itself, then wake the
 * parent's waiters (WUNTRACED / WCONTINUED) through the PROC_CODE_STOP/CONT
 * notification handler.
 */
static int jc_state(pid_t pid, int event, int arg)
{
    root_process_t *tmp = root_proc(pid);
    int stopped = event == _JOBCTL_STOPPED;
    root_proc_info_t qinfo;

    if (!root_proc_lock(pid))
        return ESRCH;

    /* A report racing with exit must not stamp a stop onto a process that is
     * dying: proc_terminate discards stale stop siginfo only once, so one
     * written after it would make waitpid() report a stop for a zombie.
     */
    if (process_query_info(pid, &qinfo) != EOK || (qinfo.flags & JC_DEAD)) {
        root_proc_unlock(pid);
        return ESRCH;
    }
    tmp->siginfo.si_signo = SIGCHLD;
    tmp->siginfo.si_code = stopped ? CLD_STOPPED : CLD_CONTINUED;
    tmp->siginfo.si_status = stopped ? arg : SIGCONT;
    tmp->siginfo.si_pid = pid;

    /* Continuing must clear the flag first: the kernel refuses to release the
     * threads of a stopped process. Set under the lock, with the liveness
     * check, so exit cannot interleave.
     */
    if (stopped)
        (void) ProcessFlagsOp(pid, RTMUX_FLG_PROC_STOPPED,
                              RTMUX_FLG_PROC_CONTINUED);
    else
        (void) ProcessFlagsOp(pid, RTMUX_FLG_PROC_CONTINUED,
                              RTMUX_FLG_PROC_STOPPED);
    root_proc_unlock(pid);
    (void) msg_send_notification(
        ROOTTASK_COID, -1, stopped ? PROC_CODE_STOP : PROC_CODE_CONT, pid);
    jc_notify_parent(pid);
    return EOK;
}

/* proc_msg_jobctl — handle a _PROC_JOBCTL message; see sys/procmsg.h. */
int proc_msg_jobctl(ioserv_context_t *ctp, proc_jobctl_t *msg)
{
    pid_t caller = ctp->info.pid;
    pid_t pid = msg->i.pid;
    int arg = msg->i.arg;
    int arg2 = msg->i.arg2;
    int rc;

    if (ctp->info.msglen < sizeof msg->i)
        return EINVAL;

    switch (msg->i.subtype) {
    case _PROC_JOBCTL_STATE:
        switch (arg) {
        case _JOBCTL_STOPPED:
        case _JOBCTL_CONTINUED:
            return jc_state(caller, arg, arg2);
        case _JOBCTL_TTYMASK:
            root_proc(caller)->tty_hold =
                (unsigned) arg2 & (_JOBCTL_HOLD_TTIN | _JOBCTL_HOLD_TTOU);
            return EOK;
        default:
            return EINVAL;
        }

    case _PROC_JOBCTL_QUERY:
        if ((rc = proc_jobctl_query(caller, pid, &msg->o)) != EOK)
            return rc;
        return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);

    case _PROC_JOBCTL_SIGFG:
        return proc_jobctl_signal_fg(caller, pid, arg);

    case _PROC_JOBCTL_SIGBG:
        return proc_jobctl_signal_bg(caller, pid, arg);

    case _PROC_JOBCTL_SETPGRP:
        return proc_jobctl_setpgrp(caller, pid, arg);

    default:
        return ENOSYS;
    }
}
