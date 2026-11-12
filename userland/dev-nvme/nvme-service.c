/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * service surface.
 *
 * Each registered path gets its own nvme_region_t carrying its own starting LBA
 * and length. Handlers recover the region from the OCB's attr pointer and clip
 * reads to its LBA range, so partition nodes cannot read outside their own
 * boundaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
typedef struct nvme_region nvme_region_t;
typedef struct nvme_ocb nvme_ocb_t;
#define IOSERV_HANDLE_T nvme_region_t
#define IOSERV_OCB_T nvme_ocb_t
#include <sys/ioserv.h>
#include <sys/dispatch.h>
#include <devctl/common.h>

#include "nvme.h"
#include "gpt.h"

struct nvme_region {
    uint32_t nsid;
    uint64_t start_lba;
    uint64_t nlba;
    uint64_t size_bytes;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    time_t created;
    char path[32];
};

struct nvme_ocb {
    nvme_region_t *region;
    unsigned ioflag;
    off_t offset;
};

#define NVME_MAX_REGIONS (1 + 16) /* whole-NS + up to 16 partitions */

static nvme_region_t regions[NVME_MAX_REGIONS];
static unsigned n_regions;

/* driver-scope state */

static nvme_ctrl_t *g_ctrl;

/* Single page used both as the per-request DMA bounce for io_read and, during
 * init, as the GPT parser's LBA scratch buffer. The init path runs before the
 * dispatch loop, so there's no collision.
 */
static uint8_t *g_bounce;

/* handlers */

/* Resolve the byte offset for a read/write request. For _IO_XTYPE_NONE the seek
 * position (ocb_off) is used; for _IO_XTYPE_OFFSET the offset rides in the
 * message past the header at xoff_base. Parameters:
 *   ctp       : I/O service context (for the received message size)
 *   xtype     : msg->i.xtype, already masked or raw (only _IO_XTYPE_MASK used)
 *   hdr_size  : sizeof(msg->i), to bound-check the trailing _xtype_offset
 *   xoff_base : address just past msg->i, where _xtype_offset would sit
 *   ocb_off   : the OCB's current offset, used for _IO_XTYPE_NONE
 *   off       : resolved offset (out)
 * Return value: EOK on success, or an errno (EINVAL / ENOSYS) on a bad request.
 */
static int nvme_req_offset(ioserv_context_t *ctp,
                           unsigned xtype,
                           size_t hdr_size,
                           const void *xoff_base,
                           uint64_t ocb_off,
                           uint64_t *off)
{
    switch (xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        *off = ocb_off;
        return EOK;
    case _IO_XTYPE_OFFSET: {
        const struct _xtype_offset *xoff = xoff_base;

        if ((size_t) ctp->size < hdr_size + sizeof(*xoff) || xoff->offset < 0)
            return EINVAL;
        *off = (uint64_t) xoff->offset;
        return EOK;
    }
    default:
        return ENOSYS;
    }
}

/* Compute the single-bounce-page DMA geometry for a request, clipping nbytes so
 * the head-skip plus payload stays within the 4096-byte bounce page.
 * Parameters:
 *   rg        : target region (start_lba)
 *   off       : byte offset within the region
 *   nbytes    : requested length (in/out — clipped to fit one page)
 *   head_skip : byte offset of the payload inside the first LBA (out)
 *   first_lba : first LBA to transfer (out)
 *   nlba      : number of LBAs to transfer (out)
 *   clip      : if true, shrink nbytes so the transfer fits one bounce page
 *               (write path); if false, leave nbytes alone and report EIO when
 *               it would straddle past the page (read path)
 * Return value: EOK, or EIO if the geometry exceeds one bounce page.
 */
