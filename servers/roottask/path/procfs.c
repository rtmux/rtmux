/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "root-process.h"
#include <private/root-proc-info.h>

#include <align.h>
#include <debug.h>
#include <dirent.h>
#include <devctl/common.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/ftype.h>
#include <sys/fault.h>
#include <sys/iomsg.h>
#include <sys/procfs.h>
#include <unistd.h>

#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include <sys/path-registry.h>

#include "procfs.h"
#include "mm-internal.h"

static dev_t procfs_devno;

static bool procfs_track_pid_ocb(unsigned flags)
{
    return !(flags & PROCFS_FLAG_MASK) || (flags & PROCFS_FLAG_WAIT);
}

static int procfs_check_access(mode_t mode,
                               uid_t uid,
                               gid_t gid,
                               mode_t check,
                               const struct _client_info *info)
{
    mode_t allowed;

    if (info->cred.euid == 0)
        return EOK;

    check &= S_IRWXU;
    if (check == 0)
        return EPERM;

    if (info->cred.euid == uid) {
        allowed = mode;
    } else {
        int in_group = info->cred.egid == gid;

        for (uint32_t i = 0; !in_group && i < info->cred.ngroups; i++)
            in_group = info->cred.grouplist[i] == gid;
        allowed = in_group ? mode << 3 : mode << 6;
    }

    return (allowed & check) == check ? EOK : EACCES;
}


/** Proc filesystem callouts for path namespace views. **/

/* This link code needs to be streamlined ... and it would be nice to get rid of
 * the temporary buffer that is jammed in there.
 */
static int return_a_link(ioserv_context_t *ctp,
                         struct _io_connect_link_reply *link_reply,
                         char *path,
                         char *append,
                         unsigned eflag,
                         int newlookup,
                         int addslash,
                         struct _io_connect_entry *entry)
{
    char *p, buff[PATH_MAX + 1];
    size_t len;

    /* This causes the return to only back out to libc before re-searching with
     * this new path that we have provided to it.
     */
    if (newlookup) {
        _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_LINK);
    }

    p = (char *) (link_reply + 1);
    len = strlen(path);

    /* buff is PATH_MAX+1. The client connect path is bounded only by
     * msg_max_size (> PATH_MAX), so reject an over-long path before the copy;
     * otherwise it would overrun this stack buffer.
     */
    if (len > PATH_MAX)
        return ENAMETOOLONG;
    memcpy(buff, path, len + 1);
    if (append) {
        size_t alen = strlen(append);
        size_t sep = (*append && *append != '/') ? 1 : 0;

        if (len + sep + alen > PATH_MAX)
            return ENAMETOOLONG;
        if (sep)
            buff[len++] = '/';
        memcpy(&buff[len], append, alen + 1);
        len += alen;
    }
    len++;

    if (entry) {
        memcpy(p, entry, sizeof(*entry));
        p += sizeof(*entry);
    }

    if (addslash && *buff != '/') {
        *p++ = '/';
    }
    memcpy(p, buff, len);
    if (addslash && *buff != '/') {
        len++;
    }

    link_reply->eflag = (uint8_t) eflag;
    link_reply->nentries = (entry) ? 1 : 0;
    link_reply->path_len = (uint16_t) len;

    /* We return the data in the following format: {link reply} [nentries of
     * struct _io_connect_entry] {path}
     */
    return IOSERV_PTR(
        ctp, link_reply,
        sizeof(*link_reply) + len + ((entry) ? sizeof(*entry) : 0));
}

static node_t *procfs_path_resolve(unsigned type, char *path, const char **tail)
{
    if (tail)
        *tail = NULL;

    while (*path && *path == '/') {
        path++;
    }

    return path_node_lookup(0, path, PATH_LOOKUP_ATTACH, tail);
}


/* This function does the bulk of the work in looking up and manipulating the
 * path namespace entries in terms of validating them and redirecting them to
 * the appropriate person.
 */
static int procfs_path_validate(ioserv_context_t *ctp,
                                io_open_t *msg,
                                unsigned type,
                                char *path,
                                void **handle)
{
    node_t *nop;
    path_object_t *obj;
    char *cresult;
    const char *result;
    int ret;

    if (handle) {
        *handle = 0;
    }

    if (!path || !*path) { /* Top-level mount root is a directory. */
        msg->connect.mode =
            (msg->connect.mode & ~(uint32_t) S_IFMT) | (unsigned) S_IFDIR;
        return EOK;
    }

    if (!(nop = procfs_path_resolve(type, path, &result))) {
        return ENOENT;
    }

    /* If we are looking at the mount directory, and we don't have a full node
     * match, then we hope that the non-matching component is a nid,pid,chid/...
     * thingy so we can send off the request.
     */
    ret = EOK;
    if ((type & PROCFS_FLAG_MOUNT) && result && *result) {
        char *lastslash;
        struct _io_connect_entry entry;

        cresult = lastslash = (char *) result;

        ret = ENOENT;
        /* This really licks, come up w/ a smaller method */
        entry.nd = (uint32_t) strtoul(cresult, &cresult, 10);
        if (*cresult++ != ',') {
            goto nothere;
        }
        entry.pid = (int32_t) strtoul(cresult, &cresult, 10);
        if (*cresult++ != ',') {
            goto nothere;
        }
        entry.chid = (int32_t) strtoul(cresult, &cresult, 10);
        if (*cresult++ != ',') {
            goto nothere;
        }
        entry.handle = (uint32_t) strtoul(cresult, &cresult, 10);
        if (*cresult++ != ',') {
            goto nothere;
        }
        /* bstecher says this should handle -1 just fine */
        entry.file_type = (uint32_t) strtoul(cresult, &cresult, 10);
        if (*cresult != '\0' && *cresult++ != '/') {
            goto nothere;
        }

        for (obj = nop->object; obj; obj = obj->hdr.next) {
            if (obj->hdr.type != OBJECT_SERVER &&
                obj->hdr.type != OBJECT_NAME) {
                continue;
            }
            if (obj->server.nd == entry.nd && obj->server.pid == entry.pid &&
                obj->server.chid == entry.chid &&
                obj->server.handle == entry.handle &&
                obj->server.file_type == entry.file_type) {
                entry.key = 0;
                break;
            }
        }
        if (!obj) {
            goto nothere;
        }

        /* Procfs handles ftypes that can't handle themselves, top level */
        if ((obj->hdr.type == OBJECT_NAME) ||
            ((obj->hdr.flags & NAMESPACE_FLAG_FTYPEONLY) &&
             msg->connect.file_type != entry.file_type)) {
            if (msg->connect.ioflag & _IO_FLAG_MASK) {
                ret = ENOTSUP;
            } else {
                msg->connect.mode &= ~(uint32_t) S_IFMT;
                if (obj->hdr.flags & NAMESPACE_FLAG_DIR) {
                    msg->connect.mode |= (unsigned) S_IFDIR;
                }
                ret = EOK;
            }
        } else if (*cresult && !(obj->hdr.flags & NAMESPACE_FLAG_DIR)) {
            ret = ENOTDIR;
        } else {
            /* Adjust the various path pieces so that we can re-direct them */
            entry.prefix_len = (uint16_t) ((cresult - msg->connect.path) + 1);
            ret = return_a_link(ctp, &msg->link_reply, msg->connect.path, NULL,
                                msg->connect.eflag, 1, 1, &entry);
        }
        if (handle) {
            *handle = obj;
        }
    } else {
        msg->connect.mode =
            (msg->connect.mode & ~(uint32_t) S_IFMT) | (unsigned) S_IFDIR;
        if (handle) {
            *handle = nop;
        }
    }
    /* It is an internal directory node */
nothere:
    path_node_detach(nop);
    return ret;
}

#define TOP_BIT 0x01000000U /* Only partway up, I know */
static int procfs_path_read(ioserv_context_t *ctp,
                            io_read_t *msg,
                            struct procfs_ocb *ocb)
{
    unsigned nbytes, amount, count;
    unsigned max_npc_size;
    node_t *nop;
    path_object_t *obj;
    struct dirent *d;

    /* Do a node lookup on the path ... */
    if (!(nop = procfs_path_resolve(ocb->flags, ocb->tail, 0))) {
        return EINVAL;
    }

    /* Actually perform the stuffing ritual for directories */
    max_npc_size =
        INT_STRLEN_MAXIMUM(pid_t) + 4 * INT_STRLEN_MAXIMUM(uint32_t) + 5;
    nbytes = (unsigned) min(
        msg->i.nbytes,
        ctp->msg_max_size -
            lib_align_up_size(offsetof(struct dirent, d_name) + max_npc_size,
                              8));
    d = (struct dirent *) (void *) msg;
    amount = 0;
    obj = NULL;

    /* Add the nodes at this level first */
    if (!(ocb->offset & TOP_BIT)) {
        obj = nop->object;
        for (count = 0; obj; obj = obj->hdr.next) {
            if (obj->hdr.type != OBJECT_SERVER &&
                obj->hdr.type != OBJECT_NAME) {
                continue;
            }

            if (count >= ocb->offset) {
                /* We really need to put the name in here too, not just guess */
                if (amount + sizeof(*d) + max_npc_size >= nbytes) {
                    break;
                }

                memset(d, 0x00, sizeof *d);
                snprintf(d->d_name, max_npc_size, "%u,%d,%d,%u,%u",
                         obj->server.nd, obj->server.pid, obj->server.chid,
                         obj->server.handle, obj->server.file_type);
                d->d_namelen = (uint16_t) (strlen(d->d_name) + 1);
                d->d_ino = (ino_t) (ocb->offset + 1);
                d->d_reclen = (uint16_t) ((offsetof(struct dirent, d_name) +
                                           (size_t) d->d_namelen + 1 + 7) &
                                          ~(size_t) 7);

                ocb->offset++;
                amount += (size_t) d->d_reclen;
                d = (struct dirent *) ((char *) d + d->d_reclen);
            }
            count++;
        }
        /* We want to read the second section too */
        if (!obj) {
            ocb->offset = TOP_BIT;
        }
    }

