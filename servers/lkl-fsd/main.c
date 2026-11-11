/*
 * lkl-fsd - gated LKL filesystem personality prototype.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

struct lklfs_ocb;
#define IOSERV_OCB_T struct lklfs_ocb
#define IOSERV_HANDLE_T void

#include <align.h>
#include <devctl/common.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/mount.h>
#include <sys/ioserv.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/linux/fcntl.h>
#include <lkl/linux/mount.h>
#include <lkl/linux/stat.h>

#include <sys/pool-canary.h>

#include "domain.h"
#include "pathbuild.h"
#include "server.h"

#define LKLFS_MAX_OCB 64
#define LKLFS_DIRENT_BUFSZ 4096

static const char *const exports[] = {
    [0] = "/ctl",
    [1] = "/stats",
    [2] = "/mounts",
    [3] = "/fs/root",
};

enum lklfs_ocb_kind {
    LKLFS_OCB_LKL,
    LKLFS_OCB_TEXT,
};

enum lklfs_text_kind {
    LKLFS_TEXT_CTL,
    LKLFS_TEXT_STATS,
    LKLFS_TEXT_MOUNTS,
};

struct lklfs_text_export {
    enum lklfs_text_kind kind;
};

struct lklfs_ocb {
    POOL_CANARY_HEAD
    enum lklfs_ocb_kind kind;
    int fd;
    int is_dir;
    off_t pos;
    unsigned ioflag;
    struct lkl_stat st;
    char text[1024];
    size_t text_len;
    char path[PATH_MAX + 1];

    /* getdents64 records are cast to struct lkl_linux_dirent64 (8-byte d_ino);
     * keep the buffer 8-aligned so RISC-V never takes a misaligned-load trap.
     */
    alignas(8) char dirbuf[LKLFS_DIRENT_BUFSZ];
    size_t dirbuf_pos;
    size_t dirbuf_len;
    int inuse;
    POOL_CANARY_TAIL
};

static char lklfs_mount[64];
static char lklfs_root[64] = "/fs/root";
static char lklfs_source[64];
static struct lklfs_ocb lklfs_ocbs[LKLFS_MAX_OCB];
static const struct lklfs_text_export lklfs_ctl_export = {
    .kind = LKLFS_TEXT_CTL,
};
static const struct lklfs_text_export lklfs_stats_export = {
    .kind = LKLFS_TEXT_STATS,
};
static const struct lklfs_text_export lklfs_mounts_export = {
    .kind = LKLFS_TEXT_MOUNTS,
};

static int lklfs_rw = 1;
static int crash_on_open; /* test hook: fault on opening a "crashme" path */

static int start_lkl(void)
{
    return rtmux_lkl_start_kernel("lkl-fsd", "Linux VFS/ext4 stack",
                                  RTMUX_LKL_FSD_CMDLINE);
}

static struct lklfs_ocb *ocb_alloc(void)
{
    for (int i = 0; i < LKLFS_MAX_OCB; i++) {
        if (!lklfs_ocbs[i].inuse) {
            pool_canary_check(&lklfs_ocbs[i], "lklfs_ocb", i);
            memset(&lklfs_ocbs[i], 0, sizeof(lklfs_ocbs[i]));
            lklfs_ocbs[i].kind = LKLFS_OCB_LKL;
            lklfs_ocbs[i].fd = -1;
            lklfs_ocbs[i].inuse = 1;
            pool_canary_arm(&lklfs_ocbs[i]);
            return &lklfs_ocbs[i];
        }
    }
    return NULL;
}

static void ocb_free(struct lklfs_ocb *ocb)
{
    pool_canary_check(ocb, "lklfs_ocb", (int) (ocb - lklfs_ocbs));
    if (ocb->fd >= 0)
        lkl_sys_close((unsigned int) ocb->fd);
    memset(ocb, 0, sizeof(*ocb));
    pool_canary_arm(ocb);
}

#ifdef CONFIG_SERVER_POOL_CANARY

/* Test-only pool user: plant a one-byte overflow past a live object and prove
 * the redzone catches it. Two adjacent slots are armed, then object a is
 * overrun by one byte, which lands in object b's head canary; the verify then
 * aborts with b's pool name and slot index. Reached only via the
 * --canary-selftest argument in a CONFIG_SERVER_POOL_CANARY build.
 */
static void canary_selftest(void)
{
    struct lklfs_ocb *a = ocb_alloc();
    struct lklfs_ocb *b = ocb_alloc();

    if (!a || !b || b != a + 1) {
        fprintf(stderr, "lkl-fsd: canary selftest: setup failed\n");
        abort();
    }

    /* Write one byte at the a/b boundary, which is object b's first byte and
     * exactly where a one-byte overflow of object a lands. Index through the
     * pool array base so the access stays in bounds and well defined.
     */
    volatile unsigned char *pool = (volatile unsigned char *) lklfs_ocbs;
    pool[(size_t) (b - lklfs_ocbs) * sizeof(*b)] = 0x41;
    pool_canary_check(b, "lklfs_ocb", (int) (b - lklfs_ocbs));
    fprintf(stderr, "lkl-fsd: canary selftest: overflow not caught\n");
    abort();
}
#endif

