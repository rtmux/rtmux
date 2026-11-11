/*
 * lkl-runmask - launch an LKL server on a CPU mask.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/procmsg.h>

extern char **environ;

int main(int argc, char **argv)
{
    char *end;
    unsigned long mask;
    struct inheritance inherit = {
        .flags = SPAWN_EXPLICIT_CPU,
        .runmask = 0,
    };
    int fd_map[] = {
        [0] = STDIN_FILENO,
        [1] = STDOUT_FILENO,
        [2] = STDERR_FILENO,
    };
    pid_t pid;

    if (argc < 3) {
        fprintf(stderr, "usage: lkl-runmask MASK COMMAND [ARG...]\n");
        return EXIT_FAILURE;
    }
    errno = 0;
    mask = strtoul(argv[1], &end, 0);
    if (errno != 0 || *end != '\0' || mask == 0 || mask > UINT32_MAX) {
        fprintf(stderr, "lkl-runmask: bad mask: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    inherit.runmask = (uint32_t) mask;
    pid = strchr(argv[2], '/') ?
              rtmux_spawn(argv[2], 3, fd_map, &inherit, &argv[2], environ) :
              rtmux_spawnp(argv[2], 3, fd_map, &inherit, &argv[2], environ);
    if (pid == -1) {
        fprintf(stderr, "lkl-runmask: spawn %s mask=%#lx errno=%d\n", argv[2],
                mask, errno);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "lkl-runmask: spawned %s pid=%d mask=%#lx\n", argv[2],
            (int) pid, mask);
    return EXIT_SUCCESS;
}
