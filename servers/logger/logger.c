/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

static int openlog(char *fname, int fsize, int flags)
{
    int fd;
    struct stat sbuf1, sbuf2;
    char fname1[256], fname2[256];
    int osync;

    if (fsize) {
        memset(&sbuf1, 0, sizeof(sbuf1));
        sprintf(fname1, "%s1", fname);
        memset(&sbuf2, 0, sizeof(sbuf2));
        sprintf(fname2, "%s2", fname);

        stat(fname1, &sbuf1);
        stat(fname2, &sbuf2);

        /* Pick the newer file (later date) if it has room for more events.
         * Otherwise truncate the older file (earlier date) and use it.
         */

        if (sbuf1.st_mtime >= sbuf2.st_mtime) {
            fname = fname1;
            if (sbuf1.st_size >= fsize) {
                printf("Unlink %s\n", fname2);
                unlink(fname = fname2);
            }
        } else {
            fname = fname2;
            if (sbuf2.st_size >= fsize) {
                printf("Unlink %s\n", fname1);
                unlink(fname = fname1);
            }
        }
    }

    osync = (flags & LOGF_FLAG_OSYNC) ? O_SYNC : 0;
    /* ipc-send: logger-drain -> romfs,lkl-fsd */
    fd = open(fname, O_WRONLY | O_APPEND | O_CREAT | osync, 0666);

    return fd;
}

/* This is a separate thread which opens /dev/slog (just like any application)
 * and reads events into a file. Building it into the logger service avoids
 * another utility for recording calls.
 */
void *logger(void *dummy)
{
    int rfd, wfd, events[2048 / sizeof(int)];
    int cnt, tty = 0;
    ssize_t n;
    size_t size;
    int *evp;

    /* ipc-send: logger-drain -> roottask-core,logger-ring */
    rfd = open("/dev/slog", O_RDONLY);
    if (rfd == -1) {
        fprintf(stderr, "%s: Unable to open /dev/slog for logging : %s\n",
                logger_program_name, strerror(errno));
        return 0;
    }

    for (wfd = -1, size = 0;;) {
        n = read(rfd, events, sizeof(events));
        if (n > 0) {
            if (size == 0) {
                close(wfd);

                wfd = openlog(LogFname, LogFsize, LogFflags);
                if (wfd != -1) {
                    size = LogFsize ? (size_t) LogFsize : (size_t) INT_MAX;
                    tty = isatty(wfd);
                }
            }

            for (evp = events; evp < &events[(size_t) n / sizeof(int)];
                 evp += cnt) {
                cnt = _SLOG_GETCOUNT(*evp) + _SLOG_HDRINTS;

                /* Filter based upon major and severity. */
                if (_SLOG_GETSEVERITY(*evp) > FilterLog)
                    continue;

                if (tty) {
                    if (_SLOG_GETTEXT(*evp))
                        write(wfd, evp + _SLOG_HDRINTS,
                              (size_t) (cnt - _SLOG_HDRINTS) * sizeof(int));
                } else {
                    size_t nbytes = (size_t) cnt * sizeof(int);
                    write(wfd, evp, nbytes);
                    size -= nbytes;
                }
            }
        } else
            sleep(1); /* So we don't spin on a hard read error */
    }
}
