/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* namespace_link — register a pathname in the roottask namespace.
 *
 * The default (IPC) implementation sends a _connect message to proc. Roottask
 * overrides this by setting _namespace_link_hook to its own function that
 * manipulates the pathname tables directly.
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/ftype.h>
#include <sys/path-registry.h>
#include <sys/pathmsg.h>
#include <sys/ioserv.h>

/* Hook for roottask to override namespace_link with its own implementation.
 * When unset, the default IPC-based version is used.
 */
__hidden int (*_namespace_link_hook)(const char *path,
                                     uint32_t nd,
                                     pid_t pid,
                                     int chid,
                                     unsigned handle,
                                     enum _file_type file_type,
                                     unsigned flags);

static int namespace_link_ipc(const char *path,
                              uint32_t nd,
                              pid_t pid,
                              int chid,
                              unsigned handle,
                              enum _file_type file_type,
                              unsigned flags)
{
    struct _ioserv_link_extra linkl;
    int fd;

    if (flags & IOSERV_FLAG_FTYPEALL) {
        file_type = _FTYPE_ALL;
    } else if (file_type < _FTYPE_ANY) {
        errno = EINVAL;
        return -1;
    }

    if (!path || !*path) {
        path = "/";
        flags |= IOSERV_FLAG_FTYPEONLY;
    } else if (flags & IOSERV_FLAG_FTYPEONLY) {
        errno = EINVAL;
        return -1;
    }

    linkl.nd = nd;
    linkl.pid = pid ? pid : getpid();
    linkl.chid = chid;
    linkl.handle = handle;
    linkl.file_type = file_type;
    linkl.flags = flags;

    if ((fd = _connect(PATH_COID, path, 0, O_NOCTTY, 0, _IO_CONNECT_LINK, 0, 0,
                       _FTYPE_LINK, _IO_CONNECT_EXTRA_SERVICE_LINK,
                       sizeof linkl, &linkl, 0, 0, 0)) == -1) {
        return -1;
    }

    if (flags & NAMESPACE_FLAG_STICKY) {
        __connect_detach((uint64_t) fd);
        fd = 0;
    }
    return fd;
}

int namespace_link(const char *path,
                   uint32_t nd,
                   pid_t pid,
                   int chid,
                   unsigned handle,
                   enum _file_type file_type,
                   unsigned flags)
{
    if (_namespace_link_hook)
        return _namespace_link_hook(path, nd, pid, chid, handle, file_type,
                                    flags);
    return namespace_link_ipc(path, nd, pid, chid, handle, file_type, flags);
}
