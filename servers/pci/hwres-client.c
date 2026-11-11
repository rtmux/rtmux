/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/rtmux-core.h>

#include "../roottask/include/hwres-msg.h"
#include "hwres-client.h"

static int hwres_pack(void *header,
                      size_t hbytes,
                      size_t nbytes_off,
                      void *data,
                      size_t dwidth,
                      size_t noff,
                      unsigned count,
                      void *reply,
                      size_t rbytes)
{
    iov_t *iovout;
    iov_t iovreply;
    char **nameptr;
    unsigned i, index;
    size_t nbytes;
    int ret;

    iovout = malloc((2 + count) * sizeof(*iovout));
    if (!iovout)
        return -1;

    nbytes = 0;
    index = 0;
    SETIOV(&iovout[index], header, hbytes);
    index++;
    SETIOV(&iovout[index], data, dwidth * count);
    index++;
    nbytes += dwidth * count;

    for (i = 0; i < count; i++) {
        nameptr = (char **) ((char *) data + (i * dwidth) + noff);
        SETIOV(&iovout[index], nameptr[0], strlen(nameptr[0]) + 1);
        index++;
        nbytes += strlen(nameptr[0]) + 1;
    }

    SETIOV(&iovreply, reply, rbytes);
    *((uint32_t *) ((char *) header + nbytes_off)) = (uint32_t) nbytes;

    /* ipc-send: pci -> roottask-core */
    ret = msg_sendv(HWRES_COID, iovout, (int) index, reply ? &iovreply : NULL,
                    reply ? 1 : 0);
    if (ret != -1) {
        for (i = 0; i < count; i++) {
            nameptr = (char **) ((char *) data + (i * dwidth) + noff);
            nameptr[0] = GETIOVBASE(&iovout[i + 2]);
        }
    }

    free(iovout);
    return ret;
}

int hwres_create(hwres_range_t *list, unsigned count)
{
    if (!list || !count) {
        errno = EINVAL;
        return -1;
    }

    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = HWRES_REQ_CREATE,
        .i.pid = 0,
        .i.count = count,
    };

    for (unsigned i = 0; i < count; i++) {
        if (!(list[i].flags & HWRES_FLAG_NAME))
            list[i].name = "";
    }

    return hwres_pack(&request, sizeof(request),
                      offsetof(struct hwres_msg, nbytes), list, sizeof(*list),
                      offsetof(hwres_range_t, name), count, NULL, 0);
}

int hwres_claim_for_pid(hwres_claim_t *list, unsigned count, pid_t pid)
{
    if (!list || !count) {
        errno = EINVAL;
        return -1;
    }

    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = HWRES_REQ_CLAIM,
        .i.pid = (uint32_t) pid,
        .i.count = count,
    };

    for (unsigned i = 0; i < count; i++) {
        if (!(list[i].flags & HWRES_FLAG_NAME))
            list[i].name = "";
    }

    return hwres_pack(&request, sizeof(request),
                      offsetof(struct hwres_msg, nbytes), list, sizeof(*list),
                      offsetof(hwres_claim_t, name), count, list,
                      count * sizeof(*list));
}

int hwres_claim(hwres_claim_t *list, unsigned count)
{
    return hwres_claim_for_pid(list, count, 0);
}

int hwres_release_for_pid(hwres_claim_t *list, unsigned count, pid_t pid)
{
    if (!list || !count) {
        errno = EINVAL;
        return -1;
    }

    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = HWRES_REQ_RELEASE,
        .i.pid = (uint32_t) pid,
        .i.count = count,
    };

    for (unsigned i = 0; i < count; i++) {
        if (!(list[i].flags & HWRES_FLAG_NAME))
            list[i].name = "";
    }

    return hwres_pack(&request, sizeof(request),
                      offsetof(struct hwres_msg, nbytes), list, sizeof(*list),
                      offsetof(hwres_claim_t, name), count, NULL, 0);
}

int hwres_release(hwres_claim_t *list, unsigned count)
{
    return hwres_release_for_pid(list, count, 0);
}

int hwres_claim_range(hwres_claim_t *claim,
                      uint64_t base,
                      size_t size,
                      uint32_t type)
{
    if (!claim || !size || size - 1 > UINT64_MAX - base) {
        errno = EINVAL;
        return -1;
    }
    *claim = (hwres_claim_t){
        .start = base,
        .end = base + size - 1,
        .length = size,
        .flags = (type & HWRES_TYPE_MASK) | HWRES_FLAG_RANGE,
    };
    return hwres_claim(claim, 1);
}

void hwres_release_range(hwres_claim_t *claim)
{
    if (!claim)
        return;
    (void) hwres_release(claim, 1);
    memset(claim, 0, sizeof(*claim));
}

static int hwres_query_filtered(hwres_range_t *list,
                                int listcnt,
                                int start,
                                pid_t pid,
                                char *name,
                                unsigned type)
{
    iov_t iovin[1], iovout[2];
    hwres_msg_t request = {.i.type = HWRES_CMD};

    if (start < 0 || listcnt < 0) {
        errno = EINVAL;
        return -1;
    }

    if (!list || !listcnt) {
        listcnt = 0;
        start = 0;
        request.i.subtype = HWRES_REQ_COUNT;
    } else {
        request.i.subtype = HWRES_REQ_QUERY_NAME;
    }

    request.i.subtype |= (type & HWRES_TYPE_MASK);
    request.i.pid = (uint32_t) pid;
    request.i.count = (uint32_t) listcnt;
    request.i.index = (uint32_t) start;
    request.i.nbytes = name ? (uint32_t) (strlen(name) + 1) : 0;

    SETIOV(&iovout[0], &request, sizeof(request));
    SETIOV(&iovout[1], name, request.i.nbytes);
    SETIOV(&iovin[0], list, (size_t) listcnt * sizeof(*list));

    /* ipc-send: pci -> roottask-core */
    return msg_sendv(HWRES_COID, iovout, 2, iovin, listcnt ? 1 : 0);
}

int hwres_query(hwres_range_t *list, int listcnt, int start, uint32_t type)
{
    pid_t pid = (type & HWRES_FLAG_USED) ? -1 : 0;
    int count = hwres_query_filtered(NULL, 0, start, pid, NULL, type);

    if (count <= 0 || !list || !listcnt)
        return count;

    return hwres_query_filtered(list, listcnt, start, pid, NULL, type) != -1
               ? count
               : -1;
}
