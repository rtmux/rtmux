/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * A compact read-only Unix filesystem format shared by romfs (I/O service) and
 * mkromfs (host tool).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROMFS_FS_H
#define _ROMFS_FS_H

#include <stdint.h>
#include <sys/types.h>

/* Fundamental constants */

#define ROMFS_MAGIC 0x52544d31 /* "RTM1" */
#define ROMFS_VERSION 2
#define ROMFS_BSIZE 4096 /* block size (bytes) */
#define ROMFS_ROOTINO 1  /* root directory inode number */

/* Inode types */
#define ROMFS_T_FREE 0
#define ROMFS_T_DIR 1
#define ROMFS_T_FILE 2
#define ROMFS_T_SLINK 3
#define ROMFS_T_DEV 4

/* Block pointers per inode.
 *
 * addrs[] layout (10 u64 slots, 80 bytes total):
 *   addrs[0..6]  - 7 direct       (28 KiB)
 *   addrs[7]     - single indirect (2 MiB)
 *   addrs[8]     - double indirect (1 GiB)
 *   addrs[9]     - triple indirect (512 GiB)
 *
 * Maximum file size: ~512 GiB; the practical 4 GiB target is comfortably within
 * the double-indirect region.
 */
#define ROMFS_NDIRECT 7
#define ROMFS_NINDIRECT (ROMFS_BSIZE / sizeof(uint64_t)) /* 512 */
#define ROMFS_NINDIRECT2                          \
    (ROMFS_NINDIRECT * ROMFS_NINDIRECT) /* 262144 \
                                         */
#define ROMFS_NINDIRECT3 (ROMFS_NINDIRECT2 * ROMFS_NINDIRECT)

/* Slot positions inside addrs[] */
#define ROMFS_SINGLE_IDX (ROMFS_NDIRECT)     /* addrs[7] */
#define ROMFS_DOUBLE_IDX (ROMFS_NDIRECT + 1) /* addrs[8] */
#define ROMFS_TRIPLE_IDX (ROMFS_NDIRECT + 2) /* addrs[9] */
#define ROMFS_NADDRS (ROMFS_NDIRECT + 3)     /* 10 — must match dinode */

/* Maximum addressable file block number (count, not index) */
#define ROMFS_MAXFILE \
    (ROMFS_NDIRECT + ROMFS_NINDIRECT + ROMFS_NINDIRECT2 + ROMFS_NINDIRECT3)

/* Maximum directory size, a format invariant shared by mkromfs and the reader.
 * A directory is capped at the single-indirect file region (7 + 512 blocks,
 * about 2 MiB, over eight thousand entries), far larger than any real
 * directory. mkromfs errors out rather than build a directory past this, and
 * the reader treats any excess as absent, so a corrupt oversized directory size
 * cannot drive an unbounded linear scan.
 */
#define ROMFS_MAX_DIR_BYTES \
    ((uint64_t) (ROMFS_NDIRECT + ROMFS_NINDIRECT) * ROMFS_BSIZE)

/* Directory entry name length (including NUL) */
#define ROMFS_NAMESIZ 252

/* Derived constants */
#define ROMFS_IPB (ROMFS_BSIZE / 128) /* 32 inodes/block */
#define ROMFS_DPB (ROMFS_BSIZE / 256) /* 16 dirents/block */
#define ROMFS_BPB (ROMFS_BSIZE * 8)   /* bitmap bits/block */

/* Block containing inode i */
#define ROMFS_IBLOCK(i, sb) ((i) / ROMFS_IPB + (sb).inodestart)

/* Block containing bit for block b */
#define ROMFS_BBLOCK(b, sb) ((b) / ROMFS_BPB + (sb).bmapstart)

/* On-disk superblock (block 1) */

struct romfs_super {
    uint32_t magic;      /* must be ROMFS_MAGIC */
    uint32_t version;    /* format version */
    uint64_t size;       /* total blocks in filesystem */
    uint64_t nblocks;    /* number of data blocks */
    uint64_t ninodes;    /* number of inodes */
    uint64_t nlog;       /* reserved, unused */
    uint64_t logstart;   /* reserved, unused */
    uint64_t inodestart; /* first inode block number */
    uint64_t bmapstart;  /* first bitmap block number */
    uint64_t datastart;  /* first data block number */
};

/* On-disk inode (128 bytes) */

struct romfs_dinode {
    uint16_t type;  /* ROMFS_T_* */
    uint16_t nlink; /* number of directory links */
    uint32_t mode;  /* permission bits (lower 12 bits of mode_t) */
    uint32_t uid;   /* owner user id */
    uint32_t gid;   /* owner group id */
    uint64_t size;  /* file size in bytes */
    uint64_t atime; /* access time (seconds since epoch) */
    uint64_t mtime; /* modification time */
    uint64_t ctime; /* status change time */
    uint64_t addrs[ROMFS_NADDRS]; /* 7 direct + single + double + triple */
};

/* Compile-time check: inode must be exactly 128 bytes */
_Static_assert(sizeof(struct romfs_dinode) == 128,
               "romfs_dinode must be 128 bytes");
_Static_assert(ROMFS_NADDRS == 10,
               "romfs_dinode addrs[] must hold 10 u64 slots (80 bytes)");

/* On-disk directory entry (256 bytes) */

struct romfs_dirent {
    uint32_t inum;            /* inode number (0 = empty slot) */
    char name[ROMFS_NAMESIZ]; /* NUL-terminated filename (max 251 chars) */
};

_Static_assert(sizeof(struct romfs_dirent) == 256,
               "romfs_dirent must be 256 bytes");

int romfs_init(void);
const struct romfs_super *romfs_super(void);
int romfs_iread(uint32_t inum, struct romfs_dinode *dip);
ssize_t romfs_readi(const struct romfs_dinode *dip,
                    void *dst,
                    uint64_t off,
                    size_t n);
uint32_t romfs_namei(const char *path);

#endif /* _ROMFS_FS_H */
