/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <fcntl.h>
#include <dirent.h>
#include <devctl/common.h>
#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include <sys/path-registry.h>

struct open_entry {
    struct open_entry *next;
    struct open_entry **prev;
    node_t *node;
    uint64_t entry_generation;
    int rcvid;
    int check_generation;
    int deferred; /* parked behind another op on the node (IOSERV_NOREPLY) */
};

static soul_t open_souls INITSOUL(-1, struct open_entry, 2, 8, ~0);
static struct open_entry *pending_opens;


struct node_ocb {
    struct node_entry *node;
    off_t offset;
};

int devmem_check_perm(ioserv_context_t *ctp,
                      mode_t mode,
                      uid_t uid,
                      gid_t gid,
                      mode_t check);

static int node_read(ioserv_context_t *ctp, io_read_t *msg, void *vocb)
{
    struct node_entry *node;
    unsigned size;
    struct dirent *d;
    struct node_ocb *ocb = vocb;
    unsigned nbytes;
    off_t skip;

    /* Our NAME objects always return 0 bytes like a dev null */
    if ((node = ocb->node) && node->object &&
        node->object->hdr.type == OBJECT_NAME) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return IOSERV_PTR(ctp, NULL, 0);
    }

    /* Lock access to the node so that it's children don't get deleted as we're
     * walking the list.
     */
    path_node_access(ocb->node);

    /* Need to be able to handle a seek here too ... */
    if (!(node = ocb->node->child)) {
        path_node_complete(ocb->node);
        return EBADF;
    }
    skip = ocb->offset;

    size = min_uint((unsigned) msg->i.nbytes, ctp->msg_max_size);
    d = (struct dirent *) (void *) msg;
    nbytes = offsetof(struct dirent, d_name) + 1;

    /* If we are in a valid proc maintained directory, we want to make sure that
     * we don't actually display any of the hidden entries used for cwd
     * operations. -- also in the open code
     */
    while (nbytes < size && node && node != (struct node_entry *) -1) {
        if (node->child_objects || node->object) {
            if (skip <= 0) {
                size_t nmlen = strlen(node->name);
                if ((nbytes + nmlen) >= size) {
                    break;
                }
                memset(d, 0, sizeof(struct dirent));
                d->d_namelen = (uint16_t) nmlen;
                d->d_reclen = (uint16_t) ((offsetof(struct dirent, d_name) +
                                           nmlen + 1 + 7) &
                                          ~(size_t) 7);
                memcpy(d->d_name, node->name,
                       nmlen + 1); /* '\0' guaranteed to be copied */
                d->d_ino = (ino_t) node;
                nbytes += d->d_reclen;
                d = (struct dirent *) ((char *) d + d->d_reclen);
                ocb->offset++;
            }
            skip--;
        }
        node = node->sibling;
    }

    path_node_complete(ocb->node);

    nbytes -= (int) (offsetof(struct dirent, d_name) + 1);

    _IO_SET_READ_NBYTES(ctp, nbytes);
    return IOSERV_PTR(ctp, msg, nbytes);
}

static int node_lseek(ioserv_context_t *ctp, io_lseek_t *msg, void *vocb)
{
    struct node_ocb *ocb = vocb;

    /* The only seek offset we support is to reset the file to 0 */
    if (msg->i.whence != SEEK_SET || msg->i.offset != 0) {
        return ENOSYS;
    }

    ocb->offset = 0;
    if (msg->i.combine_len & _IO_COMBINE_FLAG) {
        return EOK;
    }

    msg->o = (uint64_t) ocb->offset;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}


static int node_close_ocb(ioserv_context_t *ctp, void *reserved, void *vocb)
{
    struct node_ocb *ocb = vocb;

    path_node_detach(ocb->node);
    _sfree(ocb, sizeof *ocb);
    return EOK;
}

static int node_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *vocb)
{
    union {
        int oflag;
        int mountflag;
    } *data = (void *) (msg + 1);
    unsigned nbytes = 0;

    switch ((unsigned) msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        data->oflag = O_RDONLY;
        nbytes = sizeof data->oflag;
        break;

    case DEVCTL_SET_FLAGS:
        break;

    case DEVCTL_GET_MOUNT_FLAGS: {
        data->mountflag = 0;
        nbytes = sizeof data->mountflag;
        break;
    }

    default:
        return IOSERV_DEFAULT;
    }

    if (nbytes) {
        msg->o.ret_val = 0;
        return IOSERV_PTR(ctp, &msg->o, sizeof msg->o + nbytes);
    }
    return EOK;
}

