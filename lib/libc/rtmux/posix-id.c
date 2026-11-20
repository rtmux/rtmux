/*
 * SPDX-License-Identifier: Apache-2.0
 * POSIX identity functions.
 *
 * The cred-getters call connect_client_info with scoid=-1, which returns the
 * calling process's own _client_info — including its _cred_info. Using a kcall
 * (ecall, dispatched through the kernel) instead of msg_sendnc to proc service
 * means the same code works from any process, including roottask itself: there
 * is no recursive self-message, so no special case is needed.
 *
 * getpid() still reads __pid out of TLS — that field is set at thread creation
 * by thread_waaa().
 */

#include <errno.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/storage.h>
#include <sys/procmsg.h>

pid_t getpid(void)
{
    return __tls()->__pid;
}

uid_t getuid(void)
{
    struct _client_info info;

    if (connect_client_info(-1, &info, 0) == -1)
        return 0;
    return info.cred.ruid;
}

uid_t geteuid(void)
{
    struct _client_info info;

    if (connect_client_info(-1, &info, 0) == -1)
        return 0;
    return info.cred.euid;
}

gid_t getgid(void)
{
    struct _client_info info;

    if (connect_client_info(-1, &info, 0) == -1)
        return 0;
    return info.cred.rgid;
}

gid_t getegid(void)
{
    struct _client_info info;

    if (connect_client_info(-1, &info, 0) == -1)
        return 0;
    return info.cred.egid;
}

/* Ask roottask for the ids of `pid`, or of the caller when 0.
 *
 * Returns -1 when the request fails.
 */
static int getid_msg(pid_t pid, proc_getsetid_t *msg)
{
    msg->i.type = _PROC_GETSETID;
    msg->i.subtype = _PROC_ID_GETID;
    msg->i.pid = pid;
    return msg_sendnc(ROOTTASK_COID, &msg->i, sizeof msg->i, &msg->o,
                      sizeof msg->o);
}

pid_t getpgrp(void)
{
    proc_getsetid_t msg = {0};

    if (getid_msg(0, &msg) == -1)
        return getpid();
    return msg.o.pgrp > 0 ? msg.o.pgrp : getpid();
}

/* The process group (want_sid 0) or session (want_sid 1) of `pid`, or of the
 * caller when 0. proc answers EPERM for a process in another session and ESRCH
 * for no process.
 */
static pid_t getid_of(pid_t pid, int want_sid)
{
    proc_getsetid_t msg = {0};

    if (pid < 0) {
        errno = ESRCH;
        return -1;
    }
    if (getid_msg(pid, &msg) == -1)
        return -1;
    return want_sid ? msg.o.sid : msg.o.pgrp;
}

pid_t getpgid(pid_t pid)
{
    return getid_of(pid, 0);
}

pid_t getppid(void)
{
    proc_getsetid_t msg = {0};

    if (getid_msg(0, &msg) == -1)
        return 1;
    return msg.o.ppid;
}

/* getsid — the session leader's pid for `pid`, or for the caller when 0. */
pid_t getsid(pid_t pid)
{
    return getid_of(pid, 1);
}

pid_t setsid(void)
{
    proc_session_t msg = {
        .i.type = _PROC_SESSION,
        .i.event = _PROC_SESSION_SETSID,
    };

    if (msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0) == -1)
        return -1;
    return getpid();
}
