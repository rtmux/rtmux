/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <limits.h>
#include <sys/rtmux-core.h>
#include "spawn-fd-policy.h"

extern int ProcessCapMint(pid_t pid, uint32_t rights, rtmux_cap_t *cap);

static int spawn_add_size(size_t *sum, size_t add)
{
    if (add > SIZE_MAX - *sum)
        return E2BIG;
    *sum += add;
    return EOK;
}


/* Mint the key that lets the loader's _IO_DUP for one parent fd pass
 * _ioserv_access. The key covers the exact bytes the loader will send and is
 * bound to the requesting loader thread, so it authorizes only that dup. Seed 0
 * makes the kernel pick a fresh random thread key, so only one entry may be
 * keyed per call (a second would overwrite the first's thread key).
 */
static int spawn_key_entry(int rcvid,
                           pid_t ppid,
                           struct _proc_spawn_fd_info *ent)
{
    io_dup_t d;
    iov_t iov;

    spawn_dup_init(&d, ppid, ent->srcnd, ent->scoid, ent->coid);
    SETIOV(&iov, &d, offsetof(struct _io_dup, key));
    return msg_key_data_r(rcvid, RTMUX_KEYDATA_CALCULATE, 0, &ent->key, &iov,
                          1);
}

/* Scramble the loader thread's message key once loading ends, so a key minted
 * for it cannot be replayed by whatever that thread runs next.
 */
static void spawn_key_revoke(int rcvid)
{
    unsigned junk;

    (void) msg_key_data_r(rcvid, RTMUX_KEYDATA_CALCULATE, 0, &junk, NULL, 0);
}

/* Does the spawn request that created this child let its fd `base` inherit the
 * parent's fd `parent_fd`? Mirrors the loader's own selection so the key
 * service cannot be asked for anything the parent's fdmap did not offer; the
 * decision lives in spawn-fd-policy.h.
 */
static int spawn_fd_permitted(struct loader_context *lcp,
                              int base,
                              int parent_fd)
{
    const int32_t *list = (const int32_t *) ((const char *) &lcp->msg.spawn.i +
                                             sizeof lcp->msg.spawn.i);
    struct _server_info si;

    switch (spawn_fd_offer(lcp->msg.spawn.i.nfds, list,
                           (uint32_t) lcp->msg.spawn.i.parms.flags, base,
                           parent_fd)) {
    case SPAWN_OFFER_YES:
        return 1;
    case SPAWN_OFFER_SCAN:
        return connect_server_info(lcp->ctx.ppid, (uint64_t) parent_fd, &si) ==
                   parent_fd &&
               spawn_fd_scan_ok(si.flags);
    default:
        return 0;
    }
}

/* get_fdinfo — gather connection info for the fds a child should inherit.
 *
 * Either walks an explicit fd list (when nfds != 0) or scans the caller's open
 * fds, resolving each to its server's connection details (nd / pid / chid /
 * coid / scoid). Skips dead and (optionally) close-on-exec descriptors.
 *
 * Parameters:
 *   rcvid      : the loader thread's request, which the entries are keyed for
 *   pid        : process whose fds are being inspected
 *   base       : starting fd index / output base index
 *   fdinfo_max : capacity of pfdinfo[]
 *   pfdinfo    : output array of per-fd connection info
 *   nfds       : count for the explicit-list case (0 = scan)
 *   pfd        : explicit fd list (used when nfds != 0)
 *   flags      : _PROC_SPAWN_FD_* flags
 * Return value: number of entries filled in, or -1 on error (errno set).
 */
