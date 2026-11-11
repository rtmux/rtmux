/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/procmsg.h>
#include <sys/resource.h>
#include <sys/rtmux-core.h>

int getrusage(int who, struct rusage *usage)
{
    if (!usage) {
        errno = EFAULT;
        return -1;
    }

    proc_resource_usage_t msg = {
        .i.type = _PROC_RESOURCE,
        .i.subtype = _PROC_RESOURCE_USAGE,
        .i.who = (uint32_t) who,
    };

    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof(msg.i), usage,
                      sizeof(*usage));
}

int getrlimit(int resource, struct rlimit *rlim)
{
    if (resource < 0 || resource >= RLIM_NLIMITS) {
        errno = EINVAL;
        return -1;
    }
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }

    proc_resource_getlimit_t msg = {
        .i.type = _PROC_RESOURCE,
        .i.subtype = _PROC_RESOURCE_GETLIMIT,
        .i.count = 1,
        .i.resource = {(uint32_t) resource},
    };

    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof(msg.i), rlim,
                      sizeof(*rlim));
}

/* Only RLIMIT_CPU is settable (seconds of billed CPU time, enforced against the
 * process's summed thread time); other resources fail with ENOSYS.
 */
int setrlimit(int resource, const struct rlimit *rlim)
{
    if (resource < 0 || resource >= RLIM_NLIMITS) {
        errno = EINVAL;
        return -1;
    }
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }

    proc_resource_setlimit_t msg = {
        .i.type = _PROC_RESOURCE,
        .i.subtype = _PROC_RESOURCE_SETLIMIT,
        .i.count = 1,
        .i.entry = {{.resource = (uint32_t) resource, .limit = *rlim}},
    };

    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof(msg.i), NULL, 0);
}
