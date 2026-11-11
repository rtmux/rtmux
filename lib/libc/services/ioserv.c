/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/ioserv.h>
#include "../ioserv/api.h"

int ioserv_attach(dispatch_t *dpp,
                  ioserv_attr_t *attr,
                  const char *path,
                  enum _file_type file_type,
                  unsigned flags,
                  const ioserv_connect_ops_t *connect_ops,
                  const ioserv_ops_t *io_ops,
                  void *handle)
{
    return ioserv_backend_attach(dpp, attr, path, file_type, flags, connect_ops,
                                 io_ops, handle);
}

int ioserv_detach(dispatch_t *dpp, int id, unsigned flags)
{
    return ioserv_backend_detach(dpp, id, flags);
}

ioserv_context_t *ioserv_context_alloc(dispatch_t *dpp)
{
    return ioserv_backend_context_alloc(dpp);
}

void ioserv_context_free(ioserv_context_t *ctp)
{
    ioserv_backend_context_free(ctp);
}

ioserv_context_t *ioserv_block(ioserv_context_t *ctp)
{
    return ioserv_backend_block(ctp);
}

void ioserv_unblock(ioserv_context_t *ctp)
{
    ioserv_backend_unblock(ctp);
}

int ioserv_handler(ioserv_context_t *ctp)
{
    return ioserv_backend_handler(ctp);
}

int ioserv_open_bind(ioserv_context_t *ctp,
                     void *ocb,
                     const ioserv_ops_t *io_ops)
{
    return ioserv_backend_open_bind(ctp, ocb, io_ops);
}

int ioserv_msg_again(ioserv_context_t *ctp, int rcvid)
{
    return ioserv_backend_msg_again(ctp, rcvid);
}

int ioserv_msgread(ioserv_context_t *ctp, void *msg, int nbytes, int offset)
{
    return ioserv_backend_msgread(ctp, msg, nbytes, offset);
}

int ioserv_msgreadv(ioserv_context_t *ctp,
                    struct iovec *rmsg,
                    int rparts,
                    int offset)
{
    return ioserv_backend_msgreadv(ctp, rmsg, rparts, offset);
}

int ioserv_msgwrite(ioserv_context_t *ctp,
                    const void *msg,
                    int nbytes,
                    int offset)
{
    return ioserv_backend_msgwrite(ctp, msg, nbytes, offset);
}

int ioserv_msgwritev(ioserv_context_t *ctp,
                     const struct iovec *smsg,
                     int sparts,
                     int offset)
{
    return ioserv_backend_msgwritev(ctp, smsg, sparts, offset);
}

int ioserv_devino(int id, dev_t *pdevno, ino_t *pino)
{
    return ioserv_backend_devino(id, pdevno, pino);
}

void *ioserv_ocb(ioserv_context_t *ctp, struct _msg_info *info)
{
    return _ioserv_ocb(ctp, info);
}

const ioserv_ops_t *ioserv_binding_funcs(ioserv_context_t *ctp,
                                         struct _msg_info *info)
{
    return _ioserv_binding_funcs(ctp, info);
}

int ioserv_unbind(struct _msg_info *rep)
{
    return _ioserv_unbind(rep);
}
