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

#include "env-lock.h"
#include "posix.h"

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

#define ACTION_BIT(t) (1U << (t))

/* The set of action types present, as ACTION_BIT()s, in one pass. */
static unsigned action_types(const _posix_spawn_file_actions_t *fact)
{
    unsigned types = 0;

    for (unsigned i = 0; i < fact->num_entries; i++) {
        if (fact->action[i].type < 32)
            types |= ACTION_BIT(fact->action[i].type);
    }
    return types;
}

/* Working buffers for path resolution. cwd[0] is '\0' until a chdir or fchdir
 * action sets it, and the child then starts in cwd.
 */
struct spawn_paths {
    char cwd[PATH_MAX + 1];
    char scratch[PATH_MAX + 1];
};

/* Write dir/rel to out. Returns EOK or ENAMETOOLONG. */
static int join_path(char *out, const char *dir, const char *rel)
{
    size_t dlen = strlen(dir), rlen = strlen(rel);

    /* drop trailing slashes, all of them for "/", so the join is "/rel" */
    while (dlen > 0 && dir[dlen - 1] == '/')
        dlen--;
    if (dlen + 1 + rlen > PATH_MAX)
        return ENAMETOOLONG;
    memmove(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, rel, rlen + 1);
    return EOK;
}

/* Resolve a relative path against the directory the earlier chdir/fchdir
 * actions left, the way openat() would against a descriptor for it.
 *
 * Returns the path to use, or NULL with *err set. An empty path stays ENOENT
 * rather than naming the directory itself.
 */
static const char *child_path(struct spawn_paths *paths,
                              const char *path,
                              int *err)
{
    if (path[0] == '\0') {
        *err = ENOENT;
        return NULL;
    }
    if (!paths->cwd[0] || path[0] == '/')
        return path;
    *err = join_path(paths->scratch, paths->cwd, path);
    return *err == EOK ? paths->scratch : NULL;
}

/* Run a chdir or fchdir action against the simulated child descriptor table
 * map[]. The target must be a directory the caller may search, as chdir()
 * requires.
 *
 * Returns EOK or the errno the spawn fails with.
 */
static int run_chdir_action(const posix_spawn_file_actions_list_t *action,
                            const int map[],
                            struct spawn_paths *paths)
{
    const char *dir;
    int err;

    if (action->type == posix_file_action_type_CHDIR) {
        dir = child_path(paths, action->_type.chdir_path, &err);
        if (!dir)
            return err;
    } else {
        int pfd = map[action->_type.fchdir_fd];

        if (pfd == SPAWN_FDCLOSED)
            return EBADF;
        if (__atfd_abspath(pfd, ".", paths->scratch, sizeof paths->scratch))
            return errno;
        dir = paths->scratch;
    }
    return __chdir_resolve(dir, paths->cwd, sizeof paths->cwd) ? errno : EOK;
}

