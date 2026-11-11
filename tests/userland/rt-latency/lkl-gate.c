/*
 * lkl-gate - containment checks for absent LKL domains.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>

static int must_be_absent(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);

    if (fd != -1) {
        (void) close(fd);
        fprintf(stderr, "lkl-gate: %s unexpectedly present\n", path);
        return -1;
    }
    if (errno != ENOENT && errno != ENXIO) {
        fprintf(stderr, "lkl-gate: %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t before;
    uint64_t after;
    static const char *const default_paths[] = {
        "/fs/root",   "/net",           "/net/dns",       "/net/ping",
        "/net/stats", "/net/tcp/clone", "/net/udp/clone",
    };
    /* With no args, check every LKL path is gone (the full-teardown gate the
     * single-domain crash/revoke modes use). With args, check only the given
     * paths -- lkl-multi-crash kills one domain and passes just that domain's
     * paths, since the surviving domain's paths are still legitimately present.
     */
    const char *const *paths = default_paths;
    size_t npaths = sizeof(default_paths) / sizeof(default_paths[0]);

    if (argc > 1) {
        paths = (const char *const *) &argv[1];
        npaths = (size_t) (argc - 1);
    }

    if (__clock_time(CLOCK_MONOTONIC, NULL, &before) == -1)
        return EXIT_FAILURE;
    for (size_t i = 0; i < npaths; i++)
        if (must_be_absent(paths[i]) == -1)
            return EXIT_FAILURE;
    if (__clock_time(CLOCK_MONOTONIC, NULL, &after) == -1)
        return EXIT_FAILURE;
    if (after < before) {
        fprintf(stderr, "lkl-gate: monotonic clock went backward\n");
        return EXIT_FAILURE;
    }

    printf("lkl-gate: absent-domain roundtrip_ns=%llu\n",
           (unsigned long long) (after - before));
    return EXIT_SUCCESS;
}
