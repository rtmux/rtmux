/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/conf.h>
#include <sys/sysmsg.h>
#include <inttypes.h>

static int _confstr_get(int name, long *value, char *str)
{
    sys_conf_t msg;
    iov_t iov[2];

    msg.i.type = _SYS_CONF;
    msg.i.subtype = _SYS_SUB_GET;
    msg.i.cmd = _CONF_STR;
    msg.i.name = name;
    msg.i.value = *value;
    SETIOV(iov + 0, &msg.o, sizeof msg.o);
    SETIOV(iov + 1, str, *value);
    if (msg_sendvnc_r(ROOTTASK_COID, (iov_t *) &msg.i, -(ssize_t) sizeof msg.i,
                      iov, 2)) {
        return -1;
    }

    *value = msg.o.value;
    return msg.o.match;
}

static long _confstr_table[] = {
    [0] = _CONF_LINK_FCN,
    [1] = (intptr_t) _confstr_get,
};

size_t confstr(int name, char *buff, size_t len)
{
    long size = (long) len;

    /* POSIX: an invalid name is EINVAL (return 0). RTMux's valid _CS_ names are
     * the standard _CS_PATH.._CS_ARCHITECTURE and the vendor-reserved range
     * at/above _CS_VENDOR; _CS_NONE, negatives, and the gap between the two
     * ranges are invalid. A valid but unseeded name still returns 0, with no
     * errno -- so this is what lets confstr distinguish the two. Keep the
     * ranges in sync with the _CS_ constants in <confname.h>.
     */
    if (name <= _CS_NONE || (name > _CS_ARCHITECTURE && name < _CS_VENDOR)) {
        errno = EINVAL;
        return 0;
    }

    if (_conf_get(_confstr_table, name, &size, buff) == -1) {
        size = 0;
    }

    if (buff && len && (size_t) size >= len)
        buff[len - 1] = '\0';
    return (size_t) size;
}
