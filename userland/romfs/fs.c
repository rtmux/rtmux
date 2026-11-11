/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * fs.c — romfs filesystem operations
 *
 * Inode read, block mapping, directory lookup, and file read helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sched.h>

#include "fs.h"
#include "bio.h"

/* Global superblock (read once at mount time) */
static struct romfs_super sb;

int romfs_init(void)
{
    struct buf *b;
    int retries;

    /* The block device may need a moment after its driver detaches. Retry a few
     * times with sched_yield() between attempts.
     */
    for (retries = 0; retries < 50; retries++) {
        b = bread(1);
        if (b)
            break;
        sched_yield();
    }
    if (!b)
        return -1;

    memcpy(&sb, b->data, sizeof(sb));
    brelse(b);

    if (sb.magic != ROMFS_MAGIC) {
        fprintf(stderr, "romfs: bad magic 0x%x (expected 0x%x)\n", sb.magic,
                ROMFS_MAGIC);
        return -1;
    }

    if (sb.version != ROMFS_VERSION) {
        fprintf(stderr,
                "romfs: unsupported on-disk version %u "
                "(this romfs understands v%u). Rebuild image with mkromfs.\n",
                sb.version, ROMFS_VERSION);
        return -1;
    }

    fprintf(stderr, "romfs: romfs v%u, %lu blocks, %lu inodes\n", sb.version,
            (unsigned long) sb.size, (unsigned long) sb.ninodes);
    return 0;
}

const struct romfs_super *romfs_super(void)
{
    return &sb;
}

/* Inode operations */

int romfs_iread(uint32_t inum, struct romfs_dinode *dip)
{
    if (inum == 0 || inum >= sb.ninodes)
        return -1;

    uint64_t bno = ROMFS_IBLOCK(inum, sb);

    /* A corrupt superblock inodestart can wrap the add or push the inode block
     * past the filesystem; reject rather than read a wild block.
     */
    if (bno < sb.inodestart || bno >= sb.size)
        return -1;
    struct buf *bp = bread(bno);
    if (!bp)
        return -1;

    struct romfs_dinode *src =
        (struct romfs_dinode *) bp->data + inum % ROMFS_IPB;
    *dip = *src;
    brelse(bp);
    return 0;
}

/* Block mapping: logical block number -> physical block number
 *
 * Walks at most three index levels (single, double, triple indirect). A zero
 * anywhere on the walk means "block not allocated" - the caller (romfs_readi)
 * treats that as a sparse hole.
 */

/* Read a u64 slot from an index block; return 0 if the block can't be loaded.
 */
static uint64_t read_idx(uint64_t blkno, uint64_t slot)
{
    if (blkno == 0)
        return 0;

    struct buf *bp = bread(blkno);
    if (!bp)
        return 0;

    uint64_t addr = ((uint64_t *) bp->data)[slot];
    brelse(bp);
    return addr;
}

static uint64_t romfs_bmap(const struct romfs_dinode *dip, uint64_t fbn)
{
    /* Direct blocks */
    if (fbn < ROMFS_NDIRECT)
        return dip->addrs[fbn];
    fbn -= ROMFS_NDIRECT;

    /* Single indirect */
    if (fbn < ROMFS_NINDIRECT)
        return read_idx(dip->addrs[ROMFS_SINGLE_IDX], fbn);
    fbn -= ROMFS_NINDIRECT;

    /* Double indirect: addrs[8] -> [512 ptrs] -> [512 ptrs] -> data */
    if (fbn < ROMFS_NINDIRECT2) {
        uint64_t l1 = fbn / ROMFS_NINDIRECT;
        uint64_t l2 = fbn % ROMFS_NINDIRECT;
        uint64_t b = read_idx(dip->addrs[ROMFS_DOUBLE_IDX], l1);
        return read_idx(b, l2);
    }
    fbn -= ROMFS_NINDIRECT2;

    /* Triple indirect: addrs[9] -> [512] -> [512] -> [512] -> data */
    if (fbn < ROMFS_NINDIRECT3) {
        uint64_t l1 = fbn / ROMFS_NINDIRECT2;
        uint64_t r = fbn % ROMFS_NINDIRECT2;
        uint64_t l2 = r / ROMFS_NINDIRECT;
        uint64_t l3 = r % ROMFS_NINDIRECT;
        uint64_t b1 = read_idx(dip->addrs[ROMFS_TRIPLE_IDX], l1);
        uint64_t b2 = read_idx(b1, l2);
        return read_idx(b2, l3);
    }

    /* Beyond maximum file size */
    return 0;
}

