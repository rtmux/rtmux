/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawnattr.h"


/** get/setpgroup
 *
 * behavior as per POSIX
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 */
int posix_spawnattr_getpgroup(const posix_spawnattr_t *__restrict attrp,
                              pid_t *__restrict pid_p)
{
    if (!valid_attrp(attrp) || !pid_p)
        return EINVAL;

    _posix_spawnattr_t *_attrp = GET_ATTRP(attrp);
    *pid_p = !_attrp ? getpgrp() : _attrp->pgrp;
    return EOK;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attrp, pid_t pid)
{
    _posix_spawnattr_t *_attrp;
    int rc = _posix_spawnattr_for_set(attrp, &_attrp);

    if (rc != EOK) {
        return rc;
    }
    _attrp->pgrp = pid;
    return EOK;
}
