/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <hwres.h>
#include <sys/ioserv.h>
#include "root-process.h"

#define HWRES_REQ_DESTROY (4 << 8)

/* Map resource type bits to the allocator list names used internally. */
static const char *rsrc_names[] = {
    "memory", "irq", "io", "dma", /*"memory/pci" */ "pcimemory", "unknown"};
static char *hwres_id2name(unsigned id)
{
    id = id & HWRES_TYPE_MASK;
    if (id >= HWRES_RESERVED_5) {
        id = HWRES_RESERVED_5;
    }
    return (char *) rsrc_names[id];
}

/* Clamp a resource-name cursor to a NUL-terminated string inside [name, end).
 * The mutating walkers advance rsrcname one name per record and read it with
 * strlen; a client can send fewer names than records, so past the last name (or
 * a truncated tail) this returns NULL and the walker falls back to
 * hwres_id2name instead of reading past the payload. name==NULL (internal
 * callers) or end==NULL is treated as "no name region".
 */
static const char *hwres_name_ok(const char *name, const char *end)
{
    if (!name || !end || name >= end ||
        !memchr(name, '\0', (size_t) (end - name)))
        return NULL;
    return name;
}


static int do_create_destroy(int (*getfunc)(hwres_range_t *alloc, pid_t pid),
                             int (*restorefunc)(hwres_range_t *alloc,
                                                pid_t pid),
                             hwres_range_t *alloc,
                             unsigned count,
                             const char *rsrcname,
                             const char *name_end,
                             pid_t pid)
{
    unsigned indx;
    int ret;

    ret = EOK;
    for (indx = 0; indx < count; indx++) {
        rsrcname = hwres_name_ok(rsrcname, name_end);

        /* Always set a safe name: a clamped (NULL) cursor must not leave the
         * client-serialized name pointer in place for the allocator to deref.
         */
        if ((alloc[indx].flags & HWRES_FLAG_NAME) && rsrcname) {
            alloc[indx].name = rsrcname;
        } else {
            alloc[indx].name = hwres_id2name(alloc[indx].flags);
        }
        if (rsrcname) {
            rsrcname += strlen(rsrcname) + 1;
        }

        ret = getfunc(&alloc[indx], pid);
        if (ret != EOK) {
            /* Roll back entries already applied in this batch. */
            while (indx-- > 0) {
                (void) restorefunc(&alloc[indx], pid);
            }
            break;
        }
    }

    /* Mark process as having some resources allocated */
    if ((ret == EOK) && (pid != 0)) {
        (void) _hwres_pid_mark(pid);
    }
    return ret;
}


static int do_detach(hwres_claim_t *request,
                     unsigned count,
                     const char *rsrcname,
                     const char *name_end,
                     pid_t pid)
{
    int indx;
    int ret;

    /* TODO: Do we need the range flag set here? */
    ret = EOK;
    for (indx = 0; indx < (int) count; indx++) {
        rsrcname = hwres_name_ok(rsrcname, name_end);
        if ((request[indx].flags & HWRES_FLAG_NAME) && rsrcname) {
            request[indx].name = rsrcname;
        } else {
            request[indx].name = hwres_id2name(request[indx].flags);
        }
        if (rsrcname) {
            rsrcname += strlen(rsrcname) + 1;
        }

        request[indx].flags &= HWRES_TYPE_MASK;
        request[indx].flags |= HWRES_FLAG_RANGE;
        ret = _hwres_detach(&request[indx], pid);
        if (ret != EOK) {
            /* Reacquire resources if this batch detach fails. */
            for (;;) {
                --indx;
                if (indx < 0)
                    break;
                (void) _hwres_attach(&request[indx], pid);
            }
            break;
        }
    }
    return ret;
}