    if (ocb->offset & TOP_BIT) {
        node_t *tmpnode;
        count = TOP_BIT;

        /* Lock access to the node so that it's children don't get deleted as
         * we're walking the list.
         */
        path_node_access(nop);

        for (tmpnode = nop->child; tmpnode; tmpnode = tmpnode->sibling) {
            if (tmpnode->object) {
                if ((ocb->flags & PROCFS_FLAG_MOUNT) &&
                    tmpnode->object->hdr.type != OBJECT_SERVER &&
                    tmpnode->object->hdr.type != OBJECT_NAME) {
                    continue;
                }
            } else if (tmpnode->child_objects == 0) {
                continue;
            }

            if (count >= ocb->offset) {
                if (amount + sizeof(*d) + tmpnode->len >= nbytes) {
                    break;
                }

                memset(d, 0x00, sizeof *d);
                memcpy(d->d_name, tmpnode->name, tmpnode->len);
                d->d_name[tmpnode->len] = '\0';
                d->d_namelen = (uint16_t) (tmpnode->len + 1);
                d->d_ino = (ino_t) (ocb->offset + 1);
                d->d_reclen = (uint16_t) ((offsetof(struct dirent, d_name) +
                                           (size_t) d->d_namelen + 1 + 7) &
                                          ~(size_t) 7);

                ocb->offset++;
                amount += (size_t) d->d_reclen;
                d = (struct dirent *) ((char *) d + d->d_reclen);
            }
            count++;
        }

        path_node_complete(nop);
    }
    _IO_SET_READ_NBYTES(ctp, amount);
    path_node_detach(nop);
    return IOSERV_PTR(ctp, msg, amount);
}

static int procfs_read(ioserv_context_t *ctp, io_read_t *msg, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    process_t *prp = NULL;
    size_t size;
    void *ptr;
    unsigned nbytes;
    uintptr_t *pos, offset;
    struct dirent *d;

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        pos = &ocb->offset;
        break;
    case _IO_XTYPE_OFFSET:
        *(pos = &offset) =
            (uintptr_t) ((struct _xtype_offset *) (msg + 1))->offset;
        break;
    default:
        return ENOSYS;
    }

    /* Case #1 We are reading from a mount point dir. Eventually we will have
     * all these reads as callouts.
     */
    if (ocb->flags & PROCFS_FLAG_MOUNT) {
        return procfs_path_read(ctp, msg, ocb);
    }

    /* /proc/<pid>/wait: generation-pinned process-handle status scaffold. */
    if (ocb->pid && (ocb->flags & PROCFS_FLAG_WAIT)) {
        root_proc_info_t qinfo;
        char state_buf[32];
        const char *state = state_buf;
        size_t statelen, copylen;

        if (!(ocb->ioflag & _IO_FLAG_RD))
            return EBADF;
        if (!ocb->stale && ocb->pid != -1 &&
            process_query_info(ocb->pid, &qinfo) == EOK) {
            if (qinfo.flags & RTMUX_FLG_PROC_WAITINFO) {
                (void) snprintf(state_buf, sizeof(state_buf), "exited %d\n",
                                qinfo.siginfo.si_status);
            } else {
                state = "live\n";
            }
        } else {
            state = "stale\n";
        }
        statelen = strlen(state);
        if ((size_t) *pos >= statelen) {
            _IO_SET_READ_NBYTES(ctp, 0);
            return IOSERV_PTR(ctp, NULL, 0);
        }
        copylen = statelen - (size_t) *pos;
        if (copylen > (size_t) msg->i.nbytes)
            copylen = (size_t) msg->i.nbytes;
        if (copylen > ctp->msg_max_size)
            copylen = ctp->msg_max_size;
        memcpy(msg, state + *pos, copylen);
        *pos += copylen;
        _IO_SET_READ_NBYTES(ctp, (unsigned) copylen);
        return IOSERV_PTR(ctp, msg, (unsigned) copylen);
    }

    /* /proc/<pid>/fd lists normal fd-vector coids; /fd/<n> reopen comes later.
     */
    if (ocb->pid && (ocb->flags & PROCFS_FLAG_FD)) {
        struct _server_info sinfo;
        int fd;

        if (ocb->pid == -1)
            return EIO;
        if (!(ocb->ioflag & _IO_FLAG_RD))
            return EBADF;

        fd = connect_server_info(ocb->pid, (uint64_t) (int) *pos, &sinfo);
        if (fd < 0 || ((unsigned) fd & RTMUX_SIDE_CHANNEL)) {
            _IO_SET_READ_NBYTES(ctp, 0);
            return IOSERV_PTR(ctp, NULL, 0);
        }

        size = msg->i.nbytes;
        d = (struct dirent *) (void *) msg;
        memset(d, 0x00, sizeof *d);
        snprintf(d->d_name, INT_STRLEN_MAXIMUM(int) + 1, "%d", fd);
        d->d_namelen = (uint16_t) strlen(d->d_name);
        d->d_ino = PID_TO_INO(ocb->pid, PROCFS_FLAG_FD) ^ (ino_t) fd;
        d->d_reclen = (uint16_t) ((offsetof(struct dirent, d_name) +
                                   (size_t) d->d_namelen + 1 + 7) &
                                  ~(size_t) 7);
        if ((size_t) d->d_reclen > size)
            return EMSGSIZE;
        *pos = (uintptr_t) (fd + 1);
        d->d_offset = (off_t) *pos;
        _IO_SET_READ_NBYTES(ctp, (unsigned) d->d_reclen);
        return IOSERV_PTR(ctp, msg, (unsigned) d->d_reclen);
    }

    /* Case #2 we are reading from one of the process id directories */
    if (ocb->pid && !(ocb->flags & PROCFS_FLAG_MASK)) {
        if (ocb->pid == -1) {
            return EIO;
        }
        if (!(ocb->flags & (PROCFS_FLAG_AS | PROCFS_FLAG_NAME))) {
            /* Directory listing of /proc/<pid>. *pos counts entries we've
             * already returned: 0 = "as", 1 = "name", 2+ = EOF.
             */
            static const struct {
                const char *name;
                size_t namelen;
                unsigned flag;
            } entries[] = {
                {"as", 2, PROCFS_FLAG_AS},
                {"fd", 2, PROCFS_FLAG_FD},
                {"name", 4, PROCFS_FLAG_NAME},
                {"wait", 4, PROCFS_FLAG_WAIT},
            };
            unsigned eidx = (unsigned) *pos;

            if (eidx >= sizeof(entries) / sizeof(entries[0])) {
                _IO_SET_READ_NBYTES(ctp, 0);
                return IOSERV_PTR(ctp, NULL, 0);
            }
            size = msg->i.nbytes;
            d = (struct dirent *) (void *) msg;
            memset(d, 0x00, sizeof *d);
            memcpy(d->d_name, entries[eidx].name, entries[eidx].namelen + 1);
            d->d_namelen = (uint16_t) entries[eidx].namelen;
            d->d_ino = PID_TO_INO(ocb->pid, entries[eidx].flag);
            d->d_reclen = (uint16_t) ((offsetof(struct dirent, d_name) +
                                       (size_t) d->d_namelen + 1 + 7) &
                                      ~(size_t) 7);
            if ((size_t) d->d_reclen > size) {
                return EMSGSIZE;
            }
            d->d_offset = (off_t)++ * pos;
            _IO_SET_READ_NBYTES(ctp, (unsigned) d->d_reclen);
            return IOSERV_PTR(ctp, msg, (unsigned) d->d_reclen);
        }

        if (ocb->flags & PROCFS_FLAG_NAME) {
            /* /proc/<pid>/name — return the debug_name string + '\n'. Source:
             * root_proc(ocb->pid)->debug_name (roottask heap). Toolbox applets
             * and "cat /proc/N/name" reach the same data here.
             */
            root_process_t *tmp_n;
            const char *name;
            size_t namelen, copylen;

            if (!(ocb->ioflag & _IO_FLAG_RD))
                return EBADF;
            tmp_n = root_proc(ocb->pid);
            name = tmp_n->debug_name;
            if (!name)
                name = "";
            namelen = strlen(name);
            /* "<path>\n" — single-line read, EOF at pos == namelen+1. */
            if ((size_t) *pos >= namelen + 1) {
                _IO_SET_READ_NBYTES(ctp, 0);
                return IOSERV_PTR(ctp, NULL, 0);
            }
            {
                char *buf = (char *) msg;
                size_t left = (namelen + 1) - (size_t) *pos;
                size_t want = (size_t) msg->i.nbytes;
                copylen = (left < want) ? left : want;
                if (copylen > ctp->msg_max_size)
                    copylen = ctp->msg_max_size;
                if ((size_t) *pos < namelen) {
                    size_t n = namelen - (size_t) *pos;
                    if (n > copylen)
                        n = copylen;
                    memcpy(buf, name + *pos, n);
                    if (n < copylen)
                        buf[n] = '\n';
                } else {
                    /* exactly at the trailing newline */
                    buf[0] = '\n';
                }
                *pos += copylen;
                _IO_SET_READ_NBYTES(ctp, (unsigned) copylen);
                return IOSERV_PTR(ctp, msg, (unsigned) copylen);
            }
        }

        if (!(ocb->ioflag & _IO_FLAG_RD)) {
            return EBADF;
        }

        /* Avoid a deadlock between proc-service and remote IPC paths caused by
         * MsgReplying to a remote client while the targeted and locked process
         * owns remote IPC coordination. No easy way to identify that process
         * here, so do this conservatively if the client is remote.
         */

        /* RUSH2: Once the new root_vm_ops is online, we can redo this by RUSH2:
         * mmap'ing "/proc/<pid>/as" and avoiding the double copy...
         */

        if (RTMUX_NODE_CMP(RTMUX_LOCAL_NODE, ctp->info.nd) != 0) {
            size_t offset2 = 0;
            /* we reuse msg, so save off any msg->i fields we need. */
            size_t desiredsize = msg->i.nbytes;
            size_t mappedsize;
            size_t copysize;
            struct _procfs_map_info mi;

            while (1) {
                { /* Acquire needed locks */
                    if (!(prp = root_proc_lock(ocb->pid))) {
                        return ESRCH;
                    }

                    /* We get a write lock even though we're only reading
                     * because we might fault during the copy and end up in
                     * fault_notification(). That code needs a write lock and we
                     * don't have any gear to tell it to promote the read lock
                     */
                    if (proc_wlock_adp(ocb->pid) == -1) {
                        return proc_error(ESRCH, ocb->pid);
                    }
                    proc_lock_owner_mark(ocb->pid);
                    process_bind(ocb->pid);
                }

                ptr = (void *) *pos;
                mappedsize = root_vm_ops.mapinfo(ocb->pid, prp, *pos, &mi, NULL,
                                                 0, NULL, NULL, NULL);
                copysize = min(desiredsize, min(mappedsize, ctp->msg_max_size));
                if ((copysize != 0) &&
                    (mappedsize == SIZE_MAX || !(mi.flags & PROT_READ))) {
                    process_bind(0);
                    proc_unlock_adp(ocb->pid);
                    root_proc_unlock(ocb->pid);
                    return EINVAL;
                }
                memcpy(msg, ptr, copysize);

                { /* Release locks */
                    process_bind(0);
                    proc_unlock_adp(ocb->pid);
                    root_proc_unlock(ocb->pid);
                }

                if (copysize == 0) {
                    /* Done transferring data, locks released; flee. [True, this
                     * last memcpy was a NOP. Rien fait.]
                     */
                    break;
                }

                if (ioserv_msgwrite(ctp, msg, (int) copysize, (int) offset2) ==
                    -1) {
                    return errno;
                }

                offset2 += copysize;
                *pos += copysize;
                desiredsize -= copysize;
            }

            /* We reply NULL,0 as the ioserv_msgwrite()s have done all the
             * needed copying to client; We just want the client wakeup
             * 'side-effect' of msg_reply. Total bytes copied, offset2, *is*
             * returned to the client.
             */
            msg_reply(ctp->rcvid, (int) offset2, NULL, 0);
            return IOSERV_NOREPLY;
        }

        if (!(prp = root_proc_lock(ocb->pid))) {
            return ESRCH;
        }

        /* We get a write lock even though we're only reading because we might
         * fault during the copy and end up in fault_notification(). That code
         * needs a write lock and we don't have any gear to tell it to promote
         * the read lock
         */
        if (proc_wlock_adp(ocb->pid) == -1) {
            return proc_error(ESRCH, ocb->pid);
        }
        proc_lock_owner_mark(ocb->pid);

        process_bind(ocb->pid);

        ptr = (void *) *pos;
        size = root_vm_ops.mapinfo(ocb->pid, prp, *pos, NULL, NULL, 0, NULL,
                                   NULL, NULL);
        size = min(size, (size_t) msg->i.nbytes);
        *pos += size;

        if (msg_reply(ctp->rcvid, (int) size, ptr, (int) size) == -1) {
            if (errno == EFAULT) {
                msg_error(ctp->rcvid, EIO);
            } else {
                msg_error(ctp->rcvid, errno);
            }
        }

        process_bind(0);
        proc_unlock_adp(ocb->pid);

        return proc_error(IOSERV_NOREPLY, ocb->pid);
    }

    /* Case #3 We are reading from the root directory */
    size = min(msg->i.nbytes, (size_t) (ctp->msg_max_size -
                                        ((offsetof(struct dirent, d_name) +
                                          INT_STRLEN_MAXIMUM(pid_t) + 1 + 7) &
                                         ~7UL)));
    d = (struct dirent *) (void *) msg;
    nbytes = 0;

    /* Jump over the self entry on remote queries */
    if (*pos == 0 && RTMUX_NODE_CMP(RTMUX_LOCAL_NODE, ctp->info.nd) != 0) {
        ++*pos;
    }

    while (nbytes < size) {
        pid_t pid;

        memset(d, 0x00, sizeof *d);

        pid = 0;
        if (*pos > 0) {
            unsigned offset3;

            offset3 = (unsigned) *pos;
            prp = query_object(_QUERY_PROCESS, offset3, _QUERY_PROCESS_VECTOR,
                               0, &offset3, 0, 0);
            if (!prp)
                break;

            /* prp lives in kernel heap (PTE_U=0); the full pid (with generation
             * bits) comes from the index via the dedicated kernel extension.
             */
            pid = ProcessIndexToPid(offset3);
            query_object_done(pid);
            *pos = offset3;
        }

        switch (*pos) {
        case 0:
            memcpy(d->d_name, "self", sizeof("self"));
            d->d_ino = PID_TO_INO(ctp->info.pid, PROCFS_FLAG_AS);
            break;
        default:
            snprintf(d->d_name, INT_STRLEN_MAXIMUM(pid_t) + 1, "%d", pid);
            d->d_ino = PID_TO_INO(pid, PROCFS_FLAG_AS);
            break;
        }

        d->d_namelen = (uint16_t) strlen(d->d_name);
        d->d_reclen = (uint16_t) ((offsetof(struct dirent, d_name) +
                                   (size_t) d->d_namelen + 1 + 7) &
                                  ~(size_t) 7);

        { /* Clear the last bits ... great place for an easter egg to hide */
            size_t extra;
            extra = (size_t) d->d_reclen - (offsetof(struct dirent, d_name) +
                                            (size_t) d->d_namelen + 1);
            if (extra > 0) {
                memset(d->d_name + d->d_namelen + 1, '\0', extra);
            }
        }

        if ((nbytes + (unsigned) d->d_reclen) >= size) {
            if (!nbytes)
                return EMSGSIZE;
            break;
        }

        d->d_offset = (off_t)++ * pos;
        nbytes += (unsigned) d->d_reclen;
        d = (struct dirent *) ((char *) d + (unsigned) d->d_reclen);
    }
    _IO_SET_READ_NBYTES(ctp, nbytes);
    return IOSERV_PTR(ctp, msg, nbytes);
}