static int lkl_open_flags(unsigned ioflag)
{
    int flags;

    if ((ioflag & _IO_FLAG_WR) != 0 && (ioflag & _IO_FLAG_RD) != 0)
        flags = LKL_O_RDWR;
    else if ((ioflag & _IO_FLAG_WR) != 0)
        flags = LKL_O_WRONLY;
    else
        flags = LKL_O_RDONLY;
    if ((ioflag & O_CREAT) != 0)
        flags |= LKL_O_CREAT;
    if ((ioflag & O_EXCL) != 0)
        flags |= LKL_O_EXCL;
    if ((ioflag & O_TRUNC) != 0)
        flags |= LKL_O_TRUNC;
    if ((ioflag & O_APPEND) != 0)
        flags |= LKL_O_APPEND;
    return flags;
}

/* Overflow-safe base+off; rejects the add instead of invoking signed UB. */
static int seek_add(off_t base, off_t off, off_t *out)
{
    if (off > 0 && base > INT64_MAX - off)
        return -1;
    if (off < 0 && base < INT64_MIN - off)
        return -1;
    *out = base + off;
    return 0;
}

static mode_t lkl_mode_to_posix(unsigned int mode)
{
    mode_t out = (mode_t) (mode & 07777U);

    switch (mode & LKL_S_IFMT) {
    case LKL_S_IFREG:
        out |= S_IFREG;
        break;
    case LKL_S_IFDIR:
        out |= S_IFDIR;
        break;
    case LKL_S_IFLNK:
        out |= S_IFLNK;
        break;
    case LKL_S_IFCHR:
        out |= S_IFCHR;
        break;
    case LKL_S_IFBLK:
        out |= S_IFBLK;
        break;
    case LKL_S_IFIFO:
        out |= S_IFIFO;
        break;
    default:
        break;
    }
    return out;
}

static void lkl_stat_to_posix(const struct lkl_stat *lst, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = (dev_t) lst->st_dev;
    st->st_ino = (ino_t) lst->st_ino;
    st->st_mode = lkl_mode_to_posix(lst->st_mode);
    st->st_nlink = (nlink_t) lst->st_nlink;
    st->st_uid = (uid_t) lst->st_uid;
    st->st_gid = (gid_t) lst->st_gid;
    st->st_rdev = (dev_t) lst->st_rdev;
    st->st_size = (off_t) lst->st_size;
    st->st_blksize = (blksize_t) lst->st_blksize;
    st->st_blocks = (blkcnt_t) lst->st_blocks;
    st->st_atime = (time_t) lst->lkl_st_atime;
    st->st_mtime = (time_t) lst->lkl_st_mtime;
    st->st_ctime = (time_t) lst->lkl_st_ctime;
}

static void lkl_statfs_to_statvfs(const struct lkl_statfs *lsfs,
                                  struct statvfs *stv)
{
    memset(stv, 0, sizeof(*stv));
    stv->f_bsize = (unsigned long) lsfs->f_bsize;
    stv->f_frsize =
        (unsigned long) (lsfs->f_frsize != 0 ? lsfs->f_frsize : lsfs->f_bsize);
    stv->f_blocks = (fsblkcnt_t) lsfs->f_blocks;
    stv->f_bfree = (fsblkcnt_t) lsfs->f_bfree;
    stv->f_bavail = (fsblkcnt_t) lsfs->f_bavail;
    stv->f_files = (fsfilcnt_t) lsfs->f_files;
    stv->f_ffree = (fsfilcnt_t) lsfs->f_ffree;
    stv->f_favail = (fsfilcnt_t) lsfs->f_ffree;
    stv->f_flag = lklfs_rw ? 0 : ST_RDONLY;
    stv->f_namemax = (unsigned long) lsfs->f_namelen;
    snprintf(stv->f_basetype, sizeof(stv->f_basetype), "ext4");
}

static void fill_text_export(struct lklfs_ocb *ocb,
                             const struct lklfs_text_export *exp)
{
    switch (exp->kind) {
    case LKLFS_TEXT_CTL:
        ocb->text_len = rtmux_lkl_text_format(
            ocb->text, sizeof(ocb->text), "mode %s\nroot %s\n",
            lklfs_rw ? "readwrite" : "readonly", lklfs_root);
        break;
    case LKLFS_TEXT_STATS: {
        struct rtmux_lkl_host_stats stats;
        char ioserv_stats[256];

        rtmux_lkl_host_stats(&stats);
        (void) ioserv_reply_counters_format(ioserv_stats, sizeof(ioserv_stats));
        ocb->text_len = rtmux_lkl_text_format(
            ocb->text, sizeof(ocb->text),
            "server lkl-fsd\nfilesystem ext4\nreadonly %d\n"
            "lkl_mem %s\npage_current %llu\npage_high "
            "%llu\npage_total %llu\n"
            "dma_current %llu\ndma_high %llu\ndma_total %llu\n"
            "lkl_block_read_bytes %llu\nlkl_block_write_bytes %llu\n%s"
            "lkl_threads %llu\nlkl_thread_stack_bytes %llu\n",
            lklfs_rw ? 0 : 1, RTMUX_LKL_FSD_CMDLINE, stats.page_current,
            stats.page_high, stats.page_total, stats.dma_current,
            stats.dma_high, stats.dma_total, stats.lkl_block_read_bytes,
            stats.lkl_block_write_bytes, ioserv_stats, stats.thread_count,
            stats.thread_stack_bytes);
        break;
    }
    case LKLFS_TEXT_MOUNTS:
        ocb->text_len = rtmux_lkl_text_format(
            ocb->text, sizeof(ocb->text), "%s %s ext4 %s\n", lklfs_root,
            lklfs_mount, lklfs_rw ? "rw" : "ro,noload");
        break;
    }
}

