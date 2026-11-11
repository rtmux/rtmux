/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/node.h - private local node and devno packing helpers.
 *
 * RTMux is single-node. These constants only preserve in-tree IPC and stat
 * device-number packing.
 */
#ifndef _RTMUX_NODE_H_INCLUDED
#define _RTMUX_NODE_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS

#define RTMUX_LOCAL_NODE 0
#define RTMUX_NODE_BITS 16
#define RTMUX_NODE_MASK ((1 << RTMUX_NODE_BITS) - 1)
#define RTMUX_NODE_CMP(a, b) (((a) & RTMUX_NODE_MASK) - ((b) & RTMUX_NODE_MASK))

__END_DECLS

#endif
