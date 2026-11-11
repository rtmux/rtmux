/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix-spawn-file-actions.h"

#include <stdint.h>
#include <stddef.h>

enum { SPAWN_FD_TARGET_MAX = UINT16_MAX - 1 };

static int valid_spawn_fd_target(int fd)
{
    return fd >= 0 && fd <= SPAWN_FD_TARGET_MAX;
}

/** file actions object init/destroy
 *
 * behavior as per POSIX with the following implementation defined behavior
 *
 * POSIX says results are undefined when re-initializing an already initialized
 * 'posix_spawn_file_actions_t' object. Our implementation does not support the
 * re-initialization of a 'posix_spawn_file_actions_t' object without an
 * intervening destruction of the object. Repeated calls to
 * posix_spawn_file_actions_init() with the same 'posix_spawn_file_actions_t'
 * object without an intervening posix_spawn_file_actions_destroy() on the
 * object could result in a memory leak.
 *
 * posix_spawn_file_actions_destroy() will render the
 * 'posix_spawn_file_actions_t' object unusable. Before the object can be
 * reused, it must be initialized with posix_spawn_file_actions_init(). This
 * includes calling posix_spawn_file_actions_destroy() on an already destroyed
 * object.
 *
 * Note that POSIX says results are undefined when using a
 * 'posix_spawn_file_actions_t' object after it has been destroyed. Our
 * implementation will cause all get/set operations as well as posix_spawn() to
 * return EINVAL when attempting to reference a destroyed file actions object.
 *
 * Returns:
 *      EOK on success
 *      EINVAL for any invalid parameter
 *
 * Implementation Note: (not necessary for interface documentation)
 *
 * init will initialize a 'posix_spawn_file_actions_t' structure by setting
 * <*factp> is NULL. This value indicates the use of default values for
 * posix_spawn() the same way passing NULL for the 'posix_spawn_file_actions_t'
 * parameter specifies the use of default values. The reason that memory is not
 * allocated in the init is so that if a user program calls
 * posix_spawn_file_actions_init() without calling any of the _setxxx()
 * functions, we don't allocate any memory. Also we do not need to allocate
 * memory for a defaults structure. The first _setxxx() call made will cause
 * memory to be allocated. This also works nicely for a statically initialized
 * 'posix_spawn_file_actions_t' object.
 */
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fact_p)
{
    if (!fact_p)
        return EINVAL;
    SET_FACTP(fact_p, (_posix_spawn_file_actions_t *) FKEY(
                          KEY_MASK)); /* will cause NULL to be stored */
    return EOK;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fact_p)
{
    if (!valid_factp(fact_p))
        return EINVAL;

    _posix_spawn_file_actions_t *_fact_p = GET_FACTP(fact_p);
    if (_fact_p) {
        unsigned i;
        for (i = 0; i < _fact_p->num_entries; i++) {
            if (_fact_p->action[i].type == posix_file_action_type_OPEN)
                free(_fact_p->action[i]._type.open);
        }
        free(_fact_p);
        SET_FACTP(fact_p, NULL);
    }
    return EOK;
}

static int file_action_reserve(posix_spawn_file_actions_t *fact_p,
                               _posix_spawn_file_actions_t **out)
{
    _posix_spawn_file_actions_t *fact = GET_FACTP(fact_p);
    unsigned entries = fact ? fact->num_entries : 0;
    _posix_spawn_file_actions_t *grown =
        realloc(fact, FILE_ACTIONS_T_SIZE(entries + 1));

    if (!grown)
        return ENOMEM;
    grown->num_entries = entries + 1;
    SET_FACTP(fact_p, grown);
    *out = grown;
    return EOK;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fact_p,
                                      int fd)
{
    _posix_spawn_file_actions_t *fact;
    int ret;

    if (!valid_factp(fact_p) || !valid_spawn_fd_target(fd))
        return EINVAL;
    ret = file_action_reserve(fact_p, &fact);
    if (ret != EOK)
        return ret;
    fact->action[fact->num_entries - 1] = (posix_spawn_file_actions_list_t) {
        .type = posix_file_action_type_CLOSE,
        ._type.close.fd = fd,
    };
    return EOK;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fact_p,
                                     int fd,
                                     int new_fd)
{
    _posix_spawn_file_actions_t *fact;
    int ret;

    if (!valid_factp(fact_p) || !valid_spawn_fd_target(fd) ||
        !valid_spawn_fd_target(new_fd))
        return EINVAL;
    ret = file_action_reserve(fact_p, &fact);
    if (ret != EOK)
        return ret;
    fact->action[fact->num_entries - 1] = (posix_spawn_file_actions_list_t) {
        .type = posix_file_action_type_DUP,
        ._type.dup.fd = fd,
        ._type.dup.new_fd = new_fd,
    };
    return EOK;
}

int posix_spawn_file_actions_addopen(
    posix_spawn_file_actions_t *restrict fact_p,
    int new_fd,
    const char *restrict path,
    int oflags,
    mode_t omode)
{
    _posix_spawn_file_actions_open_t *open_action;
    _posix_spawn_file_actions_t *fact;
    size_t path_len;
    size_t size;
    int ret;

    if (!valid_factp(fact_p) || !valid_spawn_fd_target(new_fd) || !path)
        return EINVAL;
    path_len = strlen(path) + 1;
    size = offsetof(_posix_spawn_file_actions_open_t, path) + path_len;
    if (size > UINT32_MAX)
        return ENOMEM;
    open_action = malloc(size);
    if (!open_action)
        return ENOMEM;
    *open_action = (_posix_spawn_file_actions_open_t) {
        .size = (uint32_t) size,
        .mode = (uint32_t) omode,
        .flags = (uint32_t) oflags,
        .new_fd = new_fd,
    };
    memcpy(open_action->path, path, path_len);

    ret = file_action_reserve(fact_p, &fact);
    if (ret != EOK) {
        free(open_action);
        return ret;
    }
    fact->action[fact->num_entries - 1] = (posix_spawn_file_actions_list_t) {
        .type = posix_file_action_type_OPEN,
        ._type.open = open_action,
    };
    return EOK;
}
