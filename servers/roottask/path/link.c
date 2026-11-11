/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include "root-process.h"

#include <align.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <sys/path-registry.h>

static int path_append(const char *str, int len, char **pbuf, size_t *pmaxbuf)
{
    int nbytes;
    char *buf;

    if (len == 0 && str)
        len = (int) strlen(str);

    nbytes = len;
    buf = *pbuf;
    if (buf && *pmaxbuf > 0) {
        if (str) {
            while (*pmaxbuf > 1 && nbytes--) {
                (*pmaxbuf)--;
                *buf++ = *str++;
            }
        }
        *buf = '\0';
        *pbuf = buf;
    }
    return len;
}

int reply_symlink(ioserv_context_t *ctp,
                  unsigned eflag,
                  struct _io_connect_link_reply *linkp,
                  struct symlink_object *symlinkp,
                  const char *path,
                  const char *tail)
{
    int len;
    iov_t *iov;

    if (path) {
        _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_LINK);
    }
    linkp->eflag = (uint8_t) eflag;
    linkp->nentries = 0;
    linkp->path_len = (uint16_t) symlinkp->len;

    iov = ctp->iov;
    SETIOV(iov, linkp, sizeof *linkp);
    iov++;

    len = 0;
    if (path) {
        if (symlinkp->name[0] != '/' && (len = (int) (tail - path))) {
            while (--len && path[len - 1] != '/') {
                /* nothing to do */
            }
        }

        if (len) {
            linkp->path_len = (uint16_t) (linkp->path_len + len);
            SETIOV(iov, path, (unsigned) len);
            iov++;
        }

        len = (int) strlen(tail);
    }

    SETIOV(iov, symlinkp->name,
           len ? (unsigned) (symlinkp->len - 1) : (unsigned) symlinkp->len);
    iov++;

    if (len) {
        linkp->path_len = (uint16_t) (linkp->path_len + len + 1);
        SETIOV(iov, tail - 1, (unsigned) (len + 2));
        iov++;
    }

    return IOSERV_NPARTS(iov - ctp->iov);
}

static int path_close_ocb(ioserv_context_t *ctp, void *reserved, void *ocb)
{
    path_object_detach(ocb);

    /* Namespace change notification. */
    proc_event_trigger(PROC_EVENT_PATHSPACE);

    return EOK;
}

static int path_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = (uintptr_t) ocb ^ (uintptr_t) &path_stat;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static int path_object_exists(char *path, int type)
{
    path_object_t *obp;
    node_t *nop;

    if ((nop = path_node_lookup(0, path, PATH_LOOKUP_ATTACH, 0))) {
        for (obp = nop->object; obp; obp = obp->hdr.next) {
            if (obp->hdr.type == type) {
                path_node_detach(nop);
                return 1;
            }
        }
        path_node_detach(nop);
    }
    return 0;
}

static int path_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *vocb)
{
    path_object_t *obp = (path_object_t *) vocb;
    char *path;
    int size;
    size_t pathmax;
    int len;
    node_t *node, *prev;

    if (obp->hdr.type != OBJECT_SERVER) {
        return ENOSYS;
    }

    if ((size = (int) (ctp->msg_max_size - (unsigned) ctp->offset -
                       sizeof msg->o)) < 0) {
        return EMSGSIZE;
    }
    size = min(size, msg->i.path_len);
    pathmax = (unsigned) size;

    memset(&msg->o, 0x00, sizeof msg->o);
    path = (char *) (&msg->o + 1);

    len = 1;
    if (pathmax) {
        *path = '\0';
        if ((size_t) len < pathmax) {
            pathmax -= (size_t) (len - 1);
            path += len - 1;
        } else {
            pathmax = 0;
        }
    }

    prev = 0;
    do {
        for (node = obp->hdr.node; node != node->parent; node = node->parent) {
            if (node->parent == prev || node->parent == node) {
                break;
            }
        }
        if (node->len) {
            len += path_append("/", 0, &path, &pathmax);
            len += path_append(node->name, node->len, &path, &pathmax);
        }
        prev = node;
    } while (node != obp->hdr.node);

    if (obp->hdr.flags & IOSERV_FLAG_DIR) {
        len += path_append("/", 0, &path, &pathmax);
    }

    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o + (unsigned) min(size, len));
}

static const ioserv_ops_t link_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .close_ocb = path_close_ocb,
    .stat = path_stat,
    .fdinfo = path_fdinfo,
};

int server_create(path_object_t *obp, void *data)
{
    struct _ioserv_link_extra *extra = data;

    if (extra->file_type == _FTYPE_NAME) {
        obp->hdr.type = OBJECT_NAME;
    }
    obp->server.nd = extra->nd;
    obp->server.pid = extra->pid;
    obp->server.chid = extra->chid;
    obp->server.handle = extra->handle;
    obp->server.file_type = extra->file_type;
    return EOK;
}

static int path_service_link(ioserv_context_t *ctp,
                             io_link_t *msg,
                             void *handle,
                             struct _ioserv_link_extra *extra)
{
    path_object_t *obp;

    /* For FTYPE_NAME object, we let non-root users do the attach, but only if
     * an attached object doesn't already exist in that space.
     */
    if (extra && extra->file_type == _FTYPE_NAME) {
        if (path_object_exists(msg->connect.path, OBJECT_NAME)) {
            return EEXIST;
        }
    } else if (!proc_isaccess(0, &ctp->info)) {
        return EPERM;
    }

    if (!extra)
        return EINVAL;

