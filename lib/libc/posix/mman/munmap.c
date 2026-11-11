/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <sys/rtmux-core.h>
#include <sys/mman.h>
#include <sys/memmsg.h>

static int munmap_flags(void *addr, size_t len, unsigned flags)
{
    mem_ctrl_t msg;

    msg.i.type = _MEM_CTRL;
    msg.i.subtype = _MEM_CTRL_UNMAP;
    msg.i.addr = (uintptr_t) addr;
    msg.i.len = len;
    msg.i.flags = flags;
    return msg_sendnc(VM_COID, &msg.i, sizeof msg.i, 0, 0);
}

int munmap(void *addr, size_t len)
{
    return munmap_flags(addr, len, 0);
}