static int lklfs_open(ioserv_context_t *ctp,
                      io_open_t *msg,
                      void *handle,
                      void *extra)
{
    struct lklfs_ocb *ocb;
    char path[256];
    long fd;
    int ret;

    (void) extra;

    if (!lklfs_rw && (msg->connect.ioflag & _IO_FLAG_WR) != 0)
        return EROFS;

    ocb = ocb_alloc();
    if (!ocb)
        return ENOMEM;

    if (handle) {
        ocb->kind = LKLFS_OCB_TEXT;
        ocb->ioflag = msg->connect.ioflag;
        fill_text_export(ocb, handle);
        if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
            int saved = errno;
            ocb_free(ocb);
            return saved;
        }
        return EOK;
    }

    if (!rtmux_lkl_connect_path_ok((size_t) ctp->size, &msg->connect)) {
        ocb_free(ocb);
        return EINVAL;
    }

    /* Test hook (--serve-ext4-crash-on-open): fault after the next block
     * backend copy while opening "crashme", so the restart harness exercises a
     * serving lkl-fsd with an LKL block request in flight.
     */
    if (crash_on_open && strstr(msg->connect.path, "crashme") != NULL)
        rtmux_lkl_block_crash_after_request(true);
    ret = rtmux_lkl_admission_lock(ctp, msg, NULL);
    if (ret != EOK) {
        ocb_free(ocb);
        return ret;
    }
    if (build_lkl_path(path, sizeof(path), lklfs_mount, msg->connect.path) <
        0) {
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        ocb_free(ocb);
        return ENAMETOOLONG;
    }

    fd = lkl_sys_open(path, lkl_open_flags(msg->connect.ioflag),
                      (int) (msg->connect.mode & 0777U));
    if (fd < 0) {
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        ocb_free(ocb);
        return lkl_errno(fd);
    }
    ocb->fd = (int) fd;
    long fstat_ret = lkl_sys_fstat((unsigned int) ocb->fd, &ocb->st);
    if (fstat_ret < 0) {
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        ocb_free(ocb);
        return lkl_errno(fstat_ret);
    }
    rtmux_lkl_admission_unlock(ctp, msg, NULL);
    ocb->is_dir = LKL_S_ISDIR(ocb->st.st_mode) != 0;
    ocb->ioflag = msg->connect.ioflag;
    if ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) && !ocb->is_dir) {
        ocb_free(ocb);
        return ENOTDIR;
    }
    if (ocb->is_dir) {
        const char *rel = msg->connect.path;
        int n;

        if (rel[0] == '/')
            rel++;
        if (rel[0] == '\0' || !strcmp(lklfs_root, "/"))
            n = snprintf(ocb->path, sizeof ocb->path, "%s%s", lklfs_root, rel);
        else
            n = snprintf(ocb->path, sizeof ocb->path, "%s/%s", lklfs_root, rel);
        if (n < 0 || n >= (int) sizeof ocb->path) {
            ocb_free(ocb);
            return ENAMETOOLONG;
        }
    }
    if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
        int saved = errno;
        ocb_free(ocb);
        return saved;
    }
    return EOK;
}

static int lklfs_unlink(ioserv_context_t *ctp,
                        io_unlink_t *msg,
                        void *handle,
                        void *reserved)
{
    char path[256];
    long ret;
    int err;

    (void) reserved;
    if (handle)
        return EROFS;
    if (!rtmux_lkl_connect_path_ok((size_t) ctp->size, &msg->connect))
        return EINVAL;
    if (msg->connect.path[0] == '\0')
        return EISDIR;
    if (!lklfs_rw)
        return EROFS;
    err = rtmux_lkl_admission_lock(ctp, msg, NULL);
    if (err != EOK)
        return err;
    if (build_lkl_path(path, sizeof(path), lklfs_mount, msg->connect.path) <
        0) {
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        return ENAMETOOLONG;
    }
    ret = lkl_sys_unlink(path);
    rtmux_lkl_admission_unlock(ctp, msg, NULL);
    return lkl_errno(ret);
}

static int lklfs_admission_lock(ioserv_context_t *ctp,
                                void *msg,
                                struct lklfs_ocb *ocb)
{
    return rtmux_lkl_admission_lock(ctp, msg, ocb);
}

static int lklfs_admission_unlock(ioserv_context_t *ctp,
                                  void *msg,
                                  struct lklfs_ocb *ocb)
{
    return rtmux_lkl_admission_unlock(ctp, msg, ocb);
}

static int lklfs_write(ioserv_context_t *ctp,
                       io_write_t *msg,
                       struct lklfs_ocb *ocb)
{
    size_t nbytes;
    unsigned xtype = msg->i.xtype;
    size_t skip = sizeof(msg->i);
    off_t writepos = ocb->pos;
    const char *buf;
    long ret;