static int materialize_open_actions(_posix_spawn_file_actions_t *fact,
                                    struct spawn_paths *paths,
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
        case posix_file_action_type_CHDIR:
            break;
        case posix_file_action_type_FCHDIR:
            if (action->_type.fchdir_fd > SPAWN_FD_TARGET_MAX)
                return EBADF;
            if (action->_type.fchdir_fd > (int) max_fd)
                max_fd = (unsigned) action->_type.fchdir_fd;
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
            if (map[action->_type.dup.fd] == SPAWN_FDCLOSED) {
                ret = EBADF;
                goto fail;
            }
            if (fcntl(map[action->_type.dup.fd], F_GETFD) == -1) {
                ret = errno;
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
            const char *opath =
                child_path(paths, action->_type.open->path, &ret);
            int fd;

            if (!opath)
                goto fail;

            /* O_NOCTTY: the caller must not acquire a terminal it opens for the
             * child; that the child does not either is allowed, since acquiring
             * one on open is implementation-defined.
             */
            fd = open(opath,
                      (int) action->_type.open->flags | O_CLOEXEC | O_NOCTTY,
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
        case posix_file_action_type_CHDIR:
        case posix_file_action_type_FCHDIR:
            ret = run_chdir_action(action, map, paths);
            if (ret != EOK)
                goto fail;
            break;
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
 * File actions run in the order they were added. A relative path resolves
 * against the directory the chdir/fchdir actions leave, or against the caller's
 * cwd when there are none; use posix_spawnp() for a $PATH search. The actions
 * run here in the caller, so opens and directory searches are checked against
 * the caller's ids even when POSIX_SPAWN_RESETIDS is set.
 *
 * Errors:
 *   EOK     - success.  *pid is set to the new pid.
 *   EINVAL  - bad path / malformed posix_spawnattr_t / file_actions.
 *   ENOENT  - roottask could not open the image, or a file action failed.
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
    unsigned types = 0;
    bool fact_has_open, fact_has_chdir, image_known;
    struct spawn_paths *paths = NULL;
    size_t cwdlen = 0;
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

    if (_factp)
        types = action_types(_factp);
    fact_has_open = types & ACTION_BIT(posix_file_action_type_OPEN);
    fact_has_chdir = types & (ACTION_BIT(posix_file_action_type_CHDIR) |
                              ACTION_BIT(posix_file_action_type_FCHDIR));
    /* only open and chdir actions, and a relative image, resolve paths */
    if (fact_has_open || fact_has_chdir || *path != '/') {
        paths = malloc(sizeof(*paths));
        if (!paths)
            return ENOMEM;
        paths->cwd[0] = '\0';
    }

    /* Fail on a missing image before an open action can create a file. A
     * relative image under chdir actions is known only once they have run.
     */
    image_known = *path == '/' || !fact_has_chdir;
    if (fact_has_open && image_known && stat(path, &dummy) == -1) {
        r = errno;
        goto cleanup;
    }
    if (_factp) {
        r = materialize_open_actions(_factp, paths, &materialized_factp,
                                     &temp_fds, &ntemp_fds);
        if (r != EOK)
            goto cleanup;
        _factp = materialized_factp;
    }

    /* roottask wants an absolute image path; the child's cwd is the base. */
    if (*path != '/') {
        const char *base = paths->cwd[0]
                               ? paths->cwd
                               : getcwd(paths->scratch, sizeof paths->scratch);

        if (!base) {
            r = errno;
            goto cleanup;
        }
        r = join_path(paths->scratch, base, path);
        if (r != EOK)
            goto cleanup;
        path = paths->scratch;
        if (fact_has_open && !image_known && stat(path, &dummy) == -1) {
            r = errno;
            goto cleanup;
        }
    }
    /* pathlen rides the wire as a uint16_t; reject before it truncates. */
    if (strlen(path) + 1 > UINT16_MAX) {
        r = ENAMETOOLONG;
        goto cleanup;
    }
    if (paths && paths->cwd[0])
        cwdlen = strlen(paths->cwd) + 1;

    /* The on-the-wire attr blob is the POSIX-visible spawn attribute object. */
    msg.i.type = _PROC_POSIX_SPAWN;
    msg.i.subtype = _PROC_SPAWN_START;
    msg.i.pathlen = (uint16_t) (strlen(path) + 1);
    msg.i.attr_bytes = (uint32_t) sizeof(*_attrp);
    msg.i.cwdlen = (uint16_t) cwdlen;
    memset(msg.i.reserved, 0, sizeof(msg.i.reserved));

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

        /* Hold the environment lock from here until argv and envp are copied: a
         * concurrent setenv may edit environ, or rewrite in place a value that
         * any vector the caller built can point at, which would change a length
         * between counting and copying.
         */
        __env_lock();
        env_locked = true;
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
    /* the child cwd trails the message; an empty iov when there is none */
    SETIOV(iov + 5 + factp_open_iovs, paths ? paths->cwd : data, cwdlen);

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
    __env_unlock();
    env_locked = false;

    SETIOV(iov + 0, &msg, sizeof msg);
    SETIOV(iov + 1, _attrp, msg.i.attr_bytes);
    SETIOV(iov + 2, _factp, msg.i.filact_bytes);
    SETIOV(iov + 3 + factp_open_iovs, path, msg.i.pathlen);
    SETIOV(iov + 4 + factp_open_iovs, data, msg.i.argenv_bytes);

    *pid = msg_sendvnc(coid, iov + 0, (int) num_iovs, 0, 0);
    r = (*pid == -1) ? errno : EOK;

cleanup:
    if (env_locked)
        __env_unlock();

    /* Single teardown for every post-materialize exit. data/iov are NULL-init
     * and every pointer is freed exactly once; error paths set r and jump here.
     */
    free(data);
    free(iov);
    close_temp_fds(temp_fds, ntemp_fds);
    free(temp_fds);
    free(materialized_factp);
    free(paths);
    return r;
}