static int procfs_write(ioserv_context_t *ctp, io_write_t *msg, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    process_t *prp;
    size_t size;
    uintptr_t *pos, offset;
    int nbytes, skip;

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        pos = &ocb->offset;
        skip = (int) sizeof(io_write_t);
        break;
    case _IO_XTYPE_OFFSET:
        *(pos = &offset) =
            (uintptr_t) ((struct _xtype_offset *) (msg + 1))->offset;
        skip = (int) (sizeof(io_write_t) + sizeof(struct _xtype_offset));
        break;
    default:
        return ENOSYS;
    }

    if (ocb->pid <= 0 || (ocb->flags & PROCFS_FLAG_MASK)) {
        return EIO;
    }
    if (!(ocb->ioflag & _IO_FLAG_WR)) {
        return EBADF;
    }
    if (!(prp = root_proc_lock(ocb->pid))) {
        return ESRCH;
    }

    /* We get a write lock even though we're not manipulating the as structures
     * because we might fault during the copy and end up in
     * fault_notification(). That code needs a write lock and we don't have any
     * gear to tell it to promote the read lock.
     */
    if (proc_wlock_adp(ocb->pid) == -1) {
        return proc_error(ESRCH, ocb->pid);
    }
    proc_lock_owner_mark(ocb->pid);

    process_bind(ocb->pid);

    size = root_vm_ops.mapinfo(ocb->pid, prp, *pos, NULL, NULL, 0, NULL, NULL,
                               NULL);
    size = min(size, (size_t) msg->i.nbytes);
    SETIOV(ctp->iov + 0, (void *) *pos, size);
    *pos += size;

    if ((nbytes = ioserv_msgreadv(ctp, ctp->iov + 0, 1, skip)) == -1) {
        process_bind(0);
        proc_unlock_adp(ocb->pid);
        return proc_error(errno, ocb->pid);
    }

    process_bind(0);
    proc_unlock_adp(ocb->pid);

    _IO_SET_WRITE_NBYTES(ctp, nbytes);

    return proc_error(EOK, ocb->pid);
}

static int procfs_waitstop(pid_t pid, int rcvid)
{
    struct {
        struct _io_devctl_reply reply;
        procfs_status status;
    } msg;
    struct procfs_waiting *wap;
    root_proc_info_t qinfo;

    if (process_query_info(pid, &qinfo) != EOK) {
        return msg_error_r(rcvid, ESRCH);
    }
    if (debug_process_call(RTMUX_DEBUG_THREAD_INFO, pid, 0,
                           (union rtmux_debug_data *) &msg.status) == -1) {
        return msg_error_r(rcvid, errno);
    }
    if ((qinfo.flags & RTMUX_FLG_PROC_TERMING) ||
        (msg.status.flags & _DEBUG_FLAG_STOPPED)) {
        msg.reply.ret_val = 0;
        msg.reply.zero = 0;
        msg.reply.zero2 = 0;
        msg.reply.nbytes = (int32_t) sizeof(msg.status);
        return msg_reply_r(rcvid, 0, &msg, (int) sizeof msg);
    }

    if (!(wap = _smalloc(sizeof *wap))) {
        return msg_error_r(rcvid, ENOMEM);
    }
    wap->rcvid = rcvid;
    {
        root_process_t *tmp = root_proc(pid);
        wap->next = tmp->debug_waiting;
        tmp->debug_waiting = wap;
    }
    return EOK;
}

void procfs_wait_stale(pid_t pid)
{
    struct procfs_ocb *ocb;
    root_proc_info_t qinfo;

    if (process_query_info(pid, &qinfo) != EOK)
        return;

    for (ocb = (struct procfs_ocb *) qinfo.ocb_list; ocb; ocb = ocb->next) {
        if (ocb->flags & PROCFS_FLAG_WAIT)
            ocb->stale = true;
    }
}

static int procfs_close_ocb(ioserv_context_t *ctp, void *reserved, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    struct procfs_ocb *p, **pp;
    process_t *prp;
    root_proc_info_t qinfo;

    if (ocb->pid > 0 && procfs_track_pid_ocb(ocb->flags)) {
        if ((prp = root_proc_lock(ocb->pid))) {
            /* prp->ocb_list lives in kernel heap (PTE_U=0). We need a stable
             * head pointer to the list to walk it, but the entries themselves
             * are roottask-allocated (see procfs_ocb), so once we have the head
             * we can deref the list safely. The trick: pull the head from
             * qinfo, then walk via the entries' own ->next pointers, keeping a
             * "*pp" replacement local; if we find ocb in the list, post a Set
             * with the replacement head when removing the head, otherwise patch
             * via ->next on the predecessor (roottask-VA write, OK).
             */
            if (process_query_info(ocb->pid, &qinfo) == EOK) {
                struct procfs_ocb *head = (struct procfs_ocb *) qinfo.ocb_list;
                if (head == ocb) {
                    (void) process_set_ocb_list(ocb->pid, ocb->next);
                } else {
                    for (p = head; p && p->next != ocb; p = p->next)
                        ;
                    if (p)
                        p->next = ocb->next;
                }
                /* preserve original loop's "pp" not-found semantics */
                (void) pp;
            }

            if (ocb->ioflag & _IO_FLAG_WR) {
                (void) debug_detach(ocb->pid);
            }
            root_proc_unlock(ocb->pid);
        }
    }
    _sfree(ocb, sizeof *ocb + ((*ocb->tail) ? strlen(ocb->tail) : 0));
    return EOK;
}

static int procfs_unblock(ioserv_context_t *ctp,
                          io_notification_t *msg,
                          void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    process_t *prp;

    if (ocb && !(ocb->flags & PROCFS_FLAG_MASK) &&
        (prp = root_proc_lock(ocb->pid))) {
        struct procfs_waiting *p, **pp;
        struct _msg_info info;

        if ((msg_info(ctp->rcvid, &info) == -1) ||
            !(info.flags & RTMUX_MI_UNBLOCK_REQ)) {
            root_proc_unlock(ocb->pid);
            return IOSERV_NOREPLY;
        }
        for (pp = &root_proc(ocb->pid)->debug_waiting; (p = *pp);
             pp = &p->next) {
            if (p->rcvid == ctp->rcvid) {
                *pp = p->next;
                _sfree(p, sizeof *p);
                break;
            }
        }
        root_proc_unlock(ocb->pid);
    }
    return EINTR;
}

