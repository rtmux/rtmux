/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawnattr.h"


/** get/setschedparam
 *
 * behavior as per POSIX
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 */
int posix_spawnattr_getschedparam(const posix_spawnattr_t *__restrict attrp,
                                  struct sched_param *__restrict sched_p)
{
    if (!valid_attrp(attrp) || !sched_p)
        return EINVAL;

    _posix_spawnattr_t *_attrp = GET_ATTRP(attrp);
    *sched_p =
        !_attrp ? _default_posix_spawnattr_t.sched.param : _attrp->sched.param;
    return EOK;
}

int posix_spawnattr_setschedparam(posix_spawnattr_t *__restrict attrp,
                                  const struct sched_param *__restrict sched_p)
{
    _posix_spawnattr_t *_attrp;
    int rc;

    if (!sched_p) {
        return EINVAL;
    }
    rc = _posix_spawnattr_for_set(attrp, &_attrp);
    if (rc != EOK) {
        return rc;
    }
    _attrp->sched.param = *sched_p;
    return EOK;
}


/** get/setschedpolicy
 *
 * behavior as per POSIX
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 */
int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *__restrict attrp,
                                   int *__restrict policy_p)
{
    if (!valid_attrp(attrp) || !policy_p)
        return EINVAL;

    _posix_spawnattr_t *_attrp = GET_ATTRP(attrp);
    *policy_p = !_attrp ? _default_posix_spawnattr_t.sched.policy
                        : _attrp->sched.policy;
    return EOK;
}

int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attrp, int policy)
{
    _posix_spawnattr_t *_attrp;
    int rc = _posix_spawnattr_for_set(attrp, &_attrp);

    if (rc != EOK) {
        return rc;
    }
    _attrp->sched.policy = policy;
    return EOK;
}