static int node_stat(ioserv_context_t *ctp, io_stat_t *msg, void *vocb)
{
    path_object_t *o;
    struct node_ocb *ocb = vocb;

    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = INODE_XOR(ocb->node);
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = 0;

    o = ocb->node->object;
    if ((!o || o->hdr.type != OBJECT_PROC_SYMLINK) && ocb->node->child) {
        msg->o.st_nlink = 2;
        msg->o.st_mode = S_IFDIR | 0555;
        return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
    }

    if (o) {
        switch (o->hdr.type) {
        case OBJECT_PROC_SYMLINK:
            msg->o.st_size = o->symlink.len - 1;
            msg->o.st_nlink = 1;
            msg->o.st_mode = S_IFLNK | 0777;
            return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
        case OBJECT_NAME:
            msg->o.st_nlink = 1;
            msg->o.st_mode = S_IFNAM | 0666;
            return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
        default:
            break;
        }
    }

    return ENOSYS;
}

static const ioserv_ops_t node_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = node_read,
    .close_ocb = node_close_ocb,
    .stat = node_stat,
    .devctl = node_devctl,
    .lseek = node_lseek,
};


static int complete_open(ioserv_context_t *ctp, struct open_entry *oep, int ret)
{
    node_t *nop;
    struct open_entry *chk;
    struct open_entry *next;
    struct open_entry *todo;
    int reserve_thread_fail = 0;

    todo = NULL;
    nop = oep->node;
    path_node_access(nop);
    LINK2_REM(oep);
    proc_object_free(&open_souls, oep);
    if (nop->flags & NODE_FLAG_DEFERRED) {
        /* Find all the deferred operations for this node. */
        chk = pending_opens;
        while (chk) {
            next = chk->next;

            /* Restart only the operations that were parked behind another op on
             * this node. An entry that is not deferred belongs to an operation
             * still running on another roottask thread: it owns its entry and
             * will complete it itself. Stealing it here replayed the request a
             * second time and freed the entry under its owner, whose later
             * LINK2_REM then corrupted pending_opens and dropped the parked
             * operations, so their clients waited in MsgSend forever.
             */
            if (chk->node == nop && chk->deferred) {
                LINK2_REM(chk);
                chk->next = todo;
                todo = chk;
            }
            chk = next;
        }
        nop->flags &= (uint16_t) ~NODE_FLAG_DEFERRED;
    }
    path_node_complete(nop);
    path_node_detach_flags(nop, NODE_FLAG_UNLINKING);
    if (todo) {
        if (proc_thread_pool_reserve() != 0) {
            /* ***** NOTE ***** the thrad pool reserve operation failed this
             * means we are running low on memory but we cannot leave the
             * pending opens hanging so we continue on here this reservation is
             * mainly a performance improvement not a necessity. Without this
             * reservation, other unrelated operations, would have to wait for
             * this to complete if there were no other threads around.
             */
            reserve_thread_fail = 1;
        }

        /* We're going to be reusing ctp below to restart the deferred
         * operations, so we need to do the reply for this op first to free up
         * the message buffer.
         */
        switch ((unsigned) ret) {
        case (unsigned) IOSERV_NOREPLY:
            break;
        case (unsigned) IOSERV_DEFAULT:
            ret = ENOSYS;
            /* Fall through */
        default:
            if (ret <= 0) {
                (void) msg_replyv(ctp->rcvid, ctp->status, ctp->iov, -ret);
            } else {
                msg_error(ctp->rcvid, ret);
            }
            break;
        }
        ret = IOSERV_NOREPLY;
        do {
            next = todo->next;
            /* Restart the deferred operation on the node */
            ioserv_msg_again(ctp, todo->rcvid);
            proc_object_free(&open_souls, todo);
            todo = next;
        } while (todo);

        /* if the original thread pool reservation did not fail release the
         * reservation here
         */
        if (!reserve_thread_fail) {
            proc_thread_pool_reserve_done();
        }
    }
    return ret;
}

static int open_entry_stale(struct open_entry *oep)
{
    int stale;

    if (!oep->check_generation)
        return 0;

    path_node_access(oep->node);
    stale = oep->node->entry_generation != oep->entry_generation;
    path_node_complete(oep->node);
    return stale;
}