static int do_attach(hwres_claim_t *request,
                     unsigned count,
                     const char *rsrcname,
                     const char *name_end,
                     pid_t pid)
{
    int indx;
    int bestindx;
    int bestpri;
    int startindx;
    int ret;

    ret = EOK;
    for (indx = 0; !ret && indx < (int) count; indx++) {
        startindx = bestindx = indx;

        rsrcname = hwres_name_ok(rsrcname, name_end);
        if ((request[indx].flags & HWRES_FLAG_NAME) && rsrcname) {
            request[indx].name = rsrcname;
        } else {
            request[indx].name = hwres_id2name(request[indx].flags);
        }
        if (rsrcname) {
            rsrcname += strlen(rsrcname) + 1;
        }

        if ((ret = _hwres_find(&request[indx], 0 /* Ignored */, &bestpri))) {
            bestindx = -1;
            bestpri = INT_MAX;
        }
        while (request[indx].flags & HWRES_FLAG_LIST &&
               (ret == EOK || ret == EINVAL)) {
            int newpri;

            if (indx + 1 >= (int) count) {
                ret = EINVAL;
                break;
            }

            rsrcname = hwres_name_ok(rsrcname, name_end);
            if ((request[indx + 1].flags & HWRES_FLAG_NAME) && rsrcname) {
                request[indx + 1].name = rsrcname;
            } else {
                request[indx + 1].name = hwres_id2name(request[indx + 1].flags);
            }
            if (rsrcname) {
                rsrcname += strlen(rsrcname) + 1;
            }

            if (strcmp(request[indx].name, request[indx + 1].name) != 0) {
                ret = EINVAL;
                break;
            }
            indx++;

            /* For now ignore any errors we encounter here ... */
            if (_hwres_find(&request[indx], pid, &newpri) == EOK &&
                newpri < bestpri) {
                bestpri = newpri;
                bestindx = indx;
                ret = EOK;
            }
        }

        if (ret == EOK && bestindx == -1) {
            ret = EINVAL;
        }

        if (ret == EOK) {
            request[bestindx].flags |= HWRES_FLAG_RANGE;
            ret = _hwres_remove(&request[bestindx], pid, NULL);

            while (ret == EOK && startindx <= indx) {
                memcpy(&request[startindx++], &request[bestindx],
                       sizeof(*request));
            }
        }
    }

    /* Detach resources if we fail to attach to them */
    for (indx -= 2; ret && indx >= 0; indx--) {
        if (!(request[indx].flags & HWRES_FLAG_LIST)) {
            (void) _hwres_detach(&request[indx], pid);
        }
    }

    if ((ret == EOK) && (pid != 0)) {
        (void) _hwres_pid_mark(pid);
    }
    return ret;
}

/* Entry point for roottask hardware-resource messages. All range validation is
 * done here before the request mutates the allocator lists.
 */