static int get_fdinfo(int rcvid,
                      pid_t pid,
                      int base,
                      int fdinfo_max,
                      struct _proc_spawn_fd_info *pfdinfo,
                      int nfds,
                      int *pfd,
                      int flags)
{
    struct _server_info sinfo;
    int max = fdinfo_max;
    int fd;

    root_trace("get_fdinfo: pid=%d base=%d max=%d nfds=%d flags=0x%x\n", pid,
               base, fdinfo_max, nfds, flags);

    if (nfds != 0) {
        /* given fd list */
        for (; nfds-- && max > 0; pfd++, base++) {
            if ((fd = *pfd) != SPAWN_FDCLOSED) {
                root_trace(
                    "get_fdinfo: list fd=%d connect_server_info(pid=%d)...\n",
                    fd, pid);
                if (connect_server_info(pid, (uint64_t) fd, &sinfo) != fd ||
                    (sinfo.flags & RTMUX_FLG_COF_DEAD)) {
                    errno = EBADF;
                    return -1;
                }

                pfdinfo->fd = base;
                pfdinfo->nd = sinfo.nd;
                pfdinfo->srcnd = RTMUX_LOCAL_NODE;
                pfdinfo->pid = sinfo.pid;
                pfdinfo->chid = sinfo.chid;
                pfdinfo->scoid = sinfo.scoid;
                pfdinfo->coid = sinfo.coid;
                if ((errno = spawn_key_entry(rcvid, pid, pfdinfo)) != EOK) {
                    return -1;
                }
                pfdinfo++;
                max--;
            }
        }
    } else {
        root_trace("get_fdinfo: scanning fds for pid=%d from base=%d\n", pid,
                   base);
        while (connect_server_info(pid, (uint64_t) base, &sinfo) != -1 &&
               !((unsigned) sinfo.coid & RTMUX_SIDE_CHANNEL)) {
            root_trace(
                "get_fdinfo: found fd=%d coid=%d pid=%d chid=%d scoid=0x%x "
                "flags=0x%x\n",
                base, sinfo.coid, sinfo.pid, sinfo.chid, sinfo.scoid,
                sinfo.flags);
            if (!((sinfo.flags & RTMUX_FLG_COF_DEAD) ||
                  ((sinfo.flags & RTMUX_FLG_COF_CLOEXEC) &&
                   (flags & _PROC_SPAWN_FD_NOCLOEXC)))) {
                pfdinfo->fd = sinfo.coid;
                pfdinfo->nd = sinfo.nd;
                pfdinfo->srcnd = RTMUX_LOCAL_NODE;
                pfdinfo->pid = sinfo.pid;
                pfdinfo->chid = sinfo.chid;
                pfdinfo->coid = sinfo.coid;
                pfdinfo->scoid = sinfo.scoid;
                if ((errno = spawn_key_entry(rcvid, pid, pfdinfo)) != EOK) {
                    return -1;
                }
                pfdinfo++;
                if (--max <= 0) {
                    break;
                }
            }
            base = sinfo.coid + 1;
        }
    }

    return fdinfo_max - max;
}

/* proc_spawn — handle a _PROC_SPAWN message and its sub-protocol.
 *
 * Drives the multi-step spawn handshake by message subtype:
 *   SPAWN_START  : create the child, copy the message, start the loader
 *   SPAWN_DEBUG  : record the executable name for /proc
 *   SPAWN_FD     : return inherited-fd connection info to the loader
 *   SPAWN_ARGS   : stream argv/env from the originator to the loader
 *   SPAWN_EXEC   : perform the exec() image swap
 *   SPAWN_DONE   : finish startup and reply to the originator
 *
 * Parameters:
 *   ctp  : I/O service context (ctp->info.pid = spawning process)
 *   vmsg : the spawn message
 * Return value: a IOSERV_* status code, or an errno on failure.
 */
