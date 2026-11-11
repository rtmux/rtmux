/*
 * bio.h — block I/O layer for romfs
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROMFS_BIO_H
#define _ROMFS_BIO_H

#include <stdint.h>
#include "fs.h"

/* Buffer cache entry */
struct buf {
    uint64_t blockno;
    int valid;
    uint32_t refcnt;
    uint32_t lru_tick;
    uint8_t data[ROMFS_BSIZE];
};

/* Initialize block I/O layer. devpath = e.g. "/dev/nvme0n1p8" */
int bio_init(const char *devpath);

/* Read block; returns pointer to cached data (valid until next bread) */
struct buf *bread(uint64_t blockno);

/* Release buffer */
void brelse(struct buf *b);

#endif /* _ROMFS_BIO_H */
