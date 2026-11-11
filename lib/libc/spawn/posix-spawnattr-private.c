/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawnattr.h"


/** get_attrp set_attrp
 *
 * These 2 routine translate between a real allocated '_posix_spawnattr_t *' and
 * the token stored in the 'posix_spawnattr_t *'.
 *
 * A valid, initialized 'posix_spawnattr_t' object has 2 possible values.
 *      - NULL
 *      - a valid, allocated address
 * All other values are considered invalid. These 2 routines attempt to catch
 * uninitialized 'posix_spawnattr_t' objects from being used. The example is an
 * automatic 'posix_spawnattr_t' which contains stack garbage. It is non NULL
 * but does not represent valid, allocated memory.
 *
 * Because we know that allocated memory is always aligned on given boundary we
 * use a key that will always turn a properly allocated memory address into a
 * token which always has the lower 3 bits set (8 byte alignment). This works
 * for all bogus values that do not have 7 as the LSB
 */
void set_attrp(posix_spawnattr_t *pp, void *_p, unsigned key_mask)
{
    _posix_spawnattr_t *p = (_posix_spawnattr_t *) _p;
    assert(pp);
    assert(((uintptr_t) p == AKEY(key_mask)) ||
           (((uintptr_t) p & key_mask) ==
            0)); /* must be aligned or keying will fail */
    *pp = ((uintptr_t) p ^ AKEY(key_mask));
}
