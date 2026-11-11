/*
 * RTMux libc — POSIX process times.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/rtmux-core.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000ULL

static clock_t nsec_to_ticks(uint64_t nsec, long hz)
{
    uint64_t ticks = (nsec / NSEC_PER_SEC) * (uint64_t) hz +
                     ((nsec % NSEC_PER_SEC) * (uint64_t) hz) / NSEC_PER_SEC;

    return (clock_t) ticks;
}

static clock_t timeval_to_ticks(const struct timeval *tv, long hz)
{
    uint64_t usec = (uint64_t) tv->tv_sec * 1000000ULL + (uint64_t) tv->tv_usec;

    return (clock_t) ((usec * (uint64_t) hz) / 1000000ULL);
}

clock_t times(struct tms *buf)
{
    long hz = sysconf(_SC_CLK_TCK);
    uint64_t elapsed_ns;
    struct rusage self;
    struct rusage children;

    if (!buf || hz <= 0) {
        errno = EINVAL;
        return (clock_t) -1;
    }
    if (__clock_time(CLOCK_MONOTONIC, NULL, &elapsed_ns) == -1 ||
        getrusage(RUSAGE_SELF, &self) == -1 ||
        getrusage(RUSAGE_CHILDREN, &children) == -1)
        return (clock_t) -1;

    *buf = (struct tms){
        .tms_utime = timeval_to_ticks(&self.ru_utime, hz),
        .tms_stime = timeval_to_ticks(&self.ru_stime, hz),
        .tms_cutime = timeval_to_ticks(&children.ru_utime, hz),
        .tms_cstime = timeval_to_ticks(&children.ru_stime, hz),
    };
    return nsec_to_ticks(elapsed_ns, hz);
}
