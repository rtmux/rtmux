/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdalign.h>

#include "posix-spawnattr.h"


/* default_posix_spawnattr_t
 *
 * The default 'posix_spawnattr_t' object will be used in the following cases...
 * 1. to return default value for any _getxxx() for which a user
 * 'posix_spawnattr_t'
 *      object has not yet been created. A user object will only be created
 *      (memory will only be allocated) if a call to one of the _setxxx()
 * functions is made. This avoids unnecessary allocations for objects which will
 * only have default values.
 * 2. to provide the 'posix_spawnattr_t' values in a posix_spawn() call if
 *      the 'attrp' argument is NULL
 *
 * Note re alignas(8) for _default_posix_spawnattr_t
 *
 * We need this variable to be 8 byte aligned for the same reason we expect any
 * allocated posix_spawnattr_t memory within libc to be 8 byte aligned (see
 * KEY_MASK).
 */
alignas(8) const _posix_spawnattr_t _default_posix_spawnattr_t = {
    .flags = 0,
    .pgrp = 0,
    .sig =
        {
            .dflt = {.bits = {[0] = 0, [1] = 0}},
            .mask = {.bits = {[0] = 0, [1] = 0}},
        },
    .sched = {
        .param =
            {
                .sched_priority = 0,
                .sched_curpriority = 0,
            },
        .policy = 0,
    }};


/*                          Internal Support Routines */

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

_posix_spawnattr_t *get_attrp(uintptr_t x, unsigned key_mask)
{
    if (x != 0)
        x ^= AKEY(key_mask);
    return (x & key_mask) ? NULL : (_posix_spawnattr_t *) x;
}

/* The proc service reconstructs a posix_spawnattr_t from an untrusted message
 * region of attr_bytes bytes.
 *
 * Return non-zero iff that region is large enough for the internal
 * _posix_spawnattr_t the getters read, so proc can reject an undersized attr
 * region before reading it. (The sender always transmits
 * sizeof(_posix_spawnattr_t), so legit messages pass.)
 */
int rtmux_spawnattr_msg_ok(uint32_t attr_bytes)
{
    return attr_bytes >= sizeof(_posix_spawnattr_t);
}

/** valid_attrp
 *
 * Return true (!0) or FALSE (0) depending on whether <attrp> represents a valid
 * initialized 'posix_spawnattr_t' object.
 *
 * A valid object is as follows
 * - <attrp> is non NULL
 * - <*attrp> is NULL or represents allocated memory
 *
 * In order to differentiate between allocated memory and a non 0 value, any
 * memory actually allocated will be translated. The get_attrp() and set_attrp()
 * routines will be used to translate back and forth
 */
unsigned valid_attrp(const posix_spawnattr_t *__restrict attrp)
{
    if (!attrp)
        return 0; /* not valid */
    if (*(posix_spawnattr_t *) ((uintptr_t) attrp &
                                ~(uintptr_t) MIN_KEY_MASK) == (uintptr_t) -1)
        return 0; /* not valid */
    if (*(posix_spawnattr_t *) ((uintptr_t) attrp &
                                ~(uintptr_t) MIN_KEY_MASK) == 0)
        return !0; /* valid */
    if (GET_ATTRP(attrp))
        return !0; /* valid */
    return 0;
}


/** _posix_spawnattr_t_realloc
 *
 * If <attrp> is NULL, this function will allocate and initialize to default
 * values, memory for an internal '_posix_spawnattr_t' structure of <size>
 * bytes. If <attrp> is not NULL, this function will reallocate the memory
 * pointed to by <attrp> to <size> bytes. If <size> == 0 and <attrp> is NULL,
 * then <size> it will be set to the default (ie. sizeof(*attrp))
 *
 * Note: The intent of this function is to grow the _posix_spawnattr_t and
 * although it is possible to release the memory for the <attrp> (or resize it
 * to something smaller than sizeof(_posix_spawnattr_t)) by passing in a
 * non-null <attrp> and a <size> < sizeof(_posix_spawnattr_t) (the API for this
 * internal function does not prevent this), it is currently considered an error
 * to do so and the caller is responsible for any side effects.
 *
 * Returns a pointer to the _posix_spawnattr_t structure or NULL if memory could
 * not be allocated.
 */
_posix_spawnattr_t *_posix_spawnattr_t_realloc(_posix_spawnattr_t *attrp,
                                               size_t size)
{
    bool first_time = false;

    if ((!attrp) && (size == 0))
        size = sizeof(*attrp);
    if (!attrp) {
        attrp =
            calloc(1, size); /* ensure zeroed, aligned memory the first time */
        first_time = true;
    } else {
        attrp = realloc(attrp, size);
    }
    if (attrp) {
        /* if first time, set default values */
        if (first_time) {
            /* first time 'posix_spawnattr_t' allocation */
            memcpy(attrp, &_default_posix_spawnattr_t, sizeof(*attrp));
        }
    }
    return attrp;
}