static int reply_redirect(ioserv_context_t *ctp,
                          io_open_t *msg,
                          path_object_t *obp)
{
    unsigned eflag;
    struct _io_connect_link_reply *linkp = (void *) msg;
    struct _server_info *info = (void *) (linkp + 1);

    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_MSG);
    eflag = msg->connect.eflag;

    memset(linkp, 0x00, sizeof *linkp);
    memset(info, 0x00, sizeof *info);

    linkp->eflag = (uint8_t) eflag;
    linkp->path_len =
        0; /* This is key, no message stream comes after the info! */

    /* All we need for the new fd is the nd/pid/chid trio */
    info->nd = obp->server.nd;
    info->pid = obp->server.pid;
    info->chid = obp->server.chid;

    return IOSERV_PTR(ctp, msg, sizeof *linkp + sizeof *info);
}


enum { PATH_SERVICE_OPEN, PATH_SERVICE_CREAT, PATH_SERVICE_LS };
static int service_error(io_open_t *msg, int action)
{
    if (action != PATH_SERVICE_OPEN) {
        switch (msg->connect.file_type) {
        case _FTYPE_ANY:
            break;
        case _FTYPE_FILE:
        case _FTYPE_SHMEM:
        case _FTYPE_NAME:
            break;
        case _FTYPE_SOCKET:
            if (action != PATH_SERVICE_CREAT)
                return (ENOSYS);
            break;
        default:
            return (ENOSYS);
        }
    } else if (msg->connect.file_type != _FTYPE_ANY) {
        return (ENOSYS);
    }
    return (ENOENT);
}

