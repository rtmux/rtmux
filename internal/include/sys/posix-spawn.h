/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Private posix_spawn handoff definitions shared by libc and roottask. */
#ifndef _POSIX_SPAWN_H_INCLUDED
#define _POSIX_SPAWN_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __CDEFS_H_INCLUDED
#include <sys/cdefs.h>
#endif

#include <spawn.h>

__BEGIN_DECLS

/* KEY_MASK
 *
 * see set_attrp()/get_attrp() and set_factp()/get_factp for description of use
 */
#define KEY_MASK 0x7UL     /* bits 0, 1 and 2 are always set */
#define MIN_KEY_MASK 0x3UL /* bits 0 and 1 are always set */


/* posix_spawn_file_actions related types for the spawn message handoff. */
typedef enum {
    posix_file_action_type_CLOSE = 1,
    posix_file_action_type_DUP = 2,
    posix_file_action_type_OPEN = 3,
} posix_file_action_type;

typedef struct {
    int32_t fd;
} _posix_spawn_file_actions_close_t;

typedef struct {
    int32_t fd;
    int32_t new_fd;
} _posix_spawn_file_actions_dup_t;

typedef struct {
    uint32_t size; /* size in bytes of this structure including path */
    uint32_t mode;
    uint32_t flags;
    int32_t new_fd;
    char path[1]; /* space will be allocated when the addopen() is called */
} _posix_spawn_file_actions_open_t;

/* because file actions must be processed in the order in which they are added,
 * we grow an array which is a union of the types of actions. A pointer to each
 * action type could be used however this leads to small allocations and 'open'
 * actiosn are the only "special" type. This type is a pointer to a separate
 * area of memory where enough space for all open() args including the variable
 * length 'path' will have been allocated
 */
typedef struct {
    uint32_t type; /* of type posix_file_action_type */
    union {
        _posix_spawn_file_actions_close_t close;
        _posix_spawn_file_actions_dup_t dup;
        _posix_spawn_file_actions_open_t *open; /* treat as READONLY */
    } _type;
} posix_spawn_file_actions_list_t;


/* Internal libc spawn handoff helpers. */
int rtmux_spawn_file_getactions(
    const posix_spawn_file_actions_t *restrict fact_p,
    int *num,
    posix_spawn_file_actions_list_t fact_list[]) __hidden;
int file_open_actions_fixup(void *p1,
                            void *p2,
                            uint32_t fact_bytes,
                            uint32_t open_bytes) __hidden;
int rtmux_spawnattr_msg_ok(uint32_t attr_bytes) __hidden;

void set_attrp(posix_spawnattr_t *pp, void *p, unsigned key_mask) __hidden;
void set_factp(posix_spawn_file_actions_t *pp, void *p, unsigned key_mask)
    __hidden;


__END_DECLS

#endif /* _POSIX_SPAWN_H_INCLUDED */
