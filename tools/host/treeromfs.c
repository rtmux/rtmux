/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * treeromfs — display the directory tree of a romfs image file
 *
 * Usage: treeromfs <image>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fs.h"

static int img_fd;
static struct romfs_super sb;

/* Counters for the summary line */
static int count_dirs;
static int count_files;

/* read_block() - read a single block from the image
 *
 * @blk: block number @buf: destination buffer (must be at least ROMFS_BSIZE
 * bytes)
 *
 * Returns 0 on success, -1 on error.
 */
static int read_block(uint64_t blk, void *buf)
{
    if (pread(img_fd, buf, ROMFS_BSIZE, blk * ROMFS_BSIZE) != ROMFS_BSIZE) {
        fprintf(stderr, "treeromfs: read error at block %lu\n",
                (unsigned long) blk);
        return -1;
    }
    return 0;
}

/* read_inode() - read an on-disk inode by number
 *
 * @inum: inode number @dip: destination inode structure
 *
 * Returns 0 on success, -1 on error.
 */
static int read_inode(uint32_t inum, struct romfs_dinode *dip)
{
    char blk[ROMFS_BSIZE];
    uint64_t bnum = inum / ROMFS_IPB + sb.inodestart;

    if (read_block(bnum, blk) < 0)
        return -1;

    memcpy(dip, blk + (inum % ROMFS_IPB) * sizeof(struct romfs_dinode),
           sizeof(*dip));
    return 0;
}

/* type_char() - return a single character indicating inode type
 *
 * @type: ROMFS_T_* value
 */
static char type_char(uint16_t type)
{
    switch (type) {
    case ROMFS_T_DIR:
        return 'd';
    case ROMFS_T_SLINK:
        return 'l';
    case ROMFS_T_DEV:
        return 'c';
    default:
        return '-';
    }
}

/* format_mode() - format Unix permission bits into an "ls -l" style string
 *
 * @type: ROMFS_T_* value @mode: permission bits @buf: output buffer (at least
 * 11 bytes)
 */
static void format_mode(uint16_t type, uint32_t mode, char *buf)
{
    buf[0] = type_char(type);
    buf[1] = (mode & 0400) ? 'r' : '-';
    buf[2] = (mode & 0200) ? 'w' : '-';
    buf[3] = (mode & 0100) ? 'x' : '-';
    buf[4] = (mode & 040) ? 'r' : '-';
    buf[5] = (mode & 020) ? 'w' : '-';
    buf[6] = (mode & 010) ? 'x' : '-';
    buf[7] = (mode & 04) ? 'r' : '-';
    buf[8] = (mode & 02) ? 'w' : '-';
    buf[9] = (mode & 01) ? 'x' : '-';
    buf[10] = '\0';
}

/* walk_dir() - recursively walk a directory and print its tree
 *
 * @inum: inode number of the directory to walk @prefix: string printed before
 * each entry (indentation/connectors)
 */
