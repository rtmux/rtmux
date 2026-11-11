/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/rtmux-core.h>

#include "../../../servers/roottask/include/hwres-msg.h"
#include "../../../servers/roottask/include/hwres-types.h"

static int rtmux_hwres_send(unsigned subtype,
                            uint64_t base,
                            size_t size,
                            unsigned type,
                            pid_t pid,
                            int want_reply)
{
    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = (uint16_t) subtype,
        .i.pid = pid > 0 ? (uint32_t) pid : 0,
        .i.count = 1,
    };
    hwres_claim_t claim = {
        .length = size,
        .align = 1,
        .start = base,
        .end = base + size - 1,
        .flags = (type & HWRES_TYPE_MASK) | HWRES_FLAG_RANGE,
        .name = "",
    };
    iov_t out[3];
    iov_t in;

    if (size == 0 || UINT64_MAX - base < size - 1) {
        errno = EINVAL;
        return -1;
    }

    request.i.nbytes = (uint32_t) (sizeof(claim) + strlen(claim.name) + 1);
    out[0].iov_base = &request;
    out[0].iov_len = sizeof(request);
    out[1].iov_base = &claim;
    out[1].iov_len = sizeof(claim);
    out[2].iov_base = (void *) claim.name;
    out[2].iov_len = strlen(claim.name) + 1;
    in.iov_base = &claim;
    in.iov_len = sizeof(claim);
    return msg_sendv(ROOTTASK_COID, out, 3, want_reply ? &in : NULL,
                     want_reply ? 1 : 0);
}

int rtmux_hwres_claim_range(uint64_t base, size_t size, unsigned type)
{
    return rtmux_hwres_send(HWRES_REQ_CLAIM, base, size, type, 0, 1);
}

int rtmux_hwres_claim_range_for_pid(uint64_t base,
                                    size_t size,
                                    unsigned type,
                                    pid_t pid)
{
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    return rtmux_hwres_send(HWRES_REQ_CLAIM, base, size, type, pid, 1);
}

void rtmux_hwres_release_range(uint64_t base, size_t size, unsigned type)
{
    (void) rtmux_hwres_send(HWRES_REQ_RELEASE, base, size, type, 0, 0);
}

void rtmux_hwres_release_range_for_pid(uint64_t base,
                                       size_t size,
                                       unsigned type,
                                       pid_t pid)
{
    (void) rtmux_hwres_send(HWRES_REQ_RELEASE, base, size, type, pid, 0);
}
