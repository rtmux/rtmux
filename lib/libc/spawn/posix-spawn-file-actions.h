/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <spawn.h>
#include <sys/posix-spawn.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <endian.h>

#include <assert.h>

/* FKEY
 *
 * see set_factp()/get_factp for description of use
 */
#define FKEY(_km_) (0x07426fc0 | (_km_))

/* GET_FACTP SET_FACTP
 *
 * These macros wrap the get_factp()/set_factp() routines. For set we always use
 * KEY_MASK to enforce 8 byte alignment within this library however the get
 * operation will inspect the lower MIN_KEY_MASK bits of the incoming
 * 'posix_spawn_file_actions_t' object pointer and if non zero, will extract
 * those bits for use as the key mask established during a set operation for the
 * object. This allows proc service to use an alternate alignment constraint
 * when processing the _PROC_POSIX_SPAWN message. All user programs will however
 * be forced to the alignment established by KEY_MASK.
 */
#define SET_FACTP(a, b) set_factp((a), (b), KEY_MASK)
#define GET_FACTP(a)                                                       \
    get_factp(                                                             \
        *(posix_spawn_file_actions_t *) ((uintptr_t) (a) & ~MIN_KEY_MASK), \
        ((uintptr_t) (a) & MIN_KEY_MASK) ? (uintptr_t) (a) & MIN_KEY_MASK  \
                                         : KEY_MASK)

/* _posix_spawn_file_actions_t (internal version of
 * 'posix_spawn_file_actions_t')
 */
typedef struct {
    uint32_t num_entries;
    posix_spawn_file_actions_list_t
        action[1]; /* variable sized data structure */
} _posix_spawn_file_actions_t;

/* FILE_ACTIONS_T_SIZE
 *
 * convenience macro to calculate the size (in bytes) of the resulting
 * '_posix_spawn_file_actions_t' structure given 'n' where 'n' (typically)
 * represents the value of the '_posix_spawn_file_actions_t.num_entries' field.
 *
 * Note:
 *      'n' must be >= 0
 *      implementation is coded to ensure the proper result even for 'n' == 0
 */
#define FILE_ACTIONS_T_SIZE(n)                   \
    ((sizeof(_posix_spawn_file_actions_t) -      \
      sizeof(posix_spawn_file_actions_list_t)) + \
     ((n) * sizeof(posix_spawn_file_actions_list_t)))


/* internal prototypes */
extern _posix_spawn_file_actions_t *get_factp(uintptr_t x,
                                              unsigned int key_mask) __hidden;
extern unsigned valid_factp(
    const posix_spawn_file_actions_t *__restrict factp) __hidden;
