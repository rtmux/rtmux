/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * sync-ignore: a mount that opens normally but never answers DEVCTL_FS_SYNC.
 * It lets the shutdown-sync-ignore regression prove that roottask's sync sweep
 * gives up on a wedged server after its per-mount 2 s budget instead of hanging
 * shutdown, and then carries on to flush the healthy LKL ext4 mount the init
 * script mounted before it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <devctl/common.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/rtmux-core.h>
#include <unistd.h>

/* A new top-level node: the sweep walks the newest child first, so this mount
 * is visited before one that was mounted earlier.
 */
#define MOUNT_PATH "/sync-ignore"

static int ignore_open(ioserv_context_t *ctp,
                       io_open_t *msg,
                       void *handle,
                       void *extra)
{
    void *ocb = calloc(1, sizeof(int));

    (void) msg;
    (void) handle;
    (void) extra;
    if (!ocb)
        return ENOMEM;
    if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
        int err = errno;

        free(ocb);
        return err;
    }
    return EOK;
}

static int ignore_close_ocb(ioserv_context_t *ctp, void *reserved, void *ocb)
{
    (void) ctp;
    (void) reserved;
    free(ocb);
    return EOK;
}

/* -w: the only dispatch thread parks inside the handler, so the unblock request
 * is never received and the kernel keeps the sync client reply-blocked.
 */
static int wedge;

static int ignore_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *ocb)
{
    (void) ctp;
    (void) ocb;
    if (msg->i.devctl_cmd == DEVCTL_FS_SYNC) {
        while (wedge)
            sleep(1);
        return IOSERV_NOREPLY; /* the client stays blocked in send */
    }
    return ENOSYS;
}

/* The kernel will not time a reply-blocked client out until the server has seen
 * the unblock request, so a server that ignores only the sync still releases a
 * client whose timeout fired.
 */
static int ignore_unblock(ioserv_context_t *ctp,
                          io_notification_t *msg,
                          void *ocb)
{
    struct _msg_info info;

    (void) msg;
    (void) ocb;
    if (msg_info(ctp->rcvid, &info) != -1 &&
        (info.flags & RTMUX_MI_UNBLOCK_REQ))
        (void) msg_error(ctp->rcvid, EINTR);
    return IOSERV_NOREPLY;
}

static const ioserv_connect_ops_t connect_ops = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = ignore_open,
};

static const ioserv_ops_t io_ops = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .close_ocb = ignore_close_ocb,
    .devctl = ignore_devctl,
    .unblock = ignore_unblock,
};

static ioserv_context_t *pool_block(ioserv_context_t *ctp)
{
    return (ioserv_context_t *) dispatch_block((dispatch_context_t *) ctp);
}

static int pool_handler(ioserv_context_t *ctp)
{
    return dispatch_handler((dispatch_context_t *) ctp);
}

static ioserv_context_t *pool_alloc(dispatch_t *dpp)
{
    return (ioserv_context_t *) dispatch_context_alloc(dpp);
}

static void pool_free(ioserv_context_t *ctp)
{
    dispatch_context_free((dispatch_context_t *) ctp);
}

int main(int argc, char **argv)
{
    dispatch_t *dpp = dispatch_create();
    thread_pool_attr_t attr = {
        .block_func = pool_block,
        .handler_func = pool_handler,
        .context_alloc = pool_alloc,
        .context_free = pool_free,
        .lo_water = 1,
        .increment = 1,
        .hi_water = 2,
        .maximum = 2,
    };
    thread_pool_t *pool;

    if (argc > 1 && !strcmp(argv[1], "-w")) {
        wedge = 1;
        attr.hi_water = attr.maximum = 1;
    }
    if (!dpp) {
        fprintf(stderr, "sync-ignore: dispatch_create: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    attr.handle = dpp;
    if (ioserv_attach(dpp, NULL, MOUNT_PATH, _FTYPE_ANY, IOSERV_FLAG_DIR,
                      &connect_ops, &io_ops, NULL) == -1) {
        fprintf(stderr, "sync-ignore: attach %s: %s\n", MOUNT_PATH,
                strerror(errno));
        return EXIT_FAILURE;
    }
    /* The main thread exits and the pool threads carry on serving. */
    pool = thread_pool_create(&attr, POOL_FLAG_EXIT_SELF);
    if (!pool || thread_pool_start(pool) != EOK) {
        fprintf(stderr, "sync-ignore: thread pool: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
