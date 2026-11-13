/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * sysfs - synthesized read-only I/O service mounted at /sys.
 *
 * /proc keeps just per-pid directories; everything that's "the kernel
 * describing itself" lives here.
 *
 * /sys/log — roottask's log ring, root only (mode 0400). Streamed from the ring
 * by absolute sequence, not snapshotted: it is larger than roottask's arena.
 *
 * Initial entries:
 *
 * /sys/cmdline — kernel boot command line (read from syspage)
 *
 * /sys/version — RTMUX_VERSION_STRING " " RTMUX_BUILD_DATE
 *
 * /sys/kernelversion — kernel release and build id (read from syspage)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <align.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/iomsg.h>
#include <sys/mount.h>
#include <sys/pool-canary.h>
#include <sys/syspage.h>
#include <devctl/common.h>
#include "externs.h"
#include <generated/autoconf.h> /* CONFIG_MAX_BOOTLINE_SIZE */
#include <rtmux-version.h>
#include <private/kstats.h>
#include "mm-internal.h"
#include "root-process.h"
#include "pinode.h"
#include "root-log.h"

/* Buffer sizes derived from the actual constraints they bound. A hardcoded
 * literal here would silently truncate /sys/cmdline if a future Kconfig change
 * bumps CONFIG_MAX_BOOTLINE_SIZE.
 */

/* RTMUX_VERSION_STRING + ' ' + RTMUX_BUILD_DATE + '\n' + NUL. sizeof on a
 * string literal includes the NUL — we add one extra for the separator +
 * newline; the second NUL is absorbed.
 */
#define SYSFS_VERSION_BUFSZ \
    (sizeof(RTMUX_VERSION_STRING) + sizeof(RTMUX_BUILD_DATE) + 2)

/* boot_command_line + '\n' + NUL.  Tracks the kernel-side limit. */
#define SYSFS_CMDLINE_BUFSZ (CONFIG_MAX_BOOTLINE_SIZE + 2)

/* board_compatible[128] in board.c, with '\n' + NUL. */
#define SYSFS_BOARD_BUFSZ (128 + 2)
#define SYSFS_IOSERV_ACCOUNTING_BUFSZ 2048
#define SYSFS_NAMESPACE_STATS_BUFSZ 512

/* 3 lines per pool (~40 B each) + pa.free_bytes + a line per hart, with
 * headroom.
 */
#define SYSFS_KSTATS_BUFSZ 4096

static dev_t sysfs_devno;

/* The two synthesized files. Content is materialised once at sysfs_init time
 * and stays valid for the life of roottask.
 */
static char sysfs_cmdline[SYSFS_CMDLINE_BUFSZ];
static const char *sysfs_cmdline_ptr; /* always = sysfs_cmdline */
static size_t sysfs_cmdline_len;
static char sysfs_version[SYSFS_VERSION_BUFSZ];
static const char *sysfs_version_ptr; /* always = sysfs_version */
static size_t sysfs_version_len;
static char sysfs_board[SYSFS_BOARD_BUFSZ];
static const char *sysfs_board_ptr; /* always = sysfs_board */
static size_t sysfs_board_len;
/* release[64] + ' ' + build_id[16] + '\n' + NUL. */
static char sysfs_kernelversion[64 + 1 + 16 + 2];
static const char *sysfs_kernelversion_ptr; /* always = sysfs_kernelversion */
static size_t sysfs_kernelversion_len;

static size_t synth_namespace_stats(char *buf, size_t cap)
{
    return namespace_stats_snapshot(buf, cap);
}

/* /sys/kstats: "key=value" lines, one per gauge. Keys are documented in
 * docs/kstats.md. Only aggregate counts; no object ids or pointers.
 */
