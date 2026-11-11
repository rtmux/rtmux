/*
 * RTMux namesrv: path resolution entry points.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include "root-process.h"

#include <sys/path-registry.h>
#include <sys/trace.h>

int namesrv_resolve_io(ioserv_context_t *ctp, struct _io_connect *connect)
{
    struct node_entry *node;
    process_t *prp;
    const char *result;
    int ret;

    if (ctp->rcvid == 0)
        return IOSERV_DEFAULT;
    if (connect->type != _IO_CONNECT)
        return ENOSYS;

    /* Large pathname continuation is still missing in the connect protocol;
     * fail fast at the namesrv boundary.
     */
    if (connect->path_len + sizeof(*connect) >= ctp->msg_max_size)
        return ENAMETOOLONG;

    ret = ENOSYS;
    ctp->status = 0;

#if defined(CONFIG_INSTRUMENTATION)
    {
        iov_t iov[3];

        SETIOV(iov, &ctp->info.pid, sizeof(ctp->info.pid));
        SETIOV(iov + 1, &ctp->info.tid, sizeof(ctp->info.tid));
        SETIOV(iov + 2, connect->path, connect->path_len);
        (void) kext_add_trace_event_iov(_TRACE_SYSTEM_C, RTMUX_TRACE_SYS_PATH,
                                        iov, 3);
    }
#endif

    switch (connect->handle) {
    case PATH_HANDLE:
        namespace_stats_roundtrip(NAMESPACE_RT_RESOLVE);
        if (!(prp = root_proc_lock(ctp->info.pid))) {
            ret = EL2HLT;
            break;
        }
        {
            root_process_t *tmp = root_proc(ctp->info.pid);
            struct node_entry *root;
            struct node_entry *start;

            root = path_node_clone(tmp->root);
            if (connect->path[0] == '/') {
                start = root;
            } else if (tmp->cwd == tmp->root) {
                start = root;
            } else {
                start = path_node_clone(tmp->cwd);
            }
            root_proc_unlock(ctp->info.pid);

            node = path_resolve_path(start, connect, connect->path, root,
                                     PATH_LOOKUP_STATS);
            ret = path_resolve_servers(ctp, node, connect, connect->path, root);
            path_node_detach(node);
            if (start != root)
                path_node_detach(start);
            path_node_detach(root);
        }
        break;

    case PATH_HANDLE_ROOT:
        namespace_stats_roundtrip(NAMESPACE_RT_ROOT_RESOLVE);
        if ((node = path_node_lookup(0, connect->path,
                                     PATH_LOOKUP_ATTACH | PATH_LOOKUP_STATS,
                                     &result))) {
            ret = path_resolve_servers(ctp, node, connect, result, 0);
            path_node_detach(node);
        } else {
            ret = ENOENT;
        }
        break;

    default:
        break;
    }

    return ret;
}

int namesrv_change_root_cwd(message_context_t *ctp, struct _io_connect *connect)
{
    struct node_entry *node;
    struct node_entry *root;
    struct node_entry *start;
    struct node_entry *tmpnode;
    struct node_entry **np;
    process_t *prp;
    int ret;

    ret = ENOSYS;
    ctp->status = 0;

    switch (connect->type) {
    case _PATH_CHDIR:
    case _PATH_CHROOT:
        namespace_stats_roundtrip(connect->type == _PATH_CHROOT
                                      ? NAMESPACE_RT_CHROOT
                                      : NAMESPACE_RT_CHDIR);
        if (!(prp = root_proc_lock(ctp->info.pid))) {
            ret = EL2HLT;
            break;
        }

        {
            root_process_t *tmp = root_proc(ctp->info.pid);

            start = (connect->path[0] == '/') ? tmp->root : tmp->cwd;
            root = tmp->root;
            (void) path_node_lookup(start, "", PATH_LOOKUP_ATTACH, 0);
            if (root != start)
                (void) path_node_lookup(root, "", PATH_LOOKUP_ATTACH, 0);
            root_proc_unlock(ctp->info.pid);

            tmpnode = path_resolve_path(start, connect, connect->path, root,
                                        PATH_LOOKUP_STATS);
            if (connect->path[0] == '\0') {
                node = tmpnode;
                tmpnode = NULL;
            } else {
                node = path_node_lookup(tmpnode, connect->path,
                                        PATH_LOOKUP_ATTACH | PATH_LOOKUP_CREATE,
                                        0);
                path_node_detach(tmpnode);
            }
            if (!node) {
                path_node_detach(start);
                if (root != start)
                    path_node_detach(root);
                ret = ENOMEM;
                break;
            }

            if (!(prp = root_proc_lock(ctp->info.pid))) {
                path_node_detach(node);
                path_node_detach(start);
                if (root != start)
                    path_node_detach(root);
                ret = EL2HLT;
                break;
            }

            tmp = root_proc(ctp->info.pid);
            np = (connect->type == _PATH_CHROOT) ? &tmp->root : &tmp->cwd;
            tmpnode = *np;
            *np = node;
            root_proc_unlock(ctp->info.pid);
            path_node_detach(tmpnode);
            path_node_detach(start);
            if (root != start)
                path_node_detach(root);
            ret = EOK;
        }
        break;

    default:
        break;
    }

    return ret;
}
