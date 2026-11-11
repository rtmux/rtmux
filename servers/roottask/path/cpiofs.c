/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * cpiofs - read-only I/O service for the CPIO ramdisk.
 *
 * Mounts the in-memory CPIO archive (loaded by the kernel from the "ramdisk"
 * asinfo entry) at /. Files are read directly from the archive data in physical
 * memory — no copying needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <align.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/cpio.h>
#include <sys/stat.h>
#include <sys/iomsg.h>
#include <sys/mount.h>
#include <sys/syspage.h>
#include <sys/mman.h> /* mmap, MAP_PHYS|MAP_SHARED, NOFD */
#include <sys/pool-canary.h>
#include <devctl/common.h>
#include "externs.h"
#include "pinode.h"

/* CPIO archive location (filled in by cpiofs_init) */
static const uint8_t *cpio_base;
static size_t cpio_size;
static dev_t cpiofs_devno;

/* Device number of the read-only boot cpio, resolved from stat.st_rdev on any
 * file this service serves. The loader uses it to bind a bootstrap capability
 * grant to the boot image specifically, not to any read-only filesystem. Zero
 * until cpiofs_start registers the device.
 */
dev_t cpiofs_boot_devno(void)
{
    return cpiofs_devno;
}

/* Expose the in-memory boot cpio so roottask-internal code can read a boot file
 * directly (no IPC), e.g. the generated boot-manifest.hashes.
 *
 * Returns 0 and sets base and size once cpiofs_init has mapped the archive, -1
 * before that.
 */
int cpiofs_cpio_image(const uint8_t **base, size_t *size)
{
    if (!cpio_base || cpio_size == 0)
        return -1;
    *base = cpio_base;
    *size = cpio_size;
    return 0;
}

/* per-open state Both buffers below hold paths — bound by the system's PATH_MAX
 * rather than a magic literal. POSIX guarantees at least 256; our libc defines
 * PATH_MAX = 1024 (include/limits.h).
 */
#define CPIOFS_PREFIX_MAX PATH_MAX
#define CPIOFS_SYMLINK_MAX PATH_MAX

struct cpiofs_ocb {
    POOL_CANARY_HEAD
    const uint8_t *data;            /* pointer into CPIO archive */
    uint32_t size;                  /* file size */
    uint32_t mode;                  /* file mode */
    off_t pos;                      /* current read position */
    char prefix[CPIOFS_PREFIX_MAX]; /* directory path for readdir */
    int prefix_len;
    POOL_CANARY_TAIL
};

/* Maximum number of simultaneous opens (static pool, no malloc needed). Dynamic
 * executables transiently hold several cpiofs OCBs while the loader reads the
 * main image, interpreter, and mmap-backed file handles, so early boot can
 * burst well past a small pool.
 */
#define CPIOFS_MAX_OCBS 128

static struct cpiofs_ocb ocb_pool[CPIOFS_MAX_OCBS];
static int ocb_inuse[CPIOFS_MAX_OCBS];
static pthread_mutex_t ocb_lock = PTHREAD_MUTEX_INITIALIZER;

static struct cpiofs_ocb *ocb_alloc(void)
{
    struct cpiofs_ocb *ocb = NULL;

    pthread_mutex_lock(&ocb_lock);
    for (int i = 0; i < CPIOFS_MAX_OCBS; i++) {
        if (!ocb_inuse[i]) {
            pool_canary_check(&ocb_pool[i], "cpiofs_ocb", i);
            ocb_inuse[i] = 1;
            memset(&ocb_pool[i], 0, sizeof(ocb_pool[i]));
            pool_canary_arm(&ocb_pool[i]);
            ocb = &ocb_pool[i];
            break;
        }
    }
    pthread_mutex_unlock(&ocb_lock);
    return ocb;
}

