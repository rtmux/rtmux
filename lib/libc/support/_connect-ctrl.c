/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Core connection control logic — manages symlink resolution, server discovery,
 * and message routing.
 */

#include <alloca.h>
#include <align.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/node.h>
#include <sys/path-registry.h>
#include <sys/pathmsg.h>
#include <sys/procmsg.h>
#include <sys/rtmux-core.h>
#include <unistd.h>
#include "connect.h"

#ifndef SYMLOOP_MAX
#define SYMLOOP_MAX _POSIX_SYMLOOP_MAX
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define CTRL_ALLOC(flag, len) \
    (((flag) & FLAG_STACK_ALLOC) ? alloca((len)) : malloc((len)))
#define CTRL_FREE(flag, data) \
    (((flag) & FLAG_STACK_ALLOC) ? ((void) 0) : free((data)))

static const struct _io_connect_entry _connect_proc_entry = {
    RTMUX_LOCAL_NODE,
    PATH_PID,
    PATH_CHID,
    PATH_HANDLE,
};

static int _connect_io(struct _connect_ctrl const *ctrl,
                       int fd,
                       const char *prefix,
                       unsigned prefix_len,
                       const char *path,
                       void *buffer,
                       const struct _io_connect_entry *entry)
{
    struct _io_connect *msg = ctrl->msg;
    struct _io_connect_link_reply *reply = &ctrl->reply->link;
    iov_t siov[5], riov[2];
    unsigned sparts;
    static const char padding[RTMUX_MSG_ALIGN - 1];
    int ret, tryagain;
    struct _io_connect_entry *ent = ctrl->entry;

    SETIOV(siov + 0, msg, offsetof(struct _io_connect, path));
    if (prefix_len) {
        SETIOV(siov + 1, prefix, prefix_len);
        sparts = 2;
    } else {
        sparts = 1;
    }
    SETIOV(siov + sparts, path, msg->path_len);
    sparts++;
    msg->path_len += (uint16_t) prefix_len;
    if (msg->extra_len) {
        size_t len = offsetof(struct _io_connect, path) + msg->path_len;
        size_t align = lib_align_up_size(len, RTMUX_MSG_ALIGN) - len;

        if (align) {
            SETIOV(siov + sparts, padding, align);
            sparts++;
        }
        SETIOV(siov + sparts, ctrl->extra, msg->extra_len);
        sparts++;
    }
    SETIOV(riov + 0, reply, sizeof *reply);
    SETIOV(riov + 1, buffer, msg->reply_max - sizeof *reply);

    if (!(ctrl->flags & FLAG_TEST_ENTRY)) {
        ret = 1;
    } else if (ent->pid == entry->pid && ent->chid == entry->chid &&
               RTMUX_NODE_CMP(ent->nd, entry->nd) == 0 &&
               ((ctrl->flags & FLAG_TEST_NPC_ONLY) ||
                ent->handle == entry->handle)) {
        ret = 1;
    } else {
        ret = 0;
    }

    if (ret && (!(ctrl->flags & FLAG_TEST_ND_ONLY) ||
                RTMUX_NODE_CMP(ctrl->nd, entry->nd) == 0)) {
        ret = ctrl->send(fd, siov, (int) sparts, riov, 2);
        if (ret != -1 || (ctrl->flags & FLAG_TEST_ENTRY))
            return ret;
    }

    if (ctrl->flags & FLAG_NO_RETRY) {
        tryagain = 0;
    } else {
        tryagain = (ret != -1) || (errno == ENOSYS);
        switch (msg->subtype) {
        case _IO_CONNECT_OPEN:
        case _IO_CONNECT_COMBINE:
        case _IO_CONNECT_COMBINE_CLOSE:
            break;
        case _IO_CONNECT_LINK:
            tryagain |= (errno == ENXIO) ? 1 : 0;
            break;
        default:
            break;
        }
    }

    if (tryagain) {
        uint32_t saved_ioflag;
        uint16_t saved_type, saved_elen;
        uint8_t saved_eflag, saved_etype;
        int saved_errno;

        saved_ioflag = msg->ioflag;
        saved_type = msg->subtype;
        saved_eflag = msg->eflag;
        saved_etype = msg->extra_type;
        saved_elen = msg->extra_len;

        msg->ioflag = 0;
        msg->extra_len = 0;
        msg->extra_type = _IO_CONNECT_EXTRA_NONE;
        msg->subtype = _IO_CONNECT_COMBINE_CLOSE;

        saved_errno = errno;
        ret = ctrl->send(fd, siov, (int) sparts, riov, 2);
        if (ret == -1) {
            errno = (ctrl->flags & FLAG_TEST_ENTRY) ? ENXIO : saved_errno;
        } else if (((unsigned) ret &
                    (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_TYPE_MASK)) !=
                   (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_LINK)) {
            if (ctrl->flags & FLAG_ERR_COLLISION)
                errno = EEXIST;
            else
                errno = (ctrl->flags & FLAG_TEST_ENTRY) ? ENXIO : saved_errno;
            ret = -1;
        }

        msg->subtype = saved_type;
        msg->eflag = saved_eflag;
        msg->extra_type = saved_etype;
        msg->extra_len = saved_elen;
        msg->ioflag = saved_ioflag;
    } else {
        errno = (errno == EOK) ? ENOENT : errno;
        ret = -1;
    }
    return ret;
}

