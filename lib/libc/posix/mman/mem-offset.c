/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * mem_offset / posix_mem_offset — translate virtual address to physical address
 * via the VM service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <devctl.h>
#include <devctl/proc.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ftype.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <sys/mman.h>
#include <sys/memmsg.h>

static int mem_offset_request(const void *addr,
                              size_t len,
                              off_t *off,
                              size_t *contig_len,
                              int *fd,
                              int subtype)
{
    mem_offset_t msg = {
        .i =
            {
                .type = _MEM_OFFSET,
                .subtype = (uint16_t) subtype,
                .reserved = -1,
                .addr = (uintptr_t) addr,
                .len = len,
            },
    };

    if (msg_sendnc(VM_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) == -1)
        return -1;
    if (off)
        *off = (off_t) msg.o.offset;
    if (contig_len)
        *contig_len = (size_t) msg.o.size;
    if (fd)
        *fd = msg.o.fd;
    return 0;
}

int mem_offset(const void *addr,
               int fd,
               size_t len,
               off_t *off,
               size_t *contig_len)
{
    if (fd != NOFD) {
        errno = ENODEV;
        return -1;
    }
    return mem_offset_request(addr, len, off, contig_len, NULL,
                              _MEM_OFFSET_PHYS);
}

int posix_mem_offset(const void *addr,
                     size_t len,
                     off_t *off,
                     size_t *contig_len,
                     int *fd)
{
    return mem_offset_request(addr, len, off, contig_len, fd, _MEM_OFFSET_FD);
}

int posix_typed_mem_open(const char *name, int oflag, int tflag)
{
    static const char prefix[] = "/dev/tymem";
    uint32_t extra = (uint32_t) tflag;
    char path[PATH_MAX + 1];
    int n;

    if (!name || !*name) {
        errno = EINVAL;
        return -1;
    }

    n = snprintf(path, sizeof path, "%s%s%s", prefix, name[0] == '/' ? "" : "/",
                 name);
    if (n < 0 || (size_t) n >= sizeof path) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return _connect(0, path, 0, (unsigned) oflag | O_NOCTTY, 0,
                    _IO_CONNECT_OPEN, 0, 0, _FTYPE_TYMEM,
                    _IO_CONNECT_EXTRA_TYMEM, sizeof extra, &extra, 0, 0, 0);
}

int posix_typed_mem_get_info(int fd, struct posix_typed_mem_info *info)
{
    devctl_mem_object_info memobj;
    int ret;

    if (!info) {
        errno = EINVAL;
        return -1;
    }

    memset(&memobj, 0, sizeof memobj);
    ret = devctl(fd, DEVCTL_MEM_OBJECT_INFO, &memobj, sizeof memobj, NULL);
    if (ret != EOK) {
        errno = ret;
        return -1;
    }
    if (memobj.type != MEMOBJ_INFO_TYPE_TYPED) {
        errno = ENODEV;
        return -1;
    }

    info->posix_tmi_length = (size_t) memobj.size;
    memset(info->__reserved, 0, sizeof info->__reserved);
    return 0;
}
