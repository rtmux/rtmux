/*
 * RTMux userspace I/O service interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_IOSERV_H
#define _SYS_IOSERV_H

#ifndef IOSERV_HANDLE_T
#define IOSERV_HANDLE_T void
#endif

#ifndef IOSERV_OCB_T
#define IOSERV_OCB_T void
#endif

#include <sys/dispatch.h>
#include <stdint.h>
#include <stddef.h>

typedef enum ioserv_reply_status {
    IOSERV_REPLY_OK = 0,
    IOSERV_REPLY_DENIED,
    IOSERV_REPLY_UNAVAILABLE,
    IOSERV_REPLY_TIMEOUT,
    IOSERV_REPLY_BAD_REQUEST,
    IOSERV_REPLY_IO_FAILURE,
    IOSERV_REPLY_FAULTED,
    IOSERV_REPLY_BUSY,
} ioserv_reply_status_t;

typedef struct ioserv_reply_counters {
    uint64_t sends;
    uint64_t receives;
    uint64_t denied_sends;
    uint64_t timeouts;
    uint64_t queue_full;
    uint64_t max_depth;
    uint64_t disconnects; /* client-disconnect notices handled */
} ioserv_reply_counters_t;

int ioserv_attach(dispatch_t *dpp,
                      ioserv_attr_t *attr,
                      const char *path,
                      enum _file_type file_type,
                      unsigned flags,
                      const ioserv_connect_ops_t *connect_ops,
                      const ioserv_ops_t *io_ops,
                      IOSERV_HANDLE_T *handle);
int ioserv_detach(dispatch_t *dpp, int id, unsigned flags);
ioserv_context_t *ioserv_context_alloc(dispatch_t *dpp);
void ioserv_context_free(ioserv_context_t *ctp);
ioserv_context_t *ioserv_block(ioserv_context_t *ctp);
void ioserv_unblock(ioserv_context_t *ctp);
int ioserv_handler(ioserv_context_t *ctp);
int ioserv_open_bind(ioserv_context_t *ctp,
                         IOSERV_OCB_T *ocb,
                         const ioserv_ops_t *io_ops);
int ioserv_msg_again(ioserv_context_t *ctp, int rcvid);
int ioserv_msgread(ioserv_context_t *ctp,
                       void *msg,
                       int nbytes,
                       int offset);
int ioserv_msgreadv(ioserv_context_t *ctp,
                        struct iovec *rmsg,
                        int rparts,
                        int offset);
int ioserv_msgwrite(ioserv_context_t *ctp,
                        const void *msg,
                        int nbytes,
                        int offset);
int ioserv_msgwritev(ioserv_context_t *ctp,
                         const struct iovec *smsg,
                         int sparts,
                         int offset);
int ioserv_devino(int id, dev_t *pdevno, ino_t *pino);
void *ioserv_ocb(ioserv_context_t *ctp, struct _msg_info *info);
const ioserv_ops_t *ioserv_binding_funcs(ioserv_context_t *ctp,
                                                 struct _msg_info *info);
int ioserv_unbind(struct _msg_info *rep);
ioserv_reply_status_t ioserv_reply_status_from_errno(int error);
const char *ioserv_reply_status_name(ioserv_reply_status_t status);
void ioserv_reply_counters_record_receive(uint64_t queue_depth);
void ioserv_reply_counters_record_disconnect(void);
void ioserv_reply_counters_record(int error, uint64_t queue_depth);
void ioserv_reply_counters_snapshot(ioserv_reply_counters_t *out);
int ioserv_reply_counters_format(char *buf, size_t size);

#endif /* _SYS_IOSERV_H */