    if (!lklfs_rw)
        return EROFS;
    if (!ocb || ocb->kind != LKLFS_OCB_LKL || ocb->fd < 0)
        return EINVAL;
    if ((ocb->ioflag & _IO_FLAG_WR) == 0)
        return EBADF;
    if (ocb->is_dir)
        return EISDIR;
    if (msg->i.nbytes < 0)
        return EINVAL;
    nbytes = (size_t) msg->i.nbytes;

    switch (xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    case _IO_XTYPE_OFFSET: {
        struct _xtype_offset *xoff = (void *) (&msg->i + 1);

        skip += sizeof(*xoff);
        if ((size_t) ctp->size < skip)
            return EINVAL;
        writepos = (off_t) xoff->offset;
        break;
    }
    default:
        return ENOSYS;
    }
    if (writepos < 0)
        return EINVAL;
    if (nbytes > ctp->msg_max_size - skip)
        nbytes = ctp->msg_max_size - skip;
    if ((size_t) ctp->size < skip + nbytes)
        nbytes = (size_t) ctp->size > skip ? (size_t) ctp->size - skip : 0;
    if ((ocb->ioflag & O_APPEND) != 0) {
        ret = lkl_sys_fstat((unsigned int) ocb->fd, &ocb->st);
        if (ret < 0)
            return lkl_errno(ret);
        writepos = (off_t) ocb->st.st_size;
    }

    buf = (const char *) (void *) msg + skip;
    ret = lkl_sys_pwrite64((unsigned int) ocb->fd, buf, nbytes, writepos);
    if (ret < 0)
        return lkl_errno(ret);
    if ((xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE &&
        seek_add(writepos, (off_t) ret, &ocb->pos) < 0)
        return EOVERFLOW;
    (void) lkl_sys_fstat((unsigned int) ocb->fd, &ocb->st);
    _IO_SET_WRITE_NBYTES(ctp, (size_t) ret);
    return EOK;
}

static int lklfs_readdir(ioserv_context_t *ctp,
                         io_read_t *msg,
                         struct lklfs_ocb *ocb)
{
    size_t max_bytes;
    struct dirent *dp = (struct dirent *) (void *) msg;
    size_t used = 0;

    if (msg->i.nbytes < 0)
        return EINVAL;
    max_bytes = (size_t) msg->i.nbytes;

    /* Client controls nbytes; the reply is built into the shared receive
     * buffer, so cap it to the buffer capacity to avoid an overrun.
     */
    if (max_bytes > ctp->msg_max_size)
        max_bytes = ctp->msg_max_size;

    while (used < max_bytes) {
        if (ocb->dirbuf_pos >= ocb->dirbuf_len) {
            long nread;

            nread = lkl_sys_getdents64(
                (unsigned int) ocb->fd,
                (struct lkl_linux_dirent64 *) (void *) ocb->dirbuf,
                sizeof(ocb->dirbuf));
            if (nread < 0)
                return lkl_errno(nread);
            if (nread == 0)
                break;
            ocb->dirbuf_pos = 0;
            ocb->dirbuf_len = (size_t) nread;
        }
        struct lkl_linux_dirent64 *ld =
            (struct lkl_linux_dirent64 *) (void *) (ocb->dirbuf +
                                                    ocb->dirbuf_pos);
        size_t avail = ocb->dirbuf_len - ocb->dirbuf_pos;
        size_t hdr = offsetof(struct lkl_linux_dirent64, d_name);

        /* Don't trust d_reclen from the LKL fs: a zero/short/oversized record
         * would otherwise loop forever or read past dirbuf. d_reclen must also
         * be 8-byte aligned: dirbuf is alignas(8) and pos advances by d_reclen,
         * so an unaligned record would land the next record's 64-bit fields
         * off-alignment and trap on RISC-V.
         */
        if (avail < hdr || ld->d_reclen < hdr + 1 || ld->d_reclen > avail ||
            (ld->d_reclen & 7U) != 0)
            return EIO;
        size_t namemax = (size_t) ld->d_reclen - hdr;
        char *nul = memchr(ld->d_name, '\0', namemax);

        if (!nul) /* name not NUL-terminated within record */
            return EIO;
        size_t namelen = (size_t) (nul - ld->d_name);
        size_t reclen = offsetof(struct dirent, d_name) + namelen + 1;

        reclen = lib_align_up_size(reclen, 8);
        if (used + reclen > max_bytes) {
            if (used == 0)
                return EINVAL;
            break;
        }

        struct dirent *ep = (struct dirent *) ((char *) dp + used);
        memset(ep, 0, reclen);
        ep->d_ino = (ino_t) ld->d_ino;
        ep->d_offset = (off_t) ld->d_off;
        ep->d_reclen = (uint16_t) reclen;
        ep->d_namelen = (uint16_t) namelen;
        memcpy(ep->d_name, ld->d_name, namelen);
        ep->d_name[namelen] = '\0';

        used += reclen;
        ocb->dirbuf_pos += ld->d_reclen;
    }

    _IO_SET_READ_NBYTES(ctp, used);
    if (used > 0)
        return IOSERV_PTR(ctp, dp, used);
    return EOK;
}

static int lklfs_read(ioserv_context_t *ctp,
                      io_read_t *msg,
                      struct lklfs_ocb *ocb)
{
    char *buf = (char *) (void *) msg;
    size_t nbytes;
    unsigned xtype = msg->i.xtype;

