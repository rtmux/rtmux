/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* namespace_symlink — create a roottask namespace symlink. */

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/ftype.h>
#include <sys/path-registry.h>
#include <sys/pathmsg.h>
#include <sys/stat.h>

int namespace_symlink(const char *path, const char *symlinkp)
{
    int fd;

    fd = _connect(PATH_COID, symlinkp, S_IFLNK | 0777,
                  O_CREAT | O_EXCL | O_NOCTTY, 0, _IO_CONNECT_LINK, 0, 0,
                  _FTYPE_LINK, _IO_CONNECT_EXTRA_PROC_SYMLINK,
                  (unsigned) (strlen(path) + 1), path, 0, 0, 0);
    if (fd == -1)
        return -1;
    __connect_detach((uint64_t) fd);
    return 0;
}