static int procfs_lseek(ioserv_context_t *ctp, io_lseek_t *msg, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    uintptr_t off;

    if (ocb->pid < 0 && !(ocb->flags & PROCFS_FLAG_MASK)) {
        return EIO;
    }
    if (ocb->pid == 0 && (msg->i.whence != SEEK_SET || msg->i.offset)) {
        return EIO;
    }
    switch (msg->i.whence) {
    case SEEK_SET:
        off = (uintptr_t) msg->i.offset;
        break;
    case SEEK_CUR:
        off = ocb->offset + (uintptr_t) msg->i.offset;
        break;
    case SEEK_END:
        off = ULONG_MAX + (uintptr_t) msg->i.offset;
        break;
    default:
        return EINVAL;
    }
    ocb->offset = off;

    if (msg->i.combine_len & _IO_COMBINE_FLAG) {
        return EOK;
    }

    msg->o = off;
    SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o);
    return IOSERV_NPARTS(1);
}

static int procfs_stat(ioserv_context_t *ctp, io_stat_t *msg, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    process_t *prp;

    /* /proc/<pid>/wait — process-handle scaffold pinned by pid generation. */
    if (ocb->pid && (ocb->flags & PROCFS_FLAG_WAIT)) {
        memset(&msg->o, 0x00, sizeof msg->o);
        msg->o.st_ino = PID_TO_INO(ocb->pid, PROCFS_FLAG_WAIT);
        msg->o.st_size = 6;
        msg->o.st_mode = (unsigned) S_IFREG | (0444U & ~roottask_procfs_umask);
        msg->o.st_nlink = 1;
        {
            root_proc_info_t qinfo;
            if (process_query_info(ocb->pid, &qinfo) == EOK) {
                msg->o.st_gid = qinfo.cred.rgid;
                msg->o.st_uid = qinfo.cred.ruid;
            }
        }
        SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o);
        return IOSERV_NPARTS(1);
    }

    /* /proc/<pid>/fd — directory scaffold for descriptor introspection. */
    if (ocb->pid && (ocb->flags & PROCFS_FLAG_FD)) {
        memset(&msg->o, 0x00, sizeof msg->o);
        msg->o.st_ino = PID_TO_INO(ocb->pid, PROCFS_FLAG_FD);
        msg->o.st_size = 1;
        msg->o.st_mode = (unsigned) S_IFDIR | (0555U & ~roottask_procfs_umask);
        msg->o.st_nlink = 2;
        {
            root_proc_info_t qinfo;
            if (process_query_info(ocb->pid, &qinfo) == EOK) {
                msg->o.st_gid = qinfo.cred.rgid;
                msg->o.st_uid = qinfo.cred.ruid;
            }
        }
        SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o);
        return IOSERV_NPARTS(1);
    }

    /* /proc/<pid>/name — regular file holding the executable path. */
    if (ocb->pid && (ocb->flags & PROCFS_FLAG_NAME)) {
        root_process_t *tmp_n = root_proc(ocb->pid);
        const char *name = tmp_n->debug_name;

        memset(&msg->o, 0x00, sizeof msg->o);
        msg->o.st_ino = PID_TO_INO(ocb->pid, PROCFS_FLAG_NAME);
        msg->o.st_size = name ? (off_t) (strlen(name) + 1) : 0;
        msg->o.st_mode = (unsigned) S_IFREG | (0444U & ~roottask_procfs_umask);
        msg->o.st_nlink = 1;
        {
            root_proc_info_t qinfo;
            if (process_query_info(ocb->pid, &qinfo) == EOK) {
                msg->o.st_gid = qinfo.cred.rgid;
                msg->o.st_uid = qinfo.cred.ruid;
            }
        }
        SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o);
        return IOSERV_NPARTS(1);
    }

    /* Case #1 a pid/AS stat */
    if (ocb->pid && (ocb->flags & PROCFS_FLAG_AS)) {
        struct _procfs_map_info info;
        uintptr_t vaddr;

        if (ocb->pid == -1) {
            return EIO;
        }
        if (!(prp = root_proc_lock(ocb->pid))) {
            return EBADF;
        }
        memset(&msg->o, 0x00, sizeof msg->o);
        msg->o.st_ino = PID_TO_INO(ocb->pid, 0);
        msg->o.st_size = 0;
        if (proc_rlock_adp(ocb->pid) != -1) {
            for (vaddr = 0; root_vm_ops.mapinfo(ocb->pid, prp, vaddr, &info,
                                                NULL, 0, NULL, NULL, NULL);
                 vaddr = info.vaddr + info.size) {
                if (info.flags & MAP_SYSRAM) {
                    msg->o.st_size += (off_t) info.size;
                }
            }
            proc_unlock_adp(ocb->pid);
        }
        {
            root_proc_info_t qinfo;
            if (process_query_info(ocb->pid, &qinfo) == EOK) {
                msg->o.st_gid = qinfo.cred.rgid;
                msg->o.st_uid = qinfo.cred.ruid;
            }
        }
        msg->o.st_mode = (unsigned) S_IFREG | (0666U & ~roottask_procfs_umask);
        msg->o.st_nlink = 1;
        root_proc_unlock(ocb->pid);

        /* Case #2 all other stats */
    } else {
        memset(&msg->o, 0x00, sizeof msg->o);
        msg->o.st_size = 1;
        if (ocb->pid && !(ocb->flags & PROCFS_FLAG_MASK)) {
            msg->o.st_ino = PID_TO_INO(ocb->pid, PROCFS_FLAG_AS);
            msg->o.st_nlink = ocb->pid == ctp->info.pid ? 4 : 2;
            msg->o.st_mode =
                (unsigned) S_IFDIR | (0555U & ~roottask_procfs_umask);
        } else if (ocb->flags & PROCFS_FLAG_PROC) {
            memsize_t st_size;
            prp = root_proc_lookup(ctp->info.pid);
            st_size = pa_free_size();

            /* NYI: What we really should do here is compile with
             * _FILE_OFFSET_BITS=64, but that'll cause code bloat and
             * destabilize things. Something for another day.... bstecher
             */
            msg->o.st_size = (off_t) st_size;
            msg->o.st_ino = ~(ino_t) 0;
            msg->o.st_nlink = 2;
            msg->o.st_mode = (unsigned) S_IFDIR | 0555U;
        } else {
            if ((ocb->flags & (PROCFS_FLAG_MOUNT | PROCFS_FLAG_DIR)) ==
                PROCFS_FLAG_MOUNT) {
                msg->o.st_ino = (ino_t) ocb->pid;
                msg->o.st_mode = (unsigned) S_IFREG | 0444U;
                msg->o.st_nlink = 1;
            } else {
                msg->o.st_ino = (ino_t) ocb->pid;
                msg->o.st_mode = (unsigned) S_IFDIR | 0555U;
                msg->o.st_nlink = 2;
            }
        }
    }
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | procfs_devno;
    msg->o.st_rdev = 0;

    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

/* this function will create a private mapping from a shared mapping The mapped
 * address will be the same as for the shared mapping but will be a private copy
 * of <size> bytes
 */
static int vm_shared_to_private(pid_t pid,
                                process_t *prp,
                                uintptr_t addr,
                                size_t size)
{
    procfs_mapinfo info;
    struct {
        procfs_debuginfo debug;
        char buf[_POSIX_PATH_MAX + 1];
    } dinfo;
    struct {
        struct _mem_debug_info debug;
        char buf[_POSIX_PATH_MAX + 1];
    } minfo;
    void *tmap, *nmap;
    size_t map_size;
    int status;

    size_t r = root_vm_ops.mapinfo(
        pid, prp, addr & ~(__PAGESIZE - 1), &info, &dinfo.debug,
        sizeof(dinfo) - offsetof(procfs_debuginfo, path), NULL, NULL, NULL);
    if ((r == 0) || ((uintptr_t) info.vaddr != (addr & ~(__PAGESIZE - 1))) ||
        (size > info.size)) {
        return -1;
    }

    /* create a temporary anoymous mapping for the page to be privatized and
     * copy the shared page to it
     */
    proc_lock_owner_mark(pid);
    process_bind(pid);
    status = root_vm_ops.mmap(pid, prp, 0, size, PROT_WRITE | PROT_READ,
                              MAP_PRIVATE | MAP_ANON, NULL, /* object */
                              0,                            /* offset */
                              0,                            /* align */
                              0,                            /* prealloc */
                              NOFD, &tmap, &map_size);
    if (status != EOK || MAP_FAILED == (void *) tmap || map_size != size) {
        process_bind(0);
        errno = status;
        return -1;
    } else {
        memcpy(tmap, (void *) ((uintptr_t) info.vaddr), map_size);
    }

    /* now, unmap the shared page that will be privatized and create a new
     * private mapping at the same address as the previous shared mapping and
     * copy the original contents to it (from the temp mapping)
     */
    status = root_vm_ops.munmap(pid, prp, info.vaddr, size, 0);
    if (status != EOK) {
        /* release temporary mapping and return a failure */
        (void) root_vm_ops.munmap(pid, prp, (uintptr_t) tmap, map_size, 0);
        process_bind(0);
        return -1;
    }

    status =
        root_vm_ops.mmap(pid, prp, info.vaddr, /* address */
                         size, (int) (info.flags & PROT_MASK) | PROT_WRITE,
                         (int) (info.flags & ~(MAP_RESERVMASK | MM_PG_MASK |
                                               PROT_MASK | MAP_SHARED)) |
                             MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                         NULL, /* object */
                         0,    /* offset */
                         0,    /* align */
                         0,    /* prealloc */
                         NOFD, &nmap, &map_size);
    if (status != EOK || nmap != (void *) ((uintptr_t) info.vaddr) ||
        map_size != size) {
        /* release temporary mapping and return a failure */
        (void) root_vm_ops.munmap(pid, prp, (uintptr_t) tmap, map_size, 0);
        process_bind(0);
        return -1;
    } else {
        memcpy(nmap, tmap, map_size);
    }

    /* now, release the temporary mapping and restablish the original protection
     * attributes on the newly privatized page
     */
    status = root_vm_ops.munmap(pid, prp, (uintptr_t) tmap, size, 0);
    if (status != EOK) {
        process_bind(0);
        return -1;
    }
    status = root_vm_ops.mprotect(pid, prp, (uintptr_t) nmap, map_size,
                                  (int) (info.flags & PROT_MASK));
    if (status != EOK) {
        process_bind(0);
        return -1;
    }

    /* finally, copy the debug info */
    memset(&minfo, 0, sizeof(minfo.debug));
    minfo.debug.type = (uint16_t) _MEM_DEBUG_INFO;
    minfo.debug.vaddr = (uintptr_t) nmap;
    minfo.debug.size = map_size;
    minfo.debug.flags =
        (info.flags & ~(MAP_RESERVMASK | MM_PG_MASK | PROT_MASK | MAP_SHARED)) |
        (unsigned) MAP_PRIVATE;
    minfo.debug.old_vaddr = dinfo.debug.vaddr;
    STRLCPY(minfo.debug.path, dinfo.debug.path,
            sizeof(minfo) - offsetof(struct _mem_debug_info, path));

    (void) root_vm_ops.debuginfo(pid, prp, &minfo.debug);

    /* Phew! */
    process_bind(0);
    return 0;
}

