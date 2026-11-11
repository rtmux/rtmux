/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dmesg applet
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
#include <sys/slog.h>

static int events[4096];

static void print_record(const int *evp, int cnt)
{
    int severity = _SLOG_GETSEVERITY(evp[0]);
    int major = _SLOG_GETMAJOR(evp[1]);
    int minor = _SLOG_GETMINOR(evp[1]);
    int text = _SLOG_GETTEXT(evp[0]);
    time_t sec = evp[2];
    char timebuf[32];

    strftime(timebuf, sizeof(timebuf), "%b %d %T", localtime(&sec));
    printf("%s sev=%d major=%d minor=%d ", timebuf, severity, major, minor);

    if (text) {
        int payload_ints = cnt - _SLOG_HDRINTS;
        int payload_bytes =
            payload_ints > 0 ? payload_ints * (int) sizeof(int) : 0;
        printf("%.*s", payload_bytes, (const char *) &evp[_SLOG_HDRINTS]);
    } else {
        for (int i = _SLOG_HDRINTS; i < cnt; i++)
            printf("%d%s", evp[i], i + 1 == cnt ? "" : " ");
    }
    printf("\n");
}

int toolbox_dmesg_main(int argc, char *argv[])
{
    const char *path = "/dev/slog";
    size_t residual = 0;
    int wait = 0;
    int opt;
    int fd;

    while ((opt = getopt(argc, argv, "w")) != -1) {
        switch (opt) {
        case 'w':
            wait = 1;
            break;
        default:
            fprintf(stderr, "usage: dmesg [-w] [FILE]\n");
            return EXIT_FAILURE;
        }
    }
    if (optind < argc)
        path = argv[optind];

    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "dmesg: %s: %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }

    for (;;) {
        ssize_t n =
            read(fd, (char *) events + residual, sizeof(events) - residual);
        if (n == -1) {
            if (errno != EAGAIN) {
                fprintf(stderr, "dmesg: %s: %s\n", path, strerror(errno));
                return EXIT_FAILURE;
            }
            if (!wait)
                return EXIT_SUCCESS;
            int flags = fcntl(fd, F_GETFL);
            if (flags == -1 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
                fprintf(stderr, "dmesg: %s: %s\n", path, strerror(errno));
                return EXIT_FAILURE;
            }
            continue;
        }
        if (n == 0)
            return EXIT_SUCCESS;

        size_t total = (size_t) n + residual;
        size_t nints = total / sizeof(int);
        size_t sub = total - nints * sizeof(int);
        int *evp;

        residual = sub;
        for (evp = events; evp < &events[nints];) {
            size_t left = (size_t) (&events[nints] - evp);
            int cnt;

            if (left < _SLOG_HDRINTS) {
                residual = left * sizeof(int) + sub;
                break;
            }
            cnt = _SLOG_GETCOUNT(evp[0]) + _SLOG_HDRINTS;
            if (cnt < _SLOG_HDRINTS) {
                fprintf(stderr, "dmesg: malformed record\n");
                return EXIT_FAILURE;
            }
            if (evp + cnt > &events[nints]) {
                residual = left * sizeof(int) + sub;
                break;
            }
            print_record(evp, cnt);
            evp += cnt;
        }
        if (residual)
            memmove(events, evp, residual);
        fflush(stdout);
    }
}