int proc_spawn(ioserv_context_t *ctp, void *vmsg)
{
    struct loader_context *lcp;
    struct _thread_attr attr;
    int status;
    process_t *prp;
    unsigned msgsize;
    pid_t ppid = ctp->info.pid;
    union msg_s {
        proc_spawn_t spawn;
        proc_spawn_args_t spawn_args;
        proc_spawn_debug_t spawn_debug;
        proc_spawn_fd_t spawn_fd;
        proc_spawn_done_t spawn_done;
        proc_spawn_dir_t spawn_dir;
    } *msg = vmsg;

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP16(&msg->spawn.i.subtype);
        switch (msg->spawn.i.subtype) {
        case _PROC_SPAWN_START:
            ENDIAN_SWAP16(&msg->spawn.i.searchlen);
            ENDIAN_SWAP16(&msg->spawn.i.pathlen);
            ENDIAN_SWAP32(&msg->spawn.i.parms.flags);
            ENDIAN_SWAP32(&msg->spawn.i.parms.pgroup);
            ENDIAN_SWAP32(&msg->spawn.i.parms.sigmask.bits[0]);
            ENDIAN_SWAP32(&msg->spawn.i.parms.sigmask.bits[1]);
            ENDIAN_SWAP32(&msg->spawn.i.parms.sigdefault.bits[0]);
            ENDIAN_SWAP32(&msg->spawn.i.parms.sigdefault.bits[1]);
            ENDIAN_SWAP32(&msg->spawn.i.parms.sigignore.bits[0]);
            ENDIAN_SWAP32(&msg->spawn.i.parms.sigignore.bits[1]);
            ENDIAN_SWAP32(&msg->spawn.i.parms.stack_max);
            ENDIAN_SWAP32(&msg->spawn.i.parms.policy);
            ENDIAN_SWAP32(&msg->spawn.i.parms.runmask);
            ENDIAN_SWAP32(&msg->spawn.i.parms.param.sched_priority);
            ENDIAN_SWAP32(&msg->spawn.i.parms.param.sched_curpriority);
            ENDIAN_SWAP16(&msg->spawn.i.nfds);
            ENDIAN_SWAP16(&msg->spawn.i.nargv);
            ENDIAN_SWAP16(&msg->spawn.i.narge);
            ENDIAN_SWAP32(&msg->spawn.i.nbytes);
            break;
        case _PROC_SPAWN_FD:
            ENDIAN_SWAP16(&msg->spawn_fd.i.flags);
            ENDIAN_SWAP16(&msg->spawn_fd.i.nfds);
            ENDIAN_SWAP32(&msg->spawn_fd.i.base);
            ENDIAN_SWAP32(&msg->spawn_fd.i.ppid);
            break;
        default:
            return ENOTSUP;
        }
    }

    switch (msg->spawn.i.subtype) {
    case _PROC_SPAWN_START: {
        size_t off, len;

        if (ctp->info.msglen < sizeof(struct _proc_spawn))
            return EMSGSIZE;

        off = sizeof(struct _proc_spawn);
        if (spawn_add_size(&off, msg->spawn.i.searchlen) != EOK ||
            spawn_add_size(&off, msg->spawn.i.pathlen) != EOK ||
            spawn_add_size(
                &off, (size_t) msg->spawn.i.nfds * sizeof(int32_t)) != EOK ||
            spawn_add_size(&off, msg->spawn.i.nbytes) != EOK)
            return E2BIG;
        if (off > ctp->info.srcmsglen)
            return EMSGSIZE;
        if (off > (size_t) INT_MAX)
            return E2BIG;
        msgsize = (unsigned) off;

        /* most time one msg can hold full args */
        if (msgsize > sizeof(lcp->msg)) {
            msgsize -= (unsigned) msg->spawn.i.nbytes;
            if (msgsize < sizeof(lcp->msg)) {
                msgsize = (unsigned) sizeof(lcp->msg);
            }
        }

        if (!(lcp = loader_context_alloc(
                  msgsize,
                  LC_SPAWN | (((ctp->info.flags & RTMUX_MI_ENDIAN_DIFF))
                                  ? LC_CROSS_ENDIAN
                                  : 0)))) {
            return ENOMEM;
        }

        lcp->ctx.flags = msg->spawn.i.parms.flags;
        ppid = lcp->ctx.ppid = ctp->info.pid;
        if (msgsize <= (unsigned) ctp->info.msglen) {
            memcpy(&lcp->msg, msg, msgsize);
        } else {
            memcpy(&lcp->msg, msg, (len = (size_t) ctp->info.msglen));
            (void) msg_read(ctp->rcvid, (char *) &lcp->msg + len,
                            (int) (msgsize - len), (int) len);
        }
        lcp->ctx.rcvid = ctp->rcvid;

        /* Signal mask inheritance removed — signals eliminated */
        memset(&lcp->ctx.mask, 0, sizeof(lcp->ctx.mask));

        {
            pid_t new_pid = 0;
            proc_wlock_adp(ROOTTASK_PID);
            prp = lcp->ctx.process = process_create(ppid, lcp, &new_pid);
            proc_unlock_adp(ROOTTASK_PID);
            if (prp == (void *) -1) {
                int err = errno ? errno : EAGAIN;

                root_log(
                    "proc_spawn: process_create failed for parent %d: %s\n",
                    (int) ppid, strerror(err));
                loader_context_free(lcp);
                return err;
            }
            lcp->ctx.pid = new_pid;

            /* Signal default/ignore inheritance removed — signals eliminated */

            if ((prp = root_proc_lock(ppid))) {
                root_process_t *parent_tmp = root_proc(ppid);
                root_process_t *child_tmp = root_proc(new_pid);
                child_tmp->root = path_node_clone(parent_tmp->root);
                child_tmp->cwd = path_node_clone(parent_tmp->cwd);

                root_proc_unlock(ppid);
            }
        }

        loader_thread_attr(&attr, lcp, ctp);
        if (msg->spawn.i.parms.flags & SPAWN_EXPLICIT_SCHED) {
            memcpy(&attr.sched_param, &msg->spawn.i.parms.param,
                   sizeof attr.sched_param);
            if (msg->spawn.i.parms.policy != RTMUX_SCHED_NOCHANGE) {
                attr.policy = msg->spawn.i.parms.policy;
            }
        }

        prp = lcp->ctx.process;
        root_trace("proc_spawn: SPAWN_START pid=%d loader=%p lcp=%p\n",
                   lcp->ctx.pid, loader_load, lcp);
        if ((status = thread_create_r(lcp->ctx.pid, loader_load, lcp, &attr)) <
            0) {
            /* RTMUX_FLG_PROC_TERMING set via msg_send_notification_r below;
             * writing prp->flags directly deref'd the kernel-VA process_t,
             * which faults from U-mode roottask. TODO: kext to set flag by pid;
             * for now rely on the destroy path.
             */
            (void) msg_send_notification_r(ROOTTASK_COID,
                                           attr.sched_param.sched_priority,
                                           PROC_CODE_TERM, lcp->ctx.pid);
            return -status;
        }
        return IOSERV_NOREPLY;
    }

    case _PROC_SPAWN_DEBUG: {
        void *ps_result = process_startup(ctp->info.pid, 0, 0);

        /* process_startup returns -errno cast to void* on failure (any
         * kext_process_startup error path through kcall_err). A NULL || -1
         * check misses that range, so a kernel ENOSYS (returned as 0xFF..FFA7)
         * would fall through and get dereferenced. Treat any value in [-256,
         * -1] as failure.
         */
        if (ctp->info.nd != RTMUX_LOCAL_NODE || !ps_result ||
            (uintptr_t) ps_result >= (uintptr_t) -256L) {
            break;
        }
        /* Stash the executable path in root_proc(pid)->debug_name. Used by:
         *   - DEVCTL_PROC_MAPDEBUG_BASE in servers/roottask/path/procfs.c.
         *   - /proc/<pid>/name virtual file (same source).
         *   - servers/roottask/mem/as/mapinfo.c, which stamps the
         *     mapdebug.path of the primary text/data segment.
         *
         * The kernel-side base_addr / kdebug.* writes that used to live here
         * are gone: there is no kernel debugger wired in RTMux right now.
         *
         * Return EOK directly (NOT via proc_error: that would root_proc_unlock
         * a pid we never locked, crashing on the NULL slot).
         */
        {
            const char *name = msg->spawn_debug.i.name;
            root_process_t *tmp = root_proc(ctp->info.pid);
            char *dup = strdup(name);

            if (dup) {
                free(tmp->debug_name); /* tolerates NULL on first spawn */
                tmp->debug_name = dup;
            }

            /* strdup failure is non-fatal: /proc name readers fall back to
             * "(n/a)" for this process; the spawn itself proceeds.
             */
        }
        return EOK;
    }

        /* In remote spawn, when the fd map is too big to be passed at one shot
         * by remote spawn, the remote loader will send this msg to ask for more
         * fd map. In local spawn, when fds are from another network node, the
         * loader will send this message to get fdinfo.
         */
    case _PROC_SPAWN_FD: {
        int nfds, max;

        /* Only a child's own loader may ask, only about its parent, only while
         * it is still inheriting fds, and only for one fd the parent's spawn
         * request offered. The reply names the parent's connection and carries
         * a key that authorizes a dup of it.
         */
        if (ctp->info.nd != RTMUX_LOCAL_NODE)
            return EACCES;
        lcp = process_startup(ctp->info.pid, 0, 0);
        if (!lcp || (uintptr_t) lcp >= (uintptr_t) -256L ||
            lcp->ctx.ppid != msg->spawn_fd.i.ppid)
            return EACCES;
        if (msg->spawn_fd.i.flags & _PROC_SPAWN_FD_REVOKE) {
            spawn_key_revoke(ctp->rcvid);
            return EOK;
        }
        if (!spawn_fd_request_ok(lcp->ctx.fds_done, msg->spawn_fd.i.flags,
                                 msg->spawn_fd.i.nfds, ctp->info.msglen) ||
            !spawn_fd_permitted(lcp, msg->spawn_fd.i.base,
                                *(int32_t *) ((char *) &msg->spawn_fd +
                                              sizeof msg->spawn_fd.i)))
            return EACCES;

        root_trace(
            "SPAWN_FD: msg.ppid=%d msg.nfds=%d msg.base=%d msg.flags=0x%x\n",
            msg->spawn_fd.i.ppid, msg->spawn_fd.i.nfds, msg->spawn_fd.i.base,
            msg->spawn_fd.i.flags);

        nfds = 0;
        max = msg->spawn_fd.i.nfds;
        ctp->iov[1].iov_base =
            (char *) &msg->spawn_fd.i + sizeof msg->spawn_fd.i;
        if (msg->spawn_fd.i.flags & _PROC_SPAWN_FD_LIST) {
            nfds = max;
            ctp->iov[1].iov_base = (char *) ctp->iov[1].iov_base +
                                   (unsigned) nfds * sizeof(int32_t);
        }
        /* One entry per call: each key overwrites the thread's key. */
        max = min(
            max, (int) ((ctp->msg_max_size - ((uintptr_t) ctp->iov[1].iov_base -
                                              (uintptr_t) &msg->spawn_fd)) /
                        sizeof(struct _proc_spawn_fd_info)));
        max = min(max, 1);
        nfds = get_fdinfo(
            ctp->rcvid, msg->spawn_fd.i.ppid, msg->spawn_fd.i.base, max,
            ctp->iov[1].iov_base, nfds,
            (int *) ((char *) &msg->spawn_fd + sizeof msg->spawn_fd.i),
            msg->spawn_fd.i.flags);
        if (nfds == -1) {
            return errno;
        }
        msg->spawn_fd.o.flags = 0;
        if (nfds >= max) {
            msg->spawn_fd.o.flags = _PROC_SPAWN_FDREPLY_MORE;
        }
        msg->spawn_fd.o.nfds = (uint16_t) nfds;

        ctp->iov[0].iov_base = (char *) &msg->spawn_fd.o;
        ctp->iov[0].iov_len = sizeof msg->spawn_fd.o;
        ctp->iov[1].iov_len =
            (unsigned) nfds * sizeof(struct _proc_spawn_fd_info);
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            ENDIAN_SWAP16(&msg->spawn_fd.o.flags);
            ENDIAN_SWAP16(&msg->spawn_fd.o.nfds);
        }
        return IOSERV_NPARTS(2);
    }

    case _PROC_SPAWN_ARGS:
        if (ctp->info.nd == RTMUX_LOCAL_NODE &&
            (lcp = process_startup(ctp->info.pid, 0, 0)) &&
            (uintptr_t) lcp < (uintptr_t) -256L) {
            void *buff = &msg->spawn_args.i + sizeof msg->spawn_args.i;
            unsigned off = 0;
            unsigned len;

            while (msg->spawn_args.i.nbytes) {
                len = min(msg->spawn_args.i.nbytes, 512u);
                if (msg_read(lcp->ctx.rcvid, buff, (int) len,
                             (int) msg->spawn_args.i.offset) == -1) {
                    return errno;
                }
                if (msg_write(ctp->rcvid, buff, (int) len, (int) off) == -1) {
                    return errno;
                }
                off += len;
                msg->spawn_args.i.offset += len;
                msg->spawn_args.i.nbytes -= len;
            }
            return EOK;
        }
        break;

    case _PROC_SPAWN_EXEC:
        root_trace("SPAWN_EXEC: pid=%d nd=%d\n", ctp->info.pid, ctp->info.nd);
        if (ctp->info.nd != RTMUX_LOCAL_NODE) {
            root_trace("SPAWN_EXEC: non-local nd=%d, breaking\n", ctp->info.nd);
            break;
        }
        spawn_key_revoke(ctp->rcvid);
        lcp = process_startup(ctp->info.pid, 0, 0);
        root_trace("SPAWN_EXEC: process_startup(%d) returned %p\n",
                   ctp->info.pid, lcp);
        if (!lcp || (uintptr_t) lcp >= (uintptr_t) -256L) {
            root_trace("SPAWN_EXEC: process_startup FAILED\n");
            break;
        }
        lcp->ctx.fds_done = 1;
        root_trace(
            "SPAWN_EXEC: lcp=%p state=0x%x flags=0x%lx ppid=%d rcvid=%d\n", lcp,
            lcp->ctx.state, lcp->ctx.flags, lcp->ctx.ppid, lcp->ctx.rcvid);
        lcp->ctx.state = LC_EXEC_SWAP;
        if (lcp->ctx.flags & SPAWN_NOZOMBIE) {
            root_trace("SPAWN_EXEC: NOZOMBIE path, ppid=%d\n", lcp->ctx.ppid);
            proc_nozombie(lcp->ctx.ppid, EOK);
        }
        {
            int rcvid = lcp->ctx.rcvid;

            root_trace(
                "SPAWN_EXEC: calling process_exec lcp=%p rcvid=%d "
                "ctp->rcvid=%d "
                "ppid=%d\n",
                lcp, rcvid, ctp->rcvid, lcp->ctx.ppid);
            if (process_exec(lcp, &lcp->ctx.rcvid, ctp->rcvid, lcp->ctx.ppid) ==
                -1) {
                msg_error(ctp->rcvid, errno);
                return IOSERV_NOREPLY;
            }

            msg_error(rcvid, EOK);
        }
        return IOSERV_NOREPLY;

    case _PROC_SPAWN_DONE: {
        uint64_t caps = 0;

        root_trace("proc_spawn: SPAWN_DONE from pid=%d\n", ctp->info.pid);
        if (ctp->info.nd != RTMUX_LOCAL_NODE) {
            break;
        }
        spawn_key_revoke(ctp->rcvid);
        process_bind(ctp->info.pid);

        /* Commit the child's bootstrap capabilities. The loader resolved them
         * from the absolute path it opened and stashed them on the loader
         * context (see proc-loader.c), so the grant binds to the trusted
         * read-only image rather than a caller-supplied name. Fetch the context
         * to read that grant, then start the child with it. The kernel rechecks
         * the grant against roottask's own caps at the start commit; an
         * unlisted image gets none.
         */
        lcp = process_startup(ctp->info.pid, 0, 0);
        if (lcp && (uintptr_t) lcp < (uintptr_t) -256L) {
            lcp->ctx.fds_done = 1;
            caps = lcp->ctx.grant_caps;
            root_proc(ctp->info.pid)->system = lcp->ctx.grant_system != 0;
        }
        lcp = process_startup(ctp->info.pid, 1, caps);
        process_bind(0);
        if (!lcp || (uintptr_t) lcp >= (uintptr_t) -256L) {
            break;
        }
        /* SignalProcmask removed - signals eliminated */
        if (lcp->ctx.flags & SPAWN_PROC_CAP) {
            rtmux_cap_t pcap = 0;

            /* Mint a process cap over the child into this proc service's own
             * endpoint table, then carry it to the parent as an _IO_NATIVE
             * reply transfer; msg_replyv installs it into the parent's table
             * and rewrites the id to the parent-local handle. The reply status
             * stays the child pid, so a caller that did not set SPAWN_PROC_CAP
             * sees no change. The transfer is a copy, so this service keeps its
             * own copy; that copy is revoked when the child dies (the process
             * teardown fan-out walks every table), so it is bounded, not
             * leaked.
             */
            if (ProcessCapMint(ctp->info.pid, RTMUX_CAP_PROCESS_ALL, &pcap) ==
                EOK) {
                struct _io_native_hdr hdr;
                iov_t riov;

                memset(&hdr, 0, sizeof hdr);
                hdr.msg.type = _IO_MSG;
                hdr.msg.protocol = _IOMSG_RTMUX_NATIVE;
                hdr.version = _IO_NATIVE_VERSION;

                /* A reply that carries a payload has its positive status
                 * clamped to the reply length, so the child pid cannot ride the
                 * status here; carry it in the payload instead and reply status
                 * EOK.
                 */
                hdr.request_id = (uint64_t) ctp->info.pid;
                hdr.nxfers = 1;
                hdr.xfers[0].id = pcap;
                hdr.xfers[0].rights = RTMUX_CAP_PROCESS_ALL;

                /* Best-effort: if the parent table is full the cap is dropped
                 * and the reply still delivers the pid, never an error.
                 */
                hdr.xfers[0].flags = _IO_NATIVE_CAP_XFER_OPTIONAL;
                SETIOV(&riov, &hdr, sizeof hdr);
                msg_replyv(lcp->ctx.rcvid, EOK, &riov, 1);
                loader_context_free(lcp);
                return EOK;
            }

            /* Mint failed (no cap created): fall through to the plain reply, so
             * the parent still gets the child pid, just without a cap.
             */
        }
        msg_replyv(lcp->ctx.rcvid, ctp->info.pid, 0, 0);
        loader_context_free(lcp);
        return EOK;
    }

    default:
        break;
    }
    return ENOSYS;
}
