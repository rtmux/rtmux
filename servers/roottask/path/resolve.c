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
#include <sys/path-registry.h>

struct node_entry *path_resolve_path(struct node_entry *node,
                                     struct _io_connect *connect,
                                     char *path,
                                     struct node_entry *root,
                                     unsigned extra_lookup_flags)
{
    register char *p1, *p2;
    register struct node_entry *locked_nop;
    register unsigned eflag;
    int uncacheable_acc;

    eflag = 0;
    p2 = 0;
    p1 = path;
    locked_nop = 0;
    uncacheable_acc = 0;

    for (;;) {
        /* remove extra slashes */
        if (p1[0] == '/') {
            eflag |= _IO_CONNECT_EFLAG_DIR;
            eflag &=
                ~(unsigned) (_IO_CONNECT_EFLAG_DOT | _IO_CONNECT_EFLAG_DOTDOT);
            while (p1[0] == '/') {
                p1++;
            }
        }
        /* check for dot and dotdot */
        if (p1[0] == '.') {
            if (p1[1] == '.') {
                if (p1[2] == '/' || p1[2] == '\0') {
                    /* dotdot so go back a directory */
                    if (p2) {
                        /* go back through path string */
                        p2 -= 2;
                        while (p2[-1] != '/') {
                            if (p2-- == path) {
                                p2 = 0;
                                break;
                            }
                        }
                    } else {
                        /* if the process root is not the node root, don't allow
                         * escaping
                         */
                        if (node != root) {
                            /* go back through nodes */
                            node = node->parent;
                        }
                    }
                    p1 += 2;
                    eflag |= _IO_CONNECT_EFLAG_DIR | _IO_CONNECT_EFLAG_DOT |
                             _IO_CONNECT_EFLAG_DOTDOT;
                    continue;
                }
            } else if (p1[1] == '/' || p1[1] == '\0') {
                /* found dot so remove it */
                p1++;
                eflag |= _IO_CONNECT_EFLAG_DIR | _IO_CONNECT_EFLAG_DOT;
                continue;
            }
        }

        if (*p1) {
            /* more to the path, doesn't end in dot or slash */
            eflag &=
                ~(unsigned) (_IO_CONNECT_EFLAG_DIR | _IO_CONNECT_EFLAG_DOT |
                             _IO_CONNECT_EFLAG_DOTDOT);
            if (!p2) {
                register struct node_entry *n;

                if ((n = path_node_lookup(
                         node, p1, PATH_LOOKUP_PRUNE | PATH_LOOKUP_ATTACH,
                         0))) {
                    /* Sticky uncacheable across the per-component walk. Counted
                     * exactly once at function exit (see below) so
                     * path_resolve_path matches path_open and PATH_HANDLE_ROOT,
                     * which already counts once per root-table resolve via the
                     * single-call lookup.
                     *
                     * The n->object check is an unlocked fast-path: n is
                     * link-bumped so it can't disappear, the head pointer is
                     * naturally aligned, and a missed concurrent attach just
                     * shows up on the next lookup -- this accounting is
                     * advisory, not security-load-bearing. Skipping the
                     * path_mutex round-trip on object-free intermediate
                     * components matters under SMP contention.
                     */
                    if (!uncacheable_acc && n->object) {
                        path_node_access(n);
                        uncacheable_acc = path_node_is_uncacheable(n);
                        path_node_complete(n);
                    }
                    if (locked_nop) {
                        /* release the previous locked nop */
                        path_node_detach(locked_nop);
                    }
                    locked_nop = node = n;
                    p1 += node->len;
                } else {
                    p2 = path;
                }
            }
        }

        if (p2) {
            /* write path string for component */
            while ((p2[0] = p1[0])) {
                p2++;
                if (p2[-1] == '/') {
                    break;
                }
                p1++;
            }
        }

        if (p1[0] == '\0') {
            break;
        }
    }
    if (p2) {
        /* if path ends in a slash, flag it and remove the slash */
        if (p2[-1] == '/') {
            eflag |= _IO_CONNECT_EFLAG_DIR;
            *--p2 = '\0';
        }
    } else {
        /* everythins was in the nodes */
        *path = '\0';
    }
    if (connect) {
        connect->eflag |= (uint8_t) eflag;
    }

    if (node != locked_nop) {
        /* Empty path: the helper's per-component loop never runs, so this is
         * bookkeeping (links++ under the chain mutex), not a real cache event.
         * Do not pass extra_lookup_flags -- including STATS would over-report
         * cache_misses whenever a resolve backtracked via '..' past the last
         * locked node.
         */
        (void) path_node_lookup(node, "", PATH_LOOKUP_ATTACH, 0);

        if (locked_nop) {
            path_node_detach(locked_nop);
        }
    }

    /* One stat event per external resolve, matching path_open and the
     * PATH_HANDLE_ROOT single-call path. uncacheable_acc was tainted by the
     * per-component walk above if any traversed node was marked UNCACHEABLE.
     */
    if (extra_lookup_flags & PATH_LOOKUP_STATS)
        namespace_stats_lookup(uncacheable_acc);

    /* at this point, "node" has the lowest node, and "path" has the remaining
     * path without dots or extra slashes. connect->eflag is set correctly
     */
    return node;
}