static int nvme_req_geometry(const nvme_region_t *rg,
                             uint64_t off,
                             size_t *nbytes,
                             unsigned *head_skip,
                             uint64_t *first_lba,
                             uint64_t *nlba,
                             int clip)
{
    uint64_t lba_size = g_ctrl->ns_lba_size;
    uint64_t last_lba;
    size_t dma_bytes;

    *head_skip = (unsigned) (off % lba_size);
    if (clip && *nbytes > 4096 - *head_skip)
        *nbytes = 4096 - *head_skip;
    *first_lba = rg->start_lba + off / lba_size;
    last_lba = rg->start_lba + (off + *nbytes - 1) / lba_size;
    *nlba = last_lba - *first_lba + 1;
    dma_bytes = (size_t) (*nlba * lba_size);
    return dma_bytes > 4096 ? EIO : EOK;
}

static int nvme_open(ioserv_context_t *ctp,
                     io_open_t *msg,
                     nvme_region_t *rg,
                     void *extra)
{
    nvme_ocb_t *ocb;
    unsigned want;
    mode_t need;
    mode_t allowed;
    struct _client_info info;
    int status;

    (void) extra;

    if (msg->connect.subtype != _IO_CONNECT_OPEN &&
        msg->connect.subtype != _IO_CONNECT_COMBINE &&
        msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)
        return ENOSYS;
    if (msg->connect.file_type != _FTYPE_ANY)
        return ENOSYS;

    want = msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR);

    status = connect_client_info_r(ctp->info.scoid, &info, NGROUPS_MAX);
    if (status != EOK)
        return status;
    need = 0;
    if (want & _IO_FLAG_RD)
        need |= S_IROTH;
    if (want & _IO_FLAG_WR)
        need |= S_IWOTH;
    if (info.cred.euid == 0) {
        allowed = need;
    } else if (info.cred.euid == rg->uid) {
        allowed = (rg->mode >> 6) & 7;
    } else {
        int in_group = info.cred.egid == rg->gid;

        for (uint32_t i = 0; !in_group && i < info.cred.ngroups; i++)
            in_group = info.cred.grouplist[i] == rg->gid;
        allowed = in_group ? ((rg->mode >> 3) & 7) : (rg->mode & 7);
    }
    if ((allowed & need) != need)
        return EACCES;

    ocb = calloc(1, sizeof(*ocb));
    if (!ocb)
        return ENFILE;
    ocb->region = rg;
    ocb->ioflag =
        msg->connect.ioflag & (~(unsigned) _IO_FLAG_MASK | msg->connect.access);
    if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
        int rc = errno;

        free(ocb);
        return rc;
    }
    return EOK;
}

/* io_read handler. Clips the request to the region's byte range, reads the
 * covering LBAs through the bounce page, and replies with the requested slice.
 *
 * Parameters:
 *   ctp : I/O service context
 *   msg : read message
 *   ocb : open control block (its attr is the region)
 * Return value: IOSERV_NOREPLY after msg_reply, IOSERV_NPARTS(0) at
 *   EOF / zero-length, or EIO on a failed device read.
 */
static int nvme_read(ioserv_context_t *ctp, io_read_t *msg, nvme_ocb_t *ocb)
{
    nvme_region_t *rg = ocb->region;
    size_t nbytes;
    uint64_t off;
    unsigned head_skip;
    uint64_t first_lba;
    uint64_t nlba;
    int rc;

    if (!(ocb->ioflag & _IO_FLAG_RD))
        return EBADF;
    if (msg->i.nbytes < 0)
        return EINVAL;
    nbytes = (size_t) msg->i.nbytes;

    if (nbytes == 0)
        return IOSERV_NPARTS(0);
    if (nbytes > 4096)
        nbytes = 4096;

    rc = nvme_req_offset(ctp, msg->i.xtype, sizeof(msg->i), &msg->i + 1,
                         (uint64_t) ocb->offset, &off);
    if (rc != EOK)
        return rc;
    if (off >= rg->size_bytes)
        return IOSERV_NPARTS(0);
    if (nbytes > rg->size_bytes - off)
        nbytes = (size_t) (rg->size_bytes - off);

    /* One bounce page per request: an unaligned transfer that would straddle
     * past the page is rejected with EIO (no read-side clip).
     */
    rc = nvme_req_geometry(rg, off, &nbytes, &head_skip, &first_lba, &nlba, 0);
    if (rc != EOK)
        return rc;

    if (nvme_cmd_read(g_ctrl, rg->nsid, first_lba, (uint16_t) nlba, g_bounce) !=
        0)
        return EIO;

    msg_reply(ctp->rcvid, (int) nbytes, g_bounce + head_skip, (int) nbytes);
    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE)
        ocb->offset += (off_t) nbytes;
    return IOSERV_NOREPLY;
}

