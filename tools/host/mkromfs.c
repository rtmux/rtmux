/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * mkromfs — create a romfs filesystem image
 *
 * Usage: mkromfs [-s size_mb] [-n ninodes] image_file [dir_to_populate]
 *
 * Creates image_file with a romfs filesystem. If dir_to_populate is given,
 * copies its contents into the root directory.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <assert.h>
#ifdef __linux__
#include <linux/fs.h> /* BLKZEROOUT */
#endif

#include "fs.h"

/* Default filesystem parameters */
#define DEFAULT_SIZE_MB 8
#define DEFAULT_NINODES 128
#define DEFAULT_NLOG 0 /* no journal for now */

static int fsfd;
static struct romfs_super sb;
static uint8_t zeroes[ROMFS_BSIZE];

/* Low-level block I/O */

static void wsect(uint64_t bno, const void *buf)
{
    if (lseek(fsfd, (off_t) (bno * ROMFS_BSIZE), SEEK_SET) < 0) {
        perror("lseek");
        exit(1);
    }
    if (write(fsfd, buf, ROMFS_BSIZE) != ROMFS_BSIZE) {
        perror("write");
        exit(1);
    }
}

static void rsect(uint64_t bno, void *buf)
{
    if (lseek(fsfd, (off_t) (bno * ROMFS_BSIZE), SEEK_SET) < 0) {
        perror("lseek");
        exit(1);
    }
    if (read(fsfd, buf, ROMFS_BSIZE) != ROMFS_BSIZE) {
        perror("read");
        exit(1);
    }
}

/* Inode operations */

static void winode(uint32_t inum, const struct romfs_dinode *dip)
{
    uint8_t buf[ROMFS_BSIZE];
    uint64_t bno = ROMFS_IBLOCK(inum, sb);
    rsect(bno, buf);
    struct romfs_dinode *p = (struct romfs_dinode *) buf + inum % ROMFS_IPB;
    *p = *dip;
    wsect(bno, buf);
}

static void rinode(uint32_t inum, struct romfs_dinode *dip)
{
    uint8_t buf[ROMFS_BSIZE];
    uint64_t bno = ROMFS_IBLOCK(inum, sb);
    rsect(bno, buf);
    struct romfs_dinode *p = (struct romfs_dinode *) buf + inum % ROMFS_IPB;
    *dip = *p;
}

/* Next free inode to allocate */
static uint32_t freeinode = 1;

static uint32_t ialloc(uint16_t type, uint32_t mode)
{
    uint32_t inum = freeinode++;
    struct romfs_dinode di;
    memset(&di, 0, sizeof(di));
    di.type = type;
    di.nlink = 1;
    di.mode = mode;
    di.atime = di.mtime = di.ctime = (uint64_t) time(NULL);
    winode(inum, &di);
    return inum;
}

/* Bitmap operations */

/* Mark block bno as used in the bitmap */
static void bmark(uint64_t bno)
{
    uint8_t buf[ROMFS_BSIZE];
    uint64_t bi = bno / ROMFS_BPB;
    rsect(sb.bmapstart + bi, buf);
    uint64_t off = bno % ROMFS_BPB;
    buf[off / 8] |= (uint8_t) (1 << (off % 8));
    wsect(sb.bmapstart + bi, buf);
}

/* Next free data block */
static uint64_t freeblock;

static uint64_t balloc(void)
{
    uint64_t b = freeblock++;
    bmark(b);
    return b;
}

/* Directory / file append */

/* walk_indirect — descend "level" index blocks rooted at *slot, and return the
 * data block number for "fbn" (which is already adjusted to be relative to this
 * region). Allocates index/data blocks as needed and writes them back. level=0
 * means "*slot is the data block itself".
 */