#define _PMFLAG_SEEN_PATH_SERVICE 0x0001
#define _PMFLAG_SEEN_ROOT 0x0002
#define _PMFLAG_SEEN_OPAQUE 0x0004
#define _PMFLAG_IS_LINK 0x0008
int path_resolve_servers(ioserv_context_t *ctp,
                         struct node_entry *node,
                         struct _io_connect *connect,
                         const char *path,
                         struct node_entry *root)
{
    struct node_entry *n;
    char *reply;
    struct _io_connect_entry *server, *s;
    unsigned len;
    unsigned pos;
    unsigned eflag;
    unsigned local_flags;
    struct _io_connect_link_reply *linkp;
    unsigned file_type;
    ptrdiff_t server_max_space;
    process_t *prp;
    unsigned ret;

    eflag = connect->eflag;
    file_type = connect->file_type;
    local_flags = 0;

    if (connect->type == _IO_CONNECT && connect->subtype == _IO_CONNECT_LINK &&
        connect->extra_type == _IO_CONNECT_EXTRA_SERVICE_LINK) {
        local_flags |= _PMFLAG_IS_LINK;
    }

    for (len = 1, n = node; n != n->parent; n = n->parent) {
        len += n->len + 1;
    }
    reply = (char *) connect +
            ((offsetof(struct _io_connect, path) + connect->path_len +
              connect->extra_len + len + _MALLOC_ALIGN - 1) &
             ~(_MALLOC_ALIGN - 1));

    {
        char *end_reply = (char *) reply + connect->reply_max;
        char *end_msg = (char *) connect + ctp->msg_max_size;
        server_max_space =
            (end_reply < end_msg ? end_reply : end_msg) - (char *) reply;
    }

    if (server_max_space <=
        0) { /* Early check to see if we don't have enough space */
        return ENAMETOOLONG;
    }

    s = server = (struct _io_connect_entry *) reply;
    linkp = (struct _io_connect_link_reply *) (void *) connect;

    if (path[0]) {
        *--reply = '/';
    } else {
        len--;
    }
    pos = len;
    linkp->path_len = (uint16_t) (strlen(path) + len + 1);

    linkp->eflag = (uint8_t) eflag;
    linkp->nentries = 0;
    linkp->chroot_len = 0;

    if (proc_thread_pool_reserve() != 0) {
        return EAGAIN;
    }