    int attach_err;
    if (!(obp = path_object_attach(ctp->info.pid, 0, msg->connect.path,
                                   OBJECT_SERVER, extra->flags, extra,
                                   &attach_err))) {
        return attach_err;
    }

    if (!(extra->flags & NAMESPACE_FLAG_STICKY)) {
        if (ioserv_open_bind(ctp, obp, &link_io_funcs) == -1) {
            path_object_detach(obp);
            path_object_done(obp);
            return errno;
        }
    }
    path_object_done(obp);

    /* Namespace change notification. */
    proc_event_trigger(PROC_EVENT_PATHSPACE);

    return EOK;
}

int symlink_create(path_object_t *obp, void *data)
{
    char *path = data;
    size_t len;

    len = strlen(path) + 1;
    if (!obp) {
        return (int) (offsetof(struct symlink_object, name) + len);
    }

    obp->symlink.len = (int) len;
    memcpy(obp->symlink.name, path, len);
    return EOK;
}

static int path_sys_symlink(ioserv_context_t *ctp,
                            io_link_t *msg,
                            void *handle,
                            char *path)
{
    path_object_t *obp;

    if (!proc_isaccess(0, &ctp->info)) {
        return EPERM;
    }

    if (path_object_exists(msg->connect.path, OBJECT_PROC_SYMLINK)) {
        return EBUSY;
    }

    int attach_err;
    obp = path_object_attach(ctp->info.pid, 0, msg->connect.path,
                             OBJECT_PROC_SYMLINK, 0, path, &attach_err);
    if (!obp) {
        /* Preserve real resource errors (quota, OOM) so the client sees why it
         * failed; only an actual node-creation/name failure maps to
         * ENAMETOOLONG.
         */
        return (attach_err == EDQUOT || attach_err == ENOMEM) ? attach_err
                                                              : ENAMETOOLONG;
    }
    path_object_done(obp);
    return EOK;
}

int path_link(ioserv_context_t *ctp,
              io_link_t *msg,
              void *handle,
              io_link_extra_t *extra)
{
    size_t path_len = msg->connect.path_len;
    size_t extra_len = msg->connect.extra_len;
    size_t max = ctp->info.msglen;
    size_t base = offsetof(struct _io_connect, path);

    /* Bound user-controlled lengths against the actually-received message
     * length, not msg_max_size (the receive *buffer* size). Otherwise a sender
     * that ships a short message with inflated path_len/extra_len would pass
     * the check and we would consume stale bytes left over from a prior message
     * in the buffer. Subtractive checks avoid any pre-comparison wrap.
     */
    if (max < base || path_len == 0 || path_len > max - base)
        return EINVAL;

    /* The extra payload starts at the next RTMUX_MSG_ALIGN boundary past the
     * path, not immediately at base+path_len. Only enforce the alignment hole
     * when the sender actually supplies an extra payload; a zero-length
     * invalidate doesn't require any pad bytes to be present.
     */
    if (extra_len > 0) {
        size_t extra_off = lib_align_up_size(base + path_len, RTMUX_MSG_ALIGN);

        if (extra_off > max || extra_len > max - extra_off)
            return EINVAL;
    }

    /* path_node_lookup walks msg->connect.path as a C string. Senders vary on
     * whether path_len includes the trailing NUL; some declare strlen(), some
     * strlen()+1. Search the inclusive window [path, path+path_len], capped so
     * the +1 byte stays in the received region when path_len == max - base
     * exactly.
     */
    if (!memchr(msg->connect.path, '\0',
                (path_len < max - base) ? path_len + 1 : path_len))
        return EINVAL;

    switch (msg->connect.extra_type) {
    case _IO_CONNECT_EXTRA_SERVICE_LINK:
        /* someone is making a new device */
        if (extra_len < sizeof(struct _ioserv_link_extra))
            return EINVAL;
        return path_service_link(ctp, msg, handle, &extra->service);

    case _IO_CONNECT_EXTRA_PROC_SYMLINK:
        /* someone is trying to make a symlink */
        if (extra_len == 0 || extra->path[extra_len - 1] != '\0')
            return EINVAL;
        return path_sys_symlink(ctp, msg, handle, extra->path);

    default:
        break;
    }

    return ENOSYS;
}


static char *add_piece(char *p, char *endp, char *src, unsigned len)
{
    int left;
    char *r = p + len;

    left = (int) (endp - p);
    if (left < 0)
        left = 0;
    if ((unsigned) left < len)
        len = (unsigned) left;
    memcpy(p, src, len);
    return r;
}


/* get the fullpath of a node; return len (include the last NULL character) */

int path_node_fullpath(node_t *node, char *path, int pathmax)
{
    int len;
    char *p;
    char *endp;
    node_t *n, *prev, *parent;

    len = 0;
    p = path + len;
    endp = path + pathmax;

    if ((len > 0) && (p <= endp) && (p[-1] == '/')) {
        --p;
    }

    prev = 0;
    do {
        n = node;
        for (;;) {
            parent = n->parent;
            if (parent == n)
                break;
            if (parent == node)
                break;
            if (parent == prev)
                break;
            n = parent;
        }
        if (n->len) {
            p = add_piece(p, endp, "/", 1u);
            p = add_piece(p, endp, n->name, (unsigned) n->len);
        }
        prev = n;
    } while (n != node);
    len = (int) (p - path) + 1; /* len includes the trailing '\0' */
    if (pathmax > 0) {
        /* append the '\0' (if there's space) */
        path[(min(len, pathmax)) - 1] = '\0';
    }

    return len;
}
