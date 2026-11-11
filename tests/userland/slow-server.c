/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * slow-server: a mount that opens normally but holds its first two
 * DEVCTL_FS_SYNC requests for SLOW_HOLD_SEC each before answering. It lets the
 * roottask-wait-watch regression prove that the stall detector names a roottask
 * thread that is reply-blocked on a slow user server, which the heartbeat
 * alone never notices.
 *
 * A sync() makes roottask's sweep send DEVCTL_FS_SYNC to every mount from a
 * worker thread of its own. The sweep gives up on this mount after its 2 s
 * budget, but the abandoned worker stays reply-blocked until the server
 * answers: the server ignores the kernel's unblock request, as a wedged server
 * would. That worker is the pid 1 thread the scan must name.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <devctl/common.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLOW_PATH "/slow-mount"
#define SLOW_WITHHELD 2
#define SLOW_HOLD_SEC 8

static atomic_int syncs;

static int slow_open(ioserv_context_t *ctp,
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

static int slow_close_ocb(ioserv_context_t *ctp, void *reserved, void *ocb)
{
    (void) ctp;
    (void) reserved;
    free(ocb);
    return EOK;
}

/* waitfor stats the mount point, which is how init knows the mount is up. */
static int slow_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    (void) ocb;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_ino = 1;
    msg->o.st_mode = S_IFDIR | 0755;
    msg->o.st_nlink = 1;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

/* The first SLOW_WITHHELD syncs sleep in the handler, so the roottask worker
 * that sent them stays reply-blocked for SLOW_HOLD_SEC.
 */
static int slow_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *ocb)
{
    int n;

    (void) ctp;
    (void) ocb;
    if (msg->i.devctl_cmd != DEVCTL_FS_SYNC)
        return ENOSYS;
    n = atomic_fetch_add(&syncs, 1) + 1;
    if (n <= SLOW_WITHHELD) {
        printf("slow-server: WITHHOLD %d\n", n);
        fflush(stdout);
        sleep(SLOW_HOLD_SEC);
    }
    return EOK;
}

/* A wedged server never lets a timed-out client go; neither does this one. */
static int slow_unblock(ioserv_context_t *ctp,
                        io_notification_t *msg,
                        void *ocb)
{
    (void) ctp;
    (void) msg;
    (void) ocb;
    return IOSERV_NOREPLY;
}

static const ioserv_connect_ops_t connect_ops = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = slow_open,
};

static const ioserv_ops_t io_ops = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .close_ocb = slow_close_ocb,
    .stat = slow_stat,
    .devctl = slow_devctl,
    .unblock = slow_unblock,
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

int main(void)
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

    if (!dpp) {
        fprintf(stderr, "slow-server: dispatch_create: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    attr.handle = dpp;
    if (ioserv_attach(dpp, NULL, SLOW_PATH, _FTYPE_ANY, IOSERV_FLAG_DIR,
                      &connect_ops, &io_ops, NULL) == -1) {
        fprintf(stderr, "slow-server: attach %s: %s\n", SLOW_PATH,
                strerror(errno));
        return EXIT_FAILURE;
    }
    /* The mount is attached, so a sweep can reach it. Start the pool last: with
     * POOL_FLAG_EXIT_SELF the main thread exits and the pool carries on.
     */
    printf("slow-server: READY\n");
    fflush(stdout);
    pool = thread_pool_create(&attr, POOL_FLAG_EXIT_SELF);
    if (!pool || thread_pool_start(pool) != EOK) {
        fprintf(stderr, "slow-server: thread pool: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