int path_open(ioserv_context_t *ctp, io_open_t *msg, void *handle, void *extra)
{
    node_t *nop;
    struct node_ocb *ocb;
    const char *result;
    unsigned long long namespace_version;
    int retry;

    if (msg->connect.path_len + sizeof(*msg) >= ctp->msg_max_size) {
        return ENAMETOOLONG;
    }
    switch (msg->connect.subtype) {
    case _IO_CONNECT_READLINK:
        namespace_stats_roundtrip(NAMESPACE_RT_READLINK);
        break;
    case _IO_CONNECT_UNLINK:
        namespace_stats_roundtrip(NAMESPACE_RT_UNLINK);
        break;
    default:
        namespace_stats_roundtrip(NAMESPACE_RT_OPEN);
        break;
    }

    retry = 0;
again:
    namespace_version = namespace_version_read();
    nop = path_node_lookup(
        0, msg->connect.path,

        /* Break out if we hit non-server object, attach to the node to prevent
         * it's deletion and avoid returning empty nodes (for links, we could
         * probably remove this)
         */
        PATH_LOOKUP_NOSERVER | PATH_LOOKUP_ATTACH | PATH_LOOKUP_NOEMPTIES |
            PATH_LOOKUP_STATS |

            /* When looking up links, ignore the autocreated entries, this isn't
             * perfect, but it avoids the obvious case when we have overlaid two
             * links. It would be better to be able to specify one specific node
             * to lookup as an alternative. Add to the TODO list.
             */
            ((ctp->id == link_root_id) ? PATH_LOOKUP_NOAUTO : 0),
        &result);

    if (nop) {
        path_object_t *o;
        struct open_entry *oep;
        struct open_entry *chk;

        if (namespace_version_read() != namespace_version) {
            path_node_detach(nop);
            if (retry++ == 0)
                goto again;
            return ESTALE;
        }

        oep = proc_object_alloc(&open_souls);
        if (!oep) {
            path_node_detach(nop);
            return EAGAIN;
        }
        path_node_access(nop);
        o = nop->object;
        LINK2_BEG(pending_opens, oep, struct open_entry);
        oep->node = nop;
        oep->entry_generation = nop->entry_generation;
        oep->rcvid = ctp->rcvid;
        oep->deferred = 0;
        oep->check_generation = (msg->connect.subtype != _IO_CONNECT_UNLINK);
        if (msg->connect.subtype == _IO_CONNECT_UNLINK) {
            nop->flags |= NODE_FLAG_UNLINKING;
        }
        if (nop->flags & NODE_FLAG_UNLINKING) {
            for (chk = pending_opens->next; chk; chk = chk->next) {
                if (chk->node == nop) {
                    /* Somebody (maybe us) is attempting to unlink this node
                     * while somebody else is trying to do something with it -
                     * defer this operation until things are settled.
                     */
                    nop->flags |= NODE_FLAG_DEFERRED;
                    oep->deferred = 1;
                    path_node_complete(nop);
                    path_node_detach(nop);
                    return IOSERV_NOREPLY;
                }
            }
        }

        /* Only look for server/symlink entries (ie non-server) */
        if (ctp->id == link_root_id) {
            if (o && o->hdr.type != OBJECT_PROC_SYMLINK) {
                path_node_complete(nop);
                return complete_open(ctp, oep, ENOENT);
            }
        } else {
            /* Only look for non-link entries here */
            while (o && (o->hdr.type == OBJECT_PROC_SYMLINK)) {
                o = o->hdr.next;
            }

            /* If we don't have an object but we still have path then drop out
             * right away since code later on doesn't handle this situation
             */
            if (!o && result[0] != '\0') {
                path_node_complete(nop);
                return complete_open(ctp, oep,
                                     service_error(msg, PATH_SERVICE_OPEN));
            }
        }
        path_node_complete(nop);

        if (o) {
            switch (o->hdr.type) {
            case OBJECT_PROC_SYMLINK:
                if (result[0] == '\0') {
                    if (msg->connect.subtype == _IO_CONNECT_READLINK) {
                        return complete_open(
                            ctp, oep,
                            reply_symlink(ctp, 0, &msg->link_reply, &o->symlink,
                                          0, 0));
                    }
                    if (S_ISLNK(msg->connect.mode) &&
                        !(msg->connect.eflag & _IO_CONNECT_EFLAG_DIR)) {
                        break;
                    }
                }
                return complete_open(
                    ctp, oep,
                    reply_symlink(ctp, msg->connect.eflag, &msg->link_reply,
                                  &o->symlink, msg->connect.path, result));

            case OBJECT_MEM_SHARED:
                if ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) ||
                    result[0] != '\0') {
                    return complete_open(ctp, oep, ENOTDIR);
                }

                if ((msg->connect.subtype != _IO_CONNECT_OPEN) &&
                    (msg->connect.subtype != _IO_CONNECT_COMBINE) &&
                    (msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)) {
                    break;
                }

                if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) ==
                    (O_CREAT | O_EXCL)) {
                    return complete_open(ctp, oep, EEXIST);
                }

                return complete_open(ctp, oep, mem_open(ctp, msg, o, extra));

            case OBJECT_NAME:
                if (result[0] == '\0' &&
                    msg->connect.file_type == _FTYPE_NAME) {
                    if (open_entry_stale(oep))
                        return complete_open(ctp, oep, ESTALE);
                    return complete_open(ctp, oep, reply_redirect(ctp, msg, o));
                }
                if (result[0] == '\0') {
                    break;
                }
                return complete_open(ctp, oep, ENOENT);

            case OBJECT_SERVER:
                /* The endpoint itself is not a symlink: a readlink landing on
                 * it with no path left is EINVAL, children or not. Check before
                 * the child shortcut so a mount-root readlink doesn't fall
                 * through to service_error()/ENOSYS. A readlink with path
                 * remaining still forwards, so in-filesystem symlinks resolve.
                 */
                if (result[0] == '\0') {
                    if (msg->connect.subtype == _IO_CONNECT_READLINK)
                        return complete_open(ctp, oep, EINVAL);
                    if (nop->child)
                        break;
                }
                return complete_open(ctp, oep,
                                     service_error(msg, PATH_SERVICE_OPEN));

            default:
                return complete_open(ctp, oep, ENOENT);
            }
        }
        switch (msg->connect.subtype) {
        case _IO_CONNECT_READLINK:
            /* No terminal object, but the path node itself exists: a directory
             * node (one with children) is not a symbolic link, so readlink of
             * it is EINVAL per POSIX rather than ENOSYS.
             */
            return complete_open(ctp, oep, nop->child ? EINVAL : ENOSYS);

        case _IO_CONNECT_UNLINK:
            if (!o) {
                return complete_open(ctp, oep, nop->child ? ENOTEMPTY : ENOENT);
            }
            switch (o->hdr.type) {
            case OBJECT_NAME:
            case OBJECT_SERVER:
                if (msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) {
                    return complete_open(
                        ctp, oep,
                        (o->server.hdr.flags & NAMESPACE_FLAG_DIR) ? ENOSYS
                                                                   : ENOTDIR);
                }
                if (!(o->server.hdr.flags & NAMESPACE_FLAG_STICKY)) {
                    return complete_open(
                        ctp, oep,
                        (o->server.hdr.flags & NAMESPACE_FLAG_DIR) ? EBUSY
                                                                   : ENOSYS);
                }
                path_object_detach(o);
                return complete_open(ctp, oep, EOK);

            case OBJECT_PROC_SYMLINK:
                if (!proc_isaccess(0, &ctp->info)) {
                    return complete_open(ctp, oep, EPERM);
                }
                path_object_detach(o);
                return complete_open(ctp, oep, EOK);
            case OBJECT_MEM_SHARED:
                /* @@@ We should really have an object callout for this */
                if (o->mem.pm.mode &&
                    devmem_check_perm(ctp, o->mem.pm.mode, o->mem.pm.uid,
                                      o->mem.pm.gid, S_ISUID)) {
                    return complete_open(ctp, oep, EPERM);
                }
                path_object_detach(o);
                return complete_open(ctp, oep, EOK);
            default:
                break;
            }
            return complete_open(ctp, oep, ENOSYS);

        default:
            break;
        }

        /* We don't want to show proc's hidden entries, so if that is all the
         * object that there is then don't allow opening. Also if we have a
         * longer path, we should exit out now since a long path should have
         * matched some sort of object.
         */
        if (!o && nop->child_objects == 0) {
            if (open_entry_stale(oep))
                return complete_open(ctp, oep, ESTALE);
            return complete_open(ctp, oep, ENOENT);
        }
        if (service_error(msg, PATH_SERVICE_LS) == ENOSYS) {
            return complete_open(ctp, oep, ENOSYS);
        }

        /* A namespace directory node has no writable form: it is bound here for
         * listing (node_funcs carries no write op), so any write intent -- a
         * write access mode or O_TRUNC (write intent even under O_RDONLY) --
         * must fail EISDIR, matching the devmem and cpiofs directory handling,
         * rather than hand back a directory handle that cannot be written.
         */
        if (msg->connect.ioflag & (_IO_FLAG_WR | O_TRUNC)) {
            return complete_open(ctp, oep, EISDIR);
        }

        if (!(ocb = _scalloc(sizeof *ocb))) {
            return complete_open(ctp, oep, ENOMEM);
        }
        if (open_entry_stale(oep)) {
            _sfree(ocb, sizeof *ocb);
            return complete_open(ctp, oep, ESTALE);
        }
        ocb->node = path_node_clone(nop);
        if (ioserv_open_bind(ctp, ocb, &node_funcs) == -1) {
            path_node_detach(ocb->node);
            return complete_open(ctp, oep, errno);
        }
        return complete_open(ctp, oep, EOK);
    }
    /* !nop at this point */

    if (namespace_version_read() != namespace_version) {
        if (retry++ == 0)
            goto again;
        return ESTALE;
    }

    if (!(msg->connect.ioflag & O_CREAT)) {
        return service_error(msg, PATH_SERVICE_CREAT);
    }

    switch (msg->connect.file_type) {
    case _FTYPE_FILE:
    case _FTYPE_SHMEM: {
        path_object_t *obp;
        int status;
        int attach_err;
        if (!(obp = path_object_attach(ctp->info.pid, 0, msg->connect.path,
                                       OBJECT_MEM_SHARED, 0, 0, &attach_err))) {
            return attach_err;
        }
        status = mem_open(ctp, msg, obp, extra);
        if (status != EOK) {
            path_object_detach(obp);
        }
        path_object_done(obp);
        return status;
    }

    case _FTYPE_TYMEM:
        return tymem_open(ctp, msg, NULL, extra);

    default:
        break;
    }
    return service_error(msg, PATH_SERVICE_OPEN);
}

int path_unlink(ioserv_context_t *ctp,
                io_unlink_t *msg,
                void *handle,
                void *extra)
{
    return path_open(ctp, (io_open_t *) msg, handle, extra);
}

int path_readlink(ioserv_context_t *ctp,
                  io_readlink_t *msg,
                  void *handle,
                  void *extra)
{
    return path_open(ctp, (io_open_t *) msg, handle, extra);
}
