/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Internal connection control structure and flags. */

#ifndef _RTMUX_LIBC_CONNECT_H
#define _RTMUX_LIBC_CONNECT_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>

/* Flags used in the _connect_ctrl structure */
#define FLAG_NOCTTY 0x0001
#define FLAG_SET_ENTRY 0x0002
#define FLAG_TEST_ENTRY 0x0004
#define FLAG_NO_PREFIX 0x0008
#define FLAG_NO_SYM 0x0010
#define FLAG_MALLOC_FDS 0x0020
#define FLAG_TEST_NPC_ONLY 0x0040
#define FLAG_REUSE_BUFFER 0x0080
#define FLAG_STACK_ALLOC 0x0100
#define FLAG_TEST_ND_ONLY 0x0200
#define FLAG_NO_RETRY 0x0400
#define FLAG_ERR_COLLISION 0x0800

#define PCAST(typ, val) ((typ *) (void *) (val))
#define FD_BUF_INCREMENT 10

struct _connect_ctrl {
    int base;
    struct _io_connect *msg;

    union {
        struct _io_connect_link_reply link;
        struct _io_connect_ftype_reply ftype;
    } *reply;

    struct _io_connect_entry *entry;
    const void *extra;
    int status;
    void *response;
    int (*send)(int coid,
                const iov_t *smsg,
                int sparts,
                const iov_t *rmsg,
                int rparts);
    int flags;
    char *path;
    int pathsize;
    int pathlen;
    int prefix_len;

    uint16_t fds_len;
    uint16_t fds_index;
    int *fds;
    int response_len;
    uint32_t nd;
    size_t chroot_len;
};

int _connect_ctrl(struct _connect_ctrl *ctrl,
                  const char *path,
                  unsigned response_len,
                  void *response) __hidden;

int _connect_entry(int base,
                   const char *path,
                   mode_t mode,
                   unsigned oflag,
                   unsigned sflag,
                   unsigned subtype,
                   int testcancel,
                   unsigned accessl,
                   unsigned file_type,
                   unsigned extra_type,
                   unsigned extra_len,
                   const void *extra,
                   unsigned response_len,
                   void *response,
                   int *status,
                   struct _io_connect_entry *entry,
                   int enoretry) __hidden;

int _connect_fd(int base,
                const char *path,
                mode_t mode,
                unsigned oflag,
                unsigned sflag,
                unsigned subtype,
                int testcancel,
                unsigned accessl,
                unsigned file_type,
                unsigned extra_type,
                unsigned extra_len,
                const void *extra,
                unsigned response_len,
                void *response,
                int *status,
                int *fd_len,
                void *fd_array) __hidden;

int _connect_combine(const char *path,
                     mode_t mode,
                     unsigned oflag,
                     unsigned sflag,
                     int testcancel,
                     unsigned file_type,
                     unsigned extra_len,
                     void *extra,
                     unsigned response_len,
                     void *response) __hidden;

int _connect_object(const char *name,
                    const char *prefix,
                    mode_t mode,
                    int oflag,
                    unsigned file_type,
                    unsigned extra_type,
                    unsigned extra_len,
                    const void *extra) __hidden;

#endif /* _RTMUX_LIBC_CONNECT_H */
