/*
 * Fuzz harness for the romfs on-disk decoder (userland/romfs/fs.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* romfs parses an untrusted block device: a superblock at block 1, an inode
 * table, and directory entries, with block-map indices and offsets taken from
 * on-disk fields. This harness backs romfs's one I/O seam (bread/brelse) with
 * the raw fuzz bytes as the device image, then drives the decode paths that
 * consume those fields: romfs_init (superblock), romfs_namei (directory walk
 * feeding romfs_iread, romfs_dirlookup, romfs_readi, and the direct/indirect
 * block map), and a direct sweep of low inode numbers so inode and file-read
 * parsing is hit even when no path resolves.
 *
 * Return values fold into a volatile sink so the calls are not elided.
 *
 * The device is modelled as reading zeros past the image end rather than a read
 * error; that keeps the fuzzer on the decode math instead of the short-read
 * bookkeeping. A fuzzed blockno beyond the image serves a zero block.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fs.h"
#include "bio.h"

static const uint8_t *g_image;
static size_t g_image_size;

#define NBUF 8
static struct buf g_bufs[NBUF];
static unsigned g_next;

int bio_init(const char *devpath)
{
    (void) devpath;
    return 0;
}

struct buf *bread(uint64_t blockno)
{
    struct buf *b = &g_bufs[g_next++ % NBUF];

    b->blockno = blockno;
    b->valid = 1;
    b->refcnt = 1;
    memset(b->data, 0, ROMFS_BSIZE);
    /* Guard the blockno * ROMFS_BSIZE multiply against wrap before using it as
     * an image offset; a block wholly past the image reads as zeros.
     */
    if (blockno <= g_image_size / ROMFS_BSIZE) {
        uint64_t off = blockno * ROMFS_BSIZE;
        size_t avail = g_image_size - (size_t) off;

        if (avail > ROMFS_BSIZE)
            avail = ROMFS_BSIZE;
        memcpy(b->data, g_image + off, avail);
    }
    return b;
}

void brelse(struct buf *b)
{
    (void) b;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static volatile uint32_t sink;

    g_image = data;
    g_image_size = size;
    g_next = 0;

    if (romfs_init() != 0)
        return 0;

    sink = romfs_namei("/bin/sh");
    sink = romfs_namei("/etc/passwd");

    for (uint32_t inum = 1; inum < 24; inum++) {
        struct romfs_dinode di;
        uint8_t buf[300];

        if (romfs_iread(inum, &di) != 0)
            continue;
        sink = (uint32_t) romfs_readi(&di, buf, 0, sizeof(buf));
    }
    return 0;
}
