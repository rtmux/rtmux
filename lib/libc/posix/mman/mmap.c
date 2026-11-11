/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/mman.h>
#include <sys/memmsg.h>

__hidden void *_mmap2(void *addr,
                      size_t len,
                      int prot,
                      int flags,
                      int fd,
                      off64_t off,
                      unsigned align,
                      unsigned preload,
                      void **base,
                      size_t *size)
{
    mem_map_t msg;

    msg.i.type = _MEM_MAP;
    msg.i.zero = 0;
    msg.i.addr = (uintptr_t) addr;
    msg.i.len = len;
    msg.i.prot = (unsigned) prot;
    msg.i.flags = (unsigned) flags;
    msg.i.fd = fd;
    msg.i.offset = off;
    msg.i.align = align;
    msg.i.preload = preload;
    msg.i.reserved1 = 0;
    msg.i.mem_handle = 0;
    msg.i.mem_rights = 0;
    msg.i.reserved2 = 0;
    if (msg_sendnc(VM_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) == -1)
        return MAP_FAILED;
    if (base)
        *base = (void *) (uintptr_t) msg.o.real_addr;
    if (size)
        *size = msg.o.real_size;
    return (void *) (uintptr_t) msg.o.addr;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return _mmap2(addr, len, prot, flags, fd, off, 0, 0, 0, 0);
}

int memobj_handle_from_fd_r(int fd, unsigned rights, uint64_t *handle)
{
    mem_handle_t msg;

    if (!handle) {
        errno = EINVAL;
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    msg.i.type = _MEM_HANDLE;
    msg.i.subtype = _MEM_HANDLE_CREATE;
    msg.i.fd = fd;
    msg.i.rights = rights;
    if (msg_sendnc(VM_COID, &msg.i, sizeof(msg.i), &msg.o, sizeof(msg.o)) == -1)
        return -1;
    *handle = msg.o.handle;
    return 0;
}

uint64_t memobj_handle_from_fd(int fd, unsigned rights)
{
    uint64_t handle;

    if (memobj_handle_from_fd_r(fd, rights, &handle) == -1)
        return 0;
    return handle;
}

int memobj_handle_close(uint64_t handle)
{
    mem_handle_t msg;

    memset(&msg, 0, sizeof(msg));
    msg.i.type = _MEM_HANDLE;
    msg.i.subtype = _MEM_HANDLE_CLOSE;
    msg.i.handle = handle;
    return msg_sendnc(VM_COID, &msg.i, sizeof(msg.i), NULL, 0);
}

void *mmap_handle(void *addr,
                  size_t len,
                  int prot,
                  int flags,
                  uint64_t handle,
                  off64_t off)
{
    mem_map_t msg;

    memset(&msg, 0, sizeof(msg));
    msg.i.type = _MEM_MAP;
    msg.i.addr = (uintptr_t) addr;
    msg.i.len = len;
    msg.i.prot = (unsigned) prot;
    msg.i.flags = (unsigned) flags;
    msg.i.fd = NOFD;
    msg.i.offset = off;
    msg.i.mem_handle = handle;
    if (msg_sendnc(VM_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) == -1)
        return MAP_FAILED;
    return (void *) (uintptr_t) msg.o.addr;
}
