/*
 * lkl-fsd-read-test - read-path check against a host-authored ext4.
 *
 * Runs inside the lkl-fsd-tricky scenario after lkl-fsd mounts an ext4 that
 * host e2fsprogs populated (scripts/lkl/author-tricky-ext4.sh), the reverse of
 * the --differential write-path oracle. It confirms lkl-fsd exposes the on-disk
 * shapes the host image carries: a hardlink (shared inode, link count 2), a
 * sparse file (logical size >> allocated blocks), a directory with many
 * entries, and byte-exact content. Prints one PASS/FAIL line and exits non-zero
 * on any failure so the init chain gates on it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/fs/root"
#define SPARSE_SIZE 1048576
#define BIGDIR_ENTRIES 200

static int fails;

static void check(int ok, const char *name)
{
    if (ok) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (errno=%d)\n", name, errno);
        fails++;
    }
}

int main(void)
{
    struct stat orig, hlink, sparse;
    char buf[32];
    ssize_t n;
    int fd;
    int count = 0;
    DIR *dir;

    /* Hardlink: orig.txt and hardlink.txt are one inode with link count 2. */
    if (stat(ROOT "/orig.txt", &orig) == 0 &&
        stat(ROOT "/hardlink.txt", &hlink) == 0) {
        check(orig.st_nlink == 2, "orig.txt link count is 2");
        check(hlink.st_nlink == 2, "hardlink.txt link count is 2");
        check(orig.st_ino == hlink.st_ino && orig.st_dev == hlink.st_dev,
              "hardlink shares orig.txt inode");
    } else {
        check(0, "stat orig.txt/hardlink.txt");
    }

    /* Content read back byte-exact through lkl-fsd. */
    fd = open(ROOT "/orig.txt", O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
    if (fd >= 0)
        close(fd);
    check(n == 9 && memcmp(buf, "original\n", 9) == 0, "orig.txt content");

    /* Sparse: 1 MiB logical, but only a few blocks actually allocated. */
    if (stat(ROOT "/sparse.bin", &sparse) == 0) {
        check(sparse.st_size == SPARSE_SIZE, "sparse.bin logical size 1 MiB");
        check((off_t) sparse.st_blocks * 512 < sparse.st_size / 2,
              "sparse.bin is under-allocated on disk");
    } else {
        check(0, "stat sparse.bin");
    }

    /* Large directory: every entry Linux wrote is enumerable. */
    dir = opendir(ROOT "/bigdir");
    if (dir) {
        struct dirent *de;

        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
                count++;
        }
        closedir(dir);
    }
    check(count == BIGDIR_ENTRIES, "bigdir enumerates all 200 entries");

    if (fails == 0) {
        printf("lkl-fsd-read-test: PASS\n");
        return 0;
    }
    printf("lkl-fsd-read-test: FAIL (%d checks)\n", fails);
    return 1;
}
