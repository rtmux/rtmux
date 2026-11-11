/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <minmax.h>
#include "posix-spawn-file-actions.h"


/*          RTMux internal posix_spawn_file_actions_t helpers */

/** getactions
 *
 * Retrieve the file actions currently set in the 'posix_spawn_file_actions_t'
 * object
 *
 * The caller must provide the storage for _posix_spawn_file_actions_t's to be
 * returned. The <num> pointer is initialized by the caller with the number of
 * '_posix_spawn_file_actions_t' elements <fact_list> has space for. Upon
 * return, <num> will be set as follows ...
 *      - if the actual number of entries is <= the number for which space was
 *        allocated by the caller, then the actual number will be filled in and
 *        <*num> will be set to the actual number of entries.
 *      - if the actual number of entries is > the number for which space was
 *        allocated by the caller and as specified by <*num>, then <*num>
 * entries will be filled in and <*num> will be set to negative of the number of
 *        entries remaining that would not fit.
 *
 *        For example, if the caller provides space for 4 entries and hence
 *        sets <*num> == 4 and there are 6 entries in the
 * posix_spawn_file_actions_t object, then 4 entries will be placed into the
 * structure and <*num> will be set to -2. If the caller sets <*num> == 0, then
 * they can easly find out how many entries there are and allocate enough space
 * to retrieve all of the entries.
 *
 * Returns:
 *      EOK on success
 *      EINVAL if there are any parameter errors
 */
int rtmux_spawn_file_getactions(
    const posix_spawn_file_actions_t *__restrict fact_p,
    int *num,
    posix_spawn_file_actions_list_t fact_list[])
{
    if (!valid_factp(fact_p) || !num || *num < 0)
        return EINVAL;

    _posix_spawn_file_actions_t *_fact_p = GET_FACTP(fact_p);
    if (!_fact_p) {
        *num = 0;
        return EOK;
    }

    unsigned num_entries = min_uint(_fact_p->num_entries, (unsigned) *num);
    unsigned i;

    for (i = 0; i < num_entries; i++)
        fact_list[i] = _fact_p->action[i];

    if (num_entries < _fact_p->num_entries)
        *num = -(int) (_fact_p->num_entries - num_entries);
    else
        *num = (int) _fact_p->num_entries;
    return EOK;
}