    for (n = node;; n = n->parent) {
        path_object_t *o;

        path_node_access(n);

        /* A path component still remains at the deepest resolved node: if that
         * node is a non-directory endpoint — a device leaf carrying only
         * non-DIR OBJECT_SERVER object(s) and no children — the client is
         * trying to walk through a file, so the result is ENOTDIR rather than
         * the ENOENT the root catch-all would otherwise emit. A
         * directory-flagged mount (NAMESPACE_FLAG_DIR) still forwards its
         * remaining path unchanged. Skipped during symlink resolution to leave
         * that path intact.
         */
        if (n == node && path[0] != '\0' && !n->child &&
            !(local_flags & _PMFLAG_IS_LINK)) {
            int nondir_server = 0, other = 0;

            for (o = n->object; o; o = o->hdr.next) {
                /* A non-DIR server is a device leaf; anything else — a DIR
                 * mount, a symlink, a name redirect, a memory object — might
                 * still resolve the remaining path, so only fail closed when
                 * the node is purely non-directory device endpoint(s).
                 */
                if (o->hdr.type == OBJECT_SERVER &&
                    !(o->hdr.flags & NAMESPACE_FLAG_DIR))
                    nondir_server = 1;
                else
                    other = 1;
            }
            if (nondir_server && !other) {
                path_node_complete(n);
                proc_thread_pool_reserve_done();
                return ENOTDIR;
            }
        }

        for (o = n->object; o; o = o->hdr.next) {
#ifndef NDEBUG
            if (!n->object) {
                crash();
            }
#endif

            /* Only the path service resolves after an OPAQUE entry, and only
             * one path-service entry should be reported or we get duplicates.
             */
            if (local_flags & _PMFLAG_SEEN_OPAQUE) {
                if (o->hdr.type != OBJECT_SERVER ||
                    (local_flags & _PMFLAG_SEEN_PATH_SERVICE) ||
                    o->server.pid != PATH_PID || o->server.handle != root_id) {
                    continue;
                }
            }

            if (o->hdr.type == OBJECT_SERVER) {
                if ((!(local_flags & _PMFLAG_IS_LINK) ||
                     o->server.pid == PATH_PID) &&
                    (path[0] == '\0' || (o->hdr.flags & NAMESPACE_FLAG_DIR)) &&
                    (o->server.pid != ctp->info.pid ||
                     RTMUX_NODE_CMP(o->server.nd, ctp->info.nd) ||
                     (o->hdr.flags & NAMESPACE_FLAG_SELF))) {
                    if ((eflag & _IO_CONNECT_EFLAG_DIR) &&
                        !(o->hdr.flags & NAMESPACE_FLAG_DIR)) {
                        path_node_complete(n);
                        proc_thread_pool_reserve_done();
                        return ENOTDIR;
                    }
                    /* Check to see if we have the right file_type info */
                    if (o->hdr.flags & NAMESPACE_FLAG_FTYPEONLY &&
                        o->server.file_type != file_type) {
                        continue;
                    }

                    if (server_max_space < (int) sizeof(*s)) {
                        path_node_complete(n);
                        proc_thread_pool_reserve_done();
                        return ENAMETOOLONG;
                    }
                    server_max_space -= (int) sizeof(*s);

                    if (o->hdr.flags & NAMESPACE_FLAG_OPAQUE) {
                        local_flags |= _PMFLAG_SEEN_OPAQUE;
                    }
                    linkp->nentries++;

                    if (RTMUX_NODE_CMP(ctp->info.nd, RTMUX_LOCAL_NODE) == 0) {
                        s->nd = o->server.nd;
                    } else if (RTMUX_NODE_CMP(ctp->info.nd, o->server.nd) ==
                               0) {
                        s->nd = RTMUX_LOCAL_NODE;
                    } else {
                        s->nd = o->server.nd;
                    }
                    s->pid = o->server.pid;
                    s->chid = o->server.chid;
                    s->handle = o->server.handle;
                    s->file_type = o->server.file_type;
                    s->prefix_len = (uint16_t) pos;
                    s->key = 0;
                    s++;
                }
            }
            /* Always replace the last symlink as we climb up the tree */
            else {
                if (server_max_space < (int) sizeof(*s)) {
                    path_node_complete(n);
                    proc_thread_pool_reserve_done();
                    return ENAMETOOLONG;
                }
                server_max_space -= (int) sizeof(*s);

                /* Proc symlinks are like any other OPAQUE entry, except for the
                 * fact that they re-direct to the link_root_id handle of the
                 * path service. OPAQUE entries stop all entries underneath them
                 * (except the path service) from being reported.
                 */
                local_flags |= _PMFLAG_SEEN_OPAQUE;
                linkp->nentries++;

                /* seen_path_service = (o->hdr.type == OBJECT_PROC_SYMLINK) ?
                 * seen_path_service : 1;
                 */
                if (o->hdr.type != OBJECT_PROC_SYMLINK) {
                    local_flags |= _PMFLAG_SEEN_PATH_SERVICE;
                }

                s->nd = ctp->info.srcnd;
                s->pid = PATH_PID;
                s->chid = PATH_CHID;
                s->file_type = (uint32_t) file_type;
                s->handle = (uint32_t) ((o->hdr.type == OBJECT_PROC_SYMLINK)
                                            ? link_root_id
                                            : root_id);
                s->prefix_len = 1;
                s->key = 0;
                s++;
            }
        }

        path_node_complete(n);

        if (n == root) {
            local_flags |= _PMFLAG_SEEN_ROOT;
        }

        if (n->len) {
            reply -= n->len;
            memcpy(reply, n->name, n->len);
            *--reply = '/';
            pos -= n->len;
            if (n != node || path[0]) {
                pos--;
            }
            if (local_flags & _PMFLAG_SEEN_ROOT) {
                linkp->chroot_len += (uint16_t) (n->len + 1);
            }
        }

        if (n == n->parent) {
            break;
        }
    }

    proc_thread_pool_reserve_done();

    ret = _IO_CONNECT_RET_LINK | _IO_CONNECT_RET_CHROOT;
    if ((prp = root_proc_lock(ctp->info.pid))) {
        root_proc_info_t info;

        linkp->umask = root_proc(ctp->info.pid)->umask;
        ret |= _IO_CONNECT_RET_UMASK;

        /* Session-leader test for "no controlling tty". Pre-PTE_U=0 this was
         * the literal "prp->session && prp->session->leader == prp->pid &&
         * prp->session->fd == -1" deref chain — three kernel-heap loads in a
         * row. process_query_info copies the three session fields into a U-mode
         * buffer in one kernel extension.
         */
        if (process_query_info(ctp->info.pid, &info) == EOK &&
            info.sess_leader != 0 && info.sess_leader == ctp->info.pid &&
            info.sess_fd == -1) {
            ret |= _IO_CONNECT_RET_NOCTTY;
        }
        root_proc_unlock(ctp->info.pid);
    }
    _IO_SET_CONNECT_RET(ctp, ret);

    SETIOV(ctp->iov + 0, linkp, sizeof *linkp);
    SETIOV(ctp->iov + 1, server, sizeof *server * linkp->nentries);
    SETIOV(ctp->iov + 2, reply, len);
    SETIOV(ctp->iov + 3, path, linkp->path_len - len);
    return IOSERV_NPARTS(4);
}
