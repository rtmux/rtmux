/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * bio.c — block I/O layer for romfs
 *
 * Reads blocks from the block device fd. Simple LRU buffer cache (static array,
 * no malloc).
 *
 * Block drivers may complete partial sector reads, so we loop to fill a 4K
 * block.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "bio.h"

#define NBUF 32
#define SECTOR_SIZE 512

static int blkfd = -1;
static struct buf bcache[NBUF];
static uint32_t tick;

int bio_init(const char *devpath)
{
    /* Open read-only. Block-device I/O services (dev-nvme today) reject
     * anything other than O_RDONLY with EROFS in their direct open handler.
     */
    /* ipc-send: romfs -> dev-nvme */
    blkfd = open(devpath, O_RDONLY);
    if (blkfd < 0)
        return -1;
    memset(bcache, 0, sizeof(bcache));
    tick = 0;
    return 0;
}

/* Read a full block from the device. Uses lseek+read since pread may not be in
 * libc yet.
 */
static int dev_read(uint64_t blockno, uint8_t *buf)
{
    off_t off = (off_t) (blockno * ROMFS_BSIZE);
    if (lseek(blkfd, off, SEEK_SET) < 0)
        return -1;

    size_t remain = ROMFS_BSIZE;
    uint8_t *p = buf;
    while (remain > 0) {
        ssize_t n = read(blkfd, p, SECTOR_SIZE);
        if (n <= 0)
            return -1;
        p += n;
        remain -= (size_t) n;
    }
    return 0;
}

/* Find or evict a buffer cache entry. */
static struct buf *bget(uint64_t blockno)
{
    /* Look for cached entry */
    for (int i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].blockno == blockno) {
            bcache[i].refcnt++;
            bcache[i].lru_tick = ++tick;
            return &bcache[i];
        }
    }

    /* Find free or LRU entry */
    struct buf *victim = NULL;
    uint32_t oldest = ~0u;
    for (int i = 0; i < NBUF; i++) {
        if (bcache[i].refcnt == 0 && bcache[i].lru_tick < oldest) {
            oldest = bcache[i].lru_tick;
            victim = &bcache[i];
        }
    }

    if (!victim) {
        /* All buffers in use — should not happen with proper usage */
        fprintf(stderr, "romfs: bio: no free buffers\n");
        return NULL;
    }

    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->lru_tick = ++tick;
    return victim;
}

struct buf *bread(uint64_t blockno)
{
    struct buf *b = bget(blockno);
    if (!b)
        return NULL;

    if (!b->valid) {
        if (dev_read(blockno, b->data) < 0) {
            b->refcnt--;
            return NULL;
        }
        b->valid = 1;
    }
    return b;
}

void brelse(struct buf *b)
{
    if (b->refcnt > 0)
        b->refcnt--;
}
