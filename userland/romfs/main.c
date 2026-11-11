/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * romfs — Unix filesystem I/O service for RTMux
 *
 * Reads a romfs filesystem from a block device and serves it as an I/O service
 * at a given mountpoint. Invoked either directly (e.g. "romfs /dev/nvme0n1p8
 * /usr") or via /bin/mount, which spawns us as "romfs [-o opts] <device>
 * <mountpoint>".
 *
 * Read-only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Override I/O service types BEFORE including headers */
struct fs_ocb;
#define IOSERV_OCB_T struct fs_ocb
#define IOSERV_HANDLE_T void

#include <align.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/rtmux-core.h>
#include <devctl/common.h>

#include "fs.h"
#include "bio.h"

/* Open control block (per-open state) */

struct fs_ocb {
    uint32_t inum;           /* inode number */
    struct romfs_dinode di;  /* cached on-disk inode */
    off_t pos;               /* current file position */
    unsigned ioflag;         /* mutable per-open flags for F_GETFL/F_SETFL */
    char path[PATH_MAX + 1]; /* absolute path for directory fdinfo */
    int inuse;               /* 1 if allocated */
};

#define MAX_OCBS 64
static struct fs_ocb ocb_pool[MAX_OCBS];
static char fs_mountpoint[PATH_MAX + 1];

static struct fs_ocb *ocb_alloc(void)
{
    for (int i = 0; i < MAX_OCBS; i++) {
        if (!ocb_pool[i].inuse) {
            memset(&ocb_pool[i], 0, sizeof(ocb_pool[i]));
            ocb_pool[i].inuse = 1;
            return &ocb_pool[i];
        }
    }
    return NULL;
}

static void ocb_free(struct fs_ocb *ocb)
{
    ocb->inuse = 0;
}

/* Convert romfs inode to mode_t for struct stat */

static mode_t romfs_mode(const struct romfs_dinode *di)
{
    mode_t m = di->mode & 07777;
    switch (di->type) {
    case ROMFS_T_DIR:
        m |= S_IFDIR;
        break;
    case ROMFS_T_FILE:
        m |= S_IFREG;
        break;
    case ROMFS_T_SLINK:
        m |= S_IFLNK;
        break;
    case ROMFS_T_DEV:
        m |= S_IFCHR;
        break;
    default:
        break;
    }
    return m;
}

/* Report the POSIX errno for O_CREAT of a not-found leaf. On this read-only
 * filesystem the create itself is EROFS once the path prefix resolves, but
 * path-resolution errors take precedence: a missing prefix component is ENOENT,
 * a non-directory one is ENOTDIR. romfs_namei() collapses both to "not found",
 * so walk each prefix component in increasing length and report the first that
 * is missing (ENOENT) or a non-directory (ENOTDIR); an all-directory prefix
 * means the create fails EROFS.
 */
static int romfs_create_miss_errno(const char *path)
{
    char prefix[PATH_MAX];

    while (*path == '/')
        path++;
    for (const char *s = strchr(path, '/'); s; s = strchr(s + 1, '/')) {
        size_t plen = (size_t) (s - path);
        struct romfs_dinode di;
        uint32_t pinum;

        if (plen == 0)
            continue;
        if (plen >= sizeof(prefix))
            return ENAMETOOLONG;
        memcpy(prefix, path, plen);
        prefix[plen] = '\0';
        pinum = romfs_namei(prefix);
        if (pinum == 0)
            return ENOENT;
        if (romfs_iread(pinum, &di) < 0)
            return EIO;
        if (di.type != ROMFS_T_DIR)
            return ENOTDIR;
    }
    return EROFS;
}

/* Connect function: open (path resolution) */

static int fs_open(ioserv_context_t *ctp,
                   io_open_t *msg,
                   void *handle,
                   void *extra)
{
    (void) handle;
    (void) extra;

    const char *path = msg->connect.path;
    uint32_t inum = romfs_namei(path);
    if (inum == 0) {
        /* Not found: a plain miss is ENOENT; an O_CREAT miss reports the
         * prefix-precedence errno (ENOENT/ENOTDIR for a bad prefix, else EROFS
         * since romfs is read-only).
         */
        if (msg->connect.ioflag & O_CREAT)
            return romfs_create_miss_errno(path);
        return ENOENT;
    }

    struct romfs_dinode di;
    if (romfs_iread(inum, &di) < 0)
        return EIO;

    /* A trailing slash (EFLAG_DIR, set by the resolver) requires the path to
     * name a directory, so a non-directory with a trailing slash is ENOTDIR --
     * a resolution error, checked before the write-intent errno.
     */
    if ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) && di.type != ROMFS_T_DIR)
        return ENOTDIR;

    /* O_CREAT|O_EXCL on an existing path is EEXIST regardless of the read-only
     * medium (the file already exists, so no create is attempted).
     */
    if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        return EEXIST;

    /* romfs is read-only, so any write intent -- a write access mode or O_TRUNC
     * -- fails at open with the type-specific errno: a directory takes the more
     * specific EISDIR, a regular file EROFS.
     */
    if (msg->connect.ioflag & (_IO_FLAG_WR | O_TRUNC))
        return (di.type == ROMFS_T_DIR) ? EISDIR : EROFS;

    struct fs_ocb *ocb = ocb_alloc();
    if (!ocb)
        return ENOMEM;

    ocb->inum = inum;
    ocb->di = di;
    ocb->pos = 0;
    ocb->ioflag = msg->connect.ioflag;
    if (di.type == ROMFS_T_DIR) {
        const char *rel = path;
        int n;

        if (rel[0] == '/')
            rel++;
        if (rel[0] == '\0' || !strcmp(fs_mountpoint, "/"))
            n = snprintf(ocb->path, sizeof ocb->path, "%s%s", fs_mountpoint,
                         rel);
        else
            n = snprintf(ocb->path, sizeof ocb->path, "%s/%s", fs_mountpoint,
                         rel);
        if (n < 0 || n >= (int) sizeof ocb->path) {
            ocb_free(ocb);
            return ENAMETOOLONG;
        }
    }

    if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
        ocb_free(ocb);
        return errno;
    }
    return EOK;
}