int hwres_handler(message_context_t *mctp,
                  int code,
                  unsigned flags,
                  void *handle)
{
    ioserv_context_t *ctp = (ioserv_context_t *) mctp;
    hwres_msg_t *msg = (hwres_msg_t *) ctp->msg;
    hwres_range_t *alloc = (hwres_range_t *) ((char *) msg + sizeof(*msg));
    hwres_claim_t *request = (hwres_claim_t *) alloc;
    char *rsrcname;
    uint64_t record_bytes = 0;
    iov_t reply_iov[2];
    int ret;
    pid_t pid;
    int cmd;


    (void) msg_info(ctp->rcvid, &ctp->info);

    /* Validate that we handle this type of message in the first place then
     * validate that the message fits entirely into the rcv buffer.
     */
    switch ((cmd = msg->i.subtype & HWRES_REQ_MASK)) {
    case HWRES_REQ_CREATE:
    case HWRES_REQ_DESTROY:
        record_bytes = (uint64_t) msg->i.count * sizeof(*alloc);
        goto check;
    case HWRES_REQ_CLAIM:
    case HWRES_REQ_RELEASE:
        record_bytes = (uint64_t) msg->i.count * sizeof(*request);
    check:
        if (!proc_isaccess(0, &ctp->info)) {
            return proc_status(ctp, EPERM);
        } else if (msg->i.nbytes == 0) {
            return proc_status(ctp, EOK);
        } else if (msg->i.nbytes >= ctp->msg_max_size - sizeof(*msg)) {
            return proc_status(ctp, EMSGSIZE);
        } else if (record_bytes >= msg->i.nbytes) {
            /* The count records precede the resource name in the nbytes
             * payload; reject a count whose records would reach or run past it.
             * Unlike the QUERY branch (which clamps count), the mutating ops
             * walk and write these records, so a wild count is an OOB access,
             * not a truncated reply. record_bytes is computed in 64-bit before
             * forming any pointer, so the count*size product cannot overflow or
             * form an out-of-bounds pointer.
             */
            return proc_status(ctp, EMSGSIZE);
        }
        /* Records are in bounds; the name(s) follow them in the payload. */
        rsrcname = (char *) alloc + record_bytes;
        break;
    case HWRES_REQ_COUNT:
    case HWRES_REQ_QUERY:
    case HWRES_REQ_QUERY_NAME:
        /* A name may follow the header. It is client-supplied and
         * strlen/strcmp'd by _hwres_query (rsrc_root_locate/rsrc_node_locate).
         * This branch is UNGATED (no proc_isaccess, unlike the mutating check:
         * block), so use the name only if it is NUL-terminated within the
         * received message; otherwise (no name, or a truncated/unterminated
         * one) fall back to the subtype name rather than reading past the
         * message. nbytes is the reply-buffer size here, not a name length, so
         * it cannot gate this.
         */
        rsrcname = (char *) hwres_name_ok((char *) msg + sizeof(msg->i),
                                          (char *) msg + ctp->info.msglen);
        if (!rsrcname) {
            rsrcname = hwres_id2name(msg->i.subtype);
        }

        /* Limit msg->i.count to our ctp buffer size. */
        {
            uint32_t maxcount = (uint32_t) ((ctp->msg_max_size - sizeof(*msg)) /
                                            sizeof(*alloc));
            if (msg->i.count > maxcount) {
                msg->i.count = maxcount;
            }
        }
        break;
    default:
        return proc_status(ctp, EINVAL);
    }

    pid = (msg->i.pid > 0) ? (pid_t) msg->i.pid : ctp->info.pid;
    if ((cmd == HWRES_REQ_CREATE || cmd == HWRES_REQ_CLAIM) &&
        pid == ctp->info.pid && root_proc(pid)->phys_contained) {
        return proc_status(ctp, EPERM);
    }
    ctp->status = ret = EOK;

    /* End of the client name region: the mutating walkers must not strlen a
     * resource name past it. (Only the mutating cmds reach here; nbytes was
     * validated in the check: block above.)
     */
    char *name_end = (char *) alloc + msg->i.nbytes;

    /* Handle all other resources types here */
    pthread_mutex_lock(&g_rsrc_mutex);
    switch (cmd) {
    case HWRES_REQ_CREATE:
        /* Permanently add to list */
        ret = do_create_destroy(_hwres_create, _hwres_destroy, alloc,
                                msg->i.count, rsrcname, name_end, pid);
        break;

    case HWRES_REQ_DESTROY:
        /* Permanently remove from list */
        ret = do_create_destroy(_hwres_destroy, _hwres_create, alloc,
                                msg->i.count, rsrcname, name_end, pid);
        break;

    case HWRES_REQ_RELEASE:
        /* Move used to unused list */
        ret = do_detach(request, msg->i.count, rsrcname, name_end, pid);
        break;

    case HWRES_REQ_CLAIM:
        /* Move unused ranges to the caller's used list. */
        ret = do_attach(request, msg->i.count, rsrcname, name_end, pid);

        if (ret == EOK) {
            SETIOV(&reply_iov[0], request, msg->i.nbytes);
            msg_replyv(ctp->rcvid, ret, reply_iov, 1);
            ret = IOSERV_NOREPLY;
        }
        break;

    case HWRES_REQ_QUERY:
        /* Convert compact query messages into the common query path. */
        msg->i.pid = (msg->i.subtype & HWRES_FLAG_USED) ? (uint32_t) -1 : 0;
        msg->i.index = msg->i.count;
        msg->i.count = msg->i.nbytes / sizeof(*alloc);
        /* The check: block clamped count to the reply-buffer capacity, but the
         * line above overwrote it from the query-branch-unvalidated nbytes.
         * Re-clamp: _hwres_query() writes up to count records into the ctp
         * buffer, so an unclamped count lets a boot state with more active
         * ranges than the buffer holds overrun it.
         */
        {
            uint32_t maxcount = (uint32_t) ((ctp->msg_max_size - sizeof(*msg)) /
                                            sizeof(*alloc));
            if (msg->i.count > maxcount) {
                msg->i.count = maxcount;
            }
        }
        msg->i.nbytes = 0;
        /* Fall through */

    case HWRES_REQ_COUNT:
    case HWRES_REQ_QUERY_NAME:
        ret = _hwres_query(rsrcname, (pid_t) msg->i.pid,
                           (cmd == HWRES_REQ_COUNT) ? FLAG_QUERY_COUNT : 0,
                           (int) msg->i.index, alloc,
                           msg->i.count * (uint32_t) sizeof(*alloc), NULL);

        if (ret >= 0) {
            SETIOV(&reply_iov[0], alloc, (size_t) ret * sizeof(*alloc));
            if (cmd == HWRES_REQ_QUERY) {
                ret = _hwres_query(rsrcname, (pid_t) msg->i.pid,
                                   FLAG_QUERY_COUNT, 0, NULL, 0, NULL);
            }

            msg_replyv(ctp->rcvid, ret, reply_iov,
                       (cmd == HWRES_REQ_COUNT) ? 0 : 1);
            ret = IOSERV_NOREPLY;
        } else {
            ret = errno;
        }
        break;
    default:
        break;
    }
    ctp->status = ret;

    pthread_mutex_unlock(&g_rsrc_mutex);

    return proc_status(ctp, ret);
}

