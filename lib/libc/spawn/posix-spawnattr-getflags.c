/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawnattr.h"


/** get/setflags
 *
 * behavior as per POSIX
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 */
int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict attrp,
                             short *__restrict flags_p)
{
    if (!valid_attrp(attrp) || !flags_p)
        return EINVAL;

    _posix_spawnattr_t *_attrp = GET_ATTRP(attrp);
    *flags_p =
        (short) (!_attrp ? _default_posix_spawnattr_t.flags : _attrp->flags);
    return EOK;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attrp, short flags)
{
    const short supported = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                            POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETSIGDEF |
                            POSIX_SPAWN_SETSCHEDULER |
                            POSIX_SPAWN_SETSCHEDPARAM;
    _posix_spawnattr_t *_attrp;
    int rc;

    if (!valid_attrp(attrp) || (flags & ~supported)) {
        return EINVAL;
    }

    rc = _posix_spawnattr_for_set(attrp, &_attrp);
    if (rc != EOK) {
        return rc;
    }
    _attrp->flags = (uint32_t) flags;
    return EOK;
}