static uint64_t walk_indirect(uint64_t *slot, int level, uint64_t fbn)
{
    int fresh = 0;
    if (*slot == 0) {
        *slot = balloc();
        fresh = 1;
    }

    if (level == 0)
        return *slot;

    /* Compute fan-out at this level (entries per child sub-tree) */
    uint64_t fanout = 1;
    for (int i = 0; i < level - 1; i++)
        fanout *= ROMFS_NINDIRECT;

    uint64_t idx = fbn / fanout;
    uint64_t rem = fbn % fanout;

    uint64_t ibuf[ROMFS_NINDIRECT];
    if (fresh)
        memset(ibuf, 0, sizeof(ibuf)); /* recycled data block may be dirty */
    else
        rsect(*slot, ibuf);

    uint64_t prev = ibuf[idx];
    uint64_t bno = walk_indirect(&ibuf[idx], level - 1, rem);
    if (fresh || ibuf[idx] != prev)
        wsect(*slot, ibuf);
    return bno;
}

/* iappend_bmap — return the data block number that backs file block "fbn" of
 * inode "di", allocating the indirect chain on demand. "di" is updated in
 * place; caller is responsible for winode().
 */
static uint64_t iappend_bmap(struct romfs_dinode *di, uint64_t fbn)
{
    if (fbn < ROMFS_NDIRECT)
        return walk_indirect(&di->addrs[fbn], 0, 0);
    fbn -= ROMFS_NDIRECT;

    if (fbn < ROMFS_NINDIRECT)
        return walk_indirect(&di->addrs[ROMFS_SINGLE_IDX], 1, fbn);
    fbn -= ROMFS_NINDIRECT;

    if (fbn < ROMFS_NINDIRECT2)
        return walk_indirect(&di->addrs[ROMFS_DOUBLE_IDX], 2, fbn);
    fbn -= ROMFS_NINDIRECT2;

    assert(fbn < ROMFS_NINDIRECT3);
    return walk_indirect(&di->addrs[ROMFS_TRIPLE_IDX], 3, fbn);
}

static void iappend(uint32_t inum, const void *data, size_t n)
{
    struct romfs_dinode di;
    rinode(inum, &di);

    uint64_t off = di.size;
    const uint8_t *p = data;

    while (n > 0) {
        uint64_t fbn = off / ROMFS_BSIZE; /* file block number */
        assert(fbn < ROMFS_MAXFILE);

        uint64_t bno = iappend_bmap(&di, fbn);

        size_t boff = (size_t) (off % ROMFS_BSIZE);
        size_t chunk = ROMFS_BSIZE - boff;
        if (chunk > n)
            chunk = n;

        uint8_t buf[ROMFS_BSIZE];
        rsect(bno, buf);
        memcpy(buf + boff, p, chunk);
        wsect(bno, buf);

        off += chunk;
        p += chunk;
        n -= chunk;
    }

    di.size = off;
    if (di.type == ROMFS_T_DIR && di.size > ROMFS_MAX_DIR_BYTES) {
        fprintf(stderr,
                "mkromfs: directory inode %u size %llu exceeds the format "
                "maximum %llu bytes\n",
                inum, (unsigned long long) di.size,
                (unsigned long long) ROMFS_MAX_DIR_BYTES);
        exit(1);
    }
    winode(inum, &di);
}

/* Populate from host directory (recursive) */

static void populate(uint32_t parent_inum, const char *hostdir);

