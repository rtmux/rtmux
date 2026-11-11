/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <startup.h>
#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include "root-process.h"

#include <sys/path-registry.h>

static int path_resolve_handler(ioserv_context_t *ctp, void *msg)
{
    return namesrv_resolve_io(ctp, msg);
}

static int path_msg_handler(message_context_t *ctp,
                            int code,
                            unsigned value,
                            void *handle)
{
    int ret;

    ret =
        namesrv_change_root_cwd(ctp, (struct _io_connect *) (void *) ctp->msg);
    return proc_status((ioserv_context_t *) ctp, ret);
}

void path_init(void)
{
    static ioserv_connect_ops_t path_connect_ops;
    static ioserv_ops_t path_io_ops;
    message_attr_t mattr = {0};
    ioserv_attr_t rattr = {
        .other_func = path_resolve_handler,
    };

    /* Install roottask's direct namespace link before any ioserv_attach calls
     */
    _namespace_link_hook = roottask_namespace_link;

    /* Redirect path-control messages to the namespace handler */
    message_attach(dpp, &mattr, _PATH_MSG_BASE, _PATH_MSG_MAX, path_msg_handler,
                   0);

    /* Allocate the primary pathname-resolution entry. */
    if (ioserv_attach(dpp, &rattr, "", _FTYPE_ANY, NAMESPACE_FLAG_DIR, 0, 0,
                      0) != PATH_HANDLE) {
        crash();
    }

    /* Allocate the root path-table entry. */
    if (ioserv_attach(dpp, &rattr, "", _FTYPE_ANY,
                      NAMESPACE_FLAG_OPAQUE | NAMESPACE_FLAG_DIR, 0, 0,
                      0) != PATH_HANDLE_ROOT) {
        crash();
    }

    if (hwres_proc_devno("namespace", &path_devno, -1, 0) == -1) {
        crash();
    }
    /* Make sure path_devno is not used... */
    if (path_devno == 0 &&
        hwres_proc_devno("namespace", &path_devno, -1, 0) == -1) {
        crash();
    }

    /* Initialize the path namespace operations */
    memset(&path_connect_ops, 0, sizeof(path_connect_ops));
    memset(&path_io_ops, 0, sizeof(path_io_ops));
    path_connect_ops.nfuncs = IOSERV_CONNECT_NFUNCS;
    path_io_ops.nfuncs = IOSERV_IO_NFUNCS;
    path_connect_ops.open = path_open;
    path_connect_ops.unlink = path_unlink;
    path_connect_ops.readlink = path_readlink;
    path_connect_ops.link = path_link;

    /* Put the proc symlink handler on the bottom of the stack (save the id?) */
    link_root_id = ioserv_attach(
        dpp, &rattr, "", _FTYPE_ALL,
        NAMESPACE_FLAG_OPAQUE | NAMESPACE_FLAG_DIR | NAMESPACE_FLAG_AFTER,
        &path_connect_ops, &path_io_ops, NULL);

    /* Attach to root to catch requests that do not resolve to another service.
     */
    root_id = ioserv_attach(
        dpp, &rattr, "", _FTYPE_ALL,
        NAMESPACE_FLAG_OPAQUE | NAMESPACE_FLAG_DIR | NAMESPACE_FLAG_AFTER,
        &path_connect_ops, &path_io_ops, NULL);
}

/* Direct pathname-table link used by roottask. Installed via
 * _namespace_link_hook so libc's ioserv_attach calls it instead of the
 * IPC-based default.
 */
int roottask_namespace_link(const char *path,
                            uint32_t nd,
                            pid_t pid,
                            int chid,
                            unsigned handle,
                            enum _file_type file_type,
                            unsigned flags)
{
    struct node_entry *node;
    char buff[512 + 1]; /* a large internal size... */
    int ret;

    /* Make sure proc namespace entries can be looked up by procfs. */
    flags |= NAMESPACE_FLAG_SELF;

    switch (handle) {
    case PATH_HANDLE:
        /* Nothing required for this. */
        return 0;

    case PATH_HANDLE_ROOT:
        /* Allocate a system process root. */
        {
            root_process_t *tmp = root_proc(ROOTTASK_PID);
            tmp->cwd = tmp->root = node = path_node_alloc(0, 0);
            node->parent = node;
        }
        return 0;

    default:
        break;
    }

    /* Copy path to a buffer as the passed in path is const, and the
     * path_resolve_path function will modify the path. No error checking is
     * performed, but this path mangling is done so that we remain consistent
     * with the C library function in that if a NULL path is provided. We expect
     * the user flags to be set properly.
     */
    if (path) {
        STRLCPY(buff, path, sizeof(buff));
    } else {
        buff[0] = '/';
        buff[1] = '\0';
    }

    /* Find the existing node. */
    ret = -1;
    {
        root_process_t *tmp = root_proc(ROOTTASK_PID);
        node = path_resolve_path(tmp->root, 0, buff, tmp->root, 0);
    }
    if (node) {
        struct _ioserv_link_extra extra;
        path_object_t *obp;

        extra.nd = RTMUX_LOCAL_NODE;
        extra.pid = pid ? pid : ROOTTASK_PID;
        extra.chid = chid;
        extra.handle = handle;
        extra.file_type = file_type;
        extra.flags = flags;
        obp = path_object_attach(ROOTTASK_PID, node, buff, OBJECT_SERVER, flags,
                                 &extra, NULL);
        if (obp) {
            ret = 0;
            path_object_done(obp);
        }
        path_node_detach(node);
    }
    return ret;
}
