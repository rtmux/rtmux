/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux ldso virtual-memory shim implementations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rtmux-shim.h"

#include <sys/memmsg.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

void *rtmux_ldso_mmap(void *addr,
                      size_t len,
                      int prot,
                      int flags,
                      int fd,
                      off_t offset)
{
    return mmap(addr, len, prot, flags, fd, offset);
}

int rtmux_ldso_mprotect(void *addr, size_t len, int prot)
{
    mem_ctrl_t msg;

    msg.i.type = _MEM_CTRL;
    msg.i.subtype = _MEM_CTRL_PROTECT;
    msg.i.addr = (uintptr_t) addr;
    msg.i.len = len;
    msg.i.flags = (unsigned) prot;
    return msg_sendnc(VM_COID, &msg.i, sizeof msg.i, 0, 0);
}

int mprotect(void *addr, size_t len, int prot)
    __attribute__((alias("rtmux_ldso_mprotect")));

int rtmux_ldso_munmap(void *addr, size_t len)
{
    mem_ctrl_t msg;

    msg.i.type = _MEM_CTRL;
    msg.i.subtype = _MEM_CTRL_UNMAP;
    msg.i.addr = (uintptr_t) addr;
    msg.i.len = len;
    msg.i.flags = 0;
    return msg_sendnc(VM_COID, &msg.i, sizeof msg.i, 0, 0);
}

int munmap(void *addr, size_t len) __attribute__((alias("rtmux_ldso_munmap")));