static void add_file(uint32_t parent_inum,
                     const char *hostpath,
                     const char *name,
                     mode_t hostmode)
{
    uint16_t type;
    if (S_ISDIR(hostmode))
        type = ROMFS_T_DIR;
    else if (S_ISLNK(hostmode))
        type = ROMFS_T_SLINK;
    else
        type = ROMFS_T_FILE;

    uint32_t inum = ialloc(type, hostmode & 07777);

    /* Add directory entry in parent */
    struct romfs_dirent de;
    memset(&de, 0, sizeof(de));
    de.inum = inum;
    size_t nlen = strlen(name);
    if (nlen >= ROMFS_NAMESIZ) {
        fprintf(stderr,
                "mkromfs: name '%s' (%zu bytes) exceeds ROMFS_NAMESIZ-1 "
                "(%d)\n",
                name, nlen, ROMFS_NAMESIZ - 1);
        exit(1);
    }
    memcpy(de.name, name, nlen); /* de was zeroed; trailing NUL already there */
    iappend(parent_inum, &de, sizeof(de));

    if (type == ROMFS_T_DIR) {
        /* Add . and .. */
        struct romfs_dirent dot;
        memset(&dot, 0, sizeof(dot));
        dot.inum = inum;
        strcpy(dot.name, ".");
        iappend(inum, &dot, sizeof(dot));

        dot.inum = parent_inum;
        strcpy(dot.name, "..");
        iappend(inum, &dot, sizeof(dot));

        /* Update nlink for the directory inode */
        struct romfs_dinode di;
        rinode(inum, &di);
        di.nlink = 2; /* . and parent's entry */
        winode(inum, &di);

        /* Increment parent nlink for .. */
        rinode(parent_inum, &di);
        di.nlink++;
        winode(parent_inum, &di);

        populate(inum, hostpath);
    } else if (type == ROMFS_T_FILE) {
        int fd = open(hostpath, O_RDONLY);
        if (fd < 0) {
            perror(hostpath);
            return;
        }
        uint8_t buf[ROMFS_BSIZE];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            iappend(inum, buf, (size_t) n);
        close(fd);
    }
}

static void populate(uint32_t parent_inum, const char *hostdir)
{
    DIR *dp = opendir(hostdir);
    if (!dp) {
        perror(hostdir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dp))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", hostdir, ent->d_name);

        struct stat st;
        if (lstat(path, &st) < 0) {
            perror(path);
            continue;
        }

        /* Only handle regular files and directories */
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
            continue;

        add_file(parent_inum, path, ent->d_name, st.st_mode);
    }
    closedir(dp);
}

/* Main */

