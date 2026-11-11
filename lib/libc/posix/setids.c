/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

static int setids_msg(pid_t pid, id_t eid, id_t rid, unsigned subtype)
{
    proc_getsetid_t msg;

    msg.i.type = _PROC_GETSETID;
    msg.i.subtype = (uint16_t) subtype;
    msg.i.pid = pid;
    msg.i.eid = eid;
    msg.i.rid = rid;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, &msg.o,
                      sizeof msg.o);
}

int setuid(uid_t uid)
{
    if (uid == (uid_t) -1) {
        errno = EINVAL;
        return -1;
    }
    return setids_msg(0, uid, (id_t) -1, _PROC_ID_SETUID);
}

int seteuid(uid_t uid)
{
    if (uid == (uid_t) -1) {
        errno = EINVAL;
        return -1;
    }
    return setids_msg(0, uid, (id_t) -1, _PROC_ID_SETEUID);
}

int setreuid(uid_t rid, uid_t eid)
{
    return setids_msg(0, eid, rid, _PROC_ID_SETREUID);
}

int setgid(gid_t gid)
{
    if (gid == (gid_t) -1) {
        errno = EINVAL;
        return -1;
    }
    return setids_msg(0, gid, (id_t) -1, _PROC_ID_SETGID);
}

int setegid(gid_t gid)
{
    if (gid == (gid_t) -1) {
        errno = EINVAL;
        return -1;
    }
    return setids_msg(0, gid, (id_t) -1, _PROC_ID_SETEGID);
}

int setregid(gid_t rid, gid_t eid)
{
    return setids_msg(0, eid, rid, _PROC_ID_SETREGID);
}
