/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * bootimage_init — initialize the boot image and start /rd/init.
 *
 * RTMux uses a CPIO archive. The CPIO romfs is mounted at /rd/ by
 * cpiofs_init().
 *
 * Starting /rd/init is deferred via a notification: bootimage_init() runs
 * before the dispatch loop, but open() and process startup require it to be
 * running. The notification is delivered after message_start() enters the
 * dispatch loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioserv.h>
#include <sys/path-registry.h>
#include <sys/procmsg.h>
#include "externs.h"
#include "bootstrap-manifest.h"
#include <struct.h>

/* Provided by servers/roottask/path/cpiofs.c */
int cpiofs_init(void);
int cpiofs_cpio_image(const uint8_t **base, size_t *size);
int sysfs_init(void);
int capfs_init(void);

static atomic_int bootimage_started;

/* Deferred boot-image startup, delivered as a notification once the dispatch
 * loop is running.
 *
 * Installs the cross-filesystem /etc and /home symlinks, points /dev/console at
 * the boot console, opens it for stdin/stdout/stderr, and spawns /sbin/init.
 * Parameters:
 *   ctp    : message context for the notification (unused)
 *   code   : notification code (unused)
 *   flags  : notification flags (unused)
 *   handle : notification handle (unused)
 * Return value: 0 on success (including a duplicate-delivery no-op),
 *               -1 if the worker pool reserve or console open failed.
 */
static int bootimage_start(message_context_t *ctp,
                           int code,
                           unsigned flags,
                           void *handle)
{
    int fds[3];
    pid_t pid;
    struct inheritance inherit;
    char *argv[] = {
        [0] = "init",
        [1] = NULL,
    };

    (void) ctp;
    (void) code;
    (void) flags;
    (void) handle;

    /* Guard against duplicate notification delivery (SMP race in msg_receive)
     */
    if (atomic_exchange_explicit(&bootimage_started, 1, memory_order_acq_rel))
        return 0;

    if (proc_thread_pool_reserve() != 0) {
        root_log("bootimage_start: proc_thread_pool_reserve FAILED\n");
        return -1;
    }

    /* CPIO is now mounted at / directly; the previous "/rd-to-top" bridge
     * symlinks are no longer needed (bin/, sbin/, lib/ are top-level in the
     * archive).
     *
     * /etc -> /usr/conf and /home -> /usr/home are cross-fs links (cpio root vs
     * romfs at /usr) so install them at the namespace level rather than baking
     * them into the cpio archive.
     */
    namespace_symlink("/usr/conf", "/etc");
    namespace_symlink("/usr/home", "/home");

    /* /dev/console initially points to /dev/text (kernel SBI console). After
     * dev-serial-8250 starts, "reopen /dev/ser1" updates the symlink to the
     * interrupt-driven driver.
     */
    if (namespace_symlink("/dev/text", "/dev/console") == -1)
        root_log("bootimage: WARNING: /dev/console symlink failed (%d)\n",
                 errno);

    /* Open /dev/console for stdin, stdout, stderr */
    root_log("bootimage_start: opening /dev/console\n");
    /* ipc-send: roottask-fsio -> roottask-core,logger-ring,dev-serial */
    fds[0] = open("/dev/console", O_RDWR);
    if (fds[0] == -1) {
        root_log("bootimage: cannot open /dev/console (%d)\n", errno);
        proc_thread_pool_reserve_done();
        return -1;
    }
    fds[1] = fds[2] = fds[0];

    memset(&inherit, 0, sizeof(inherit));
    inherit.flags = SPAWN_SETSID | SPAWN_TCSETPGROUP;

    pid = rtmux_spawn("/sbin/init", 3, fds, &inherit, argv, NULL);
    if (pid < 0) {
        root_log("bootimage: cannot spawn /sbin/init (pid=%d errno=%d)\n", pid,
                 errno);
    } else {
        root_log("bootimage: spawned /sbin/init (pid %d)\n", pid);
    }

    close(fds[0]);
    proc_thread_pool_reserve_done();
    return 0;
}

/* Initialize the boot-image filesystems and arrange for /sbin/init to be
 * spawned.
 *
 * Mounts the CPIO romfs (/rd) and the /sys filesystem, then attaches a
 * notification so bootimage_start() runs once the dispatch loop is up. Any
 * failure here is fatal and calls crash().
 */
void bootimage_init(void)
{
    const char *diag;
    int code;

    if (bootstrap_cap_manifest_validate(&diag) != EOK) {
        root_log("bootstrap manifest invalid: %s\n", diag ? diag : "unknown");
        crash();
    }

    if (cpiofs_init() == -1)
        crash();

    /* Load the generated per-image hashes from the boot cpio now that cpiofs
     * has mapped it, before any boot server is spawned. Fail-closed: a missing
     * or malformed table denies every path-hash grant, so treat it as fatal
     * here.
     */
    {
        const uint8_t *cpio_base;
        size_t cpio_size;

        if (cpiofs_cpio_image(&cpio_base, &cpio_size) == -1 ||
            bootstrap_cap_manifest_load_hashes(cpio_base, cpio_size) != EOK) {
            root_log("boot-manifest.hashes missing or invalid\n");
            crash();
        }
    }

    if (sysfs_init() == -1)
        crash();

    if (capfs_init() == -1)
        crash();

    code = notification_attach(dpp, MSG_FLAG_ALLOC_NOTIFICATION, 0,
                               bootimage_start, NULL);
    if (code == -1)
        crash();

    msg_send_notification(ROOTTASK_COID, PROC_INIT_PRIORITY, code, 0);
}