/* io_write handler. Clips writes to the region and issues one-page NVMe writes.
 *
 * Parameters:
 *   ctp : I/O service context
 *   msg : write message
 *   ocb : open control block
 * Return value: EOK on success, or EIO on a failed device write.
 */
static int nvme_write(ioserv_context_t *ctp, io_write_t *msg, nvme_ocb_t *ocb)
{
    nvme_region_t *rg = ocb->region;
    size_t nbytes;
    uint64_t off;
    uint64_t first_lba;
    uint64_t nlba;
    unsigned head_skip;
    int skip = (int) sizeof(io_write_t);
    int nread;
    int rc;

    if (!(ocb->ioflag & _IO_FLAG_WR))
        return EBADF;
    rc = nvme_req_offset(ctp, msg->i.xtype, sizeof(msg->i), &msg->i + 1,
                         (uint64_t) ocb->offset, &off);
    if (rc != EOK)
        return rc;
    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_OFFSET)
        skip += (int) sizeof(struct _xtype_offset);
    if (msg->i.nbytes < 0)
        return EINVAL;
    nbytes = (size_t) msg->i.nbytes;
    if (nbytes == 0)
        return EOK;
    if (nbytes > 4096)
        nbytes = 4096;

    if (off >= rg->size_bytes)
        return ENOSPC;
    if (nbytes > rg->size_bytes - off)
        nbytes = (size_t) (rg->size_bytes - off);

    /* One bounce page per request: clip so head_skip + nbytes stays in-page. */
    rc = nvme_req_geometry(rg, off, &nbytes, &head_skip, &first_lba, &nlba, 1);
    if (rc != EOK)
        return rc;

    /* Read-modify-write when the payload does not cover whole LBAs. */
    if (head_skip != 0 || nbytes != (size_t) (nlba * g_ctrl->ns_lba_size)) {
        if (nvme_cmd_read(g_ctrl, rg->nsid, first_lba, (uint16_t) nlba,
                          g_bounce) != 0)
            return EIO;
    }
    nread = ioserv_msgread(ctp, g_bounce + head_skip, (int) nbytes, skip);
    if (nread < 0)
        return errno;
    if ((size_t) nread != nbytes)
        return EIO;
    if (nvme_cmd_write(g_ctrl, rg->nsid, first_lba, (uint16_t) nlba,
                       g_bounce) != 0)
        return EIO;

    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE)
        ocb->offset += (off_t) nbytes;
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return EOK;
}