static int _connect_request(struct _connect_ctrl *ctrl,
                            const char *prefix,
                            unsigned prefix_len,
                            const char *path,
                            unsigned path_skip,
                            const struct _io_connect_entry *entry,
                            void *buffer,
                            int chrootlen)
{
    struct _io_connect *msg = ctrl->msg;
    struct _io_connect_link_reply *reply = &ctrl->reply->link;
    int fd;
    int ftype;

    fd = connect_attach(entry->nd, entry->pid, entry->chid,
                        (unsigned) ctrl->base, RTMUX_FLG_COF_CLOEXEC);
    if (fd == -1)
        return -1;

    msg->handle = entry->handle;
    msg->eflag = reply->eflag;

    ctrl->status = _connect_io(ctrl, fd, prefix, prefix_len, path + path_skip,
                               buffer, entry);
    if (ctrl->status == -1) {
        connect_detach_r((uint64_t) fd);
        return -1;
    }

    ftype = (int) msg->file_type;
    if ((unsigned) ctrl->status & _IO_CONNECT_RET_FLAG) {
        if ((ftype == _FTYPE_ANY) &&
            (ctrl->status & _IO_CONNECT_RET_TYPE_MASK) ==
                _IO_CONNECT_RET_FTYPE) {
            ftype = (int) (msg->file_type = ctrl->reply->ftype.file_type);
            ctrl->nd = entry->nd;
            ctrl->flags |= FLAG_TEST_ND_ONLY;
        }
    }

    /* Link returned: traverse namespace entries. */
    if (((unsigned) ctrl->status &
         (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_TYPE_MASK)) ==
        (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_LINK)) {
        connect_detach_r((uint64_t) fd);

        if (!(ctrl->status & _IO_CONNECT_RET_CHROOT))
            reply->chroot_len = 0;

        if (entry->pid == PATH_PID && entry->chid == PATH_CHID && !prefix) {
            if (ctrl->status & _IO_CONNECT_RET_UMASK)
                msg->mode &= ~(reply->umask & ~(uint32_t) S_IFMT);
            if (ctrl->status & _IO_CONNECT_RET_NOCTTY)
                ctrl->flags |= FLAG_NOCTTY;
        }

        if (msg->entry_max == 0) {
            errno = ELOOP;
            return -1;
        }
        if (sizeof(*reply) + reply->nentries * sizeof(*entry) +
                reply->path_len >=
            msg->reply_max) {
            errno = ENAMETOOLONG;
            return -1;
        }

        if (reply->nentries) {
            char *save;
            const struct _io_connect_entry *save_entry;
            void *buff;
            unsigned path_len;
            uint16_t cnt, num;

            path_len = reply->path_len;
            cnt = num = reply->nentries;
            chrootlen += reply->chroot_len;
            ctrl->chroot_len = reply->chroot_len;

            buff = (struct _io_connect_entry *) buffer + num;
            if (path_skip > 1 && path[path_skip - 1] == '/')
                path_skip--;

            save = CTRL_ALLOC(ctrl->flags, path_len + path_skip);
            if (!save) {
                errno = ENOMEM;
                return -1;
            }
            memcpy(save, path, path_skip);
            memcpy(save + path_skip, buff, path_len);

            if (ctrl->flags & FLAG_REUSE_BUFFER) {
                msg->reply_max -= (uint16_t) (num * sizeof *entry);
                entry = buffer;
                save_entry = NULL;
            } else {
                save_entry = entry =
                    CTRL_ALLOC(ctrl->flags, num * sizeof(*entry));
                if (!save_entry) {
                    CTRL_FREE(ctrl->flags, save);
                    errno = ENOMEM;
                    return -1;
                }
                memcpy((void *) entry, buffer, num * sizeof(*entry));
                buff = buffer;
            }

            msg->entry_max--;
            for (; cnt > 0; entry++, cnt--) {
                if (msg->file_type == (unsigned) _FTYPE_ALL)
                    continue;

                if ((msg->file_type == _FTYPE_ANY) ||
                    (entry->file_type == (unsigned) _FTYPE_ALL) ||
                    (msg->file_type == entry->file_type)) {
                    int pad_len, save_errno, marker;

                    save_errno = errno;
                    msg->path_len = (uint16_t) (path_len - entry->prefix_len);

                    pad_len = entry->prefix_len;
                    if (pad_len > 0)
                        pad_len -=
                            (save[path_skip + (unsigned) pad_len - 1] == '/');
                    if (ctrl->flags & FLAG_NO_PREFIX ||
                        (ctrl->pathsize - ctrl->pathlen) < pad_len)
                        pad_len = 0;

                    ctrl->pathlen += pad_len;
                    marker = ctrl->pathlen;

                    fd = _connect_request(ctrl, 0, 0, save,
                                          entry->prefix_len + path_skip, entry,
                                          buff, chrootlen);

                    if (pad_len && marker == ctrl->pathlen) {
                        ctrl->pathlen -= pad_len;
                        if (fd != -1 && ctrl->path && ctrl->pathsize)
                            memcpy(&ctrl->path[ctrl->pathlen], &save[path_skip],
                                   (size_t) pad_len);
                    }

                    if (fd != -1 && ctrl->fds)
                        continue;

                    if (fd == -1 &&
                        msg->file_type != (unsigned) _FTYPE_MATCHED) {
                        switch (errno) {
                        default:
                            if (errno < ENETDOWN || errno > EHOSTUNREACH)
                                break;
                            /* FALL THROUGH */
                        case EROFS:
                        case ENOSYS:
                        case ENOENT:
                        case ENXIO:
                        case ESRCH:
                        case EIO:
                            if (save_errno != EOK && save_errno != ENOSYS)
                                errno = save_errno;
                            continue;
                        }
                    }
                    break;
                }
            }
            msg->entry_max++;

            if (ctrl->flags & FLAG_REUSE_BUFFER)
                msg->reply_max += (uint16_t) (num * sizeof *entry);
            else
                CTRL_FREE(ctrl->flags, (void *) save_entry);
            CTRL_FREE(ctrl->flags, save);

        } else {
            /* Symlink — new path to traverse */
            char *savedpath, *buff = buffer;
            int hold_pathlen;
            uint16_t preoffset, postoffset;

            msg->path_len = reply->path_len;
            hold_pathlen = ctrl->pathlen;
            ctrl->pathlen = 0;

            if (ctrl->path && (ctrl->flags & FLAG_NO_SYM)) {
                savedpath = ctrl->path;
                ctrl->path = NULL;
            } else {
                savedpath = NULL;
            }

            if (*buff == '/') {
                preoffset = (uint16_t) path_skip;
                postoffset = 0;
            } else if (chrootlen < (int) path_skip) {
                preoffset = (uint16_t) chrootlen;
                postoffset = 0;
            } else {
                preoffset = (uint16_t) path_skip;
                postoffset = (uint16_t) ((unsigned) chrootlen - path_skip);
            }

            fd = _connect_request(
                ctrl, path + preoffset, (unsigned) (path_skip - preoffset),
                buff + postoffset, 0, &_connect_proc_entry, buff, 0);

            if (savedpath && fd != -1) {
                ctrl->path = savedpath;
                ctrl->pathlen = hold_pathlen;
                goto copy_path;
            }
        }

    } else if (((unsigned) ctrl->status &
                (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_TYPE_MASK)) ==
               (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_FTYPE)) {
        connect_detach_r((uint64_t) fd);
        if (ctrl->reply->ftype.file_type == (unsigned) _FTYPE_MATCHED)
            msg->file_type = ctrl->reply->ftype.file_type;
        errno = ctrl->reply->ftype.status;
        errno = (errno == EOK) ? ENOSYS : errno;
        fd = -1;

    } else if (((unsigned) ctrl->status &
                (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_MSG)) ==
               (_IO_CONNECT_RET_FLAG | _IO_CONNECT_RET_MSG)) {
        msg->file_type = (unsigned) _FTYPE_MATCHED;
        connect_detach_r((uint64_t) fd);
        if ((msg->subtype == _IO_CONNECT_OPEN && msg->extra_len == 0) ||
            ((msg->subtype == _IO_CONNECT_COMBINE ||
              msg->subtype == _IO_CONNECT_COMBINE_CLOSE) &&
             reply->path_len)) {
            struct _server_info *info = buffer;

            fd = connect_attach(info->nd, info->pid, info->chid,
                                (unsigned) ctrl->base, RTMUX_FLG_COF_CLOEXEC);
            if (fd != -1) {
                if (reply->path_len) {
                    struct _io_combine *next = (void *) (info + 1);
                    iov_t siov2[2], riov2[2];
                    int sparts2 = 1;

                    SETIOV(siov2 + 0, next, reply->path_len);
                    if (msg->extra_len) {
                        next->combine_len |= _IO_COMBINE_FLAG;
                        SETIOV(siov2 + 1, ctrl->extra, msg->extra_len);
                        sparts2++;
                    }
                    SETIOV(riov2 + 0, reply, sizeof *reply);
                    SETIOV(riov2 + 1, buffer, msg->reply_max - sizeof *reply);
                    ctrl->status = ctrl->send(fd, siov2, sparts2, riov2, 2);
                    if (ctrl->status == -1 ||
                        msg->subtype == _IO_CONNECT_COMBINE_CLOSE) {
                        io_close_t msg2;
                        int save_errno;

                        msg2.i.type = _IO_CLOSE;
                        msg2.i.combine_len = sizeof msg2.i;
                        SETIOV(siov2 + 0, &msg2.i, sizeof msg2.i);
                        save_errno = errno;
                        (void) ctrl->send(fd, siov2, 1, 0, 0);
                        errno = save_errno;
                        if (ctrl->status == -1) {
                            connect_detach_r((uint64_t) fd);
                            fd = -1;
                        }
                    }
                }
            }
        } else {
            fd = -1;
            errno = ENOTSUP;
        }

    } else {
        /* Direct match — collect fd and response */
        if (ctrl->fds) {
            if (ctrl->fds_index >= ctrl->fds_len - 1) {
                int *tmp;

                if (!(ctrl->flags & FLAG_MALLOC_FDS) ||
                    !(tmp = realloc(ctrl->fds,
                                    (ctrl->fds_len + FD_BUF_INCREMENT) *
                                        sizeof(*ctrl->fds)))) {
                    errno = ENOMEM;
                    return -1;
                }
                ctrl->fds = tmp;
                ctrl->fds_len += FD_BUF_INCREMENT;
            }
            ctrl->fds[ctrl->fds_index++] = fd;
        }

        if (ctrl->response_len) {
            int b;

            b = (int) min(sizeof(*ctrl->reply), (size_t) ctrl->response_len);
            memmove(ctrl->response, ctrl->reply, (size_t) b);
            if (ctrl->response_len > b)
                memmove((char *) ctrl->response + b, buffer,
                        (size_t) (ctrl->response_len - b));
            ctrl->response_len = 0;
        }

        if (ctrl->flags & FLAG_SET_ENTRY) {
            *ctrl->entry = *entry;
            ctrl->entry->prefix_len = (uint16_t) (prefix_len + path_skip);
        }

    copy_path:
        if (ctrl->path) {
            char *insert;
            int cplen;

            cplen = (int) strlen(path + path_skip) + 1;

            if (cplen != 1 && !(ctrl->flags & FLAG_NO_PREFIX)) {
                insert = &ctrl->path[ctrl->pathlen];
                cplen++;
            } else {
                insert = NULL;
            }

            if ((ctrl->pathsize - ctrl->pathlen) < cplen) {
                ctrl->path[0] = '\0';
                ctrl->pathsize = 0;
            } else {
                if (insert) {
                    *insert++ = '/';
                    cplen--;
                } else {
                    insert = &ctrl->path[ctrl->pathlen];
                }
                memcpy(insert, path + path_skip, (size_t) cplen);
            }
        }
    }

    return (fd == -1 && ctrl->fds && ctrl->fds_index) ? ctrl->fds[0] : fd;
}