static int procfs_regset_read(struct procfs_ocb *ocb,
                              process_t *prp,
                              unsigned regset_id,
                              void *buf,
                              int *nbytes)
{
    switch (regset_id) {
    case REGSET_GPREGS:
        if (debug_process_call(RTMUX_DEBUG_GET_GREG, ocb->pid, ocb->tid, buf) ==
            -1) {
            return errno;
        }
        *nbytes = (int) sizeof(CPU_REGISTERS);
        return EOK;
    default:
        if (!(prp = root_proc_lock(ocb->pid))) {
            return ESRCH;
        }
        switch (regset_id) {
        case REGSET_FPREGS:
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_GET_FPREG, ocb->pid, ocb->tid,
                                   buf) == -1) {
                process_bind(0);
                return proc_error(errno, ocb->pid);
            }
            process_bind(0);
            *nbytes = (int) sizeof(FPU_REGISTERS);
            break;
        case REGSET_ALTREGS:
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_GET_ALTREG, ocb->pid, ocb->tid,
                                   buf) == -1) {
                process_bind(0);
                return proc_error(errno, ocb->pid);
            }
            process_bind(0);
            *nbytes = (int) sizeof(ALT_REGISTERS);
            break;
        case REGSET_PERFREGS:
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_GET_PERFREG, ocb->pid, ocb->tid,
                                   buf) == -1) {
                process_bind(0);
                return proc_error(errno, ocb->pid);
            }
            process_bind(0);
            *nbytes = (int) sizeof(PERF_REGISTERS);
            break;
        default:
            return proc_error(EINVAL, ocb->pid);
        }
    }
    return proc_error(EOK, ocb->pid);
}

static int procfs_regset_write(struct procfs_ocb *ocb,
                               process_t *prp,
                               unsigned regset_id,
                               void *buf,
                               int *nbytes)
{
    switch (regset_id) {
        /* The first set of registers don't need any locking */
    case REGSET_GPREGS:
        if (debug_process_call(RTMUX_DEBUG_SET_GREG, ocb->pid, 0, buf) == -1) {
            return errno;
        }
        *nbytes = (int) sizeof(CPU_REGISTERS);
        return EOK;
    default:
        if (!(prp = root_proc_lock(ocb->pid))) {
            return ESRCH;
        }
        switch (regset_id) {
        case REGSET_FPREGS:
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_SET_FPREG, ocb->pid, 0, buf) ==
                -1) {
                process_bind(0);
                return proc_error(errno, ocb->pid);
            }
            process_bind(0);
            *nbytes = (int) sizeof(FPU_REGISTERS);
            break;
        case REGSET_ALTREGS:
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_SET_ALTREG, ocb->pid, 0, buf) ==
                -1) {
                process_bind(0);
                return proc_error(errno, ocb->pid);
            }
            process_bind(0);
            *nbytes = (int) sizeof(ALT_REGISTERS);
            break;
        case REGSET_PERFREGS:
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_SET_PERFREG, ocb->pid, 0, buf) ==
                -1) {
                process_bind(0);
                return proc_error(errno, ocb->pid);
            }
            process_bind(0);
            *nbytes = 0;
            break;
        default:
            return proc_error(EINVAL, ocb->pid);
        }
    }
    return proc_error(EOK, ocb->pid);
}

