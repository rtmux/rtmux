/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * rtmux_phys_contain() — seal this process's physical-mapping rights.
 *
 * Sends _PROC_PHYS_CONTAIN to roottask. After it returns, every further
 * specific-address MAP_PHYS from this process is refused (MAP_ANON DMA buffers
 * stay allowed). The restriction is irreversible. An LKL domain calls this once
 * it has mapped all of its device MMIO during hardware discovery, so a later
 * bug cannot reach new physical registers.
 *
 * License: Apache 2.0
 */

#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

int rtmux_phys_contain(void)
{
    proc_phys_contain_t msg;

    msg.i.type = _PROC_PHYS_CONTAIN;
    msg.i.zero = 0;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0);
}