    /* Every read reply carries pooled data (text[], dirbuf via readdir, or file
     * bytes), so verify the OCB canaries before servicing it.
     */
    pool_canary_check(ocb, "lklfs_ocb", (int) (ocb - lklfs_ocbs));

    /* A write-only OCB must not read (file or dir); mirrors lklfs_write's WR
     * gate. This covers readdir too, which lklfs_read dispatches into below.
     */
    if ((ocb->ioflag & _IO_FLAG_RD) == 0)
        return EBADF;
    if (msg->i.nbytes < 0)
        return EINVAL;
    nbytes = (size_t) msg->i.nbytes;

    /* The read lands in the shared receive buffer; cap the client-controlled
     * size to its capacity before any copy/lkl_sys_read.
     */
    if (nbytes > ctp->msg_max_size)
        nbytes = ctp->msg_max_size;
    off_t readpos = ocb->pos;
    long ret;

    switch (xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    case _IO_XTYPE_OFFSET: {
        struct _xtype_offset *xoff = (void *) (&msg->i + 1);

        /* The offset rides in appended payload; a truncated message would let
         * us read stale receive-buffer bytes as the file position.
         */
        if ((size_t) ctp->size < sizeof(msg->i) + sizeof(*xoff))
            return EINVAL;
        readpos = (off_t) xoff->offset;
        break;
    }
    default:
        return ENOSYS;
    }

