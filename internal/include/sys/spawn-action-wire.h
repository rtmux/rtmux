/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_SPAWN_ACTION_WIRE_H
#define RTMUX_SPAWN_ACTION_WIRE_H

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/posix-spawn.h>
#include <sys/spawn-limits.h>

/* Value of _proc_posix_spawn.reserved[0] for ordered child file actions.
 * Records are consecutive with no padding between a pathname and the next
 * record. Decode into an aligned local before accessing the integer fields. */
#define PSPAWN_CHILD_ACTIONS 1U
struct spawn_action_record {
    uint32_t type;
    int32_t fd, target;
    uint32_t flags, mode, pathlen;
};

static inline int spawn_action_next(const unsigned char **cursor, size_t *left,
                                    struct spawn_action_record *r, const char **path)
{
    if (*left < sizeof(*r))
        return EINVAL;
    memcpy(r, *cursor, sizeof(*r));
    *cursor += sizeof(*r);
    *left -= sizeof(*r);
    if (r->pathlen > *left)
        return EINVAL;
    *path = (const char *) *cursor;
    if (r->type == posix_file_action_type_OPEN || r->type == posix_file_action_type_CHDIR) {
        if (!r->pathlen || memchr(*path, 0, r->pathlen) != *path + r->pathlen - 1)
            return EINVAL;
    } else if (r->pathlen)
        return EINVAL;
    switch (r->type) {
    case posix_file_action_type_CLOSE:
    case posix_file_action_type_FCHDIR:
        if (r->fd < 0 || r->fd >= UINT16_MAX)
            return EBADF;
        break;
    case posix_file_action_type_DUP:
        if (r->fd < 0 || r->fd >= UINT16_MAX)
            return EBADF;
        /* fall through */
    case posix_file_action_type_OPEN:
        if (r->target < 0 || r->target >= UINT16_MAX)
            return EBADF;
        break;
    case posix_file_action_type_CHDIR:
        break;
    default:
        return EINVAL;
    }
    *left -= r->pathlen;
    *cursor += r->pathlen;
    return EOK;
}
#endif