/* PROC INTERFACE */

/* Command can be any of
 *
 * HWRES_REQ_CREATE and the private destroy command take an array of count
 * elements to place into or remove from the resource database.
 * Returns errno on failure
 *
 * HWRES_REQ_CLAIM, HWRES_REQ_RELEASE
 */
int hwres_proc_interface(void *list, unsigned count, int cmd)
{
    int ret;

    ret = ENOTSUP;
    pthread_mutex_lock(&g_rsrc_mutex);
    switch (cmd & HWRES_REQ_MASK) {
    case HWRES_REQ_CREATE:
        ret = do_create_destroy(_hwres_create, _hwres_destroy, list, count,
                                NULL, NULL, ROOTTASK_PID);
        break;
    case HWRES_REQ_DESTROY:
        ret = do_create_destroy(_hwres_destroy, _hwres_create, list, count,
                                NULL, NULL, ROOTTASK_PID);
        break;
    case HWRES_REQ_RELEASE:
        ret = do_detach(list, count, NULL, NULL, ROOTTASK_PID);
        break;
    case HWRES_REQ_CLAIM:
        ret = do_attach(list, count, NULL, NULL, ROOTTASK_PID);
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&g_rsrc_mutex);

    return ret;
}


int hwres_proc_query(hwres_range_t *list, int count, int start, int flags)
{
    int ret;

    pthread_mutex_lock(&g_rsrc_mutex);
    ret = _hwres_query(hwres_id2name((unsigned) flags), 0, 0, start, list,
                       (uint32_t) ((unsigned) count * sizeof(*list)), NULL);
    pthread_mutex_unlock(&g_rsrc_mutex);
    return ret;
}

int hwres_proc_query_pid(pid_t pid,
                         hwres_range_t *list,
                         int count,
                         int start,
                         int flags)
{
    int ret;

    pthread_mutex_lock(&g_rsrc_mutex);
    ret = _hwres_query(hwres_id2name((unsigned) flags), pid, 0, start, list,
                       (uint32_t) ((unsigned) count * sizeof(*list)), NULL);
    pthread_mutex_unlock(&g_rsrc_mutex);
    return ret;
}

/* Pass NULL in for name and we will free the devno, otherwise we will be
 * setting a minor request based on minor_request.
 *
 * Return EOK if things succeed.
 */
int hwres_proc_devno(char *name, dev_t *devno, int minor_request, int flags)
{
    int ret, major, minor;

    pthread_mutex_lock(&g_rsrc_mutex);

    major = hwres_dev_major(*devno);
    minor = name ? minor_request : hwres_dev_minor(*devno);
    ret = _rsrc_minor(name, &major, &minor,
                      name ? HWRES_REQ_CLAIM : HWRES_REQ_RELEASE);
    if (name) {
        *devno = hwres_dev_make(major, minor);
    }

    pthread_mutex_unlock(&g_rsrc_mutex);

    return ret;
}