static size_t synth_kstats(char *buf, size_t cap)
{
    static const char *const names[KSTAT_SOUL_COUNT] = {
#define KSTAT_SOUL_NAME(name, var) [KSTAT_SOUL_##name] = #name,
        KSTAT_SOUL_LIST(KSTAT_SOUL_NAME)
#undef KSTAT_SOUL_NAME
    };
    struct kstat_soul_rec recs[KSTAT_SOUL_COUNT];
    unsigned count = 0;
    unsigned live_threads = 0;
    unsigned live_timeouts = 0;
    uint64_t timer_intr[CONFIG_PROCESSORS_MAX];
    unsigned timer_count = 0;
    size_t len = 0;
    int n, rc; /* n: snprintf result; rc: soul_stats status */

    rc = soul_stats(recs, KSTAT_SOUL_COUNT, &count, &live_threads,
                    &live_timeouts, timer_intr, CONFIG_PROCESSORS_MAX,
                    &timer_count);
    if (rc != EOK) {
        /* Say so rather than return a file that looks healthy but has no pool
         * gauges.
         */
        count = 0;
        len = (size_t) snprintf(buf, cap, "soul.error=%d\n", rc);
    }
    for (unsigned i = 0; i < count && len < cap; i++) {
        n = snprintf(buf + len, cap - len,
                     "soul.%s.used=%u\nsoul.%s.reserved=%u\nsoul.%s.total=%u\n",
                     names[recs[i].id], recs[i].used, names[recs[i].id],
                     recs[i].reserved, names[recs[i].id], recs[i].total);
        if (n < 0 || (size_t) n >= cap - len)
            break;
        len += (size_t) n;
    }
    if (len < cap) {
        n = snprintf(buf + len, cap - len, "thread.live=%u\ntimeout.live=%u\n",
                     live_threads, live_timeouts);
        if (n > 0 && (size_t) n < cap - len)
            len += (size_t) n;
    }
    if (len < cap) {
        n = snprintf(buf + len, cap - len, "pa.free_bytes=%llu\nroottask.heap_used=%zu\n",
                     (unsigned long long) pa_free_size(), roottask_heap_in_use());
        if (n > 0 && (size_t) n < cap - len)
            len += (size_t) n;
    }
    if (len < cap) {
        ioserv_reply_counters_t rc;

        /* roottask's own process-local libc counter, not a system-wide one. */
        ioserv_reply_counters_snapshot(&rc);
        n = snprintf(buf + len, cap - len, "ioserv.disconnects=%llu\n",
                     (unsigned long long) rc.disconnects);
        if (n > 0 && (size_t) n < cap - len)
            len += (size_t) n;
    }
    if (rc == EOK && timer_count > CONFIG_PROCESSORS_MAX)
        timer_count = CONFIG_PROCESSORS_MAX;
    for (unsigned h = 0; rc == EOK && h < timer_count && len < cap; h++) {
        int w = snprintf(buf + len, cap - len, "timer.%u.intr=%llu\n", h,
                         (unsigned long long) timer_intr[h]);

        if (w < 0 || (size_t) w >= cap - len)
            break;
        len += (size_t) w;
    }
    return len;
}

static size_t synth_ioserv_accounting(char *buf, size_t cap)
{
    return namespace_accounting_snapshot(buf, cap);
}

/* Per-open state. mode + (data, size) is enough — readdir uses pos. Dynamic
 * entries (synthesized on each open) own a malloc'd buffer in dyn_buf which
 * sysfs_close_ocb frees; static entries leave it NULL.
 */
struct sysfs_ocb {
    POOL_CANARY_HEAD
    uint32_t mode;
    uint64_t start_seq; /* stream entries: ring window [start_seq, end_seq) */
    uint64_t end_seq;
    const char *data;
    size_t size;
    off_t pos;
    int idx;
    char *dyn_buf;
    POOL_CANARY_TAIL
};

#define SYSFS_MAX_OCBS 16
static struct sysfs_ocb ocb_pool[SYSFS_MAX_OCBS];
static int ocb_inuse[SYSFS_MAX_OCBS];

/* Static directory listing. Order matters — readdir walks it.
 *
 * An entry is either *static* (content_ptr/content_len_ptr point to a post-init
 * buffer that lives forever, e.g. /sys/version) or *dynamic* (synth()
 * materialises a snapshot into a per-open buffer at open time). When synth is
 * non-NULL, content_ptr and content_len_ptr are ignored. dyn_bufsz must be set
 * to a comfortable upper bound on the synthesized output.
 *
 * A *stream* entry (stream_read non-NULL) is too big to snapshot. Open records
 * the window the ring holds, and each read pulls at most dyn_bufsz bytes from
 * it through stream_read into the per-open dyn_buf. mode is the permission
 * bits; open fails with EACCES for a non-root caller unless others may read.
 * stat reaches the same open path, so it fails the same way: intended, there is
 * no separate metadata gate.
 */
struct sysfs_entry {
    const char *name;
    const char *const *content_ptr;
    const size_t *content_len_ptr;
    size_t (*synth)(char *buf, size_t cap);
    size_t dyn_bufsz;
    unsigned mode;
    ssize_t (*stream_read)(char *dst, uint64_t *seq, uint64_t limit, size_t n);
};

