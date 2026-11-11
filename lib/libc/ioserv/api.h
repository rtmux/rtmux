/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Private libc backend for the RTMux I/O-service facade. */

#ifndef LIBC_IOSERV_API_H
#define LIBC_IOSERV_API_H

#include <sys/dispatch.h>

__hidden int ioserv_backend_attach(dispatch_t *dpp,
                                   ioserv_attr_t *attr,
                                   const char *path,
                                   enum _file_type file_type,
                                   unsigned flags,
                                   const ioserv_connect_ops_t *connect_funcs,
                                   const ioserv_ops_t *io_funcs,
                                   IOSERV_HANDLE_T *handle);
__hidden int ioserv_backend_detach(dispatch_t *dpp, int id, unsigned flags);
__hidden ioserv_context_t *ioserv_backend_context_alloc(dispatch_t *dpp);
__hidden void ioserv_backend_context_free(ioserv_context_t *ctp);
__hidden ioserv_context_t *ioserv_backend_block(ioserv_context_t *ctp);
__hidden void ioserv_backend_unblock(ioserv_context_t *ctp);
__hidden int ioserv_backend_handler(ioserv_context_t *ctp);

__hidden int ioserv_backend_devino(int id, dev_t *pdevno, ino_t *pino);
__hidden int ioserv_backend_open_bind(ioserv_context_t *ctp,
                                      IOSERV_OCB_T *ocb,
                                      const ioserv_ops_t *io_funcs);
__hidden int ioserv_backend_msg_again(ioserv_context_t *ctp, int rcvid);
__hidden int ioserv_backend_msgwrite(ioserv_context_t *ctp,
                                     const void *msg,
                                     int size,
                                     int offset);
__hidden int ioserv_backend_msgwritev(ioserv_context_t *ctp,
                                      const struct iovec *smsg,
                                      int sparts,
                                      int offset);
__hidden int ioserv_backend_msgread(ioserv_context_t *ctp,
                                    void *msg,
                                    int size,
                                    int offset);
__hidden int ioserv_backend_msgreadv(ioserv_context_t *ctp,
                                     struct iovec *rmsg,
                                     int rparts,
                                     int offset);

__hidden void *_ioserv_ocb(ioserv_context_t *ctp, struct _msg_info *info);
__hidden const ioserv_ops_t *_ioserv_binding_funcs(ioserv_context_t *ctp,
                                                   struct _msg_info *info);
__hidden int _ioserv_unbind(struct _msg_info *rep);
__hidden int _ioserv_detach_id(ioserv_context_t *ctp, int id, unsigned flags);

#endif