static void ocb_free(struct cpiofs_ocb *ocb)
{
    pthread_mutex_lock(&ocb_lock);
    for (int i = 0; i < CPIOFS_MAX_OCBS; i++) {
        if (&ocb_pool[i] == ocb) {
            pool_canary_check(ocb, "cpiofs_ocb", i);
            ocb_inuse[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&ocb_lock);
}

/* Connect function: open */
static int cpiofs_open(ioserv_context_t *ctp,
                       io_open_t *msg,
                       void *extra,
                       void *reserved)
{
    const char *path = msg->connect.path;

    root_trace("cpiofs_open: path='%s' scoid=0x%x pid=%d coid=%d\n", path,
               ctp->info.scoid, ctp->info.pid, ctp->info.coid);
    cpio_file_info_t info;

    /* Root directory open ("/rd/" itself) */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        /* The root exists, so O_CREAT|O_EXCL is EEXIST. */
        if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
            return EEXIST;

        /* Any write intent (write mode or O_TRUNC) on the root directory is
         * EISDIR -- it has no writable form.
         */
        if (msg->connect.ioflag & (_IO_FLAG_WR | O_TRUNC))
            return EISDIR;
        struct cpiofs_ocb *ocb = ocb_alloc();
        if (!ocb)
            return ENOMEM;
        ocb->data = NULL;
        ocb->size = 0;
        ocb->mode = S_IFDIR | 0555;
        ocb->pos = 0;
        ocb->prefix[0] = '\0';
        ocb->prefix_len = 0;
        if (ioserv_open_bind(ctp, ocb, 0) == -1) {
            ocb_free(ocb);
            return errno;
        }
        return EOK;
    }

    /* Strip leading slash if present */
    if (path[0] == '/')
        path++;

    if (!cpio_find_file(cpio_base, cpio_size, path, &info)) {
        /* On this read-only filesystem O_CREAT of a not-found leaf is EROFS
         * once its path prefix resolves, but path-resolution errors take
         * precedence: walk each prefix component and report a missing one as
         * ENOENT and a non-directory one as ENOTDIR before EROFS.
         */
        if (msg->connect.ioflag & O_CREAT) {
            char prefix[CPIOFS_PREFIX_MAX];
            cpio_file_info_t pinfo;

            for (const char *s = strchr(path, '/'); s; s = strchr(s + 1, '/')) {
                size_t plen = (size_t) (s - path);

                if (plen + 1 > sizeof(prefix))
                    return ENAMETOOLONG;
                memcpy(prefix, path, plen);
                prefix[plen] = '\0';
                if (!cpio_find_file(cpio_base, cpio_size, prefix, &pinfo))
                    return ENOENT;
                if (!S_ISDIR(pinfo.mode))
                    return ENOTDIR;
            }
            return EROFS;
        }
        return ENOENT;
    }

    /* Follow symlinks (one level only). Targets may be:
     *   - absolute (start with '/'): strip leading '/' and look up.
     *   - relative: resolve against the symlink's parent directory.
     * If the resolved target isn't in our archive we return ENOENT —
     * crossing-fs links (e.g. /etc → usr/conf) live as namespace symlinks
     * installed in bootimage_init, not as cpio entries.
     */
    if (S_ISLNK(info.mode)) {
        char target_buf[CPIOFS_SYMLINK_MAX];
        char resolved[CPIOFS_SYMLINK_MAX];
        const char *target = target_buf;
        size_t len = info.filesize;

        /* Refuse rather than truncate: a partial path is wrong. */
        if (len + 1 > sizeof(target_buf))
            return ENAMETOOLONG;
        memcpy(target_buf, info.data, len);
        target_buf[len] = '\0';

        if (target_buf[0] == '/') {
            target = target_buf + 1;
        } else {
            /* prepend parent dir of "path" */
            const char *slash = strrchr(path, '/');
            if (slash) {
                size_t parent_len = (size_t) (slash - path) + 1;
                if (parent_len + len + 1 > sizeof(resolved))
                    return ENAMETOOLONG;
                memcpy(resolved, path, parent_len);
                memcpy(resolved + parent_len, target_buf, len + 1);
                target = resolved;
            }
        }
        if (!cpio_find_file(cpio_base, cpio_size, target, &info))
            return ENOENT;
    }

    /* Only allow opening regular files and directories */
    if (!S_ISREG(info.mode) && !S_ISDIR(info.mode))
        return ENOSYS;

    /* A trailing slash (EFLAG_DIR, set by the resolver) requires the path to
     * name a directory, so a regular file with a trailing slash is ENOTDIR --
     * checked before the write-intent errno since it is a resolution error.
     */
    if ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) && !S_ISDIR(info.mode))
        return ENOTDIR;

    /* O_CREAT|O_EXCL on an existing path is EEXIST regardless of the read-only
     * medium (the file already exists, so no create is attempted).
     */
    if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        return EEXIST;

    /* cpiofs is a read-only filesystem, so any write intent -- a write access
     * mode or O_TRUNC -- fails at open time rather than deferring to a later
     * write. A directory takes the more specific EISDIR; a regular file is
     * EROFS.
     */
    if (msg->connect.ioflag & (_IO_FLAG_WR | O_TRUNC))
        return S_ISDIR(info.mode) ? EISDIR : EROFS;

    struct cpiofs_ocb *ocb = ocb_alloc();
    if (!ocb)
        return ENOMEM;

    ocb->data = info.data;
    ocb->size = info.filesize;
    ocb->mode = info.mode;
    ocb->pos = 0;

    /* Store the directory prefix so readdir can filter correctly */
    if (S_ISDIR(info.mode)) {
        size_t plen = strlen(path);
        /* Refuse rather than truncate: a partial path is wrong. */
        if (plen + 1 > CPIOFS_PREFIX_MAX) {
            ocb_free(ocb);
            return ENAMETOOLONG;
        }
        memcpy(ocb->prefix, path, plen);
        ocb->prefix[plen] = '\0';
        ocb->prefix_len = (int) plen;
    } else {
        ocb->prefix[0] = '\0';
        ocb->prefix_len = 0;
    }

    if (ioserv_open_bind(ctp, ocb, 0) == -1) {
        ocb_free(ocb);
        return errno;
    }
    return EOK;
}

