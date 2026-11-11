/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawn-file-actions.h"


/** file_open_actions_fixup
 *
 * This is a proc service support routine only. Because of the way that the
 * '_posix_spawn_file_actions_open_t' structures are stored (pointers within the
 * '_posix_spawn_file_actions_t' structure) when the structures are messaged to
 * roottask, the open actions will be appended to the end of the message (if
 * there are any). This routine will fix those pointers up in the proc service
 * local message buffer
 *
 * <p1> is expected to be a pointer to a '_posix_spawn_file_actions_t' structure
 * and <p2> to a stream of bytes which are a series of
 * '_posix_spawn_file_actions_open_t' structures
 */
int file_open_actions_fixup(void *p1,
                            void *p2,
                            uint32_t fact_bytes,
                            uint32_t open_bytes)
{
    if (!p1 || !p2)
        return EINVAL;

    unsigned i;
    _posix_spawn_file_actions_t *_factp = (_posix_spawn_file_actions_t *) p1;
    _posix_spawn_file_actions_open_t *open_action =
        (_posix_spawn_file_actions_open_t *) p2;
    char *open_end = (char *) p2 + open_bytes;

    /* p1/p2 point into the untrusted spawn message. Reject a num_entries whose
     * action[] array does not fit in the declared fact_bytes region. The
     * FILE_ACTIONS_T_SIZE(0) guard keeps num_entries inside the fixed header.
     */
    if (fact_bytes < FILE_ACTIONS_T_SIZE(0) ||
        fact_bytes < FILE_ACTIONS_T_SIZE(_factp->num_entries))
        return EINVAL;

    for (i = 0; i < _factp->num_entries; i++) {
        if (_factp->action[i].type != posix_file_action_type_OPEN)
            continue;

        char *rec = (char *) open_action;

        /* Bound each open record before reading size or storing the pointer. */
        if ((size_t) (open_end - rec) < sizeof(*open_action) ||
            open_action->size < sizeof(*open_action) ||
            open_action->size > (uint32_t) (open_end - rec))
            return EINVAL;

        _factp->action[i]._type.open = open_action;
        open_action =
            (_posix_spawn_file_actions_open_t *) (rec + open_action->size);
    }
    return EOK;
}

/** get_factp set_factp
 *
 * These 2 routine translate between a real allocated
 * '_posix_spawn_file_actions_t *' and the token stored in the
 * 'posix_spawn_file_actions_t *'.
 *
 * A valid, initialized '_posix_spawn_file_actions_t' object has 2 possible
 * values.
 *      - null
 *      - a valid, allocated address
 * All other values are considered invalid. These 2 routines attempt to catch
 * uninitialized 'posix_spawn_file_actions_t' objects from being used. The
 * example is an automatic 'posix_spawn_file_actions_t' which contains stack
 * garbage. It is non-null but does not represent valid, allocated memory.
 *
 * Because we know that allocated memory is always aligned on given boundary we
 * use a key that will always turn a properly allocated memory address into a
 * token which always has the lower 3 bits set (8 byte alignment). This works
 * for all bogus values that do not have 7 as the LSB
 */
void set_factp(posix_spawn_file_actions_t *pp, void *_p, unsigned key_mask)
{
    _posix_spawn_file_actions_t *p = (_posix_spawn_file_actions_t *) _p;
    assert(pp);
    assert(((uintptr_t) p == FKEY(key_mask)) ||
           (((uintptr_t) p & key_mask) ==
            0)); /* must be aligned or keying will fail */
    *pp = ((uintptr_t) p ^ FKEY(key_mask));
}