/* Read file data */

ssize_t romfs_readi(const struct romfs_dinode *dip,
                    void *dst,
                    uint64_t off,
                    size_t n)
{
    if (off > dip->size)
        return 0;

    /* Clamp without off + n, which can wrap when a corrupt inode carries a
     * large size and the caller passes a large offset.
     */
    if (n > dip->size - off)
        n = (size_t) (dip->size - off);

    size_t total = 0;
    uint8_t *p = dst;

    while (total < n) {
        uint64_t fbn = (off + total) / ROMFS_BSIZE;
        uint64_t bno = romfs_bmap(dip, fbn);

        size_t boff = (size_t) ((off + total) % ROMFS_BSIZE);
        size_t chunk = ROMFS_BSIZE - boff;
        if (chunk > n - total)
            chunk = n - total;

        if (bno == 0) {
            /* Sparse hole: POSIX says reads of holes return zero bytes, not a
             * short read.
             */
            memset(p + total, 0, chunk);
            total += chunk;
            continue;
        }

        struct buf *bp = bread(bno);
        if (!bp)
            break;

        memcpy(p + total, bp->data + boff, chunk);
        brelse(bp);

        total += chunk;
    }

    return (ssize_t) total;
}

/* Directory lookup: find name in directory inode, return inum */

static uint32_t romfs_dirlookup(const struct romfs_dinode *dip,
                                const char *name)
{
    if (dip->type != ROMFS_T_DIR)
        return 0;

    struct romfs_dirent de;
    uint64_t scan = dip->size;

    /* Bound the walk by the directory-size invariant so a corrupt size field,
     * whose tail blocks read back as sparse-zero entries, cannot spin without
     * end. mkromfs never writes a directory past this, so no real entry is
     * lost.
     */
    if (scan > ROMFS_MAX_DIR_BYTES)
        scan = ROMFS_MAX_DIR_BYTES;

    for (uint64_t off = 0; off < scan; off += sizeof(de)) {
        ssize_t n = romfs_readi(dip, &de, off, sizeof(de));
        if (n != sizeof(de))
            break;
        if (de.inum == 0)
            continue;
        if (strncmp(de.name, name, ROMFS_NAMESIZ) == 0)
            return de.inum;
    }
    return 0;
}

/* Path resolution: walk from root (or given inode) to final inode */

uint32_t romfs_namei(const char *path)
{
    uint32_t inum = ROMFS_ROOTINO;
    struct romfs_dinode di;

    /* Skip leading slash */
    while (*path == '/')
        path++;

    /* Empty path = root */
    if (*path == '\0')
        return inum;

    while (*path) {
        if (romfs_iread(inum, &di) < 0)
            return 0;
        if (di.type != ROMFS_T_DIR)
            return 0;

        /* Extract next path component */
        const char *end = path;
        while (*end && *end != '/')
            end++;

        size_t len = (size_t) (end - path);
        if (len == 0) {
            path = end;
            while (*path == '/')
                path++;
            continue;
        }

        char name[ROMFS_NAMESIZ];
        if (len >= ROMFS_NAMESIZ)
            return 0; /* name too long */
        memcpy(name, path, len);
        name[len] = '\0';

        uint32_t next = romfs_dirlookup(&di, name);
        if (next == 0)
            return 0; /* not found */

        inum = next;
        path = end;
        while (*path == '/')
            path++;
    }

    return inum;
}
