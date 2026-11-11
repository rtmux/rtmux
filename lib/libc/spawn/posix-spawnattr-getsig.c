/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawnattr.h"


/** get/setsigdfault
 *
 * behavior as per POSIX
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 */
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *__restrict attrp,
                                  sigset_t *__restrict sigset_p)
{
    if (!valid_attrp(attrp) || !sigset_p)
        return EINVAL;

    _posix_spawnattr_t *_attrp = GET_ATTRP(attrp);
    *sigset_p =
        !_attrp ? _default_posix_spawnattr_t.sig.dflt : _attrp->sig.dflt;
    return EOK;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *__restrict attrp,
                                  const sigset_t *__restrict sigset_p)
{
    _posix_spawnattr_t *_attrp;
    int rc;

    if (!sigset_p) {
        return EINVAL;
    }
    rc = _posix_spawnattr_for_set(attrp, &_attrp);
    if (rc != EOK) {
        return rc;
    }
    _attrp->sig.dflt = *sigset_p;
    return EOK;
}

/** get/setsigmask
 *
 * behavior as per POSIX
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 */
int posix_spawnattr_getsigmask(const posix_spawnattr_t *__restrict attrp,
                               sigset_t *__restrict sigset_p)
{
    if (!valid_attrp(attrp) || !sigset_p)
        return EINVAL;

    _posix_spawnattr_t *_attrp = GET_ATTRP(attrp);
    *sigset_p =
        !_attrp ? _default_posix_spawnattr_t.sig.mask : _attrp->sig.mask;
    return EOK;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *__restrict attrp,
                               const sigset_t *__restrict sigset_p)
{
    _posix_spawnattr_t *_attrp;
    int rc;

    if (!sigset_p) {
        return EINVAL;
    }
    rc = _posix_spawnattr_for_set(attrp, &_attrp);
    if (rc != EOK) {
        return rc;
    }
    _attrp->sig.mask = *sigset_p;
    return EOK;
}