static const struct sysfs_entry sysfs_entries[] = {
    {"board", &sysfs_board_ptr, &sysfs_board_len, NULL, 0, 0444, NULL},
    {"cmdline", &sysfs_cmdline_ptr, &sysfs_cmdline_len, NULL, 0, 0444, NULL},
    {"namespace-stats", NULL, NULL, synth_namespace_stats,
     SYSFS_NAMESPACE_STATS_BUFSZ, 0444, NULL},
    {"ioserv-accounting", NULL, NULL, synth_ioserv_accounting,
     SYSFS_IOSERV_ACCOUNTING_BUFSZ, 0444, NULL},
    {"kernelversion", &sysfs_kernelversion_ptr, &sysfs_kernelversion_len, NULL,
     0, 0444, NULL},
    {"kstats", NULL, NULL, synth_kstats, SYSFS_KSTATS_BUFSZ, 0444, NULL},
    {"log", NULL, NULL, NULL, ROOT_LOG_READ_MAX, 0400, root_log_read},
    {"version", &sysfs_version_ptr, &sysfs_version_len, NULL, 0, 0444, NULL},
};
#define SYSFS_NENTRIES \
    ((int) (sizeof(sysfs_entries) / sizeof(sysfs_entries[0])))

static struct sysfs_ocb *ocb_alloc(void)
{
    for (int i = 0; i < SYSFS_MAX_OCBS; i++) {
        if (!ocb_inuse[i]) {
            pool_canary_check(&ocb_pool[i], "sysfs_ocb", i);
            ocb_inuse[i] = 1;
            memset(&ocb_pool[i], 0, sizeof(ocb_pool[i]));
            pool_canary_arm(&ocb_pool[i]);
            return &ocb_pool[i];
        }
    }
    return NULL;
}

static void ocb_free(struct sysfs_ocb *ocb)
{
    for (int i = 0; i < SYSFS_MAX_OCBS; i++) {
        if (&ocb_pool[i] == ocb) {
            pool_canary_check(ocb, "sysfs_ocb", i);
            ocb_inuse[i] = 0;
            return;
        }
    }
}

/* Resolve a path relative to /sys. On success returns the kind:
 *   0 — not found
 *   1 — root directory
 *   2 — known file (sets *idx_out to the entry index)
 *
 * Caller is expected to inspect sysfs_entries[*idx_out].synth to decide whether
 * the entry is static or dynamic.
 */
static int resolve_path(const char *path, int *idx_out)
{
    /* "/sys" itself, or "/sys/" — the root directory */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return 1;

    if (path[0] == '/')
        path++;

    for (int i = 0; i < SYSFS_NENTRIES; i++) {
        if (!strcmp(path, sysfs_entries[i].name)) {
            *idx_out = i;
            return 2;
        }
    }
    return 0;
}

/* Take a fresh ring window for a stream entry: everything the ring holds now.
 */
static void stream_resample(struct sysfs_ocb *ocb)
{
    root_log_bounds(&ocb->start_seq, &ocb->end_seq);
    ocb->size = (size_t) (ocb->end_seq - ocb->start_seq);
}

/* Connect functions */
static int sysfs_open(ioserv_context_t *ctp,
                      io_open_t *msg,
                      void *extra,
                      void *reserved)
{
    int kind, idx = -1;
    struct sysfs_ocb *ocb;

    (void) extra;
    (void) reserved;
    kind = resolve_path(msg->connect.path, &idx);
    if (kind == 0)
        return ENOENT;

    if (!(ocb = ocb_alloc()))
        return ENOMEM;

    if (kind == 1) {
        ocb->mode = S_IFDIR | 0555;
        ocb->idx = -1;
    } else {
        const struct sysfs_entry *e = &sysfs_entries[idx];

        if (!(e->mode & 0004) && !proc_isaccess(0, &ctp->info)) {
            ocb_free(ocb);
            return EACCES;
        }
        ocb->mode = S_IFREG | e->mode;
        ocb->idx = idx;
        if (e->stream_read) {
            ocb->dyn_buf = malloc(e->dyn_bufsz);
            if (!ocb->dyn_buf) {
                ocb_free(ocb);
                return ENOMEM;
            }
            stream_resample(ocb);
        } else if (e->synth) {
            /* Dynamic — snapshot into a per-open buffer. */
            ocb->dyn_buf = malloc(e->dyn_bufsz);
            if (!ocb->dyn_buf) {
                ocb_free(ocb);
                return ENOMEM;
            }
            ocb->size = e->synth(ocb->dyn_buf, e->dyn_bufsz);
            ocb->data = ocb->dyn_buf;
        } else {
            ocb->data = *e->content_ptr;
            ocb->size = *e->content_len_ptr;
        }
    }
    ocb->pos = 0;

    if (ioserv_open_bind(ctp, ocb, 0) == -1) {
        if (ocb->dyn_buf)
            free(ocb->dyn_buf);
        ocb_free(ocb);
        return errno;
    }
    return EOK;
}

