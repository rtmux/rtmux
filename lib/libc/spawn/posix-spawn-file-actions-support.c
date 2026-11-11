/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawn-file-actions.h"


/*                          Internal Support Routines */

/** get_factp set_factp
 *
 * These 2 routine translate between a real allocated
 * '_posix_spawn_file_actions_t *' and the token stored in the
 * 'posix_spawn_file_actions_t *'.
 *
 * A valid, initialized '_posix_spawn_file_actions_t' object has 2 possible
 * values.
 *      - NULL
 *      - a valid, allocated address
 * All other values are considered invalid. These 2 routines attempt to catch
 * uninitialized 'posix_spawn_file_actions_t' objects from being used. The
 * example is an automatic 'posix_spawn_file_actions_t' which contains stack
 * garbage. It is non NULL but does not represent valid, allocated memory.
 *
 * Because we know that allocated memory is always aligned on given boundary we
 * use a key that will always turn a properly allocated memory address into a
 * token which always has the lower 3 bits set (8 byte alignment). This works
 * for all bogus values that do not have 7 as the LSB
 */

_posix_spawn_file_actions_t *get_factp(uintptr_t x, unsigned key_mask)
{
    if (x != 0)
        x ^= FKEY(key_mask);
    return (x & key_mask) ? NULL : (_posix_spawn_file_actions_t *) x;
}

/** valid_factp
 *
 * Return true (!0) or FALSE (0) depending on whether <factp> represents a valid
 * initialized 'posix_spawn_file_actions_t' object.
 *
 * A valid object is as follows
 * - <factp> is non NULL
 * - <*factp> is NULL or represents allocated memory
 *
 * In order to differentiate between allocated memory and a non 0 value, any
 * memory actually allocated will be translated. The get_factp() and set_factp()
 * routines will be used to translate back and forth
 */
unsigned valid_factp(const posix_spawn_file_actions_t *__restrict factp)
{
    if (!factp)
        return 0; /* not valid */
    if (*(posix_spawn_file_actions_t *) ((uintptr_t) factp &
                                         ~(uintptr_t) MIN_KEY_MASK) == 0)
        return !0; /* valid */
    if (GET_FACTP(factp))
        return !0; /* valid */
    return 0;
}
