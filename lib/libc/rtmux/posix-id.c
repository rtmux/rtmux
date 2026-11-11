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

pid_t getpgrp(void)
{
    proc_getsetid_t msg = {
        .i.type = _PROC_GETSETID,
        .i.subtype = _PROC_ID_GETID,
    };

    if (msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) ==
        -1) {
        return getpid();
    }
    return msg.o.pgrp > 0 ? msg.o.pgrp : getpid();
}

pid_t getppid(void)
{
    proc_getsetid_t msg = {
        .i.type = _PROC_GETSETID,
        .i.subtype = _PROC_ID_GETID,
    };

    if (msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) ==
        -1) {
        return 1;
    }
    return msg.o.ppid;
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