static int procfs_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    union {
        int oflag;
        int mountflag;
        procfs_sysinfo sysinfo;
        procfs_info info;
        procfs_mapinfo mapinfo;
        procfs_debuginfo mapdebug;
        procfs_status status;
        procfs_run run;
        procfs_break brk;
        procfs_greg greg;
        procfs_fpreg fpreg;
        procfs_altreg altreg;
        procfs_irq irq;
        procfs_timer timer;
        procfs_regset regset;
        procfs_threadctl threadctl;
        struct sigevent event;
        uint32_t flags;
        pthread_t tid;
    } *ioctl = (void *) (msg + 1);
    int nbytes = 0;
    process_t *prp;

    /* lock_pid mirrors the pre-refactor "prp" local: NULL meant "no
     * root_proc_lock held", non-NULL meant "lock held, pass to proc_error so it
     * unlocks on the way out". Post heap-PTE_U=0 we can't deref prp, so the
     * same idea is encoded as a pid_t — 0 = no lock held, ocb->pid after
     * root_proc_lock() succeeds. Every proc_error(_, lock_pid) call below is
     * correct under both the take-lock-then-fall-through and
     * break-out-without-locking paths.
     */
    pid_t lock_pid = 0;
    int ret_val = 0;
    int combine = msg->i.combine_len & _IO_COMBINE_FLAG;

    switch (msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
    case DEVCTL_SET_FLAGS:
    case DEVCTL_GET_MOUNT_FLAGS:
        break;

    case DEVCTL_PROC_SYSINFO:
        /* syspage data is not endian-safe; the /proc client must swap!
         * [alternative is to fail this ENOTSUP as below]
         */
        break;

        /* A process is needed, we do validation elsewhere */
    case DEVCTL_PROC_INFO:
    case DEVCTL_PROC_CURTHREAD:
    case DEVCTL_PROC_GETGREG:
    case DEVCTL_PROC_GETFPREG:
    case DEVCTL_PROC_GETALTREG:
    case DEVCTL_PROC_GETREGSET:
    case DEVCTL_PROC_STATUS:
    case DEVCTL_PROC_TIDSTATUS:
    case DEVCTL_PROC_GET_BREAKLIST:
    case DEVCTL_PROC_MAPDEBUG:
    case DEVCTL_PROC_MAPDEBUG_BASE:
    case DEVCTL_PROC_MAPINFO:
    case DEVCTL_PROC_PAGEDATA:
    case DEVCTL_PROC_TIMERS:
    case DEVCTL_PROC_IRQS:
    case DEVCTL_PROC_THREADCTL: /* Verify permission later */
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            return ENOTSUP;
        }
        if (!ocb || (ocb->flags & PROCFS_FLAG_MOUNT)) {
            return EINVAL;
        }
        break;

    default:

        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            return ENOTSUP;
        }
        if (!ocb || (ocb->flags & PROCFS_FLAG_MASK)) {
            return ENOSYS;
        }
        /* We require access to be able to perform */
        if (!(ocb->ioflag & _IO_FLAG_WR)) {
            return EBADF;
        }

        break;
    }

    prp = 0;
    switch (msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        ioctl->oflag =
            ocb ? (ocb->ioflag & ~O_ACCMODE) | ((ocb->ioflag - 1) & O_ACCMODE)
                : O_RDONLY;
        nbytes = (int) sizeof ioctl->oflag;
        break;

    case DEVCTL_SET_FLAGS:
        if (ocb) {
            ocb->ioflag =
                (ocb->ioflag & ~O_SETFLAG) | (ioctl->oflag & O_SETFLAG);
        }
        break;

    case DEVCTL_GET_MOUNT_FLAGS: {
        ioctl->mountflag = 0;
        nbytes = (int) sizeof ioctl->mountflag;
        break;
    }

    case DEVCTL_PROC_SYSINFO:
        ioctl = (void *) _syspage_ptr;
        ret_val = nbytes = _syspage_ptr->total_size;
        break;

    case DEVCTL_PROC_INFO:
        if (debug_process_call(RTMUX_DEBUG_PROCESS_INFO, ocb->pid, 0,
                               (union rtmux_debug_data *) &ioctl->info) == -1) {
            return errno;
        }
        nbytes = (int) sizeof ioctl->info;
        break;

    case DEVCTL_PROC_FREEZETHREAD:
        if (debug_process_call(RTMUX_DEBUG_FREEZE, ocb->pid, ioctl->tid, 0) ==
            -1) {
            return errno;
        }
        break;

    case DEVCTL_PROC_THAWTHREAD:
        if (debug_process_call(RTMUX_DEBUG_THAW, ocb->pid, ioctl->tid, 0) ==
            -1) {
            return errno;
        }
        break;

    case DEVCTL_PROC_SET_FLAG:
        if (debug_process_call(RTMUX_DEBUG_SET_FLAG, ocb->pid, 0,
                               (union rtmux_debug_data *) &ioctl->flags) ==
            -1) {
            return errno;
        }
        break;

    case DEVCTL_PROC_CLEAR_FLAG:
        if (debug_process_call(RTMUX_DEBUG_CLEAR_FLAG, ocb->pid, 0,
                               (union rtmux_debug_data *) &ioctl->flags) ==
            -1) {
            return errno;
        }
        break;

    case DEVCTL_PROC_RUN:
        if (ioctl->run.flags & _DEBUG_RUN_ARM) {
            ioctl->run.flags &= ~(uint32_t) _DEBUG_RUN_ARM;
            ocb->rcvid = ctp->rcvid;
        }
        if (debug_process_call(RTMUX_DEBUG_RUN, ocb->pid, 0,
                               (union rtmux_debug_data *) &ioctl->run) == -1) {
            ocb->rcvid = 0;
            return errno;
        }
        break;

    case DEVCTL_PROC_CURTHREAD:
        if (ocb->ioflag & _IO_FLAG_WR) {
            if ((ret_val = debug_process_call(RTMUX_DEBUG_CURTHREAD, ocb->pid,
                                              ioctl->tid, 0)) == -1) {
                return errno;
            }
        } else {
            ocb->tid = ioctl->tid;
        }
        break;

    case DEVCTL_PROC_EVENT:
        ocb->event = ioctl->event;
        break;

    case DEVCTL_PROC_STATUS:
        ioctl->status.tid = ocb->tid; /* Set to use current thread */
        /* Fall Through */
    case DEVCTL_PROC_TIDSTATUS:
        nbytes = (int) sizeof ioctl->status;
        if (debug_process_call(
                RTMUX_DEBUG_THREAD_INFO, ocb->pid, ioctl->status.tid,
                (union rtmux_debug_data *) &ioctl->status) == -1) {
            return errno;
        }
        break;

    case DEVCTL_PROC_GETREGSET:
        if ((ret_val = procfs_regset_read(ocb, prp, ioctl->regset.id,
                                          ioctl->regset.buf, &nbytes)) != EOK) {
            return ret_val;
        }
        nbytes += (int) sizeof(ioctl->regset.id);
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_SETREGSET:
        if ((ret_val = procfs_regset_write(ocb, prp, ioctl->regset.id,
                                           ioctl->regset.buf, &nbytes)) !=
            EOK) {
            return ret_val;
        }
        nbytes += (int) sizeof(ioctl->regset.id);
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_GETGREG:
#ifndef NDEBUG
        if (sizeof *msg + sizeof(CPU_REGISTERS) > ctp->msg_max_size) {
            crash();
        }
#endif
        if ((ret_val = procfs_regset_read(ocb, prp, REGSET_GPREGS, &ioctl->greg,
                                          &nbytes)) != EOK) {
            return ret_val;
        }
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_SETGREG:
#ifndef NDEBUG
        if (sizeof *msg + sizeof(CPU_REGISTERS) > ctp->msg_max_size) {
            crash();
        }
#endif
        if ((ret_val = procfs_regset_write(ocb, prp, REGSET_GPREGS,
                                           &ioctl->greg, &nbytes)) != EOK) {
            return ret_val;
        }
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_GETFPREG:
#ifndef NDEBUG
        if (sizeof *msg + sizeof(FPU_REGISTERS) > ctp->msg_max_size) {
            crash();
        }
#endif
        if ((ret_val = procfs_regset_read(ocb, prp, REGSET_FPREGS,
                                          &ioctl->fpreg, &nbytes)) != EOK) {
            return ret_val;
        }
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_SETFPREG:
#ifndef NDEBUG
        if (sizeof *msg + sizeof(FPU_REGISTERS) > ctp->msg_max_size) {
            crash();
        }
#endif
        if ((ret_val = procfs_regset_write(ocb, prp, REGSET_FPREGS,
                                           &ioctl->fpreg, &nbytes)) != EOK) {
            return ret_val;
        }
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_GETALTREG:
#ifndef NDEBUG
        if (sizeof *msg + sizeof(ALT_REGISTERS) > ctp->msg_max_size) {
            crash();
        }
#endif
        if ((ret_val = procfs_regset_read(ocb, prp, REGSET_ALTREGS,
                                          &ioctl->altreg, &nbytes)) != EOK) {
            return ret_val;
        }
        ret_val = nbytes;
        break;

    case DEVCTL_PROC_SETALTREG:
#ifndef NDEBUG
        if (sizeof *msg + sizeof(ALT_REGISTERS) > ctp->msg_max_size) {
            crash();
        }
#endif
        if ((ret_val = procfs_regset_write(ocb, prp, REGSET_ALTREGS,
                                           &ioctl->altreg, &nbytes)) != EOK) {
            return ret_val;
        }
        ret_val = nbytes;
        break;

    default:
        if (!(prp = root_proc_lock(ocb->pid))) {
            return ESRCH;
        }
        lock_pid = ocb->pid;
        switch (msg->i.devctl_cmd) {
        case DEVCTL_PROC_THREADCTL:
            /* Reply with as much data as we can from the structure. We don't
             * read any more than the thread buffer worth for performance so
             * adjust other data as required. This will reply with the same
             * ioctl->threadctl that was passed in (ie tid/cmd in the top 8
             * bytes) due to the way the Kext call is made.
             */

            if ((unsigned) msg->i.nbytes > ctp->msg_max_size) {
                nbytes = (int) ctp->msg_max_size;
            } else {
                nbytes = msg->i.nbytes;
            }

            /* Switch access based on sub-command */
            switch (ioctl->threadctl.cmd) {
            case RTMUX_TCTL_NAME: {
                struct _thread_name *tn =
                    (struct _thread_name *) &ioctl->threadctl.data;
                size_t lenadj =
                    (size_t) ((char *) tn - (char *) msg) + sizeof(*tn);

                if ((size_t) tn->name_buf_len > (ctp->msg_max_size - lenadj)) {
                    tn->name_buf_len = (int) (ctp->msg_max_size - lenadj);
                }

                if (tn->new_name_len >= 0 && !(ocb->ioflag & _IO_FLAG_WR)) {
                    return proc_error(EBADF, lock_pid);
                }
            } break;

            case RTMUX_TCTL_RUNMASK_GET_AND_SET:
                if (*(unsigned *) ioctl->threadctl.data != 0 &&
                    (ocb->ioflag & _IO_FLAG_WR) == 0) {
                    /* Can't alter without write perms. */
                    return proc_error(EBADF, lock_pid);
                }
                break;

            case RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT: {
                unsigned *rmaskp;
                int rsize, *rsizep, i;

                if ((ocb->ioflag & _IO_FLAG_WR) != 0) {
                    break;
                }

                rsize = (int) RMSK_SIZE(_syspage_ptr->num_cpu);
                rsizep = (int *) ioctl->threadctl.data;
                rmaskp = (unsigned *) (rsizep + 1);

                /* Validate what they've passed in up to what we're interested
                 * in. The rsize they've passed in is itself validated later.
                 */
                rsize = min(rsize, *rsizep);
                rsize = max(rsize, 0);
                /* to catch both runmask and inherit mask */
                rsize *= 2;
                for (i = 0; i < rsize; i++) {
                    if (rmaskp[i] != 0) {
                        /* Can't alter without write perms. */
                        return proc_error(EBADF, lock_pid);
                    }
                }
                break;
            }

            default:
                if (!(ocb->ioflag & _IO_FLAG_WR)) {
                    return proc_error(EBADF, lock_pid);
                }
                break;
            }

            ret_val = kext_thread_ctl_call(prp, (int) ioctl->threadctl.tid,
                                           ioctl->threadctl.cmd,
                                           ioctl->threadctl.data);
            if (ret_val == -1) {
                return proc_error(errno, lock_pid);
            }
            break;

        case DEVCTL_PROC_MAPDEBUG_BASE:
            /* Return the executable path saved on root_proc(pid)->debug_name
             * (roottask side post heap-PTE_U=0; the kernel-side store on
             * prp->debug_name is gone — see kernel/include/objects.h). The
             * vaddr field is the process's text base, taken from the
             * process_query_info snapshot. /proc/<pid>/name is a wrapper around
             * the same source for toolbox-style applets.
             */
            {
                root_proc_info_t qinfo_md;
                root_process_t *tmp_md = root_proc(ocb->pid);
                size_t space;
                const char *name = tmp_md->debug_name;

                if (process_query_info(ocb->pid, &qinfo_md) != EOK) {
                    return proc_error(ESRCH, lock_pid);
                }
                ioctl->mapdebug.vaddr = qinfo_md.base_addr;
                if (!name) {
                    return proc_error(ENXIO, lock_pid);
                }
                if ((unsigned) msg->i.nbytes <=
                    offsetof(procfs_debuginfo, path)) {
                    return proc_error(EINVAL, lock_pid);
                }
                space = min(ctp->msg_max_size - sizeof(*msg),
                            (size_t) msg->i.nbytes);
                CRASHCHECK(space < offsetof(procfs_debuginfo, path));
                STRLCPY(ioctl->mapdebug.path, name,
                        space - offsetof(procfs_debuginfo, path));
                msg->o.ret_val = 0;
                SETIOV(ctp->iov + 0, &msg->o,
                       sizeof msg->o + offsetof(procfs_debuginfo, path) +
                           strlen(ioctl->mapdebug.path) + 1);
                return proc_error(IOSERV_NPARTS(1), lock_pid);
            }

        case DEVCTL_PROC_MAPDEBUG: {
            size_t space;
            if (proc_rlock_adp(ocb->pid) == -1) {
                return proc_error(ENXIO, lock_pid);
            }
            if ((unsigned) msg->i.nbytes <= offsetof(procfs_debuginfo, path)) {
                return proc_error(EINVAL, lock_pid);
            }
            space =
                min(ctp->msg_max_size - sizeof(*msg), (size_t) msg->i.nbytes);
            CRASHCHECK(space < offsetof(procfs_debuginfo, path));
            if (root_vm_ops.mapinfo(ocb->pid, prp, ioctl->mapdebug.vaddr, NULL,
                                    &ioctl->mapdebug,
                                    space - offsetof(procfs_debuginfo, path),
                                    NULL, NULL, NULL) == 0) {
                proc_unlock_adp(ocb->pid);
                return proc_error(ENXIO, lock_pid);
            }
            proc_unlock_adp(ocb->pid);
            msg->o.ret_val = 0;
            SETIOV(ctp->iov + 0, &msg->o,
                   sizeof msg->o + offsetof(procfs_debuginfo, path) +
                       strlen(ioctl->mapdebug.path) + 1);
            return proc_error(IOSERV_NPARTS(1), lock_pid);
        }

        case DEVCTL_PROC_PAGEDATA:
        case DEVCTL_PROC_MAPINFO: {
            procfs_mapinfo *p;
            int num;
            uintptr_t vaddr;
            size_t size;
            int offset;
            unsigned devctl_cmd = msg->i.devctl_cmd;

            offset = (int) sizeof msg->o;
            vaddr = 0;
            size = 0;
            if (proc_rlock_adp(ocb->pid) == -1) {
                return proc_error(ENXIO, lock_pid);
            }
            do {
                p = &ioctl->mapinfo;
                nbytes = 0;
                num = (int) ((ctp->msg_max_size - sizeof *msg) / sizeof *p);
                while (num-- && (size = root_vm_ops.mapinfo(
                                     ocb->pid, prp, vaddr, p, NULL, 0, NULL,
                                     NULL, NULL))) {
                    ret_val++;
                    nbytes += (int) sizeof *p;
                    vaddr = (uintptr_t) p->vaddr + p->size;
                    if (devctl_cmd == DEVCTL_PROC_MAPINFO) {
                        p->flags &= ~(uint32_t) MM_PG_MASK;
                    }
                    p++;
                }
                if (nbytes) {
                    /* Unlock to avoid deadlock */
                    proc_unlock_adp(ocb->pid);
                    if (ioserv_msgwrite(ctp, &ioctl->mapinfo, nbytes, offset) ==
                        -1) {
                        return proc_error(errno, lock_pid);
                    }
                    offset += nbytes;
                    if (proc_rlock_adp(ocb->pid) == -1) {
                        return proc_error(ENXIO, lock_pid);
                    }
                }
            } while (size);
            proc_unlock_adp(ocb->pid);
            nbytes = 0;
            break;
        }

        case DEVCTL_PROC_IRQS: {
            procfs_irq *p;
            struct interrupt_query_entry entry, *entryp = NULL;
            int num;
            int offset;
            unsigned id;

            offset = (int) sizeof msg->o;
            id = 0;
            do {
                p = &ioctl->irq;
                nbytes = 0;
                num = (int) ((ctp->msg_max_size - sizeof *msg) / sizeof *p);
                while (num &&
                       (entryp = query_object(_QUERY_INTERRUPT, id, 0, 0, &id,
                                              &entry, sizeof(entry)))) {
                    if (entry.pid == ocb->pid) {
                        p->pid = entry.pid;
                        p->tid = entry.tid;
                        p->handler = entry.info.handler;
                        p->area = entry.info.area;
                        p->flags = entry.info.flags;
                        p->level = entry.info.level;
                        p->mask_count = entry.info.mask_count;
                        p->id = entry.info.id;
                        p->vector = entry.vector;
                        p->event = entry.event;
                        p->fire_count = entry.info.fire_count;

                        ret_val++;
                        nbytes += (int) sizeof *p;
                        num--;
                        p++;
                    }
                    id++;
                }
                if (nbytes) {
                    if (ioserv_msgwrite(ctp, &ioctl->irq, nbytes, offset) ==
                        -1) {
                        return proc_error(errno, lock_pid);
                    }
                    offset += nbytes;
                }
            } while (entryp);
            nbytes = 0;
            break;
        }

        case DEVCTL_PROC_TIMERS: {
            procfs_timer *p;
            int num;
            int offset;
            int id;

            offset = (int) sizeof msg->o;
            id = 0;
            do {
                p = &ioctl->timer;
                nbytes = 0;
                num = (int) ((ctp->msg_max_size - sizeof *msg) / sizeof *p);
                while (num-- && ((id = __timer_info(ocb->pid, (timer_t) id,
                                                    RTMUX_TIMER_SEARCH,
                                                    &p->info)) != -1)) {
                    p->id = (timer_t) id;

                    /* Signal-with-code reporting was removed; signal delivery
                     * is reported as regular signal delivery.
                     */
                    ret_val++;
                    nbytes += (int) sizeof *p;
                    p++;
                    id++;
                }
                if (nbytes) {
                    if (ioserv_msgwrite(ctp, &ioctl->timer, nbytes, offset) ==
                        -1) {
                        return proc_error(errno, lock_pid);
                    }
                    offset += nbytes;
                }
            } while (id != -1);
            nbytes = 0;
            break;
        }

        case DEVCTL_PROC_STOP:
            if (debug_process_call(RTMUX_DEBUG_STOP, ocb->pid, 0, 0) == -1) {
                return proc_error(errno, lock_pid);
            }
            /* Fall Through */
        case DEVCTL_PROC_WAITSTOP:
            (void) procfs_waitstop(ocb->pid, ctp->rcvid);
            return proc_error(IOSERV_NOREPLY, lock_pid);

        case DEVCTL_PROC_BREAK: {
            procfs_mapinfo info;

            if (ioctl->brk.type & _DEBUG_BREAK_SOFT) {
                /* This flag is for output only to /proc user */
                return proc_error(EINVAL, lock_pid);
            }
            if ((ioctl->brk.type & _DEBUG_BREAK_MASK) != _DEBUG_BREAK_EXEC) {
                return proc_error(EINVAL, lock_pid);
            }

            /* Make sure address is valid, and get info on region */
            if (proc_rlock_adp(ocb->pid) == -1) {
                return proc_error(ENXIO, lock_pid);
            }
            if (root_vm_ops.mapinfo(ocb->pid, prp, ioctl->brk.addr, &info, NULL,
                                    0, NULL, NULL, NULL) == 0 ||
                (uintptr_t) info.vaddr > ioctl->brk.addr) {
                proc_unlock_adp(ocb->pid);
                return proc_error(ENXIO, lock_pid);
            }

            if ((_syspage_ptr->num_cpu > 1) &&
                (ioctl->brk.type & _DEBUG_BREAK_EXEC)) {
                /* If breakpoint is in a shared region, we need to fix it. */
                if ((info.flags & MAP_TYPE) == MAP_SHARED) {
                    /* If possible make a private mapping overtop of the shared
                     * mapping.
                     */
                    if ((info.flags & PROT_WRITE) ||
                        /* must upgrade to a address space write lock */
                        (proc_rlock_promote_adp(ocb->pid) == -1) ||
                        vm_shared_to_private(ocb->pid, prp, ioctl->brk.addr,
                                             __PAGESIZE) != EOK) {
                        proc_unlock_adp(ocb->pid);
                        return proc_error(EINVAL, lock_pid);
                    }
                }
            }

            /* Make sure debug call has the address space available... */
            process_bind(ocb->pid);
            if (debug_process_call(RTMUX_DEBUG_BREAK, ocb->pid, 0,
                                   (union rtmux_debug_data *) &ioctl->brk) ==
                -1) {
                ret_val = errno;
                process_bind(0);
                proc_unlock_adp(ocb->pid);
                return proc_error(ret_val, lock_pid);
            }
            process_bind(0);
            proc_unlock_adp(ocb->pid);
            nbytes = (int) sizeof ioctl->brk;
            break;
        }

        case DEVCTL_PROC_GET_BREAKLIST: {
            debug_breaklist_t breaklist;
            int offset;

            nbytes = (int) msg->i.nbytes; /* Number of bytes user has ready */
            ret_val = 0; /* Total number of breakpoints (set later) */
            offset = 0;

            do {
                breaklist.offset = offset;
                breaklist.breakpoints = &ioctl->brk;

                if (ctp->msg_max_size - sizeof(*msg) < (unsigned) nbytes) {
                    breaklist.count =
                        (int) ((ctp->msg_max_size - sizeof(*msg)) /
                               sizeof(*breaklist.breakpoints));
                } else {
                    breaklist.count =
                        nbytes / (int) sizeof(*breaklist.breakpoints);
                }

                /* Make sure we have addressing and lock down of structures we
                 * need
                 */
                if (proc_rlock_adp(ocb->pid) == -1) {
                    return proc_error(ENXIO, lock_pid);
                }
                process_bind(ocb->pid);
                if (debug_process_call(RTMUX_DEBUG_GET_BREAKLIST, ocb->pid, 0,
                                       (union rtmux_debug_data *) &breaklist) ==
                    -1) {
                    ret_val = errno;
                    process_bind(0);
                    proc_unlock_adp(ocb->pid);
                    return proc_error(ret_val, lock_pid);
                }
                process_bind(0);
                proc_unlock_adp(ocb->pid);

                /* Fill in the total number of breakpoints to return to the user
                 */
                if (offset == 0) {
                    ret_val = breaklist.total_count;
                }

                /* if we have no breakpoints to write then return */
                if (breaklist.count == 0) {
                    break;
                }

                /* Reply to the user with the data as we collect it */
                if (ioserv_msgwrite(
                        ctp, breaklist.breakpoints,
                        (int) ((unsigned) breaklist.count *
                               sizeof(*breaklist.breakpoints)),
                        (int) (sizeof(msg->o) +
                               (unsigned) offset *
                                   sizeof(*breaklist.breakpoints))) == -1) {
                    return proc_error(errno, lock_pid);
                }

                nbytes -= (int) ((unsigned) breaklist.count *
                                 sizeof(*breaklist.breakpoints));
                offset += breaklist.count;

            } while ((offset < ret_val) &&
                     (nbytes >= (int) sizeof(*breaklist.breakpoints)));

            /* We did our own msgwrites() to put out the data, just return the
             * total count
             */
            nbytes = 0;
            break;
        }

        default:
            return proc_error(ENOSYS, lock_pid);
        }
    }

    if (combine && ret_val == 0 && nbytes == 0) {
        return proc_error(EOK, lock_pid);
    }

    msg->o.ret_val = ret_val;
    if ((void *) ioctl != (void *) (msg + 1)) {
        SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o);
        SETIOV(ctp->iov + 1, ioctl, nbytes);
        return proc_error(IOSERV_NPARTS(2), lock_pid);
    } else {
        SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o + (unsigned) nbytes);
        return proc_error(IOSERV_NPARTS(1), lock_pid);
    }
}