/* IO: read (files and directories) */

static int fs_readdir(ioserv_context_t *ctp,
                      io_read_t *msg,
                      struct fs_ocb *ocb);

static int fs_read(ioserv_context_t *ctp, io_read_t *msg, struct fs_ocb *ocb)
{
    if (ocb->di.type == ROMFS_T_DIR)
        return fs_readdir(ctp, msg, ocb);

    /* Save fields before msg buffer is reused */
    unsigned xtype = msg->i.xtype;
    size_t nbytes = msg->i.nbytes;

    /* Handle pread via _IO_XTYPE_OFFSET */
    off_t readpos = ocb->pos;
    switch (xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    case _IO_XTYPE_OFFSET: {
        struct _xtype_offset *xoff = (void *) (&msg->i + 1);
        readpos = (off_t) xoff->offset;
        break;
    }
    default:
        return ENOSYS;
    }

    if (readpos >= (off_t) ocb->di.size) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    size_t avail = (size_t) (ocb->di.size - (uint64_t) readpos);
    if (nbytes > avail)
        nbytes = avail;
    if (nbytes > ctp->msg_max_size)
        nbytes = ctp->msg_max_size;

    ssize_t total = romfs_readi(&ocb->di, ctp->msg, (uint64_t) readpos, nbytes);
    if (total <= 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    /* Advance file position for normal reads (not pread) */
    if ((xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE)
        ocb->pos += (off_t) total;

    _IO_SET_READ_NBYTES(ctp, (size_t) total);
    return IOSERV_PTR(ctp, ctp->msg, (size_t) total);
}

/* IO: readdir */

static int fs_readdir(ioserv_context_t *ctp, io_read_t *msg, struct fs_ocb *ocb)
{
    size_t max_bytes = msg->i.nbytes;
    struct dirent *dp = (struct dirent *) (void *) msg;
    size_t used = 0;

    if (max_bytes > ctp->msg_max_size)
        max_bytes = ctp->msg_max_size;

    /* ocb->pos counts directory entries already returned */
    int skip = (int) ocb->pos;
    int idx = 0;
    struct romfs_dirent de;

    for (uint64_t off = 0; off < ocb->di.size; off += sizeof(de)) {
        ssize_t n = romfs_readi(&ocb->di, &de, off, sizeof(de));
        if (n != sizeof(de))
            break;
        if (de.inum == 0)
            continue;

        if (idx < skip) {
            idx++;
            continue;
        }

        /* Bound the scan to the on-disk field: a well-formed image NUL-
         * terminates the name, but a corrupt/hostile image may not, and an
         * unbounded strlen would read past the struct.
         */
        const char *nul = memchr(de.name, '\0', ROMFS_NAMESIZ);
        size_t namelen =
            nul ? (size_t) (nul - de.name) : (size_t) (ROMFS_NAMESIZ - 1);
        size_t reclen = offsetof(struct dirent, d_name) + namelen + 1;
        reclen = lib_align_up_size(reclen, 8);

        if (used + reclen > max_bytes)
            break;

        struct dirent *ep = (struct dirent *) ((char *) dp + used);
        ep->d_ino = de.inum;
        ep->d_reclen = (uint16_t) reclen;
        ep->d_namelen = (uint16_t) namelen;
        memcpy(ep->d_name, de.name, namelen);
        ep->d_name[namelen] = '\0';

        used += reclen;
        idx++;
    }

    ocb->pos = idx;

    _IO_SET_READ_NBYTES(ctp, used);
    if (used > 0) {
        SETIOV(ctp->iov, dp, used);
        return IOSERV_NPARTS(1);
    }
    return EOK;
}

/* IO: stat */

static int fs_stat(ioserv_context_t *ctp, io_stat_t *msg, struct fs_ocb *ocb)
{
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_ino = ocb->inum;
    msg->o.st_mode = romfs_mode(&ocb->di);
    msg->o.st_nlink = ocb->di.nlink;
    msg->o.st_uid = (uid_t) ocb->di.uid;
    msg->o.st_gid = (gid_t) ocb->di.gid;
    msg->o.st_size = (off_t) ocb->di.size;
    msg->o.st_atime = (time_t) ocb->di.atime;
    msg->o.st_mtime = (time_t) ocb->di.mtime;
    msg->o.st_ctime = (time_t) ocb->di.ctime;
    msg->o.st_blksize = ROMFS_BSIZE;
    msg->o.st_blocks =
        (blkcnt_t) ((ocb->di.size + ROMFS_BSIZE - 1) / ROMFS_BSIZE);

    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

/* IO: lseek */

static int fs_lseek(ioserv_context_t *ctp, io_lseek_t *msg, struct fs_ocb *ocb)
{
    off_t newpos;

    switch (msg->i.whence) {
    case SEEK_SET:
        newpos = (off_t) msg->i.offset;
        break;
    case SEEK_CUR:
        newpos = ocb->pos + (off_t) msg->i.offset;
        break;
    case SEEK_END:
        newpos = (off_t) ocb->di.size + (off_t) msg->i.offset;
        break;
    default:
        return EINVAL;
    }

    if (newpos < 0)
        return EINVAL;

    ocb->pos = newpos;

    /* Combined message (e.g. lseek+read from proc_read): don't reply, let the
     * I/O service framework dispatch the next message in the chain. Replying
     * here would unblock the client's msg_send prematurely.
     */
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;

    msg->o = (uint64_t) ocb->pos;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

/* IO: close_ocb */

static int fs_close_ocb(ioserv_context_t *ctp,
                        void *reserved,
                        struct fs_ocb *ocb)
{
    (void) ctp;
    (void) reserved;
    ocb_free(ocb);
    return EOK;
}

static uint64_t fs_count_free_blocks(const struct romfs_super *sb)
{
    uint64_t free_blocks = 0;

    for (uint64_t b = 0; b < sb->size;) {
        struct buf *bp = bread(ROMFS_BBLOCK(b, *sb));
        if (!bp)
            break;
        for (size_t i = 0; i < ROMFS_BSIZE && b < sb->size; i++) {
            unsigned byte = (unsigned char) bp->data[i];

            for (unsigned bit = 0; bit < 8 && b < sb->size; bit++, b++) {
                if ((byte & (1U << bit)) == 0)
                    free_blocks++;
            }
        }
        brelse(bp);
    }
    return free_blocks;
}

static int fs_devctl(ioserv_context_t *ctp,
                     io_devctl_t *msg,
                     struct fs_ocb *ocb)
{
    union {
        int oflag;
        int mountflag;
        struct statvfs stv;
    } *data = (void *) (msg + 1);

    if (msg->i.devctl_cmd == DEVCTL_GET_FLAGS) {
        if (!ocb)
            return EINVAL;
        unsigned acc = ocb->ioflag & (unsigned) O_ACCMODE;
        data->oflag = (int) (ocb->ioflag & ~(unsigned) O_ACCMODE);
        if (acc != 0)
            data->oflag |= (int) ((acc - 1) & (unsigned) O_ACCMODE);
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(data->oflag);
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(data->oflag));
    }

    if (msg->i.devctl_cmd == DEVCTL_SET_FLAGS) {
        if (!ocb)
            return EINVAL;
        ocb->ioflag = (ocb->ioflag & ~(unsigned) O_SETFLAG) |
                      ((unsigned) data->oflag & (unsigned) O_SETFLAG);
        return EOK;
    }

    if (msg->i.devctl_cmd == DEVCTL_GET_MOUNT_FLAGS) {
        /* Read-only filesystem, but executables are allowed */
        int flags = _MOUNT_READONLY;
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(flags);
        memcpy(&msg->o + 1, &flags, sizeof(flags));
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(flags));
    }

    if (msg->i.devctl_cmd == DEVCTL_STATVFS) {
        const struct romfs_super *sb = romfs_super();

        memset(&data->stv, 0, sizeof(data->stv));
        data->stv.f_bsize = ROMFS_BSIZE;
        data->stv.f_frsize = ROMFS_BSIZE;
        data->stv.f_blocks = (fsblkcnt_t) sb->size;
        data->stv.f_bfree = (fsblkcnt_t) fs_count_free_blocks(sb);
        data->stv.f_bavail = data->stv.f_bfree;
        data->stv.f_files = (fsfilcnt_t) sb->ninodes;
        data->stv.f_ffree = 0;
        data->stv.f_favail = 0;
        data->stv.f_flag = ST_RDONLY;
        data->stv.f_namemax = ROMFS_NAMESIZ - 1;
        snprintf(data->stv.f_basetype, sizeof(data->stv.f_basetype), "romfs");
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(data->stv);
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(data->stv));
    }

    return ENOSYS;
}

/* Function tables and main */

static int fs_sync(ioserv_context_t *ctp, io_sync_t *msg, struct fs_ocb *ocb)
{
    (void) ctp;
    (void) msg;
    (void) ocb;

    /* romfs is read-only and RAM-backed: nothing to flush, so the sync is
     * trivially complete.
     */
    return EOK;
}

static int fs_fdinfo(ioserv_context_t *ctp,
                     io_fdinfo_t *msg,
                     struct fs_ocb *ocb)
{
    char *path = (char *) (&msg->o + 1);
    size_t cap;
    size_t len;

    (void) ctp;
    if (ocb->di.type != ROMFS_T_DIR)
        return ENOSYS;

    cap = (size_t) msg->i.path_len;
    len = strlen(ocb->path) + 1;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = (uint32_t) romfs_mode(&ocb->di);
    msg->o.info.offset = (uint64_t) ocb->pos;
    msg->o.info.size = ocb->di.size;
    if (cap > 0) {
        size_t copy = cap < len ? cap : len;

        memcpy(path, ocb->path, copy);
    }
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) (cap < len ? cap : len));
}

