/*
 * namespace-roundtrip - namespace counter microbenchmark.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_ITERATIONS 100
#define STATS_PATH "/sys/namespace-stats"
#define OPEN_PATH "/bin/true"
#define LINK_PATH "/etc"

struct namespace_stats {
    unsigned long long total;
    unsigned long long open;
    unsigned long long readlink;
};

static int read_file(const char *path, char *buf, size_t cap)
{
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    n = read(fd, buf, cap - 1);
    (void) close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static unsigned long long stat_value(const char *buf, const char *name)
{
    size_t len = strlen(name);
    const char *p = buf;

    while (*p != '\0') {
        if (!strncmp(p, name, len) && p[len] == ' ')
            return strtoull(p + len + 1, NULL, 10);
        p = strchr(p, '\n');
        if (!p)
            break;
        p++;
    }
    return 0;
}

static int read_stats(struct namespace_stats *stats)
{
    char buf[512];

    if (read_file(STATS_PATH, buf, sizeof(buf)) == -1)
        return -1;
    stats->total = stat_value(buf, "total");
    stats->open = stat_value(buf, "open");
    stats->readlink = stat_value(buf, "readlink");
    return 0;
}

static unsigned long long elapsed_us(const struct timeval *a,
                                     const struct timeval *b)
{
    unsigned long long sec = (unsigned long long) (b->tv_sec - a->tv_sec);
    long usec = b->tv_usec - a->tv_usec;

    if (usec < 0) {
        sec--;
        usec += 1000000;
    }
    return sec * 1000000ULL + (unsigned long long) usec;
}

static int cmp_ull(const void *a, const void *b)
{
    unsigned long long av = *(const unsigned long long *) a;
    unsigned long long bv = *(const unsigned long long *) b;

    if (av < bv)
        return -1;
    if (av > bv)
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    int iterations = DEFAULT_ITERATIONS;
    struct namespace_stats before, after;
    struct timeval t0, t1;
    struct stat st;
    char linkbuf[128];
    unsigned long long *samples;
    unsigned long long total_ns = 0;
    unsigned long long max_ns = 0;
    unsigned long long p99_ns;

    if (argc > 1)
        iterations = atoi(argv[1]);
    if (iterations <= 0) {
        fprintf(stderr, "usage: namespace-roundtrip [iterations]\n");
        return EXIT_FAILURE;
    }

    samples = calloc((size_t) iterations, sizeof(*samples));
    if (!samples) {
        fprintf(stderr, "namespace-roundtrip: calloc: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (read_stats(&before) == -1) {
        fprintf(stderr, "namespace-roundtrip: cannot read %s: %s\n", STATS_PATH,
                strerror(errno));
        free(samples);
        return EXIT_FAILURE;
    }
    if (gettimeofday(&t0, NULL) == -1) {
        fprintf(stderr, "namespace-roundtrip: gettimeofday: %s\n",
                strerror(errno));
        free(samples);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < iterations; i++) {
        struct timeval i0, i1;
        int fd;

        if (gettimeofday(&i0, NULL) == -1) {
            fprintf(stderr, "namespace-roundtrip: gettimeofday: %s\n",
                    strerror(errno));
            free(samples);
            return EXIT_FAILURE;
        }
        fd = openat(AT_FDCWD, OPEN_PATH, O_RDONLY);
        if (fd != -1)
            (void) close(fd);
        (void) fstatat(AT_FDCWD, OPEN_PATH, &st, 0);
        (void) readlinkat(AT_FDCWD, LINK_PATH, linkbuf, sizeof(linkbuf));
        if (gettimeofday(&i1, NULL) == -1) {
            fprintf(stderr, "namespace-roundtrip: gettimeofday: %s\n",
                    strerror(errno));
            free(samples);
            return EXIT_FAILURE;
        }
        samples[i] = elapsed_us(&i0, &i1) * 1000ULL;
        total_ns += samples[i];
        if (samples[i] > max_ns)
            max_ns = samples[i];
    }

    if (gettimeofday(&t1, NULL) == -1) {
        fprintf(stderr, "namespace-roundtrip: gettimeofday: %s\n",
                strerror(errno));
        free(samples);
        return EXIT_FAILURE;
    }
    if (read_stats(&after) == -1) {
        fprintf(stderr, "namespace-roundtrip: cannot reread %s: %s\n",
                STATS_PATH, strerror(errno));
        free(samples);
        return EXIT_FAILURE;
    }

    qsort(samples, (size_t) iterations, sizeof(*samples), cmp_ull);
    p99_ns = samples[((iterations * 99) + 99) / 100 - 1];

    printf(
        "namespace-roundtrip: iterations=%d elapsed_us=%llu "
        "avg_ns=%llu max_ns=%llu p99_ns=%llu total_delta=%llu "
        "open_delta=%llu readlink_delta=%llu\n",
        iterations, elapsed_us(&t0, &t1), total_ns / (unsigned) iterations,
        max_ns, p99_ns, after.total - before.total, after.open - before.open,
        after.readlink - before.readlink);
    if (after.total <= before.total || after.readlink <= before.readlink) {
        fprintf(stderr,
                "namespace-roundtrip: counters did not advance as expected\n");
        free(samples);
        return EXIT_FAILURE;
    }
    free(samples);
    return EXIT_SUCCESS;
}
