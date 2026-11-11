/*
 * Host-build stub for <sys/iomsg.h> used by the connect-path fuzz target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Only struct _io_connect is needed, and rtmux_lkl_connect_path_ok reads it
 * generically through offsetof(path) and c->path_len, so this mirrors the real
 * field layout on host integer types rather than pulling the RTMux message
 * headers and their platform typedef chain.
 */

#ifndef RTMUX_FUZZ_SYS_IOMSG_H
#define RTMUX_FUZZ_SYS_IOMSG_H

#include <stdint.h>

struct _io_connect {
    uint16_t type;
    uint16_t subtype;
    uint32_t file_type;
    uint16_t reply_max;
    uint16_t entry_max;
    uint32_t key;
    uint32_t handle;
    uint32_t ioflag;
    uint32_t mode;
    uint16_t sflag;
    uint16_t access;
    uint32_t path_len;
    uint8_t eflag;
    uint8_t extra_type;
    uint16_t extra_len;
    char path[1];
};

#endif /* RTMUX_FUZZ_SYS_IOMSG_H */