static void walk_dir(uint32_t inum, const char *prefix)
{
    struct romfs_dinode di;
    char blk[ROMFS_BSIZE];

    if (read_inode(inum, &di) < 0)
        return;
    if (di.type != ROMFS_T_DIR)
        return;

    /* First pass: collect all entries (skipping "." and "..") so we know which
     * one is last (needed for the tree connector characters).
     */
    struct {
        uint32_t inum;
        char name[ROMFS_NAMESIZ];
    } entries[4096];
    int nent = 0;

    uint64_t nblocks = (di.size + ROMFS_BSIZE - 1) / ROMFS_BSIZE;

    for (uint64_t b = 0; b < nblocks && b < ROMFS_NDIRECT; b++) {
        if (di.addrs[b] == 0)
            continue;
        if (read_block(di.addrs[b], blk) < 0)
            continue;

        struct romfs_dirent *de = (struct romfs_dirent *) blk;
        for (int j = 0; j < ROMFS_DPB; j++) {
            if (de[j].inum == 0)
                continue;
            if (!strcmp(de[j].name, ".") || !strcmp(de[j].name, ".."))
                continue;
            if (nent < 4096) {
                entries[nent].inum = de[j].inum;
                memcpy(entries[nent].name, de[j].name, ROMFS_NAMESIZ);
                nent++;
            }
        }
    }

    /* Handle single-indirect block if the directory is large enough.
     * Directories in practice never reach the double/triple-indirect region (a
     * single-indirect alone covers ~8000 entries).
     */
    if (nblocks > ROMFS_NDIRECT && di.addrs[ROMFS_SINGLE_IDX] != 0) {
        uint64_t ibuf[ROMFS_NINDIRECT];
        if (read_block(di.addrs[ROMFS_SINGLE_IDX], ibuf) == 0) {
            for (uint64_t b = 0; b < nblocks - ROMFS_NDIRECT; b++) {
                if (ibuf[b] == 0)
                    continue;
                if (read_block(ibuf[b], blk) < 0)
                    continue;

                struct romfs_dirent *de = (struct romfs_dirent *) blk;
                for (int j = 0; j < ROMFS_DPB; j++) {
                    if (de[j].inum == 0)
                        continue;
                    if (!strcmp(de[j].name, ".") || !strcmp(de[j].name, ".."))
                        continue;
                    if (nent < 4096) {
                        entries[nent].inum = de[j].inum;
                        memcpy(entries[nent].name, de[j].name, ROMFS_NAMESIZ);
                        nent++;
                    }
                }
            }
        }
    }

    /* Second pass: print each entry with proper tree connectors */
    for (int i = 0; i < nent; i++) {
        struct romfs_dinode child;
        int is_last = (i == nent - 1);
        const char *connector =
            is_last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "  /* "--- " */
                    : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "; /* "|-- " */

        if (read_inode(entries[i].inum, &child) < 0)
            continue;

        char mode_str[12];
        format_mode(child.type, child.mode, mode_str);

        printf("%s%s[%s %7lu]  %s", prefix, connector, mode_str,
               (unsigned long) child.size, entries[i].name);

        if (child.type == ROMFS_T_DIR) {
            count_dirs++;
            printf("\n");

            /* Build the prefix for the next level */
            char next_prefix[2048];
            const char *extension =
                is_last ? "    " : "\xe2\x94\x82   "; /* "|   " */
            snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix,
                     extension);
            walk_dir(entries[i].inum, next_prefix);
        } else {
            count_files++;
            if (child.type == ROMFS_T_SLINK) {
                /* Read symlink target from the first data block */
                if (child.addrs[0] != 0) {
                    char lnk[ROMFS_BSIZE];
                    if (read_block(child.addrs[0], lnk) == 0) {
                        lnk[child.size < ROMFS_BSIZE ? child.size
                                                     : ROMFS_BSIZE - 1] = '\0';
                        printf(" -> %s", lnk);
                    }
                }
            }
            printf("\n");
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <romfs-image>\n", argv[0]);
        return 1;
    }

    img_fd = open(argv[1], O_RDONLY);
    if (img_fd < 0) {
        perror(argv[1]);
        return 1;
    }

    /* Read superblock from block 1 */
    char blk[ROMFS_BSIZE];
    if (read_block(1, blk) < 0) {
        fprintf(stderr, "treeromfs: cannot read superblock\n");
        close(img_fd);
        return 1;
    }
    memcpy(&sb, blk, sizeof(sb));

    if (sb.magic != ROMFS_MAGIC) {
        fprintf(stderr, "treeromfs: bad magic 0x%08x (expected 0x%08x)\n",
                sb.magic, ROMFS_MAGIC);
        close(img_fd);
        return 1;
    }

    if (sb.version != ROMFS_VERSION) {
        fprintf(stderr,
                "treeromfs: image is romfs v%u but this tool understands v%u; "
                "rebuild with mkromfs\n",
                sb.version, ROMFS_VERSION);
        close(img_fd);
        return 1;
    }

    printf("%s  [romfs v%u, %lu blocks, %lu inodes]\n", argv[1], sb.version,
           (unsigned long) sb.size, (unsigned long) sb.ninodes);

    walk_dir(ROMFS_ROOTINO, "");

    printf("\n%d directories, %d files\n", count_dirs, count_files);

    close(img_fd);
    return 0;
}