static int procfs_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *vocb)
{
    struct procfs_ocb *ocb = vocb;
    char *path = (char *) (&msg->o + 1);
    int len;
    size_t cap;

    (void) ctp;
    if (!ocb)
        return EBADF;

    cap = (size_t) msg->i.path_len;
    memset(&msg->o, 0, sizeof msg->o);
    if (ocb->flags & PROCFS_FLAG_PROC) {
        msg->o.info.mode = S_IFDIR | 0555;
        len = snprintf(path, cap, "/proc");
    } else if (ocb->pid && (ocb->flags & PROCFS_FLAG_FD)) {
        msg->o.info.mode = S_IFDIR | 0555;
        len = snprintf(path, cap, "/proc/%d/fd", (int) ocb->pid);
    } else if (ocb->pid && !(ocb->flags & PROCFS_FLAG_MASK)) {
        msg->o.info.mode = S_IFDIR | 0555;
        len = snprintf(path, cap, "/proc/%d", (int) ocb->pid);
    } else {
        return ENOSYS;
    }
    if (len < 0)
        return EINVAL;
    _IO_SET_FDINFO_LEN(ctp, len + 1);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) min(cap, (size_t) len + 1));
}

static const ioserv_ops_t procfs_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = procfs_read,
    .write = procfs_write,
    .close_ocb = procfs_close_ocb,
    .stat = procfs_stat,
    .devctl = procfs_devctl,
    .unblock = procfs_unblock,
    .lseek = procfs_lseek,
    .fdinfo = procfs_fdinfo,
};

/* Reopen another process's descriptor: /proc/<pid>/fd/<n>.
 *
 * libc cannot do this itself (it holds no key an I/O service would accept for a
 * foreign connection), so it sends the open here. Policy: only a root-euid
 * caller on the local node is admitted; everyone else gets EACCES before the
 * target's existence is revealed. The reply is a message link carrying an
 * _IO_OPENFD that names the target's connection, plus a key minted for the
 * opener's own thread over exactly those bytes (msg_key_data CALCULATE, which
 * only roottask may do), so the service admits that one thread's one message.
 * The service still applies its own checks (no wider access than the source).
 * A second mint for the same thread replaces the first key, so the caller
 * follows each link in turn (libc does).
 * Parameters:
 *   ctp : request context (ctp->rcvid is the opener)
 *   msg : the open request, rewritten in place as the link reply
 *   pid : target process
 *   num : the "<n>" text after "/fd/"
 * Return value: the IOSERV_* reply (message link) or an errno.
 */