static int sysfs_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *handle)
{
    struct sysfs_ocb *ocb = handle;
    char *path = (char *) (&msg->o + 1);
    int len;
    size_t cap;

    if (!ocb)
        return EBADF;

    cap = (size_t) msg->i.path_len;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = ocb->mode;
    msg->o.info.offset = (uint64_t) ocb->pos;
    msg->o.info.size = (uint64_t) ocb->size;
    if (ocb->idx < 0)
        len = snprintf(path, cap, "/sys");
    else
        len = snprintf(path, cap, "/sys/%s", sysfs_entries[ocb->idx].name);
    if (len < 0)
        return EINVAL;
    _IO_SET_FDINFO_LEN(ctp, len + 1);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) min(cap, (size_t) len + 1));
}

/* IO functions */
static int sysfs_readdir(ioserv_context_t *ctp, io_read_t *msg, void *handle)
{
    struct sysfs_ocb *ocb = handle;
    struct dirent *dp = (struct dirent *) (void *) msg;
    size_t used = 0;
    size_t max = msg->i.nbytes;

    while (ocb->pos < SYSFS_NENTRIES) {
        const char *name = sysfs_entries[ocb->pos].name;
        size_t nlen = strlen(name);
        size_t reclen = offsetof(struct dirent, d_name) + nlen + 1;
        reclen = lib_align_up_size(reclen, 8);

        if (used + reclen > max)
            break;

        struct dirent *de = (struct dirent *) ((char *) dp + used);
        de->d_ino = (ino_t) (ocb->pos + 1);
        de->d_reclen = (uint16_t) reclen;
        de->d_namelen = (uint16_t) nlen;
        memcpy(de->d_name, name, nlen + 1);

        used += reclen;
        ocb->pos++;
    }

    _IO_SET_READ_NBYTES(ctp, used);
    if (used > 0) {
        SETIOV(ctp->iov, dp, used);
        return IOSERV_NPARTS(1);
    }
    return EOK;
}

static int sysfs_read(ioserv_context_t *ctp, io_read_t *msg, void *handle)
{
    struct sysfs_ocb *ocb = handle;
    size_t nbytes;
    size_t avail;

    if (!ocb)
        return EBADF;

    if (S_ISDIR(ocb->mode))
        return sysfs_readdir(ctp, msg, handle);

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    case _IO_XTYPE_OFFSET: {
        struct _xtype_offset *xoff = (void *) (&msg->i + 1);
        if (xoff->offset < 0)
            return EINVAL;
        ocb->pos = xoff->offset;
        break;
    }
    default:
        return ENOSYS;
    }

    if (ocb->pos >= (off_t) ocb->size) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    nbytes = msg->i.nbytes;
    avail = (size_t) ((off_t) ocb->size - ocb->pos);
    if (nbytes > avail)
        nbytes = avail;

    if (sysfs_entries[ocb->idx].stream_read) {
        uint64_t seq = ocb->start_seq + (uint64_t) ocb->pos;
        ssize_t got = sysfs_entries[ocb->idx].stream_read(ocb->dyn_buf, &seq,
                                                          ocb->end_seq, nbytes);

        /* The ring wrapped past this open's window: the caller must reopen. */
        if (got < 0)
            return (int) -got;
        ocb->pos += got;
        SETIOV(ctp->iov, ocb->dyn_buf, (size_t) got);
        _IO_SET_READ_NBYTES(ctp, (size_t) got);
        return got ? IOSERV_NPARTS(1) : EOK;
    }

    SETIOV(ctp->iov, (void *) (ocb->data + ocb->pos), nbytes);
    ocb->pos += (off_t) nbytes;

    _IO_SET_READ_NBYTES(ctp, nbytes);
    return IOSERV_NPARTS(1);
}