/* IO functions: read, stat, lseek, close_ocb, readdir */
static int cpiofs_readdir(ioserv_context_t *ctp, io_read_t *msg, void *handle);

static int cpiofs_read(ioserv_context_t *ctp, io_read_t *msg, void *handle)
{
    struct cpiofs_ocb *ocb = handle;

    if (!ocb) {
        root_log("cpiofs: NULL OCB in %s (pid=%d tid=%d)\n", __func__,
                 ctp->info.pid, ctp->info.tid);
        return EBADF;
    }

    if (S_ISDIR(ocb->mode))
        return cpiofs_readdir(ctp, msg, handle);

    if (!ocb->data)
        return EBADF;

    off_t readpos = ocb->pos;
    int offset_read = 0;
    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    case _IO_XTYPE_OFFSET: {
        struct _xtype_offset *xoff = (void *) (&msg->i + 1);
        readpos = (off_t) xoff->offset;
        offset_read = 1;
        break;
    }
    default:
        return ENOSYS;
    }

    size_t nbytes = msg->i.nbytes;
    if (readpos < 0)
        return EINVAL;
    if (readpos >= (off_t) ocb->size) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    size_t avail = ocb->size - (size_t) readpos;
    if (nbytes > avail)
        nbytes = avail;

    /* Reply directly from the in-memory CPIO data */
    SETIOV(ctp->iov, (void *) (ocb->data + readpos), nbytes);
    if (!offset_read)
        ocb->pos += (off_t) nbytes;

    _IO_SET_READ_NBYTES(ctp, nbytes);
    return IOSERV_NPARTS(1);
}