static int nvme_lseek(ioserv_context_t *ctp, io_lseek_t *msg, nvme_ocb_t *ocb)
{
    nvme_region_t *rg = ocb->region;
    int64_t base;
    int64_t off;

    off = (int64_t) msg->i.offset;

    switch (msg->i.whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = (int64_t) ocb->offset;
        if ((off > 0 && base > INT64_MAX - off) ||
            (off < 0 && base < INT64_MIN - off))
            return EINVAL;
        off += base;
        break;
    case SEEK_END:
        base = (int64_t) rg->size_bytes;
        if ((off > 0 && base > INT64_MAX - off) ||
            (off < 0 && base < INT64_MIN - off))
            return EINVAL;
        off += base;
        break;
    default:
        return EINVAL;
    }
    if (off < 0)
        return EINVAL;
    ocb->offset = (off_t) off;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;
    msg->o = (uint64_t) off;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

/* io_stat handler. Reports the region as a block device of its byte size.
 *
 * Parameters:
 *   ctp : I/O service context
 *   msg : stat message (its o field is filled in)
 *   ocb : open control block (its attr is the region)
 * Return value: IOSERV_PTR pointing at the filled stat struct.
 */
static int nvme_stat(ioserv_context_t *ctp, io_stat_t *msg, nvme_ocb_t *ocb)
{
    nvme_region_t *rg = ocb->region;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_ino = 1;
    msg->o.st_size = (off_t) rg->size_bytes;
    msg->o.st_mode = rg->mode;
    msg->o.st_nlink = 1;
    msg->o.st_uid = rg->uid;
    msg->o.st_gid = rg->gid;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = rg->created;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int nvme_devctl(ioserv_context_t *ctp, io_devctl_t *msg, nvme_ocb_t *ocb)
{
    union {
        int oflag;
        int mountflag;
    } *data = (void *) (msg + 1);

    switch ((unsigned) msg->i.devctl_cmd) {
    case (unsigned) DEVCTL_GET_FLAGS:
        data->oflag =
            (ocb->ioflag & O_SETFLAG) | ((ocb->ioflag - 1) & O_ACCMODE);
        msg->o.ret_val = 0;
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(data->oflag));
    case (unsigned) DEVCTL_SET_FLAGS:
        /* Reject a client that did not actually send the oflag payload, so a
         * short message cannot make us read stale dispatch-buffer bytes as
         * flags (mirrors the guard in servers/lkl-fsd).
         */
        if (msg->i.nbytes < (int32_t) sizeof(data->oflag) ||
            (size_t) ctp->size < sizeof(*msg) + sizeof(data->oflag))
            return EINVAL;
        ocb->ioflag = (ocb->ioflag & ~(unsigned) O_SETFLAG) |
                      ((unsigned) data->oflag & (unsigned) O_SETFLAG);
        return EOK;
    case (unsigned) DEVCTL_GET_MOUNT_FLAGS:
        data->mountflag = 0;
        msg->o.ret_val = 0;
        return IOSERV_PTR(ctp, &msg->o,
                          sizeof(msg->o) + sizeof(data->mountflag));
    default:
        return IOSERV_DEFAULT;
    }
}


static int nvme_close_ocb(ioserv_context_t *ctp,
                          void *reserved,
                          nvme_ocb_t *ocb)
{
    (void) ctp;
    (void) reserved;
    free(ocb);
    return EOK;
}

/* ---- GPT read callback ------------------------------------------ *
 *
 * The parser asks for up to ~32 LBAs in one call (the partition-entry array).
 * Our nvme_cmd_read is single-PRP-page, so we loop at up to (page_size /
 * lba_size) LBAs per call, bouncing through g_bounce.
 */
static ssize_t gpt_read_cb(void *ctx, uint64_t lba, void *buf, size_t n_lba)
{
    nvme_ctrl_t *c = ctx;
    uint64_t lba_size = c->ns_lba_size;
    size_t per_call = 4096 / lba_size;
    uint8_t *dst = buf;
    size_t done = 0;

    if (per_call == 0)
        return -1;
    while (done < n_lba) {
        size_t want = n_lba - done;
        if (want > per_call)
            want = per_call;
        if (nvme_cmd_read(c, c->ns_nsid, lba + done, (uint16_t) want,
                          g_bounce) != 0)
            return -1;
        memcpy(dst + done * lba_size, g_bounce, want * lba_size);
        done += want;
    }
    return (ssize_t) n_lba;
}

/* region registration helpers */

static const ioserv_connect_ops_t connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = nvme_open,
};
static const ioserv_ops_t io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = nvme_read,
    .write = nvme_write,
    .close_ocb = nvme_close_ocb,
    .stat = nvme_stat,
    .devctl = nvme_devctl,
    .lseek = nvme_lseek,
};

/* Allocate a region, initialize its attribute, and register its path with the
 * I/O service.
 *
 * Parameters:
 *   dpp       : dispatch handle
 *   nsid      : namespace id this region lives in
 *   start_lba : region's first LBA within the namespace
 *   nlba      : region length in LBAs
 *   path      : device path to publish (e.g. "/dev/nvme0n1p1")
 * Return value: 0 on success, -1 if the table is full or attach fails.
 */
