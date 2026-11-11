/*
 * Private socket type shim for LKL UAPI headers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_LKL_SYS_SOCKET_H
#define RTMUX_LKL_SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t sa_family_t;
typedef unsigned socklen_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_pad[126];
};

#endif
