/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* RTMux named channel registry adapters. */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/ftype.h>
#include <sys/iomsg.h>
#include <sys/path-registry.h>
#include <sys/rtmux-core.h>
#include "state.h"

#define NAME_ATTACH_KNOWN_FLAGS (NAME_FLAG_ATTACH_MOUNTABLE)

#define NAME_DETACH_KNOWN_FLAGS (NAME_FLAG_DETACH_SAVEDPP)

#define NAME_PREFIX_LOCAL "/dev/name/local/"

struct name_attach_ext {
    name_attach_t pub;
    char *path;
    unsigned owns_dpp;
    unsigned owns_chid;
};

/* Normalize the user-supplied path. Bare names (no leading '/') resolve under
 * /dev/name/local/ so a server and client that disagree on CWD still find each
 * other. Rooted names are accepted only inside that namespace; arbitrary
 * absolute registrations could shadow existing I/O-service paths such as
 * /dev/mem.
 */
static char *name_path_normalize(const char *path, unsigned flags)
{
    const char *prefix;
    size_t prefix_len, path_len, total;
    char *out;

    if (!path || *path == '\0') {
        errno = EINVAL;
        return NULL;
    }

    if (path[0] == '/') {
        if (strncmp(path, NAME_PREFIX_LOCAL, sizeof NAME_PREFIX_LOCAL - 1) !=
            0) {
            errno = EINVAL;
            return NULL;
        }
        prefix = "";
        prefix_len = 0;
    } else {
        prefix = NAME_PREFIX_LOCAL;
        prefix_len = sizeof NAME_PREFIX_LOCAL - 1;
    }

    path_len = strlen(path);
    total = prefix_len + path_len + 1;
    if (!(out = malloc(total))) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, path, path_len + 1);
    return out;
}

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags)
{
    struct name_attach_ext *attach;
    char *saved_path;

    if (flags & ~(unsigned) NAME_ATTACH_KNOWN_FLAGS) {
        errno = EINVAL;
        return NULL;
    }

    saved_path = name_path_normalize(path, flags);
    if (!saved_path)
        return NULL;

    attach = calloc(1, sizeof *attach);
    if (!attach) {
        free(saved_path);
        errno = ENOMEM;
        return NULL;
    }

    if (!dpp) {
        dpp = dispatch_create();
        if (!dpp)
            goto fail;
        attach->owns_dpp = 1;
    }

    if (_DPP(dpp)->chid == -1) {
        _DPP(dpp)->chid =
            channel_create(RTMUX_FLG_CHF_UNBLOCK | RTMUX_FLG_CHF_DISCONNECT);
        if (_DPP(dpp)->chid == -1)
            goto fail;
        attach->owns_chid = 1;
    }

    {
        unsigned link_flags = NAMESPACE_FLAG_STICKY;

        if (flags & NAME_FLAG_ATTACH_MOUNTABLE)
            link_flags |= NAMESPACE_FLAG_DIR;
        if (namespace_link(saved_path, 0, getpid(), _DPP(dpp)->chid, 0,
                           _FTYPE_NAME, link_flags) == -1) {
            goto fail;
        }
    }

    attach->pub.dpp = dpp;
    attach->pub.chid = _DPP(dpp)->chid;
    attach->pub.mntid = -1;
    attach->path = saved_path;
    return &attach->pub;

fail: {
    int saved_errno = errno;

    if (dpp) {
        if (attach->owns_dpp) {
            dispatch_destroy(dpp);
        } else if (attach->owns_chid) {
            (void) channel_destroy(_DPP(dpp)->chid);
            _DPP(dpp)->chid = -1;
        }
    }
    free(saved_path);
    free(attach);
    errno = saved_errno;
}
    return NULL;
}

int name_detach(name_attach_t *pub, unsigned flags)
{
    struct name_attach_ext *attach = (struct name_attach_ext *) pub;
    int ret = 0;
    int saved_errno = 0;

    if (!attach || (flags & ~(unsigned) NAME_DETACH_KNOWN_FLAGS)) {
        errno = EINVAL;
        return -1;
    }

    /* Path-based unlink races against a hijacker recreating the same node
     * between attach and detach. A handle-based namespace unregister would
     * close that window; absent one, the unlink relies on STICKY ownership
     * being unique by (path, pid) in practice.
     */
    if (attach->path && unlink(attach->path) == -1 && errno != ENOENT) {
        ret = -1;
        saved_errno = errno;
    }

    /* NAME_FLAG_DETACH_SAVEDPP: caller is keeping the dispatch handle alive
     * (typically across a re-attach), so the channel must not be torn down nor
     * the dispatch destroyed even if name_attach created them.
     */
    if (!(flags & NAME_FLAG_DETACH_SAVEDPP) && attach->pub.dpp) {
        if (attach->owns_dpp) {
            (void) dispatch_destroy(attach->pub.dpp);
        } else if (attach->owns_chid) {
            (void) channel_destroy(attach->pub.chid);
            _DPP(attach->pub.dpp)->chid = -1;
        }
    }

    free(attach->path);
    free(attach);

    if (ret == -1)
        errno = saved_errno;
    return ret;
}

int name_open(const char *name, int flags)
{
    char *resolved;
    int fd;
    int saved_errno;

    if ((unsigned) flags & ~(unsigned) NAME_ATTACH_KNOWN_FLAGS) {
        errno = EINVAL;
        return -1;
    }

    resolved = name_path_normalize(name, (unsigned) flags);
    if (!resolved)
        return -1;

    fd = _connect(0, resolved, 0, O_NOCTTY, 0, _IO_CONNECT_OPEN, 0,
                  _IO_FLAG_RD | _IO_FLAG_WR, _FTYPE_NAME, 0, 0, NULL, 0, NULL,
                  NULL);
    saved_errno = errno;
    if (fd == -1 && saved_errno == ESRCH)
        saved_errno = ESTALE;
    free(resolved);
    errno = saved_errno;
    return fd;
}

int name_close(int fd)
{
    return __connect_detach((uint64_t) fd);
}
