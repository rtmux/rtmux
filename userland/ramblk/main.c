/* SPDX-License-Identifier: Apache-2.0 */
/* Read-only, root-owned RAM block devices backed by an immutable boot image.
 * This is an ordinary native I/O service, with no privileged MMIO or DMA.
 * It provides a storage backend for boards without a disk driver and lets the
 * same block/VFS contracts run independently of the NVMe hardware tests. */
struct ram_ocb;
#define IOSERV_OCB_T struct ram_ocb
#define IOSERV_HANDLE_T void
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/dispatch.h>
#include <sys/ioserv.h>
#include <sys/rtmux-core.h>
#include <devctl/common.h>

struct ram_ocb {
    off_t offset;
    unsigned ioflag;
};
static unsigned char *image;
static size_t image_size;

static int ram_open(ioserv_context_t *ctp, io_open_t *msg, void *handle,
                    void *extra)
{
    struct _client_info info;
    struct ram_ocb *ocb;
    int rc;
    (void) handle;
    (void) extra;
    if (msg->connect.subtype != _IO_CONNECT_OPEN &&
        msg->connect.subtype != _IO_CONNECT_COMBINE &&
        msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)
        return ENOSYS;
    if (msg->connect.file_type != _FTYPE_ANY)
        return ENOSYS;
    rc = connect_client_info_r(ctp->info.scoid, &info, 0);
    if (rc != EOK)
        return rc;
    /* Check credentials before the read-only policy, as for physical disks. */
    if (info.cred.euid != 0)
        return EACCES;
    if (msg->connect.ioflag & _IO_FLAG_WR)
        return EROFS;
    ocb = calloc(1, sizeof(*ocb));
    if (!ocb)
        return ENFILE;
    ocb->ioflag = msg->connect.ioflag;
    if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
        rc = errno;
        free(ocb);
        return rc;
    }
    return EOK;
}

static int ram_read(ioserv_context_t *ctp, io_read_t *msg, struct ram_ocb *ocb)
{
    off_t off = ocb->offset;
    size_t n;
    unsigned xtype = msg->i.xtype & _IO_XTYPE_MASK;
    if (!(ocb->ioflag & _IO_FLAG_RD))
        return EBADF;
    if (msg->i.nbytes < 0)
        return EINVAL;
    if (xtype == _IO_XTYPE_OFFSET) {
        struct _xtype_offset *x = (void *) (&msg->i + 1);
        if ((size_t) ctp->size < sizeof(msg->i) + sizeof(*x))
            return EINVAL;
        off = x->offset;
    } else if (xtype != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    if (off < 0)
        return EINVAL;
    n = (size_t) msg->i.nbytes;
    if ((uint64_t) off >= image_size || !n) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return IOSERV_NPARTS(0);
    }
    if (n > 4096)
        n = 4096;
    if (n > image_size - (size_t) off)
        n = image_size - (size_t) off;
    (void) msg_reply(ctp->rcvid, (int) n, image + off, (int) n);
    if (xtype == _IO_XTYPE_NONE)
        ocb->offset += (off_t) n;
    return IOSERV_NOREPLY;
}

static int ram_seek(ioserv_context_t *ctp, io_lseek_t *msg, struct ram_ocb *ocb)
{
    int64_t base, off = (int64_t) msg->i.offset;
    switch (msg->i.whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = ocb->offset; break;
    case SEEK_END: base = (int64_t) image_size; break;
    default: return EINVAL;
    }
    if ((off > 0 && base > INT64_MAX - off) ||
        (off < 0 && base < INT64_MIN - off) || base + off < 0)
        return EINVAL;
    ocb->offset = base + off;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;
    msg->o = (uint64_t) ocb->offset;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int ram_stat(ioserv_context_t *ctp, io_stat_t *msg, struct ram_ocb *ocb)
{
    (void) ocb;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_mode = S_IFBLK | 0600;
    msg->o.st_nlink = 1;
    msg->o.st_size = (off_t) image_size;
    msg->o.st_blksize = 512;
    msg->o.st_blocks = (blkcnt_t) ((image_size + 511) / 512);
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int ram_close(ioserv_context_t *ctp, void *reserved, struct ram_ocb *ocb)
{
    (void) ctp;
    (void) reserved;
    free(ocb);
    return EOK;
}

static int ram_sync(ioserv_context_t *ctp, io_sync_t *msg, struct ram_ocb *ocb)
{
    (void) ctp;
    (void) msg;
    (void) ocb;
    return EOK;
}

static const ioserv_connect_ops_t connect_ops = {
    .nfuncs = IOSERV_CONNECT_NFUNCS, .open = ram_open,
};
static const ioserv_ops_t io_ops = {
    .nfuncs = IOSERV_IO_NFUNCS, .read = ram_read, .lseek = ram_seek,
    .stat = ram_stat, .close_ocb = ram_close, .sync = ram_sync,
};

int main(int argc, char **argv)
{
    struct stat st;
    int fd;
    if (argc < 3) {
        fprintf(stderr, "usage: ramblk IMAGE /dev/ram0 [/dev/ram1 ...]\n");
        return 1;
    }
    /* Boot image lookup is served by roottask before publishing this device.
     * ipc-send: ramblk -> roottask-core
     */
    fd = open(argv[1], O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 ||
        st.st_size > 64 * 1024 * 1024) {
        fprintf(stderr, "ramblk: cannot load image %s\n", argv[1]);
        return 1;
    }
    image_size = (size_t) st.st_size;
    image = malloc(image_size);
    if (!image)
        return 1;
    for (size_t done = 0; done < image_size;) {
        ssize_t n = read(fd, image + done, image_size - done);
        if (n <= 0)
            return 1;
        done += (size_t) n;
    }
    close(fd);
    dispatch_t *dpp = dispatch_create();
    if (!dpp)
        return 1;
    for (int i = 2; i < argc; i++) {
        if (ioserv_attach(dpp, NULL, argv[i], _FTYPE_ANY, 0,
                          &connect_ops, &io_ops, NULL) == -1) {
            fprintf(stderr, "ramblk: attach %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
    }
    dispatch_context_t *ctx = dispatch_context_alloc(dpp);
    if (!ctx || rtmux_detach(EXIT_SUCCESS) != EOK)
        return 1;
    for (;;) {
        ctx = dispatch_block(ctx);
        if (ctx)
            dispatch_handler(ctx);
    }
}