static int cpiofs_stat(ioserv_context_t *ctp, io_stat_t *msg, void *handle)
{
    struct cpiofs_ocb *ocb = handle;

    if (!ocb) {
        root_log("cpiofs: NULL OCB in %s (pid=%d tid=%d)\n", __func__,
                 ctp->info.pid, ctp->info.tid);
        return EBADF;
    }

    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_mode = ocb->mode;
    msg->o.st_size = (off_t) ocb->size;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | cpiofs_devno;

    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int cpiofs_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *handle)
{
    struct cpiofs_ocb *ocb = handle;
    char *path = (char *) (&msg->o + 1);
    int len;
    size_t cap;

    if (!ocb) {
        root_log("cpiofs: NULL OCB in %s (pid=%d tid=%d)\n", __func__,
                 ctp->info.pid, ctp->info.tid);
        return EBADF;
    }
    if (!S_ISDIR(ocb->mode))
        return ENOSYS;

    cap = (size_t) msg->i.path_len;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = ocb->mode;
    msg->o.info.offset = (uint64_t) ocb->pos;
    msg->o.info.size = (uint64_t) ocb->size;
    if (ocb->prefix_len == 0)
        len = snprintf(path, cap, "/");
    else
        len = snprintf(path, cap, "/%s", ocb->prefix);
    if (len < 0)
        return EINVAL;
    _IO_SET_FDINFO_LEN(ctp, len + 1);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) min(cap, (size_t) len + 1));
}

static int cpiofs_lseek(ioserv_context_t *ctp, io_lseek_t *msg, void *handle)
{
    struct cpiofs_ocb *ocb = handle;
    off_t newpos;

    if (!ocb) {
        root_log("cpiofs: NULL OCB in %s (pid=%d tid=%d)\n", __func__,
                 ctp->info.pid, ctp->info.tid);
        return EBADF;
    }

    switch (msg->i.whence) {
    case SEEK_SET:
        newpos = (off_t) msg->i.offset;
        break;
    case SEEK_CUR:
        newpos = ocb->pos + (off_t) msg->i.offset;
        break;
    case SEEK_END:
        newpos = (off_t) ocb->size + (off_t) msg->i.offset;
        break;
    default:
        return EINVAL;
    }

    if (newpos < 0)
        return EINVAL;

    ocb->pos = newpos;

    /* Combined message (lseek + read): just update position, no reply */
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;

    msg->o = (uint64_t) ocb->pos;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int cpiofs_close_ocb(ioserv_context_t *ctp, void *reserved, void *handle)
{
    if (!handle) {
        root_log("cpiofs: NULL OCB in %s (pid=%d tid=%d)\n", __func__,
                 ctp->info.pid, ctp->info.tid);
        return EOK;
    }
    ocb_free(handle);
    return EOK;
}

/* Readdir context for the CPIO iteration callback */
struct readdir_ctx {
    ioserv_context_t *ctp;
    struct dirent *dp;
    size_t max_bytes;
    size_t used_bytes;
    int entry_idx;
    int skip;           /* entries to skip (for offset) */
    const char *prefix; /* directory prefix filter */
    int prefix_len;
};

static void readdir_cb(const cpio_file_info_t *info, void *user_data)
{
    struct readdir_ctx *ctx = user_data;
    const char *name = info->filename;

    /* Filter: only entries under the directory prefix */
    if (ctx->prefix_len > 0) {
        if (strncmp(name, ctx->prefix, (size_t) ctx->prefix_len) != 0)
            return;
        name += ctx->prefix_len;
        if (name[0] == '/')
            name++;
    }

    /* Skip empty names and subdirectory entries (containing '/') */
    if (name[0] == '\0')
        return;
    if (strchr(name, '/'))
        return;

    /* Skip entries before the read offset */
    if (ctx->entry_idx < ctx->skip) {
        ctx->entry_idx++;
        return;
    }

    size_t reclen = offsetof(struct dirent, d_name) + strlen(name) + 1;
    reclen = lib_align_up_size(reclen, 8);

    if (ctx->used_bytes + reclen > ctx->max_bytes)
        return;

    struct dirent *dp = (struct dirent *) ((char *) ctx->dp + ctx->used_bytes);
    dp->d_ino = (ino_t) (ctx->entry_idx + 1);
    dp->d_reclen = (uint16_t) reclen;
    dp->d_namelen = (uint16_t) strlen(name);
    memcpy(dp->d_name, name, strlen(name) + 1);

    ctx->used_bytes += reclen;
    ctx->entry_idx++;
}

static int cpiofs_readdir(ioserv_context_t *ctp, io_read_t *msg, void *handle)
{
    struct cpiofs_ocb *ocb = handle;

    if (!S_ISDIR(ocb->mode))
        return ENOTDIR;

    /* Use the message buffer area for directory entries */
    struct dirent *dp = (struct dirent *) (void *) msg;
    struct readdir_ctx ctx = {
        .ctp = ctp,
        .dp = dp,
        .max_bytes = msg->i.nbytes,
        .used_bytes = 0,
        .entry_idx = 0,
        .skip = (int) ocb->pos,
        .prefix = ocb->prefix_len > 0 ? ocb->prefix : NULL,
        .prefix_len = ocb->prefix_len,
    };

    cpio_iterate(cpio_base, cpio_size, readdir_cb, &ctx);

    /* Update position for next readdir call */
    ocb->pos = ctx.entry_idx;

    _IO_SET_READ_NBYTES(ctp, ctx.used_bytes);
    if (ctx.used_bytes > 0) {
        SETIOV(ctp->iov, dp, ctx.used_bytes);
        return IOSERV_NPARTS(1);
    }
    return EOK;
}

static int cpiofs_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *handle)
{
    (void) handle;
    root_trace("cpiofs_devctl: devctl_cmd=0x%x pid=%d\n", msg->i.devctl_cmd,
               ctp->info.pid);

    if (msg->i.devctl_cmd == DEVCTL_SET_FLAGS)
        return EOK;

    if (msg->i.devctl_cmd == DEVCTL_GET_MOUNT_FLAGS) {
        /* Report as read-only but NOT noexec — executables are allowed */
        int flags = _MOUNT_READONLY;
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(flags);
        memcpy(&msg->o + 1, &flags, sizeof(flags));
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(flags));
    }

    if (msg->i.devctl_cmd == DEVCTL_STATVFS) {
        struct statvfs *stv = (struct statvfs *) (void *) (msg + 1);

        memset(stv, 0, sizeof(*stv));
        stv->f_bsize = 512;
        stv->f_frsize = 512;
        stv->f_blocks = (fsblkcnt_t) ((cpio_size + 511U) / 512U);
        stv->f_bfree = 0;
        stv->f_bavail = 0;
        stv->f_flag = ST_RDONLY;
        stv->f_namemax = NAME_MAX;
        memcpy(stv->f_basetype, "cpiofs", sizeof("cpiofs"));
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(*stv);
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*stv));
    }

    return ENOSYS;
}