static int procfs_fd_reopen(ioserv_context_t *ctp,
                            io_open_t *msg,
                            pid_t pid,
                            const char *num)
{
    struct _io_connect_link_reply *linkp = (void *) msg;
    struct _server_info *info = (void *) (linkp + 1);
    io_openfd_t *new = (void *) (info + 1);
    struct _client_info ci;
    unsigned ioflag = msg->connect.ioflag;
    unsigned eflag = msg->connect.eflag;
    unsigned sflag = msg->connect.sflag;
    char *end;
    long fd;

    if (ctp->info.nd != RTMUX_LOCAL_NODE) {
        return EREMOTE;
    }
    if (connect_client_info(ctp->info.scoid, &ci, 0) == -1 ||
        ci.cred.euid != 0) {
        return EACCES;
    }
    if (*num < '0' || *num > '9') {
        return ENOENT;
    }
    errno = 0;
    fd = strtol(num, &end, 10);
    if (errno == ERANGE || *end != '\0' || fd < 0 || fd > INT_MAX) {
        return ENOENT;
    }
    if ((ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        return EEXIST;
    }
    if ((ioflag & O_TRUNC) || !(ioflag & (_IO_FLAG_RD | _IO_FLAG_WR))) {
        return EINVAL;
    }
    if (connect_server_info(pid, (uint64_t) (int) fd, info) != (int) fd) {
        return EBADF;
    }

    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_MSG);
    memset(linkp, 0x00, sizeof *linkp);
    linkp->eflag = (uint8_t) eflag;
    linkp->path_len = (uint16_t) sizeof *new;
    memset(new, 0x00, sizeof *new);
    new->i.type = _IO_OPENFD;
    new->i.combine_len = sizeof new->i;
    new->i.sflag = (uint16_t) sflag;
    new->i.ioflag = ioflag;
    new->i.xtype = _IO_OPENFD_NONE;
    new->i.info.nd = RTMUX_LOCAL_NODE;
    new->i.info.pid = pid;
    new->i.info.chid = info->chid;
    new->i.info.scoid = info->scoid;
    new->i.info.coid = (int32_t) fd;
    SETIOV(ctp->iov + 0, new, offsetof(io_openfd_t, i.key));
    if (msg_key_data_r(ctp->rcvid, RTMUX_KEYDATA_CALCULATE, 0, &new->i.key,
                       ctp->iov, 1) != EOK) {
        return EIO;
    }
    return IOSERV_PTR(ctp, msg, sizeof *linkp + sizeof *info + sizeof *new);
}

static int procfs_open(ioserv_context_t *ctp,
                       io_open_t *msg,
                       void *handle,
                       void *extra)
{
    struct procfs_ocb *ocb;
    char *path;
    pid_t pid;
    process_t *prp;
    unsigned flags;
    pid_t lock_pid;

    if (msg->connect.path_len + sizeof(*msg) >= ctp->msg_max_size) {
        return ENAMETOOLONG;
    }

    /* If opening "/proc" succed with an ocb of NULL */
    pid = 0;
    prp = 0;
    flags = 0;
    lock_pid = 0;
    if (*(path = msg->connect.path)) {
        if (!strncmp(path, "self", 4)) {
            if (RTMUX_NODE_CMP(RTMUX_LOCAL_NODE, ctp->info.nd) != 0) {
                return ENOENT;
            }
            path += 4;
            pid = ctp->info.pid;
            flags |= PROCFS_FLAG_SELF;
        } else if (!strncmp(path, "mount", 5)) {
            path += 5;
            flags |= PROCFS_FLAG_MOUNT;
        } else if (*path < '0' || *path > '9' ||
                   (pid = (pid_t) strtoul(path, &path, 10)) == 0) {
            return ENOENT;
        }

        if (pid) {
            if (!strncmp(path, "/as", 3)) {
                flags |= PROCFS_FLAG_AS;
                path += 3;
                if (*path == '/' ||
                    (*path == '\0' &&
                     (msg->connect.eflag & _IO_CONNECT_EFLAG_DIR))) {
                    return ENOTDIR;
                }
            } else if (!strncmp(path, "/name", 5)) {
                /* /proc/<pid>/name — virtual file holding the executable path
                 * saved at spawn time. Source is root_proc(pid)->debug_name;
                 * same data DEVCTL_PROC_MAPDEBUG_BASE returns, but accessible
                 * by anything that can "open(2)".
                 */
                flags |= PROCFS_FLAG_NAME;
                path += 5;
                if (*path == '/' ||
                    (*path == '\0' &&
                     (msg->connect.eflag & _IO_CONNECT_EFLAG_DIR))) {
                    return ENOTDIR;
                }
            } else if (!strncmp(path, "/fd", 3)) {
                flags |= PROCFS_FLAG_FD;
                path += 3;
                if (*path == '/') {
                    return procfs_fd_reopen(ctp, msg, pid, path + 1);
                }
                if (msg->connect.ioflag & _IO_FLAG_WR) {
                    return EISDIR;
                }
                msg->connect.mode = (msg->connect.mode & ~(uint32_t) S_IFMT) |
                                    (unsigned) S_IFDIR;
            } else if (!strncmp(path, "/wait", 5)) {
                flags |= PROCFS_FLAG_WAIT;
                path += 5;
                if (*path == '/' ||
                    (*path == '\0' &&
                     (msg->connect.eflag & _IO_CONNECT_EFLAG_DIR))) {
                    return ENOTDIR;
                }
                if (msg->connect.ioflag & _IO_FLAG_WR) {
                    return EACCES;
                }
            }
            if ((pid == ctp->info.pid) &&
                (RTMUX_NODE_CMP(RTMUX_LOCAL_NODE, ctp->info.nd)) == 0) {
                flags |= PROCFS_FLAG_SELF;
            }
            if (*path) {
                return ENOENT;
            }
            if (!(prp = root_proc_lock(pid))) {
                return ENOENT;
            }
            lock_pid = pid;
            /* This is a weak attempt to validate permissions */
            if (flags & PROCFS_FLAG_AS) {
                mode_t mode = 0;
                if (msg->connect.ioflag & _IO_FLAG_RD) {
                    mode |= S_IRUSR;
                }
                if (msg->connect.ioflag & _IO_FLAG_WR) {
                    mode |= S_IWUSR;
                }
                if (mode) {
                    struct _client_info info;
                    mode_t as_mode;
                    uid_t uid;
                    gid_t gid;

                    as_mode =
                        (mode_t) S_IFREG | (0666U & ~roottask_procfs_umask);
                    uid = 0;
                    gid = 0;
                    {
                        root_proc_info_t qinfo_a;
                        if (process_query_info(pid, &qinfo_a) == EOK) {
                            uid = qinfo_a.cred.euid;
                            gid = qinfo_a.cred.egid;
                        }
                    }
                    if (connect_client_info(ctp->info.scoid, &info,
                                            NGROUPS_MAX) != EOK ||
                        procfs_check_access(as_mode, uid, gid, mode, &info) !=
                            EOK) {
                        return proc_error(EPERM, lock_pid);
                    }
                }
            }
        } else if (flags & PROCFS_FLAG_MOUNT) {
            int ret;
            void *handlep;

            if ((ret = procfs_path_validate(ctp, msg, flags, path, &handlep)) !=
                EOK) {
                return ret;
            }
            if (S_ISDIR(msg->connect.mode)) {
                flags |= PROCFS_FLAG_DIR;
            }
            pid = (pid_t) INODE_XOR(handlep);
        } else if (*path) {
            return ENOENT;
        }
    } else {
        flags |= PROCFS_FLAG_PROC;
    }

    if (!(ocb = _scalloc(sizeof *ocb + ((path && *path) ? strlen(path) : 0)))) {
        return proc_error(ENOMEM, lock_pid);
    }

    ocb->pid = pid; /* PID value is used as inode */
    ocb->ioflag = (int) msg->connect.ioflag;
    ocb->event.sigev_notify = SIGEV_NONE;
    ocb->flags = flags;
    ocb->tail[0] = '\0';
    if (path && *path) {
        memcpy(ocb->tail, path, strlen(path) + 1);
    }

    if (ocb->ioflag & _IO_FLAG_WR) {
        struct procfs_ocb *ocb2;
        root_proc_info_t qinfo_w;

        if (!prp || !(ocb->flags & PROCFS_FLAG_AS) ||
            process_query_info(pid, &qinfo_w) != EOK ||
            (qinfo_w.flags & RTMUX_FLG_PROC_ZOMBIE)) {
            _sfree(ocb, sizeof *ocb);
            return proc_error(EINVAL, lock_pid);
        }

        for (ocb2 = (struct procfs_ocb *) qinfo_w.ocb_list; ocb2;
             ocb2 = ocb2->next) {
            if (ocb2->ioflag & _IO_FLAG_WR) {
                _sfree(ocb, sizeof *ocb);
                return proc_error(EBUSY, lock_pid);
            }
        }

        if (!(flags & PROCFS_FLAG_SELF) && pid != ROOTTASK_PID &&
            debug_attach(pid, _DEBUG_FLAG_RLC) == -1) {
            _sfree(ocb, sizeof *ocb);
            return proc_error(errno, lock_pid);
        }
    }

    if (ioserv_open_bind(ctp, ocb, 0) == -1) {
        if (ocb->ioflag & _IO_FLAG_WR) {
            CRASHCHECK(!prp);
            (void) debug_detach(pid);
        }
        _sfree(ocb, sizeof *ocb);
        return proc_error(errno, lock_pid);
    }

    if (prp && procfs_track_pid_ocb(flags)) {
        root_proc_info_t qinfo_h;
        if (process_query_info(pid, &qinfo_h) == EOK) {
            ocb->next = (struct procfs_ocb *) qinfo_h.ocb_list;
            (void) process_set_ocb_list(pid, ocb);
        }
    }

    return proc_error(EOK, lock_pid);
}

static const ioserv_connect_ops_t procfs_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = procfs_open,
};

static int notification(message_context_t *ctp,
                        int code,
                        unsigned flags,
                        void *handle)
{
    process_t *prp;
    union sigval value = ctp->msg->notification.value;

    if ((prp = root_proc_lock(value.sival_int))) {
        struct procfs_waiting *wap;
        struct procfs_ocb *ocb;
        root_process_t *tmp = root_proc(value.sival_int);
        root_proc_info_t qinfo_p;

        if (process_query_info(value.sival_int, &qinfo_p) != EOK) {
            root_proc_unlock(value.sival_int);
            return 0;
        }
        for (ocb = (struct procfs_ocb *) qinfo_p.ocb_list; ocb;
             ocb = ocb->next) {
            if (ocb->rcvid) {
                int rcvid = ocb->rcvid;

                ocb->rcvid = 0; /* clear before DeliverEvent to avoid race */
                (void) msg_deliver_event(rcvid, &ocb->event);
            }
        }
        if ((wap = tmp->debug_waiting)) {
            tmp->debug_waiting = wap->next;
            (void) procfs_waitstop((pid_t) value.sival_int, wap->rcvid);
            _sfree(wap, sizeof *wap);
        }
        root_proc_unlock(value.sival_int);
    }
    return 0;
}

void procfs_init(void)
{
    ioserv_attach(dpp, NULL, "/proc", _FTYPE_ANY, IOSERV_FLAG_DIR,
                  &procfs_connect_funcs, &procfs_io_funcs, 0);
    debug_install(notification_attach(dpp, MSG_FLAG_ALLOC_NOTIFICATION, 0,
                                      notification, NULL));
    hwres_proc_devno(_MAJOR_FSYS, &procfs_devno, -1, 0);
}