static int register_region(dispatch_t *dpp,
                           uint32_t nsid,
                           uint64_t start_lba,
                           uint64_t nlba,
                           const char *path)
{
    if (n_regions >= NVME_MAX_REGIONS) {
        fprintf(stderr, "dev-nvme: too many regions (max %d)\n",
                NVME_MAX_REGIONS);
        return -1;
    }
    nvme_region_t *rg = &regions[n_regions];

    /* Mode must match what nvme_stat() reports. Raw access is root-only: raw
     * block reads bypass file permissions (for example the 0600 shadow file on
     * the romfs partition). lkl-fsd runs with root authority.
     */
    rg->nsid = nsid;
    rg->start_lba = start_lba;
    rg->nlba = nlba;
    rg->mode = S_IFBLK | 0600;
    rg->uid = 0;
    rg->gid = 0;
    rg->created = time(NULL);

    /* nlba is namespace-controlled; reject sizes that would wrap before the
     * multiply poisons every later bounds check.
     */
    if (g_ctrl->ns_lba_size == 0 || nlba > UINT64_MAX / g_ctrl->ns_lba_size) {
        fprintf(stderr, "dev-nvme: region %s size overflow\n", path);
        return -1;
    }

    /* The bounce buffer is a single 4096-byte page; a larger sector can't be
     * staged and would underflow 4096 - head_skip in the read/write paths.
     */
    if (g_ctrl->ns_lba_size > 4096) {
        fprintf(stderr, "dev-nvme: LBA size %lu > 4096 unsupported\n",
                (unsigned long) g_ctrl->ns_lba_size);
        return -1;
    }
    rg->size_bytes = nlba * g_ctrl->ns_lba_size;

    /* st_size and lseek offsets are signed off_t; a region past INT64_MAX would
     * report negative and confuse stat/lseek consumers.
     */
    if (rg->size_bytes > (uint64_t) INT64_MAX) {
        fprintf(stderr, "dev-nvme: region %s too large for off_t\n", path);
        return -1;
    }
    strncpy(rg->path, path, sizeof(rg->path) - 1);
    rg->path[sizeof(rg->path) - 1] = '\0';

    int rc = ioserv_attach(dpp, NULL, rg->path, _FTYPE_ANY, 0, &connect_funcs,
                           &io_funcs, rg);
    if (rc == -1) {
        fprintf(stderr, "dev-nvme: ioserv_attach(%s) failed: %s\n", rg->path,
                strerror(errno));
        return -1;
    }
    n_regions++;
    return 0;
}

/* ASCII rendering helpers */