static int sysfs_stat(ioserv_context_t *ctp, io_stat_t *msg, void *handle)
{
    struct sysfs_ocb *ocb = handle;

    if (!ocb)
        return EBADF;

    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_mode = ocb->mode;
    msg->o.st_size = (off_t) ocb->size;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | sysfs_devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int sysfs_lseek(ioserv_context_t *ctp, io_lseek_t *msg, void *handle)
{
    struct sysfs_ocb *ocb = handle;
    off_t newpos;

    if (!ocb)
        return EBADF;

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

    /* A seek to the start of a synthesized file takes a fresh snapshot, so one
     * open descriptor can be resampled without opening a new connection.
     */
    if (msg->i.whence == SEEK_SET && newpos == 0 && ocb->dyn_buf &&
        ocb->idx >= 0) {
        if (sysfs_entries[ocb->idx].stream_read)
            stream_resample(ocb);
        else
            ocb->size = sysfs_entries[ocb->idx].synth(
                ocb->dyn_buf, sysfs_entries[ocb->idx].dyn_bufsz);
    }

    ocb->pos = newpos;
    if (ctp->rcvid == 0)
        return EOK;

    msg->o = (uint64_t) ocb->pos;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int sysfs_close_ocb(ioserv_context_t *ctp, void *reserved, void *handle)
{
    (void) ctp;
    (void) reserved;
    if (handle) {
        struct sysfs_ocb *ocb = handle;
        if (ocb->dyn_buf) {
            free(ocb->dyn_buf);
            ocb->dyn_buf = NULL;
        }
        ocb_free(ocb);
    }
    return EOK;
}

static int sysfs_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *handle)
{
    (void) handle;

    if (msg->i.devctl_cmd == DEVCTL_SET_FLAGS)
        return EOK;

    if (msg->i.devctl_cmd == DEVCTL_GET_MOUNT_FLAGS) {
        int flags = _MOUNT_READONLY | _MOUNT_NOEXEC;
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(flags);
        memcpy(&msg->o + 1, &flags, sizeof(flags));
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(flags));
    }
    return ENOSYS;
}

static const ioserv_ops_t sysfs_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = sysfs_read,
    .close_ocb = sysfs_close_ocb,
    .stat = sysfs_stat,
    .devctl = sysfs_devctl,
    .fdinfo = sysfs_fdinfo,
    .lseek = sysfs_lseek,
};

static const ioserv_connect_ops_t sysfs_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = sysfs_open,
};

int sysfs_init(void)
{
    ioserv_attr_t rattr;
    const char *cmd;
    int n;

    pool_canary_init(ocb_pool, SYSFS_MAX_OCBS);

    /* Pull cmdline out of syspage and tack on a trailing newline so "cat
     * /sys/cmdline" looks right at the shell. The kernel populates it in
     * set_syspage_cmdline() during startup.
     */
    cmd = SYSPAGE_ENTRY(cmdline)->data;
    n = snprintf(sysfs_cmdline, sizeof(sysfs_cmdline), "%s\n", cmd);
    if (n < 0 || n >= (int) sizeof(sysfs_cmdline))
        n = 0;
    sysfs_cmdline_ptr = sysfs_cmdline;
    sysfs_cmdline_len = (size_t) n;

    /* Materialise the version string. */
    n = snprintf(sysfs_version, sizeof(sysfs_version), "%s %s\n",
                 RTMUX_VERSION_STRING, RTMUX_BUILD_DATE);
    if (n < 0 || n >= (int) sizeof(sysfs_version))
        n = 0;
    sysfs_version_ptr = sysfs_version;
    sysfs_version_len = (size_t) n;

    /* The kernel's own release and build id, as it published them. */
    {
        const struct build_info_entry *bi = syspage_build_info();

        n = snprintf(sysfs_kernelversion, sizeof(sysfs_kernelversion),
                     "%.63s %.15s\n", bi ? bi->release : "unknown",
                     bi ? bi->build_id : "unknown");
        if (n < 0 || n >= (int) sizeof(sysfs_kernelversion))
            n = 0;
        sysfs_kernelversion_ptr = sysfs_kernelversion;
        sysfs_kernelversion_len = (size_t) n;
    }

    /* And the FDT /compatible string from the syspage. */
    {
        const char *board = SYSPAGE_ENTRY(board)->data;
        n = snprintf(sysfs_board, sizeof(sysfs_board), "%s\n",
                     (board && *board) ? board : "unknown");
        if (n < 0 || n >= (int) sizeof(sysfs_board))
            n = 0;
        sysfs_board_ptr = sysfs_board;
        sysfs_board_len = (size_t) n;
    }

    memset(&rattr, 0, sizeof(rattr));
    if (ioserv_attach(dpp, &rattr, "/sys", 0, IOSERV_FLAG_DIR,
                      &sysfs_connect_funcs, &sysfs_io_funcs, 0) == -1) {
        root_log("sysfs: ioserv_attach failed\n");
        return -1;
    }

    hwres_proc_devno("sys", &sysfs_devno, -1, 0);
    return 0;
}