static int cpiofs_mmap(ioserv_context_t *ctp, io_mmap_t *msg, void *handle)
{
    struct cpiofs_ocb *oldocb = handle;
    struct cpiofs_ocb *ocb;

    if (!oldocb) {
        root_log("cpiofs: NULL OCB in %s (pid=%d tid=%d)\n", __func__,
                 ctp->info.pid, ctp->info.tid);
        return EBADF;
    }

    root_trace("cpiofs_mmap: coid=%d pid=%d prot=%x offset=%lx\n",
               msg->i.info.coid, msg->i.info.pid, msg->i.prot,
               (unsigned long) msg->i.offset);

    /* Create a new OCB for the VM mapping connection. The VM path sends
     * _IO_MMAP on a fresh connect_attach, so bind an OCB to that connection
     * before subsequent proc_read calls look up the file data.
     */
    if (!(ocb = ocb_alloc()))
        return ENOMEM;

    ocb->data = oldocb->data;
    ocb->size = oldocb->size;
    ocb->mode = oldocb->mode;
    ocb->pos = 0;

    if (ioserv_open_bind(ctp, ocb, 0) == -1) {
        ocb_free(ocb);
        return errno;
    }

    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.coid = ctp->info.coid;

    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

/* Function tables and initialization */
static int cpiofs_sync(ioserv_context_t *ctp, io_sync_t *msg, void *handle)
{
    (void) ctp;
    (void) msg;
    (void) handle;

    /* cpiofs is read-only and RAM-backed: nothing to flush, so the sync is
     * trivially complete.
     */
    return EOK;
}

static const ioserv_ops_t cpiofs_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = cpiofs_read,
    .close_ocb = cpiofs_close_ocb,
    .stat = cpiofs_stat,
    .devctl = cpiofs_devctl,
    .fdinfo = cpiofs_fdinfo,
    .lseek = cpiofs_lseek,
    .mmap = cpiofs_mmap,
    .sync = cpiofs_sync,
};

