/*
 * lkl-backpressure-test - LKL admission overflow must fail fast.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PING "/net/ping"
#define STATS "/net/stats"

static void nap_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};

    nanosleep(&ts, NULL);
}

static void *blocked_ping(void *arg)
{
    int *ready = arg;
    int fd;

    fd = open(PING, O_WRONLY);
    if (fd < 0) {
        *ready = -1;
        return NULL;
    }
    *ready = 1;
    (void) write(fd, "10.0.2.254 3\n", 13);
    close(fd);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    int ready = 0;

    if (pthread_create(&thread, NULL, blocked_ping, &ready) != EOK) {
        fprintf(stderr, "lkl-backpressure-test: pthread_create failed\n");
        return 1;
    }
    for (int i = 0; i < 50 && ready == 0; i++)
        nap_ms(20);
    if (ready < 0) {
        fprintf(stderr, "lkl-backpressure-test: open %s: %s\n", PING,
                strerror(errno));
        pthread_join(thread, NULL);
        return 1;
    }
    for (int i = 0; i < 50; i++) {
        int fd = open(STATS, O_RDONLY);

        if (fd >= 0) {
            close(fd);
            nap_ms(20);
            continue;
        }

        if (errno == EAGAIN) {
            printf("lkl-backpressure-test: PASS\n");
            pthread_join(thread, NULL);
            return 0;
        }

        fprintf(stderr, "lkl-backpressure-test: open %s: %s\n", STATS,
                strerror(errno));
        pthread_join(thread, NULL);
        return 1;
    }
    fprintf(stderr, "lkl-backpressure-test: FAIL no EAGAIN while LKL busy\n");
    pthread_join(thread, NULL);
    return 1;
}
