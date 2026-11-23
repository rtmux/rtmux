/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sticky_reload: a mapping of a sticky file survives the file's reload.
 *
 * Roottask keeps the memory object of a mapped file whose mode has S_ISVTX
 * (a sticky file), and when a later mmap finds the file's mtime changed it
 * reloads the content for everyone sharing it: vm_open_fd resizes the object
 * to 0 and back. It does so holding the mapping process's address-space lock
 * and the object lock, so the growth cannot map the pages of the other
 * mappings there; vm_grow_drain does it once the locks are dropped. Before
 * that, the shrink left every page of an existing mapping marked past the end,
 * and its next access was SIGBUS.
 *
 * No file system in the test image has sticky files with a changing mtime, so
 * a child serves one (--sticky-server): every read returns the current
 * version byte, and stat reports the current mtime. A holder child maps the
 * file and reads version 'A'; the parent bumps the version to 'B' and the
 * mtime, maps the file itself (the reload), and the holder reads its page
 * again through the mapping it already had. It must see 'B' and not fault.
 * Last, the file loses S_ISVTX and is mapped once more, so roottask does not
 * keep the object (and its connection to the dead server) for the sticky
 * timeout after the test, where the leak gauges would count it.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

#define SR_PAGE 4096
#define SR_MOUNT "/k86-sticky"
#define SR_FILE SR_MOUNT "/file"
#define SR_CTL "/rtmux-sticky-reload"
#define SR_WAIT_MS 20000

/* Shared between the parent, the server and the holder. */
struct sr_ctl {
    atomic_int ready;  /* the server is attached */
    atomic_int held;   /* holder: 1 mapped and read, 2 read again */
    atomic_int go;     /* parent: the reload is done */
    atomic_int mtime;  /* what stat reports */
    atomic_int version; /* the byte every read returns */
    atomic_int seen;   /* what the holder read the second time */
    atomic_int plain;  /* stat reports the file without S_ISVTX */
};

static struct sr_ctl *sr_map_ctl(int create)
{
    int fd = shm_open(SR_CTL, O_RDWR | (create ? O_CREAT : 0), 0600);
    flock_t space;
    void *p;

    if (fd == -1)
        return NULL;
    memset(&space, 0, sizeof space);
    space.l_whence = SEEK_SET;
    space.l_len = SR_PAGE;
    if (create && fcntl(fd, F_ALLOCSP, &space) == -1) {
        (void) close(fd);
        return NULL;
    }
    p = mmap(NULL, SR_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void) close(fd);
    return p == MAP_FAILED ? NULL : p;
}

/* The server. */

static struct sr_ctl *sr_server_ctl;
static int sr_mapping_coid = -1;
static unsigned char sr_buf[SR_PAGE];

static int sr_open(ioserv_context_t *ctp, io_open_t *msg, void *handle, void *extra)
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

static int sr_close_ocb(ioserv_context_t *ctp, void *reserved, void *ocb)
{
    (void) ctp;
    (void) reserved;
    free(ocb);
    return EOK;
}

static int sr_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    (void) ocb;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.st_ino = 7;
    msg->o.st_dev = 0x4b86;
    msg->o.st_mode = S_IFREG | (atomic_load(&sr_server_ctl->plain) ? 0 : S_ISVTX) | 0644;
    msg->o.st_nlink = 1;
    msg->o.st_size = SR_PAGE;
    msg->o.st_mtime = atomic_load(&sr_server_ctl->mtime);
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static int sr_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    uint64_t pos = 0;
    size_t n = msg->i.nbytes;

    (void) ocb;
    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_OFFSET)
        pos = (uint64_t) ((struct _xtype_offset *) (&msg->i + 1))->offset;
    else if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (pos >= SR_PAGE)
        n = 0;
    else if (n > SR_PAGE - pos)
        n = (size_t) (SR_PAGE - pos);
    memset(sr_buf, atomic_load(&sr_server_ctl->version), n);
    SETIOV(ctp->iov, sr_buf, n);
    _IO_SET_READ_NBYTES(ctp, n);
    return IOSERV_NPARTS(1);
}

/* Roottask's mmap of the file: the first one binds the connection roottask
 * reads the file through; later ones name that same connection, which is how
 * roottask finds the object it already has.
 */
static int sr_mmap(ioserv_context_t *ctp, io_mmap_t *msg, void *ocb)
{
    (void) ocb;
    if (sr_mapping_coid == -1) {
        void *map_ocb = calloc(1, sizeof(int));

        if (!map_ocb)
            return ENOMEM;
        if (ioserv_open_bind(ctp, map_ocb, NULL) == -1) {
            int err = errno;

            free(map_ocb);
            return err;
        }
        sr_mapping_coid = ctp->info.coid;
    }
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.coid = sr_mapping_coid;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static const ioserv_connect_ops_t sr_connect_ops = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = sr_open,
};

static const ioserv_ops_t sr_io_ops = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .close_ocb = sr_close_ocb,
    .stat = sr_stat,
    .read = sr_read,
    .mmap = sr_mmap,
};