static const ioserv_connect_ops_t cpiofs_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = cpiofs_open,
};

/* Walk asinfo callback to find the ramdisk's physical range. */
static paddr_t cpio_phys_start;
static paddr_t cpio_phys_end;

static int find_ramdisk(struct asinfo_entry *as, char *name, void *data)
{
    (void) name;
    (void) data;
    cpio_phys_start = as->start;
    cpio_phys_end = as->end;
    return 0; /* stop after first match */
}

int cpiofs_init(void)
{
    ioserv_attr_t rattr;

    pool_canary_init(ocb_pool, CPIOFS_MAX_OCBS);

    /* Locate the CPIO archive in physical memory */
    walk_asinfo("ramdisk", find_ramdisk, NULL);
    if (cpio_phys_start == 0 || cpio_phys_end == 0) {
        root_log("cpiofs: ramdisk not found in asinfo\n");
        return -1;
    }

    /* Map the CPIO archive into roottask's user VA.
     *
     * cpiofs_init runs during boot, BEFORE message_start kicks off the pool
     * dispatch threads. The libc mmap() wrapper does msg_sendnc(VM_COID, ...) —
     * i.e., roottask sends a sync message to itself. With no receiver yet, that
     * self-deadlocks (STATE_SEND forever). as_mmap() bypasses the message hop
     * but still derefs prp->memory and adp->{rlimit,…} which faults from U-mode
     * (kernel heap is PTE_U=0).
     *
     * The bootstrap helper roottask_mmap_phys_user does the work entirely
     * kernel-side: looks up roottask's adp, picks a fixed-VA slot above
     * 0xc1000000, and installs PTEs via kext_pte_map. See
     * internal/include/sys/calls.h for the full rationale.
     */
    cpio_size = (size_t) (cpio_phys_end - cpio_phys_start + 1);
    {
        uintptr_t va;
        int r = roottask_mmap_phys_user((paddr_t) cpio_phys_start, cpio_size,
                                        PROT_READ, 0, &va);
        if (r != EOK) {
            root_log(
                "cpiofs: roottask_mmap_phys_user(MAP_PHYS) of [%lx..%lx] "
                "failed: %d\n",
                (unsigned long) cpio_phys_start, (unsigned long) cpio_phys_end,
                r);
            return -1;
        }
        cpio_base = (const uint8_t *) va;
    }

    if (!cpio_check_valid(cpio_base, cpio_size)) {
        root_log("cpiofs: invalid CPIO archive at %p\n", cpio_base);
        return -1;
    }

    root_dbg("cpiofs: CPIO archive at %p (PA %lx), %lu bytes\n", cpio_base,
             (unsigned long) cpio_phys_start, (unsigned long) cpio_size);

    memset(&rattr, 0, sizeof(rattr));

    /* Mount the CPIO image at the namespace root. Other I/O services (procfs at
     * /proc, sysfs at /sys, devnull at /dev/null, …) are mounted on
     * more-specific paths and win the longest-prefix match. romfs mounted at
     * /usr by init takes everything under /usr away from cpio.
     */
    if (ioserv_attach(dpp, &rattr, "/", 0, IOSERV_FLAG_DIR,
                      &cpiofs_connect_funcs, &cpiofs_io_funcs, 0) == -1) {
        root_log("cpiofs: ioserv_attach failed\n");
        return -1;
    }

    hwres_proc_devno("rd", &cpiofs_devno, -1, 0);

    return 0;
}
