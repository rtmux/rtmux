/*
 * ist-drain-load - drive the NVMe block device to exercise the interrupt path.
 *
 * The romfs server backs /usr from the NVMe partition through a small buffer
 * cache (32 blocks). A file larger than that cache, read start to finish,
 * evicts its own early blocks before the read ends, so reading it repeatedly
 * re-fetches every block from the device. Each fetch is one NVMe read
 * completion drained by the interrupt service thread, so this loop produces a
 * steady stream of drains. With CONFIG_MEASURE_IST_DRAIN the IST reports its
 * drain-before-unmask histogram to the serial log while this runs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 4096
#define PASSES 200

/* Candidate large files the /usr romfs ships; the first that opens is used. */
static const char *const CANDIDATES[] = {
    "/usr/bin/syscall_testing",
    "/usr/lib/libc.so",
    "/usr/bin/sh",
};

int main(void)
{
    static char buf[CHUNK];
    const char *path = NULL;
    int fd = -1;
    unsigned long blocks = 0;

    for (unsigned i = 0; i < sizeof CANDIDATES / sizeof CANDIDATES[0]; i++) {
        fd = open(CANDIDATES[i], O_RDONLY);
        if (fd != -1) {
            path = CANDIDATES[i];
            break;
        }
        printf("ist-drain-load: open(%s): %s\n", CANDIDATES[i], strerror(errno));
    }
    if (fd == -1) {
        printf("ist-drain-load: FAIL no readable /usr file\n");
        return EXIT_FAILURE;
    }
    printf("ist-drain-load: reading %s x%d passes\n", path, PASSES);

    for (int p = 0; p < PASSES; p++) {
        unsigned long pass = 0;
        ssize_t n;

        if (lseek(fd, 0, SEEK_SET) == (off_t) -1) {
            printf("ist-drain-load: FAIL lseek: %s\n", strerror(errno));
            (void) close(fd);
            return EXIT_FAILURE;
        }
        while ((n = read(fd, buf, CHUNK)) > 0) {
            blocks++;
            pass++;
        }
        if (n < 0) {
            printf("ist-drain-load: FAIL read: %s\n", strerror(errno));
            (void) close(fd);
            return EXIT_FAILURE;
        }
        /* The romfs cache holds 32 blocks; a file that fits in it is re-served
         * from cache after the first pass and drives no steady completion load.
         */
        if (p == 0 && pass <= 32) {
            printf("ist-drain-load: FAIL %s is %lu blocks, too small to evict "
                   "the 32-block cache\n",
                   path, pass);
            (void) close(fd);
            return EXIT_FAILURE;
        }
    }
    (void) close(fd);
    printf("ist-drain-load: read %lu blocks over %d passes\n", blocks, PASSES);
    printf("ist-drain-load: PASS\n");
    return EXIT_SUCCESS;
}