static int sr_server(void)
{
    dispatch_t *dpp = dispatch_create();
    dispatch_context_t *ctp;

    if (!(sr_server_ctl = sr_map_ctl(0)) || !dpp)
        return 30;
    if (ioserv_attach(dpp, NULL, SR_MOUNT, _FTYPE_ANY, IOSERV_FLAG_DIR, &sr_connect_ops,
                      &sr_io_ops, NULL) == -1)
        return 31;
    atomic_store(&sr_server_ctl->ready, 1);
    ctp = dispatch_context_alloc(dpp);
    while (ctp && (ctp = dispatch_block(ctp)))
        (void) dispatch_handler(ctp);
    return 32;
}

/* The holder. */

static int sr_hold(void)
{
    struct sr_ctl *ctl = sr_map_ctl(0);
    volatile unsigned char *p;
    int fd;

    if (!ctl)
        return 20;
    fd = open(SR_FILE, O_RDONLY);
    if (fd == -1)
        return 21;
    p = mmap(NULL, SR_PAGE, PROT_READ, MAP_SHARED, fd, 0);
    (void) close(fd);
    if (p == MAP_FAILED)
        return 22;
    if (p[0] != 'A' || p[SR_PAGE - 1] != 'A')
        return 23;
    atomic_store(&ctl->held, 1);
    if (!test_wait_flag(&ctl->go, 1, SR_WAIT_MS))
        return 24;
    atomic_store(&ctl->seen, p[SR_PAGE - 1]); /* a fault here is the failure */
    atomic_store(&ctl->held, 2);
    return 0;
}

int sticky_reload_child(const char *what)
{
    if (!strcmp(what, "server"))
        return sr_server();
    if (!strcmp(what, "hold"))
        return sr_hold();
    return 40;
}

static pid_t sr_spawn(const char *what)
{
    char *argv[] = {"syscall_testing", "--sticky-reload-child", (char *) what, NULL};
    pid_t pid;

    return posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) == 0 ? pid : -1;
}

void test_sticky_reload(void)
{
    struct sr_ctl *ctl;
    pid_t server = -1, holder = -1;
    char detail[96];
    long conn0, conn1 = -1;
    int status = 0;
    int fd;

    (void) shm_unlink(SR_CTL);
    if (!(ctl = sr_map_ctl(1))) {
        snprintf(detail, sizeof detail, "control errno %d", errno);
        test_fail("sticky reload fixture", detail);
        return;
    }
    atomic_store(&ctl->mtime, 1000);
    atomic_store(&ctl->version, 'A');
    conn0 = kstats_value("soul.connect.used");

    server = sr_spawn("server");
    if (server == -1 || !test_wait_flag(&ctl->ready, 1, SR_WAIT_MS)) {
        test_fail("sticky reload fixture", "the file server did not start");
        goto out;
    }
    holder = sr_spawn("hold");
    if (holder == -1 || !test_wait_flag(&ctl->held, 1, SR_WAIT_MS)) {
        test_fail("sticky reload fixture", "the holder did not map the file");
        goto out;
    }

    /* The file changes; the next mmap of it reloads the shared object. */
    atomic_store(&ctl->version, 'B');
    atomic_store(&ctl->mtime, 1001);
    fd = open(SR_FILE, O_RDONLY);
    if (fd == -1) {
        snprintf(detail, sizeof detail, "open errno %d", errno);
        test_fail("sticky reload remap", detail);
        goto out;
    }
    {
        void *p = mmap(NULL, SR_PAGE, PROT_READ, MAP_SHARED, fd, 0);

        (void) close(fd);
        if (p == MAP_FAILED) {
            snprintf(detail, sizeof detail, "mmap errno %d", errno);
            test_fail("sticky reload remap", detail);
            goto out;
        }
        check(((volatile unsigned char *) p)[0] == 'B', "sticky reload new mapping sees the reload",
              "a mapping made after the file changed must read the new content");
        (void) munmap(p, SR_PAGE);
    }

    atomic_store(&ctl->go, 1);
    (void) waitpid(holder, &status, 0);
    holder = -1;
    snprintf(detail, sizeof detail, "holder status %#x, read %#x", status,
             (unsigned) atomic_load(&ctl->seen));
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && atomic_load(&ctl->seen) == 'B',
          "sticky reload existing mapping reads the reloaded page", detail);

    /* No longer sticky: the last unmap lets the object go at once. */
    atomic_store(&ctl->plain, 1);
    fd = open(SR_FILE, O_RDONLY);
    if (fd != -1) {
        void *p = mmap(NULL, SR_PAGE, PROT_READ, MAP_SHARED, fd, 0);

        (void) close(fd);
        if (p != MAP_FAILED)
            (void) munmap(p, SR_PAGE);
    }

out:
    if (holder != -1) {
        (void) kill(holder, SIGKILL);
        (void) waitpid(holder, NULL, 0);
    }
    if (server != -1) {
        (void) kill(server, SIGKILL);
        (void) waitpid(server, NULL, 0);
    }
    (void) munmap(ctl, SR_PAGE);
    (void) shm_unlink(SR_CTL);

    /* Nothing of the test outlives it: roottask's object, and its connection
     * to the server, go with the last mapping once the file is not sticky.
     */
    for (int i = 0; i < 250 && (conn1 = kstats_value("soul.connect.used")) != conn0; i++)
        sleep_ms(20);
    snprintf(detail, sizeof detail, "soul.connect.used %ld before, %ld after", conn0, conn1);
    check(conn0 >= 0 && conn1 == conn0, "sticky reload leaves no connection behind", detail);
}
