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

int mprotect(void *addr, size_t len, int prot)
{
    mem_ctrl_t msg;

    msg.i.type = _MEM_CTRL;
    msg.i.subtype = _MEM_CTRL_PROTECT;
    msg.i.addr = (uintptr_t) addr;
    msg.i.len = len;
    msg.i.flags = (unsigned) prot;
    return msg_sendnc(VM_COID, &msg.i, sizeof msg.i, 0, 0);
}
