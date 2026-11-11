/*
 * waitfor — wait for a path to appear during early boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned seconds = 10;
    char *end;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
    unsigned ticks;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: waitfor PATH [SECONDS]\n");
        return 2;
    }
    if (argc == 3) {
        unsigned long v = strtoul(argv[2], &end, 10);

        if (*end != '\0' || v > 3600) {
            fprintf(stderr, "waitfor: invalid timeout '%s'\n", argv[2]);
            return 2;
        }
        seconds = (unsigned) v;
    }

    ticks = seconds * 10;
    for (;;) {
        if (access(argv[1], F_OK) == 0)
            return 0;
        if (ticks-- == 0)
            break;
        (void) nanosleep(&ts, NULL);
    }

    fprintf(stderr, "waitfor: %s: %s\n", argv[1], strerror(errno));
    return 1;
}
