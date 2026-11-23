/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Adapted for RTMux, 2026. Reformatted to RTMux K&R + 4-space style. */

#include "posix-spawnattr.h"
#include "posix-spawn-file-actions.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/spawn-action-wire.h>

#include "env-lock.h"
#include "spawn-args.h"
#include "posix.h"

extern char **environ;

/* Flatten actions without looking up paths or changing descriptors in the
 * parent. A pathname is copied with its terminator immediately after its
 * record. Both passes use the same action decoding.
 */
static int pack_actions(const _posix_spawn_file_actions_t *fact,
                        unsigned char **packed,
                        uint32_t *bytes)
{
    *packed = NULL;
    *bytes = 0;
    if (!fact || !fact->num_entries)
        return EOK;
    for (int pass = 0; pass < 2; pass++) {
        size_t used = 0;
        for (unsigned i = 0; i < fact->num_entries; i++) {
            const posix_spawn_file_actions_list_t *a = &fact->action[i];
            struct spawn_action_record r = {.type = a->type};
            const char *path = NULL;
            switch (a->type) {
            case posix_file_action_type_OPEN:
                r.target = a->_type.open->new_fd;
                r.flags = a->_type.open->flags;
                r.mode = a->_type.open->mode;
                path = a->_type.open->path;
                break;
            case posix_file_action_type_CHDIR:
                path = a->_type.chdir_path;
                break;
            case posix_file_action_type_FCHDIR:
                r.fd = a->_type.fchdir_fd;
                break;
            case posix_file_action_type_DUP:
                r.fd = a->_type.dup.fd;
                r.target = a->_type.dup.new_fd;
                break;
            case posix_file_action_type_CLOSE:
                r.fd = a->_type.close.fd;
                break;
            default:
                return EINVAL;
            }
            size_t len = path ? strlen(path) + 1 : 0;
            if (len > PSPAWN_ACTIONS_MAX - sizeof(r) ||
                used > PSPAWN_ACTIONS_MAX - sizeof(r) - len)
                return E2BIG;
            r.pathlen = (uint32_t) len;
            if (pass) {
                memcpy(*packed + used, &r, sizeof(r));
                if (len)
                    memcpy(*packed + used + sizeof(r), path, len);
            }
            used += sizeof(r) + len;
        }
        if (!pass) {
            *bytes = (uint32_t) used;
            *packed = malloc(used);
            if (!*packed)
                return ENOMEM;
        }
    }
    return EOK;
}

int posix_spawn(pid_t *restrict pid,
                const char *restrict path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *restrict attrp,
                char *const argv[restrict],
                char *const envp[restrict])
{
    proc_posixspawn_t msg;
    iov_t iov[5];
    unsigned char *packed = NULL;
    char *data = NULL;
    size_t strings;
    pid_t _pid;
    int coid;
    _posix_spawnattr_t *_attrp;
    _posix_spawn_file_actions_t *_factp = NULL;
    int r;
    bool env_locked = false;

    if (!path || *path == '\0')
        return EINVAL;
    if (!pid)
        pid = &_pid;

    if (!attrp) {
        _attrp = (_posix_spawnattr_t *) &_default_posix_spawnattr_t;
    } else {
        if (!valid_attrp(attrp))
            return EINVAL;
        _attrp = get_attrp(*attrp, KEY_MASK);
        if (!_attrp)
            _attrp = (_posix_spawnattr_t *) &_default_posix_spawnattr_t;
    }

    if (file_actions) {
        if (!valid_factp(file_actions))
            return EINVAL;
        _factp = get_factp(*file_actions, KEY_MASK);
        /* may be NULL for a valid but unset object — that's ok */
    }

    /* pathlen rides the wire as a uint16_t; reject before it truncates. */
    if (strlen(path) + 1 > UINT16_MAX) {
        r = ENAMETOOLONG;
        goto cleanup;
    }

    /* The on-the-wire attr blob is the POSIX-visible spawn attribute object. */
    msg.i.type = _PROC_POSIX_SPAWN;
    msg.i.subtype = _PROC_SPAWN_START;
    msg.i.pathlen = (uint16_t) (strlen(path) + 1);
    msg.i.attr_bytes = (uint32_t) sizeof(*_attrp);
    msg.i.cwdlen = 0;
    memset(msg.i.reserved, 0, sizeof(msg.i.reserved));
    msg.i.filact_obytes = 0;
    r = pack_actions(_factp, &packed, &msg.i.filact_bytes);
    if (r != EOK)
        goto cleanup;
    if (packed)
        msg.i.reserved[0] = PSPAWN_CHILD_ACTIONS;

    /* Hold the environment lock from here until argv and envp are copied: a
     * concurrent setenv may edit environ, or rewrite in place a value that any
     * vector the caller built can point at, which would change a length between
     * measuring and copying.
     */
    __env_lock();
    env_locked = true;
    if (!envp)
        envp = environ;

    /* ARG_MAX, then the whole request: roottask refuses either with E2BIG, so
     * do not send it.
     */
    r = spawn_measure_args(argv, envp, &msg.i.nargv, &msg.i.narge, &strings);
    if (r != EOK)
        goto cleanup;
    msg.i.argenv_bytes = (uint32_t) strings;
    if (pspawn_declared_len(&msg.i) > PSPAWN_MSG_MAX) {
        r = E2BIG;
        goto cleanup;
    }

    coid = ROOTTASK_COID;

    if (!(data = malloc(strings ? strings : 1))) {
        r = ENOMEM;
        goto cleanup;
    }
    r = spawn_copy_args(data, strings, argv, envp);
    if (r != EOK)
        goto cleanup;
    __env_unlock();
    env_locked = false;

    SETIOV(iov + 0, &msg, sizeof msg);
    SETIOV(iov + 1, _attrp, msg.i.attr_bytes);
    SETIOV(iov + 2, packed, msg.i.filact_bytes);
    SETIOV(iov + 3, path, msg.i.pathlen);
    SETIOV(iov + 4, data, msg.i.argenv_bytes);

    *pid = msg_sendvnc(coid, iov + 0, 5, 0, 0);
    r = (*pid == -1) ? errno : EOK;

cleanup:
    if (env_locked)
        __env_unlock();

    free(data);
    free(packed);
    return r;
}