int _connect_ctrl(struct _connect_ctrl *ctrl,
                  const char *path,
                  unsigned response_len,
                  void *response)
{
    struct _io_connect *msg = ctrl->msg;
    int fd, save_errno, save_ftype, save_ioflag;
#define MIN_FILLER_SIZE \
    (sizeof(struct _io_connect_entry) * 3 * SYMLOOP_MAX + PATH_MAX + 1)
    struct {
        struct _io_connect_link_reply reply;
        char filler[MIN_FILLER_SIZE];
    } *buffer;
    int oflag, freebuffer;

    ctrl->chroot_len = 0;

    if (*path == '\0') {
        errno = ENOENT;
        return -1;
    }

    if (msg->file_type == (unsigned) _FTYPE_MATCHED) {
        msg->file_type = _FTYPE_ANY;
        if (ctrl->entry)
            ctrl->flags |= FLAG_TEST_ENTRY;
    }
    save_ftype = (int) msg->file_type;
    save_ioflag = (int) msg->ioflag;

onemoretime:
    msg->file_type = (unsigned) save_ftype;
    msg->ioflag = (uint32_t) save_ioflag;

    response_len = response ? response_len : 0;
    ctrl->response_len = (int) response_len;
    ctrl->response = response;

    ctrl->flags |= FLAG_STACK_ALLOC;
    ctrl->flags |= FLAG_REUSE_BUFFER;

    if (ctrl->fds || (ctrl->flags & FLAG_MALLOC_FDS) ||
        response_len < sizeof(*buffer)) {
        freebuffer = 1;
        buffer = CTRL_ALLOC(ctrl->flags, max(response_len, sizeof(*buffer)));
        if (!buffer) {
            errno = ENOMEM;
            return -1;
        }
    } else {
        freebuffer = 0;
        buffer = response;
    }
    ctrl->reply = (void *) buffer;
    msg->reply_max = (uint16_t) max(response_len, sizeof(*buffer));

    ctrl->reply->link.eflag = 0;

    msg->type = _IO_CONNECT;
    msg->entry_max = SYMLOOP_MAX;
    msg->path_len = (uint16_t) (strlen(path) + 1);
    oflag = (int) msg->ioflag;
    msg->ioflag = (uint32_t) ((oflag & ~(O_ACCMODE | O_CLOEXEC)) |
                              ((oflag + 1) & msg->access & O_ACCMODE));

    save_errno = errno;
    errno = EOK;

    fd = _connect_request(ctrl, 0, 0, path, 0,
                          (ctrl->entry && (ctrl->flags & FLAG_NO_RETRY))
                              ? ctrl->entry
                              : &_connect_proc_entry,
                          buffer->filler, 0);
    if (fd != -1) {
        errno = save_errno;

        if (!((unsigned) ctrl->base & RTMUX_SIDE_CHANNEL) &&
            !(oflag & O_CLOEXEC))
            connect_flags_r(0, (uint64_t) fd, FD_CLOEXEC, 0);
#ifndef LDSO_MINIMAL_CONNECT
        if ((ctrl->flags & FLAG_NOCTTY) && !(oflag & O_NOCTTY) && isatty(fd)) {
            proc_session_t session_msg;

            session_msg.i.type = _PROC_SESSION;
            session_msg.i.subtype = 0;
            session_msg.i.sid = getpid();
            session_msg.i.id = fd;
            session_msg.i.event = _PROC_SESSION_TCSETSID;
            (void) msg_sendnc(ROOTTASK_COID, &session_msg.i,
                              sizeof session_msg.i, 0, 0);
        }
#endif
        if (ctrl->path && ctrl->pathsize > 1 && ctrl->path[0] == '\0') {
            ctrl->path[0] = '/';
            ctrl->path[1] = '\0';
        }
    } else if (save_ftype == _FTYPE_ANY &&
               (errno == ENOSYS || errno == ENOENT) &&
               (int) msg->file_type != save_ftype &&
               msg->file_type != (unsigned) _FTYPE_MATCHED) {
        if (freebuffer)
            CTRL_FREE(ctrl->flags, buffer);
        save_ftype = (int) msg->file_type;
        goto onemoretime;
    }

    if (freebuffer)
        CTRL_FREE(ctrl->flags, buffer);
    return fd;
}