int main(int argc, char *argv[])
{
    int size_mb = DEFAULT_SIZE_MB;
    uint64_t ninodes = DEFAULT_NINODES;
    int opt;

    while ((opt = getopt(argc, argv, "s:n:")) != -1) {
        switch (opt) {
        case 's':
            size_mb = atoi(optarg);
            break;
        case 'n':
            ninodes = (uint64_t) atoi(optarg);
            break;
        default:
            fprintf(
                stderr,
                "Usage: %s [-s size_mb] [-n ninodes] image [populate_dir]\n",
                argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Missing image filename\n");
        return 1;
    }

    const char *imgfile = argv[optind];
    const char *popdir = (optind + 1 < argc) ? argv[optind + 1] : NULL;

    uint64_t nblocks_total = (uint64_t) size_mb * (1024 * 1024 / ROMFS_BSIZE);

    /* Compute layout */
    uint64_t nlog = DEFAULT_NLOG;
    uint64_t ninode_blocks = (ninodes + ROMFS_IPB - 1) / ROMFS_IPB;
    uint64_t nbmap_blocks = (nblocks_total + ROMFS_BPB - 1) / ROMFS_BPB;

    /* Layout: [0:reserved] [1:super] [2..2+nlog-1:log] [inode blocks] [bitmap]
     * [data]
     */
    uint64_t logstart = 2;
    uint64_t inodestart = logstart + nlog;
    uint64_t bmapstart = inodestart + ninode_blocks;
    uint64_t datastart = bmapstart + nbmap_blocks;
    uint64_t ndata = nblocks_total - datastart;

    /* Fill superblock */
    memset(&sb, 0, sizeof(sb));
    sb.magic = ROMFS_MAGIC;
    sb.version = ROMFS_VERSION;
    sb.size = nblocks_total;
    sb.nblocks = ndata;
    sb.ninodes = ninodes;
    sb.nlog = nlog;
    sb.logstart = logstart;
    sb.inodestart = inodestart;
    sb.bmapstart = bmapstart;
    sb.datastart = datastart;

    printf("mkromfs: %s: %d MB (%lu blocks)\n", imgfile, size_mb,
           (unsigned long) nblocks_total);
    printf(
        "  layout: log=%lu inodes=%lu(%lu blocks) bmap=%lu data=%lu(%lu "
        "blocks)\n",
        (unsigned long) logstart, (unsigned long) ninodes,
        (unsigned long) ninode_blocks, (unsigned long) bmapstart,
        (unsigned long) datastart, (unsigned long) ndata);

    /* Open image / target. No O_TRUNC -- block devices reject it on some
     * kernels and we handle the regular-file case explicitly below via
     * ftruncate.
     */
    fsfd = open(imgfile, O_RDWR | O_CREAT, 0666);
    if (fsfd < 0) {
        perror(imgfile);
        return 1;
    }

    struct stat st;
    if (fstat(fsfd, &st) < 0) {
        perror("fstat");
        return 1;
    }

    /* Initialize the storage so subsequent metadata writes (bitmap
     * read-modify-write in particular) see zeros where they expect zeros. We
     * deliberately do NOT zero the data area: free data blocks are tracked by
     * the bitmap, so their contents don't matter, and skipping them turns a
     * multi-minute zero loop into a sub-second operation on a 2 GiB partition.
     *
     * Three paths, picked at runtime:
     *   block device + BLKZEROOUT works  -> NVMe Write Zeroes,
     *                                       near-instant for any size.
     *   block device, BLKZEROOUT fails    -> hand-rolled wsect loop
     *                                       over just the metadata
     *                                       blocks (~datastart blocks,
     *                                       a few hundred KiB).
     *   regular file                      -> ftruncate to target size,
     *                                       sparse holes read as zeros,
     *                                       no explicit writes needed.
     */
    uint64_t total_bytes = (uint64_t) nblocks_total * ROMFS_BSIZE;

    if (S_ISBLK(st.st_mode)) {
#ifdef BLKZEROOUT
        uint64_t metadata_bytes = (uint64_t) datastart * ROMFS_BSIZE;
        uint64_t range[2] = {0, metadata_bytes};
        if (ioctl(fsfd, BLKZEROOUT, range) == 0) {
            printf(
                "  init: BLKZEROOUT %lu metadata bytes (data area left "
                "as-is)\n",
                (unsigned long) metadata_bytes);
        } else
#endif
        {
            int saved = errno;
            printf(
                "  init: BLKZEROOUT unavailable (%s); writing %lu metadata "
                "blocks\n",
                strerror(saved), (unsigned long) datastart);
            for (uint64_t i = 0; i < datastart; i++)
                wsect(i, zeroes);
        }
    } else {
        if (ftruncate(fsfd, 0) < 0 ||
            ftruncate(fsfd, (off_t) total_bytes) < 0) {
            perror("ftruncate");
            return 1;
        }
        printf("  init: sparse file, %lu bytes (holes read as zeros)\n",
               (unsigned long) total_bytes);
    }

    /* Write superblock */
    uint8_t sbuf[ROMFS_BSIZE];
    memset(sbuf, 0, ROMFS_BSIZE);
    memcpy(sbuf, &sb, sizeof(sb));
    wsect(1, sbuf);

    /* Mark metadata blocks as used in bitmap */
    freeblock = datastart;
    for (uint64_t i = 0; i < datastart; i++)
        bmark(i);

    /* Allocate root directory (inode 1) */
    freeinode = 1;
    uint32_t rootino = ialloc(ROMFS_T_DIR, 0755);
    assert(rootino == ROMFS_ROOTINO);

    /* Add . and .. to root */
    struct romfs_dirent de;
    memset(&de, 0, sizeof(de));
    de.inum = rootino;
    strcpy(de.name, ".");
    iappend(rootino, &de, sizeof(de));

    de.inum = rootino;
    strcpy(de.name, "..");
    iappend(rootino, &de, sizeof(de));

    /* Set root nlink to 2 (. points to self, .. points to self) */
    struct romfs_dinode di;
    rinode(rootino, &di);
    di.nlink = 2;
    winode(rootino, &di);

    /* Populate from host directory if given */
    if (popdir)
        populate(rootino, popdir);

    /* Re-read and re-write superblock (freeblock may have advanced) (bitmap is
     * already up-to-date from bmark calls)
     */

    close(fsfd);

    printf("mkromfs: done. Root inode=%u, %lu data blocks used.\n", rootino,
           (unsigned long) (freeblock - datastart));

    return 0;
}