    if (ocb->kind == LKLFS_OCB_TEXT) {
        if (readpos < 0)
            return EINVAL;
        if (readpos >= (off_t) ocb->text_len) {
            _IO_SET_READ_NBYTES(ctp, 0);
            return EOK;
        }
        if (nbytes > ocb->text_len - (size_t) readpos)
            nbytes = ocb->text_len - (size_t) readpos;
        memcpy(buf, ocb->text + readpos, nbytes);
        if ((xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE)
            ocb->pos = readpos + (off_t) nbytes;
        _IO_SET_READ_NBYTES(ctp, nbytes);
        return nbytes > 0 ? IOSERV_PTR(ctp, buf, nbytes) : EOK;
    }

    if (ocb->is_dir) {
        if ((xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
            return ENOSYS;
        return lklfs_readdir(ctp, msg, ocb);
    }

    if (readpos < 0)
        return EINVAL;
    if (readpos >= (off_t) ocb->st.st_size) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    if (nbytes > (size_t) (ocb->st.st_size - readpos))
        nbytes = (size_t) (ocb->st.st_size - readpos);

    /* pread64 keeps the read atomic with respect to the file offset: a shared
     * OCB or a future multi-threaded dispatch must not interleave seek+read.
     */
    ret = lkl_sys_pread64((unsigned int) ocb->fd, buf, nbytes, readpos);
    if (ret < 0)
        return lkl_errno(ret);
    if ((xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE)
        ocb->pos = readpos + (off_t) ret;

    _IO_SET_READ_NBYTES(ctp, (size_t) ret);
    if (ret > 0)
        return IOSERV_PTR(ctp, buf, (size_t) ret);
    return EOK;
}

static int lklfs_stat(ioserv_context_t *ctp,
                      io_stat_t *msg,
                      struct lklfs_ocb *ocb)
{
    if (ocb->kind == LKLFS_OCB_TEXT) {
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.st_mode = S_IFREG | 0444;
        msg->o.st_nlink = 1;
        msg->o.st_size = (off_t) ocb->text_len;
        msg->o.st_blksize = 512;
        msg->o.st_blocks = 1;
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    lkl_stat_to_posix(&ocb->st, &msg->o);
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int lklfs_lseek(ioserv_context_t *ctp,
                       io_lseek_t *msg,
                       struct lklfs_ocb *ocb)
{
    off_t newpos;

    if (ocb->kind == LKLFS_OCB_TEXT) {
        switch (msg->i.whence) {
        case SEEK_SET:
            newpos = (off_t) msg->i.offset;
            break;
        case SEEK_CUR:
            if (seek_add(ocb->pos, (off_t) msg->i.offset, &newpos) < 0)
                return EOVERFLOW;
            break;
        case SEEK_END:
            if (seek_add((off_t) ocb->text_len, (off_t) msg->i.offset,
                         &newpos) < 0)
                return EOVERFLOW;
            break;
        default:
            return EINVAL;
        }
        if (newpos < 0)
            return EINVAL;
        ocb->pos = newpos;
        if (msg->i.combine_len & _IO_COMBINE_FLAG)
            return EOK;
        msg->o = (uint64_t) ocb->pos;
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    }

    /* ocb->pos is the single source of truth: every read re-seeks the LKL fd to
     * it, so resolve whence here instead of trusting the fd's implicit offset
     * (which an intervening pread would have moved).
     */
    switch (msg->i.whence) {
    case SEEK_SET:
        newpos = (off_t) msg->i.offset;
        break;
    case SEEK_CUR:
        if (seek_add(ocb->pos, (off_t) msg->i.offset, &newpos) < 0)
            return EOVERFLOW;
        break;
    case SEEK_END:
        if (seek_add((off_t) ocb->st.st_size, (off_t) msg->i.offset, &newpos) <
            0)
            return EOVERFLOW;
        break;
    default:
        return EINVAL;
    }
    if (newpos < 0)
        return EINVAL;
    ocb->pos = newpos;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;

    msg->o = (uint64_t) ocb->pos;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int lklfs_close_ocb(ioserv_context_t *ctp,
                           void *reserved,
                           struct lklfs_ocb *ocb)
{
    (void) ctp;
    (void) reserved;
    rtmux_lkl_admission_wait(ctp, NULL, ocb);
    ocb_free(ocb);
    rtmux_lkl_admission_unlock(ctp, NULL, ocb);
    return EOK;
}

static int lklfs_devctl(ioserv_context_t *ctp,
                        io_devctl_t *msg,
                        struct lklfs_ocb *ocb)
{
    union {
        int oflag;
        int mountflag;
    } *data = (void *) (msg + 1);

    if (msg->i.devctl_cmd == DEVCTL_GET_FLAGS) {
        unsigned acc;

        if (!ocb)
            return EINVAL;
        acc = ocb->ioflag & (unsigned) O_ACCMODE;
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
        if (msg->i.nbytes < (int32_t) sizeof(data->oflag) ||
            (size_t) ctp->size < sizeof(*msg) + sizeof(data->oflag))
            return EINVAL;
        ocb->ioflag = (ocb->ioflag & ~(unsigned) O_SETFLAG) |
                      ((unsigned) data->oflag & (unsigned) O_SETFLAG);
        return EOK;
    }
    if (msg->i.devctl_cmd == DEVCTL_GET_MOUNT_FLAGS) {
        int flags = lklfs_rw ? 0 : (int) _MOUNT_READONLY;

        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(flags);
        memcpy(&msg->o + 1, &flags, sizeof(flags));
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(flags));
    }
    if (msg->i.devctl_cmd == DEVCTL_STATVFS) {
        struct lkl_statfs sfs;
        struct statvfs stv;
        long ret;

        if (!ocb || ocb->kind != LKLFS_OCB_LKL || ocb->fd < 0)
            return EINVAL;
        ret = lkl_sys_fstatfs((unsigned int) ocb->fd, &sfs);
        if (ret < 0)
            return lkl_errno(ret);
        lkl_statfs_to_statvfs(&sfs, &stv);
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(stv);
        memcpy(&msg->o + 1, &stv, sizeof(stv));
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(stv));
    }
    if (msg->i.devctl_cmd == DEVCTL_FS_SYNC) {
        /* sync(2) has no failure code; this commits every LKL mount. */
        (void) lkl_sys_sync();
        return EOK;
    }
    return ENOSYS;
}

static int lklfs_msg(ioserv_context_t *ctp,
                     io_msg_t *msg,
                     struct lklfs_ocb *ocb)
{
    struct _io_native_hdr *hdr = (struct _io_native_hdr *) (void *) msg;

    if (!ocb)
        return EBADF;
    if (msg->i.protocol != _IOMSG_RTMUX_NATIVE)
        return ENOSYS;
    if (ctp->size < (int) sizeof(*hdr) || hdr->version != _IO_NATIVE_VERSION ||
        hdr->nxfers > _IO_NATIVE_CAP_TRANSFER_MAX)
        return EINVAL;

    switch (msg->i.subtype) {
    case _IO_NATIVE_STAT: {
        struct stat *st = (struct stat *) (void *) msg;

        if (ctp->size < (int) sizeof(struct _io_native_stat))
            return EINVAL;
        if (ocb->kind == LKLFS_OCB_TEXT) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = (off_t) ocb->text_len;
            st->st_blksize = 512;
            st->st_blocks = 1;
            return IOSERV_PTR(ctp, st, sizeof(*st));
        }
        lkl_stat_to_posix(&ocb->st, st);
        return IOSERV_PTR(ctp, st, sizeof(*st));
    }
    case _IO_NATIVE_READ: {
        struct _io_native_read *rd = (struct _io_native_read *) (void *) msg;
        size_t nbytes;
        long ret;

        if (ctp->size < (int) sizeof(*rd))
            return EINVAL;
        if ((ocb->ioflag & _IO_FLAG_RD) == 0)
            return EBADF;
        if (ocb->kind == LKLFS_OCB_TEXT) {
            if (rd->offset >= (uint64_t) ocb->text_len) {
                SETIOV(ctp->iov, NULL, 0);
                return IOSERV_NPARTS(1);
            }
            nbytes = ocb->text_len - (size_t) rd->offset;
            if (nbytes > rd->nbytes)
                nbytes = rd->nbytes;
            SETIOV(ctp->iov, ocb->text + rd->offset, nbytes);
            return IOSERV_NPARTS(1);
        }
        if (ocb->is_dir)
            return EISDIR;
        if (rd->offset >= (uint64_t) ocb->st.st_size) {
            SETIOV(ctp->iov, NULL, 0);
            return IOSERV_NPARTS(1);
        }
        nbytes = (size_t) ocb->st.st_size - (size_t) rd->offset;
        if (nbytes > rd->nbytes)
            nbytes = rd->nbytes;
        if (nbytes > ctp->msg_max_size)
            nbytes = ctp->msg_max_size;

        char *buf = (char *) (void *) msg;
        ret = lkl_sys_pread64((unsigned int) ocb->fd, buf, nbytes,
                              (off_t) rd->offset);
        if (ret < 0)
            return lkl_errno(ret);
        SETIOV(ctp->iov, buf, (size_t) ret);
        return IOSERV_NPARTS(1);
    }
    case _IO_NATIVE_WRITE: {
        struct _io_native_write *wr = (struct _io_native_write *) (void *) msg;
        long ret;

        if (!lklfs_rw)
            return EROFS;
        if (ctp->size < (int) sizeof(*wr))
            return EINVAL;
        if (ocb->kind == LKLFS_OCB_TEXT)
            return EROFS;
        if ((ocb->ioflag & _IO_FLAG_WR) == 0)
            return EBADF;
        if (ocb->is_dir)
            return EISDIR;
        if (wr->nbytes > (size_t) ctp->size - sizeof(*wr))
            return EINVAL;

        ret = lkl_sys_pwrite64((unsigned int) ocb->fd, (const char *) wr->data,
                               wr->nbytes, (off_t) wr->offset);
        if (ret < 0)
            return lkl_errno(ret);
        (void) lkl_sys_fstat((unsigned int) ocb->fd, &ocb->st);
        _IO_SET_WRITE_NBYTES(ctp, (size_t) ret);
        return EOK;
    }
    case _IO_NATIVE_CTL_WRITE:
        return EROFS;
    default:
        return ENOSYS;
    }
}

static int lklfs_sync(ioserv_context_t *ctp,
                      io_sync_t *msg,
                      struct lklfs_ocb *ocb)
{
    (void) ctp;
    (void) msg;
    if (!ocb)
        return EINVAL;
    if (ocb->kind == LKLFS_OCB_LKL && ocb->fd >= 0) {
        long ret = lkl_sys_fsync((unsigned int) ocb->fd);

        if (ret < 0)
            return lkl_errno(ret);
    }
    return EOK;
}

static int lklfs_fdinfo(ioserv_context_t *ctp,
                        io_fdinfo_t *msg,
                        struct lklfs_ocb *ocb)
{
    char *path = (char *) (&msg->o + 1);
    size_t cap;
    size_t len;

    (void) ctp;
    if (!ocb)
        return EINVAL;
    if (ocb->kind != LKLFS_OCB_LKL || !ocb->is_dir)
        return ENOSYS;

    cap = (size_t) msg->i.path_len;
    len = strlen(ocb->path) + 1;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = (uint32_t) ocb->st.st_mode;
    msg->o.info.offset = (uint64_t) ocb->pos;
    msg->o.info.size = (uint64_t) ocb->st.st_size;
    if (cap > 0) {
        size_t copy = cap < len ? cap : len;

        memcpy(path, ocb->path, copy);
    }
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) (cap < len ? cap : len));
}

static const ioserv_ops_t lklfs_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = lklfs_read,
    .write = lklfs_write,
    .close_ocb = lklfs_close_ocb,
    .stat = lklfs_stat,
    .devctl = lklfs_devctl,
    .fdinfo = lklfs_fdinfo,
    .lseek = lklfs_lseek,
    .msg = lklfs_msg,
    .lock_ocb = lklfs_admission_lock,
    .unlock_ocb = lklfs_admission_unlock,
    .sync = lklfs_sync,
};

static const ioserv_connect_ops_t lklfs_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = lklfs_open,
    .unlink = lklfs_unlink,
};

static int serve_ext4(const char *path, const char *root, int readonly)
{
    struct lkl_disk disk;
    alignas(8) char dirbuf[LKLFS_DIRENT_BUFSZ];
    dispatch_t *dpp;
    long ret;
    long rootfd;
    int fd;
    int disk_id;

    lklfs_rw = readonly ? 0 : 1;
    if (snprintf(lklfs_source, sizeof(lklfs_source), "%s", path) >=
        (int) sizeof(lklfs_source)) {
        fprintf(stderr, "lkl-fsd: source path too long %s\n", path);
        return -1;
    }
    if (!root || root[0] != '/' ||
        snprintf(lklfs_root, sizeof(lklfs_root), "%s", root) >=
            (int) sizeof(lklfs_root)) {
        fprintf(stderr, "lkl-fsd: invalid mountpoint %s\n",
                root ? root : "(null)");
        return -1;
    }
    /* ipc-send: lkl-fsd -> dev-nvme */
    fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "lkl-fsd: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    memset(&disk, 0, sizeof(disk));
    disk.fd = fd;
    disk.ops = &lkl_dev_blk_ops;
    disk_id = lkl_disk_add(&disk);
    if (disk_id < 0) {
        fprintf(stderr, "lkl-fsd: lkl_disk_add: %s\n", lkl_strerror(disk_id));
        close(fd);
        return -1;
    }
    ret = lkl_mount_dev((unsigned int) disk_id, 0, "ext4",
                        readonly ? LKL_MS_RDONLY : 0, readonly ? "noload" : "",
                        lklfs_mount, sizeof(lklfs_mount));
    if (ret < 0) {
        fprintf(stderr, "lkl-fsd: lkl_mount_dev ext4: %s\n",
                lkl_strerror((int) ret));
        lkl_disk_remove(disk);
        close(fd);
        return -1;
    }
    rootfd = lkl_sys_open(lklfs_mount, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
    if (rootfd < 0) {
        fprintf(stderr, "lkl-fsd: open mounted root: %s\n",
                lkl_strerror((int) rootfd));
        return -1;
    }
    ret = lkl_sys_getdents64((unsigned int) rootfd,
                             (struct lkl_linux_dirent64 *) (void *) dirbuf,
                             sizeof(dirbuf));
    lkl_sys_close((unsigned int) rootfd);
    if (ret < 0) {
        fprintf(stderr, "lkl-fsd: read mounted root: %s\n",
                lkl_strerror((int) ret));
        return -1;
    }

    dpp = rtmux_lkl_dispatch_begin("lkl-fsd");
    if (!dpp)
        return -1;
    if (ioserv_attach(dpp, NULL, lklfs_root, _FTYPE_ANY, IOSERV_FLAG_DIR,
                      &lklfs_connect_funcs, &lklfs_io_funcs, NULL) == -1) {
        fprintf(stderr, "lkl-fsd: attach %s: %s\n", lklfs_root,
                strerror(errno));
        return -1;
    }
    if (ioserv_attach(dpp, NULL, "/ctl", _FTYPE_ANY, 0, &lklfs_connect_funcs,
                      &lklfs_io_funcs, (void *) &lklfs_ctl_export) == -1 ||
        ioserv_attach(dpp, NULL, "/stats", _FTYPE_ANY, 0, &lklfs_connect_funcs,
                      &lklfs_io_funcs, (void *) &lklfs_stats_export) == -1 ||
        ioserv_attach(dpp, NULL, "/mounts", _FTYPE_ANY, 0, &lklfs_connect_funcs,
                      &lklfs_io_funcs, (void *) &lklfs_mounts_export) == -1) {
        fprintf(stderr, "lkl-fsd: attach control files: %s\n", strerror(errno));
        return -1;
    }
    fprintf(stderr, "lkl-fsd: serving %s at %s\n", path, lklfs_root);
    return rtmux_lkl_dispatch_serve(dpp, "lkl-fsd");
}

int main(int argc, char **argv)
{
    int ret;

    /* Poison every OCB slot before the first alloc so the alloc-time check has
     * a known-good baseline (no-op in a non-canary build).
     */
    pool_canary_init(lklfs_ocbs, LKLFS_MAX_OCB);

    if (argc == 2 && !strcmp(argv[1], "--host-selftest"))
        return rtmux_lkl_host_selftest() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

#ifdef CONFIG_SERVER_POOL_CANARY
    if (argc == 2 && !strcmp(argv[1], "--canary-selftest"))
        canary_selftest();
#endif

    if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "--boot-only")) &&
        !(argc == 2 && !strcmp(argv[1], "--crash-after-start")) &&
        !(argc >= 3 && argc <= 5 &&
          (!strcmp(argv[1], "--serve-ext4") ||
           !strcmp(argv[1], "--serve-ext4-crash-on-open")))) {
        fprintf(
            stderr,
            "usage: lkl-fsd [--host-selftest|--boot-only|--crash-after-start|"
            "--serve-ext4[-crash-on-open] image [mountpoint] [-r]]\n");
        return EXIT_FAILURE;
    }

    rtmux_lkl_status("lkl-fsd: init\n");
    ret = lkl_init(&lkl_host_ops);
    if (ret < 0) {
        fprintf(stderr, "lkl-fsd: lkl_init: %s\n", lkl_strerror(ret));
        return EXIT_FAILURE;
    }
    if (start_lkl() < 0) {
        lkl_cleanup();
        return EXIT_FAILURE;
    }

    /* lkl-fsd has no delegated MMIO window; once the LKL stack is up, no later
     * request path should acquire new specific-address physical mappings.
     */
    if (rtmux_phys_contain() != EOK) {
        fprintf(stderr, "lkl-fsd: rtmux_phys_contain: %s\n", strerror(errno));
        lkl_cleanup();
        return EXIT_FAILURE;
    }
    if (argc == 2 && !strcmp(argv[1], "--crash-after-start")) {
        volatile int *crash = (volatile int *) 0;

        *crash = 1;
    }
    if (argc == 2) {
        lkl_cleanup();
        return EXIT_SUCCESS;
    }
    if (argc >= 3 && (!strcmp(argv[1], "--serve-ext4") ||
                      !strcmp(argv[1], "--serve-ext4-crash-on-open"))) {
        const char *root = "/fs/root";
        int readonly = 0;

        crash_on_open = !strcmp(argv[1], "--serve-ext4-crash-on-open");

        /* Trailing args after the image are an optional mountpoint and/or the
         * "-r" readonly token, in either order.
         */
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "-r"))
                readonly = 1;
            else
                root = argv[i];
        }
        return serve_ext4(argv[2], root, readonly) < 0 ? EXIT_FAILURE
                                                       : EXIT_SUCCESS;
    }
    rtmux_lkl_status(
        "lkl-fsd: bring-up smoke OK (ext4 image backend; no I/O service loop "
        "yet)\n");
    rtmux_lkl_status("lkl-fsd: planned export surface:\n");
    for (size_t i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
        puts(exports[i]);
    lkl_cleanup();
    return EXIT_SUCCESS;
}
