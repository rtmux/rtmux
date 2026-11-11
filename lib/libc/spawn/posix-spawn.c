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
#include <limits.h>
#include <sys/stat.h>

extern char **environ;

enum {
    SPAWN_FD_TARGET_MAX = UINT16_MAX - 1,
};

static void close_temp_fds(int fds[], size_t nfds)
{
    for (size_t i = 0; i < nfds; i++) {
        if (fds[i] >= 0)
            (void) close(fds[i]);
    }
}

static bool has_action_type(_posix_spawn_file_actions_t *fact, int type)
{
    for (unsigned i = 0; i < fact->num_entries; i++) {
        if (fact->action[i].type == type)
            return true;
    }
    return false;
}

static int materialize_open_actions(_posix_spawn_file_actions_t *fact,
                                    _posix_spawn_file_actions_t **out,
                                    int **temps_out,
                                    size_t *ntemps_out)
{
    int *map;
    int *temps = NULL;
    bool *open_slot = NULL;
    bool *cloexec_slot = NULL;
    bool *literal_slot = NULL;
    bool *close_slot = NULL;
    _posix_spawn_file_actions_t *mat;
    unsigned max_fd = 2;
    unsigned nentries = 0;
    int output_high_fd = -1;
    size_t ntemps = 0;
    int ret = EOK;

    *out = NULL;
    *temps_out = NULL;
    *ntemps_out = 0;

    for (unsigned i = 0; i < fact->num_entries; i++) {
        posix_spawn_file_actions_list_t *action = &fact->action[i];

        switch (action->type) {
        case posix_file_action_type_CLOSE:
            if (action->_type.close.fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            if (action->_type.close.fd > (int) max_fd)
                max_fd = (unsigned) action->_type.close.fd;
            break;
        case posix_file_action_type_DUP:
            if (action->_type.dup.fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            if (action->_type.dup.fd > (int) max_fd)
                max_fd = (unsigned) action->_type.dup.fd;
            if (action->_type.dup.new_fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            if (action->_type.dup.new_fd > (int) max_fd)
                max_fd = (unsigned) action->_type.dup.new_fd;
            break;
        case posix_file_action_type_OPEN:
            if (action->_type.open->new_fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            if (action->_type.open->new_fd > (int) max_fd)
                max_fd = (unsigned) action->_type.open->new_fd;
            break;
        default:
            return EINVAL;
        }
    }
    if (max_fd >= INT_MAX)
        return EMFILE;
    map = malloc(((size_t) max_fd + 1) * sizeof(*map));
    if (!map) {
        ret = ENOMEM;
        goto fail;
    }
    open_slot = calloc((size_t) max_fd + 1, sizeof(*open_slot));
    if (!open_slot) {
        ret = ENOMEM;
        goto fail;
    }
    cloexec_slot = calloc((size_t) max_fd + 1, sizeof(*cloexec_slot));
    if (!cloexec_slot) {
        ret = ENOMEM;
        goto fail;
    }
    literal_slot = calloc((size_t) max_fd + 1, sizeof(*literal_slot));
    if (!literal_slot) {
        ret = ENOMEM;
        goto fail;
    }
    close_slot = calloc((size_t) max_fd + 1, sizeof(*close_slot));
    if (!close_slot) {
        ret = ENOMEM;
        goto fail;
    }
    for (unsigned i = 0; i <= max_fd; i++) {
        int flags = fcntl((int) i, F_GETFD);

        if (flags == -1) {
            map[i] = SPAWN_FDCLOSED;
        } else {
            map[i] = (int) i;
            cloexec_slot[i] = (flags & FD_CLOEXEC) != 0;
        }
    }

    temps = calloc(fact->num_entries, sizeof(*temps));
    if (!temps) {
        ret = ENOMEM;
        goto fail;
    }
    for (unsigned i = 0; i < fact->num_entries; i++)
        temps[i] = -1;

    for (unsigned i = 0; i < fact->num_entries; i++) {
        posix_spawn_file_actions_list_t *action = &fact->action[i];

        switch (action->type) {
        case posix_file_action_type_CLOSE:
            map[action->_type.close.fd] = SPAWN_FDCLOSED;
            close_slot[action->_type.close.fd] = true;
            literal_slot[action->_type.close.fd] = false;
            break;
        case posix_file_action_type_DUP:
            if (map[action->_type.dup.fd] == SPAWN_FDCLOSED ||
                fcntl(map[action->_type.dup.fd], F_GETFD) == -1) {
                ret = errno ? errno : EBADF;
                goto fail;
            }
            if (action->_type.dup.fd == action->_type.dup.new_fd)
                break;
            map[action->_type.dup.new_fd] = map[action->_type.dup.fd];
            open_slot[action->_type.dup.new_fd] = true;
            cloexec_slot[action->_type.dup.new_fd] = false;
            literal_slot[action->_type.dup.new_fd] =
                literal_slot[action->_type.dup.fd];
            break;
        case posix_file_action_type_OPEN: {
            int fd = open(action->_type.open->path,
                          (int) action->_type.open->flags | O_CLOEXEC,
                          (mode_t) action->_type.open->mode);

            if (fd == -1) {
                ret = errno;
                goto fail;
            }
            if ((unsigned) fd <= max_fd && fd != action->_type.open->new_fd &&
                map[fd] == fd) {
                map[fd] = SPAWN_FDCLOSED;
                close_slot[fd] = true;
            }
            temps[ntemps++] = fd;
            map[action->_type.open->new_fd] = fd;
            open_slot[action->_type.open->new_fd] = true;
            literal_slot[action->_type.open->new_fd] = true;
            if (action->_type.open->flags & O_CLOEXEC)
                cloexec_slot[action->_type.open->new_fd] = true;
            else
                cloexec_slot[action->_type.open->new_fd] = false;
            break;
        }
        default:
            ret = EINVAL;
            goto fail;
        }
    }
    for (unsigned i = 0; i <= max_fd; i++) {
        if (cloexec_slot[i]) {
            map[i] = SPAWN_FDCLOSED;
            close_slot[i] = true;
        }
    }

    for (unsigned i = 0; i <= max_fd; i++) {
        if (map[i] != SPAWN_FDCLOSED && (map[i] != (int) i || open_slot[i])) {
            nentries++;
            output_high_fd = (int) i;
        }
    }
    for (unsigned i = 0; i <= max_fd && (int) i <= output_high_fd; i++) {
        if (map[i] == SPAWN_FDCLOSED)
            close_slot[i] = true;
    }
    for (unsigned i = 0; i <= max_fd; i++) {
        if (map[i] == SPAWN_FDCLOSED && close_slot[i])
            nentries++;
    }
    if (nentries == 0) {
        close_temp_fds(temps, ntemps);
        free(temps);
        free(cloexec_slot);
        free(open_slot);
        free(literal_slot);
        free(close_slot);
        free(map);
        return EOK;
    }
    mat = malloc(FILE_ACTIONS_T_SIZE(nentries));
    if (!mat) {
        ret = ENOMEM;
        goto fail;
    }
    mat->num_entries = nentries;
    nentries = 0;
    for (unsigned i = 0; i <= max_fd; i++) {
        if (literal_slot[i] && map[i] != SPAWN_FDCLOSED &&
            (map[i] != (int) i || open_slot[i])) {
            mat->action[nentries++] = (posix_spawn_file_actions_list_t){
                .type = posix_file_action_type_DUP,
                ._type.dup.fd = map[i],
                ._type.dup.new_fd = (int32_t) i,
            };
        }
    }
    for (unsigned i = 0; i <= max_fd; i++) {
        if (!literal_slot[i] && map[i] != SPAWN_FDCLOSED &&
            (map[i] != (int) i || open_slot[i])) {
            mat->action[nentries++] = (posix_spawn_file_actions_list_t){
                .type = posix_file_action_type_DUP,
                ._type.dup.fd = map[i],
                ._type.dup.new_fd = (int32_t) i,
            };
        }
    }
    for (unsigned i = 0; i <= max_fd; i++) {
        if (map[i] == SPAWN_FDCLOSED && close_slot[i]) {
            mat->action[nentries++] = (posix_spawn_file_actions_list_t){
                .type = posix_file_action_type_CLOSE,
                ._type.close.fd = (int32_t) i,
            };
        }
    }

    free(cloexec_slot);
    free(open_slot);
    free(literal_slot);
    free(close_slot);
    free(map);
    *out = mat;
    *temps_out = temps;
    *ntemps_out = ntemps;
    return EOK;

fail:
    close_temp_fds(temps, ntemps);
    free(temps);
    free(literal_slot);
    free(close_slot);
    free(cloexec_slot);
    free(open_slot);
    free(map);
    return ret;
}

/* posix_spawn — POSIX-conforming entry point that packages the arguments into a
 * _PROC_POSIX_SPAWN message and sends it to roottask's process service.
 * roottask builds the new process from there.
 *
 * The path must be absolute (start with '/'); use posix_spawnp() if you want a
 * $PATH search.
 *
 * Errors:
 *   EOK     - success.  *pid is set to the new pid.
 *   EINVAL  - bad path / malformed posix_spawnattr_t / file_actions.
 *   ENOENT  - relative path, or roottask could not open the image.
 *   ENOMEM  - couldn't allocate the message iov or the argenv staging buffer.
 *   EIO     - internal error in the libc spawnattr machinery.
 *
 * RTMux does not support Adaptive Partitioning paths; spawn attributes are
 * limited to the POSIX-visible object represented below.
 */
int posix_spawn(pid_t *restrict pid,
                const char *restrict path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *restrict attrp,
                char *const argv[restrict],
                char *const envp[restrict])
{
    proc_posixspawn_t msg;
    iov_t *iov = NULL;
    unsigned num_iovs = 0;
    char *const *arg;
    char *src;
    char *dst, *data = NULL;
    pid_t _pid;
    int coid;
    _posix_spawnattr_t *_attrp;
    _posix_spawn_file_actions_t *_factp = NULL;
    int r;
    unsigned factp_open_iovs = 0;
    struct stat dummy;
    _posix_spawn_file_actions_t *materialized_factp = NULL;
    int *temp_fds = NULL;
    size_t ntemp_fds = 0;
    bool fact_has_open;

    if (!path || *path == '\0')
        return EINVAL;
    if (*path != '/')
        return ENOENT;
    /* pathlen rides the wire as a uint16_t; reject before it truncates. */
    if (strlen(path) + 1 > UINT16_MAX)
        return ENAMETOOLONG;
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

    fact_has_open =
        _factp && has_action_type(_factp, posix_file_action_type_OPEN);
    if (_factp) {
        if (fact_has_open && stat(path, &dummy) == -1)
            return errno;
        r = materialize_open_actions(_factp, &materialized_factp, &temp_fds,
                                     &ntemp_fds);
        if (r != EOK)
            return r;
        _factp = materialized_factp;
    }

    /* The on-the-wire attr blob is the POSIX-visible spawn attribute object. */
    msg.i.type = _PROC_POSIX_SPAWN;
    msg.i.subtype = _PROC_SPAWN_START;
    msg.i.pathlen = (uint16_t) (strlen(path) + 1);
    msg.i.attr_bytes = (uint32_t) sizeof(*_attrp);

    iov = calloc(num_iovs = 6, sizeof(*iov));
    if (!iov) {
        r = ENOMEM;
        goto cleanup;
    }

    /* file actions */
    if (_factp) {
        /* Send the full _posix_spawn_file_actions_t through action[n-1] --
         * FILE_ACTIONS_T_SIZE accounts for the padding after num_entries. Using
         * sizeof(num_entries) here undercounted the header and truncated the
         * last entry's final bytes, which the receiver's bound now rejects.
         */
        msg.i.filact_bytes =
            (uint32_t) FILE_ACTIONS_T_SIZE(_factp->num_entries);
        msg.i.filact_obytes = 0;
        for (unsigned i = 0; i < _factp->num_entries; i++) {
            if (_factp->action[i].type == posix_file_action_type_OPEN) {
                iov_t *new_iov;

                ++factp_open_iovs;
                new_iov = realloc(iov, ++num_iovs * sizeof(*iov));
                if (!new_iov) {
                    r = ENOMEM;
                    goto cleanup;
                }
                iov = new_iov;
                SETIOV(&iov[2 + factp_open_iovs], _factp->action[i]._type.open,
                       _factp->action[i]._type.open->size);
                msg.i.filact_obytes += _factp->action[i]._type.open->size;
            }
        }
    } else {
        msg.i.filact_bytes = msg.i.filact_obytes = 0;
    }

    /* Accumulate in size_t and bound before narrowing: argenv_bytes is uint32_t
     * and nargv/narge are uint16_t on the wire, so an oversized argv/env would
     * otherwise wrap and under-allocate `data`, overflowing it during the copy
     * loops below.
     */
    {
        size_t argenv = 0, nv = 0, ne = 0;

        if (argv)
            for (arg = argv; *arg; arg++, nv++)
                argenv += strlen(*arg) + 1;
        if (!envp)
            envp = environ;
        if (envp)
            for (arg = envp; *arg; arg++, ne++)
                argenv += strlen(*arg) + 1;
        if (argenv > UINT32_MAX || nv > UINT16_MAX || ne > UINT16_MAX) {
            r = E2BIG;
            goto cleanup;
        }
        msg.i.argenv_bytes = (uint32_t) argenv;
        msg.i.nargv = (uint16_t) nv;
        msg.i.narge = (uint16_t) ne;
    }

    coid = ROOTTASK_COID;

    if ((dst = data = malloc(msg.i.argenv_bytes ? msg.i.argenv_bytes : 1)) ==
        NULL) {
        r = ENOMEM;
        goto cleanup;
    }
    SETIOV(iov + 5 + factp_open_iovs, data, 0); /* unused slot */

    if (argv) {
        for (arg = argv; (src = *arg); arg++)
            while ((*dst++ = *src++))
                ;
    }
    if (envp) {
        for (arg = envp; (src = *arg); arg++)
            while ((*dst++ = *src++))
                ;
    }

    SETIOV(iov + 0, &msg, sizeof msg);
    SETIOV(iov + 1, _attrp, msg.i.attr_bytes);
    SETIOV(iov + 2, _factp, msg.i.filact_bytes);
    SETIOV(iov + 3 + factp_open_iovs, path, msg.i.pathlen);
    SETIOV(iov + 4 + factp_open_iovs, data, msg.i.argenv_bytes);

    *pid = msg_sendvnc(coid, iov + 0, (int) num_iovs, 0, 0);
    r = (*pid == -1) ? errno : EOK;

cleanup:
    /* Single teardown for every post-materialize exit. data/iov are NULL-init
     * and every pointer is freed exactly once; error paths set r and jump here.
     */
    free(data);
    free(iov);
    close_temp_fds(temp_fds, ntemp_fds);
    free(temp_fds);
    free(materialized_factp);
    return r;
}