/* Render a 16-byte GUID in the standard 8-4-4-4-12 mixed-endian form. */
static void guid_to_str(const uint8_t *g, char *out)
{
    snprintf(
        out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g[3], g[2], g[1], g[0], /* Data1 little-endian */
        g[5], g[4],             /* Data2 little-endian */
        g[7], g[6],             /* Data3 little-endian */
        g[8], g[9],             /* Data4 big-endian from here */
        g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* entry point */

/* Register device paths and run the I/O service dispatch loop.
 *
 * Publishes /dev/nvme0n1 for the whole namespace, parses the GPT and publishes
 * a node per partition (non-fatal if absent), detaches from proc service, then
 * blocks servicing client requests forever. Parameters:
 *   c   : controller instance
 *   dpp : dispatch handle
 * Return value: -1 on setup failure; does not return on success.
 */
int nvme_ioserv_run(nvme_ctrl_t *c, dispatch_t *dpp)
{
    g_ctrl = c;
    n_regions = 0;

    g_bounce = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                    MAP_ANON | MAP_SHARED, NOFD, 0);
    if (g_bounce == MAP_FAILED) {
        fprintf(stderr, "dev-nvme: mmap bounce: %s\n", strerror(errno));
        return -1;
    }

    /* /dev/nvme0n1 — whole namespace. */
    if (register_region(dpp, c->ns_nsid, 0, c->ns_nlba, "/dev/nvme0n1") != 0)
        return -1;

    /* Walk the GPT. Any failure (no GPT, bad CRC, hybrid MBR, ...) is
     * non-fatal: we still serve /dev/nvme0n1 as a raw device.
     */
    gpt_table_t gpt;
    int rc = gpt_parse(gpt_read_cb, c, (uint32_t) c->ns_lba_size, &gpt);
    if (rc == GPT_OK) {
        fprintf(stderr, "dev-nvme: GPT OK, %u partition(s)\n", gpt.n_parts);
        for (unsigned i = 0; i < gpt.n_parts && n_regions < NVME_MAX_REGIONS;
             i++) {
            gpt_part_t *p = &gpt.parts[i];

            /* A valid-CRC GPT can still carry a garbage entry: reject ranges
             * that underflow nlba or fall outside the namespace.
             */
            if (p->end_lba < p->start_lba || p->start_lba >= c->ns_nlba ||
                p->end_lba >= c->ns_nlba) {
                fprintf(stderr, "  p%u: out-of-range LBA %lu..%lu — skipped\n",
                        i + 1, (unsigned long) p->start_lba,
                        (unsigned long) p->end_lba);
                continue;
            }
            uint64_t nlba = p->end_lba - p->start_lba + 1;
            char path[32];
            char tguid[37];
            snprintf(path, sizeof(path), "/dev/nvme0n1p%u", i + 1);
            guid_to_str(p->type_guid, tguid);
            fprintf(
                stderr, "  p%u: LBA %lu..%lu (%lu MiB) type=%s name=\"%s\"\n",
                i + 1, (unsigned long) p->start_lba, (unsigned long) p->end_lba,
                (unsigned long) ((nlba * c->ns_lba_size) / (1024 * 1024)),
                tguid, p->name);
            if (register_region(dpp, c->ns_nsid, p->start_lba, nlba, path) !=
                0) {
                gpt_free(&gpt);
                return -1;
            }
        }
        gpt_free(&gpt);
    } else {
        fprintf(stderr,
                "dev-nvme: no usable GPT (rc=%d) — raw /dev/nvme0n1 only\n",
                rc);
    }

    fprintf(stderr,
            "dev-nvme: ready (%u path(s), first NS %lu MiB %lu B/LBA)\n",
            n_regions, (unsigned long) (c->ns_size_bytes / (1024 * 1024)),
            (unsigned long) c->ns_lba_size);

    /* Partition discovery above used the polling command path. Switch to
     * interrupt-driven completion for steady-state serving: spawn the IST, then
     * flip ist_ready so exec_cmd waits on the per-queue gate instead of
     * polling. A failed IST start is fatal, not a fall back to polling: once
     * the IST thread is spawned it may attach and drain the completion queue,
     * so a concurrent poller would race it for CQEs. Aborting here exits the
     * server and takes the IST thread with it, which is safe; a silent race is
     * not.
     */
    if (nvme_ist_start(c) != 0) {
        fprintf(stderr, "dev-nvme: IST start failed (IRQ=%u vector=%u)\n",
                c->irq, c->vector);
        return -1;
    }
    c->ist_ready = 1;
    fprintf(stderr, "dev-nvme: using interrupt mode (IRQ=%u vector=%u)\n",
            c->irq, c->vector);

    dispatch_context_t *dctp = dispatch_context_alloc(dpp);
    if (!dctp) {
        fprintf(stderr, "dev-nvme: dispatch_context_alloc failed\n");
        return -1;
    }
    if (rtmux_detach(EXIT_SUCCESS) != EOK) {
        fprintf(stderr, "dev-nvme: rtmux_detach failed: %s\n", strerror(errno));
        return -1;
    }
    for (;;) {
        dctp = dispatch_block(dctp);
        if (dctp)
            dispatch_handler(dctp);
    }
    /* NOTREACHED */
}