static const ioserv_ops_t fs_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = fs_read,
    .close_ocb = fs_close_ocb,
    .stat = fs_stat,
    .devctl = fs_devctl,
    .fdinfo = fs_fdinfo,
    .lseek = fs_lseek,
    .sync = fs_sync,
};

static const ioserv_connect_ops_t fs_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = fs_open,
};

static void usage(void)
{
    fprintf(stderr, "usage: romfs [-o options] <device> <mountpoint>\n");
}

int main(int argc, char *argv[])
{
    const char *devpath;
    const char *mountpoint;
    int opt;

    while ((opt = getopt(argc, argv, "o:")) != -1) {
        switch (opt) {
        case 'o':
            (void) optarg;
            break;
        default:
            usage();
            return 1;
        }
    }

    if (optind + 2 != argc) {
        usage();
        return 1;
    }
    devpath = argv[optind];
    mountpoint = argv[optind + 1];
    if (snprintf(fs_mountpoint, sizeof fs_mountpoint, "%s", mountpoint) >=
        (int) sizeof fs_mountpoint) {
        fprintf(stderr, "romfs: mountpoint too long %s\n", mountpoint);
        return 1;
    }

    /* Initialize block I/O */
    if (bio_init(devpath) < 0) {
        fprintf(stderr, "romfs: cannot open %s: %s\n", devpath,
                strerror(errno));
        return 1;
    }

    /* Read and verify superblock */
    if (romfs_init() < 0) {
        fprintf(stderr, "romfs: filesystem init failed\n");
        return 1;
    }

    /* Set up dispatch and I/O service */
    dispatch_t *dpp = dispatch_create();
    if (!dpp) {
        fprintf(stderr, "romfs: dispatch_create failed\n");
        return 1;
    }
    ioserv_attr_t rattr = {
        .nparts_max = 1,
    };

    int id = ioserv_attach(dpp, &rattr, mountpoint, _FTYPE_ANY, IOSERV_FLAG_DIR,
                           &fs_connect_funcs, &fs_io_funcs, NULL);
    if (id == -1) {
        fprintf(stderr, "romfs: ioserv_attach(\"%s\") failed: %s\n", mountpoint,
                strerror(errno));
        return 1;
    }

    dispatch_context_t *dctp = dispatch_context_alloc(dpp);
    if (!dctp) {
        fprintf(stderr, "romfs: dispatch_context_alloc failed\n");
        return 1;
    }

    fprintf(stderr, "romfs: mounted romfs at %s (dev=%s)\n", mountpoint,
            devpath);

    if (rtmux_detach(EXIT_SUCCESS) != EOK) {
        fprintf(stderr, "romfs: rtmux_detach failed: %s\n", strerror(errno));
        return 1;
    }

    for (;;) {
        dctp = dispatch_block(dctp);
        if (dctp)
            dispatch_handler(dctp);
    }

    return 0;
}
