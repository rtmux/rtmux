/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* hwres-msg.h */

#ifndef __HWRES_MSG_H_INCLUDED
#define __HWRES_MSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __SYSMSG_H_INCLUDED
#include <sys/sysmsg.h>
#endif

#define HWRES_COID ROOTTASK_COID

enum hwres_command { HWRES_CMD = _HWRES_BASE };

/* Private roottask hardware-resource requests. */
#define HWRES_REQ_MASK 0xfe00
enum hwres_request {
    HWRES_REQ_CREATE = (2 << 8),
    HWRES_REQ_CLAIM = (6 << 8),
    HWRES_REQ_RELEASE = (8 << 8),
    HWRES_REQ_QUERY = (10 << 8),
    HWRES_REQ_COUNT = (12 << 8),
    HWRES_REQ_QUERY_NAME = (14 << 8)
};

struct hwres_msg {
    uint16_t type;
    uint16_t subtype;
    uint32_t pid;
    uint32_t nbytes;
    uint32_t count;
    uint32_t index;
};

struct hwres_msg_reply {
    uint8_t res[sizeof(struct hwres_msg)];
};

typedef union {
    struct hwres_msg i;
    struct hwres_msg_reply o;
} hwres_msg_t;

#endif
