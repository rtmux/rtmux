/*
 * lkl-measure - small LKL I/O-service round-trip probe.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/trace.h>

#include "chrome-trace.h"

#define DEFAULT_SAMPLES 25
#define MAX_SAMPLES 10000000
#define TRACE_PHASE_BEGIN 0U
#define TRACE_PHASE_WAKE 1U
#define TRACE_PHASE_END 2U

static bool load_stop;
static bool load_done;
static bool load_ready;
static bool trace_samples;
static bool split_cpus;
static bool emit_json;
static char measure_net_id[8];

static unsigned trace_mode_id(const char *mode)
{
    if (!strcmp(mode, "fs"))
        return RTMUX_TRACE_LKL_MEASURE_FS;
    if (!strcmp(mode, "net"))
        return RTMUX_TRACE_LKL_MEASURE_NET;
    if (!strcmp(mode, "net-blocked"))
        return RTMUX_TRACE_LKL_MEASURE_NET_BLOCKED;
    if (!strcmp(mode, "net-revoke"))
        return RTMUX_TRACE_LKL_MEASURE_NET_REVOKE;
    if (!strcmp(mode, "fs-rt"))
        return RTMUX_TRACE_LKL_MEASURE_FS_RT;
    if (!strcmp(mode, "net-rt"))
        return RTMUX_TRACE_LKL_MEASURE_NET_RT;
    return 0;
}

static void trace_measure_event(unsigned event, const char *mode, int sample)
{
    unsigned data[4];

    if (!trace_samples)
        return;
    data[0] = RTMUX_TRACE_INSERTSUSEREVENT;
    data[1] = event;
    data[2] = trace_mode_id(mode);
    data[3] = (unsigned) sample;
    (void) trace_event(data);
}

static void trace_measure_mono(const char *mode, int sample, unsigned phase,
                               uint64_t ns)
{
    unsigned data[4];
    unsigned mode_id = trace_mode_id(mode);
    unsigned key;

    if (!trace_samples || mode_id == 0)
        return;
    key = ((phase & 0xfU) << 28) | ((mode_id & 0xfU) << 24) |
          ((unsigned) sample & 0x00ffffffU);
    data[0] = RTMUX_TRACE_INSERTSUSEREVENT;
    data[1] = RTMUX_TRACE_USER_LKL_MEASURE_MONO_LOW;
    data[2] = key;
    data[3] = (unsigned) ns;
    (void) trace_event(data);
    data[1] = RTMUX_TRACE_USER_LKL_MEASURE_MONO_HIGH;
    data[3] = (unsigned) (ns >> 32);
    (void) trace_event(data);
}

static int monotonic_ns(uint64_t *out)
{
    if (__clock_time(CLOCK_MONOTONIC, NULL, out) == -1) {
        fprintf(stderr, "clock_time errno=%d\n", errno);
        return -1;
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *) a;
    uint64_t bv = *(const uint64_t *) b;

    return (av > bv) - (av < bv);
}

static void ns_to_ts(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec = (time_t) (ns / 1000000000ULL);
    ts->tv_nsec = (long) (ns % 1000000000ULL);
}

static int raise_to_rt(void)
{
    struct sched_param sp = {
        .sched_priority = 60,
    };

    return __sched_set(0, 0, SCHED_FIFO, &sp);
}

static int pin_to_cpu(unsigned cpu)
{
    struct {
        struct _thread_runmask hdr;
        unsigned mask[2];
    } runmask = {
        .hdr = {.size = 1U},
    };

    RMSK_SET(cpu, runmask.mask);
    RMSK_SET(cpu, runmask.mask + 1);
    return thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, &runmask);
}

static int read_file(const char *path)
{
    char buf[256];
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    do {
        n = read(fd, buf, sizeof(buf));
    } while (n > 0);
    (void) close(fd);
    return n == 0 ? 0 : -1;
}

static int read_fd(int fd)
{
    char buf[256];
    ssize_t n;

    if (lseek(fd, 0, SEEK_SET) == -1)
        return -1;
    do {
        n = read(fd, buf, sizeof(buf));
    } while (n > 0);
    return n == 0 ? 0 : -1;
}

static int ping_host(void)
{
    char reply[128];
    static const char cmd[] = "10.0.2.2 1\n";
    int fd;
    ssize_t n;

    fd = open("/net/ping", O_RDWR);
    if (fd == -1)
        return -1;
    if (write(fd, cmd, sizeof(cmd) - 1) != (ssize_t) (sizeof(cmd) - 1)) {
        (void) close(fd);
        return -1;
    }
    n = read(fd, reply, sizeof(reply));
    (void) close(fd);
    return n > 0 ? 0 : -1;
}

/* Expect the write to fail: unreachable host or a revoked /net/ping. */
static int ping_write_fails(const char *cmd)
{
    int fd;
    ssize_t n;

    fd = open("/net/ping", O_RDWR);
    if (fd == -1)
        return -1;
    n = write(fd, cmd, strlen(cmd));
    (void) close(fd);
    return n == -1 ? 0 : -1;
}

static int ping_unreachable(void)
{
    return ping_write_fails("10.0.2.254 1\n");
}

static int ping_revoke(void)
{
    return ping_write_fails("10.0.2.2 1\n");
}

static void trim_newline(char *s)
{
    char *p = s;

    while (*p != '\0') {
        if (*p == '\n' || *p == '\r') {
            *p = '\0';
            return;
        }
        p++;
    }
}

static void close_net_clone(void)
{
    char path[32];
    int fd;

    if (measure_net_id[0] == '\0')
        return;
    snprintf(path, sizeof(path), "/net/udp/%s/ctl", measure_net_id);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        (void) write(fd, "close\n", 6);
        (void) close(fd);
    }
    measure_net_id[0] = '\0';
}

static int open_udp_echo(void)
{
    char path[32];
    char ctl[80];
    int fd;
    ssize_t n;

    fd = open("/net/udp/clone", O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, measure_net_id, sizeof(measure_net_id) - 1);
    (void) close(fd);
    if (n <= 0)
        return -1;
    measure_net_id[n] = '\0';
    trim_newline(measure_net_id);
    if (measure_net_id[0] == '\0' || strchr(measure_net_id, '/'))
        return -1;
    snprintf(path, sizeof(path), "/net/udp/%s/ctl", measure_net_id);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        goto fail;
    snprintf(ctl, sizeof(ctl), "connect 10.0.2.2 8001\n");
    if (write(fd, ctl, strlen(ctl)) != (ssize_t) strlen(ctl)) {
        (void) close(fd);
        goto fail;
    }
    (void) close(fd);
    snprintf(path, sizeof(path), "/net/udp/%s/data", measure_net_id);
    fd = open(path, O_RDWR);
    if (fd >= 0)
        return fd;
fail:
    close_net_clone();
    return -1;
}

static int udp_echo_fd(int fd)
{
    char rx[64];
    static const char tx[] = "lat\n";
    const struct timespec retry = {
        .tv_sec = 0,
        .tv_nsec = 50000L,
    };
    ssize_t n;

    if (write(fd, tx, sizeof(tx) - 1) != (ssize_t) (sizeof(tx) - 1))
        return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        n = read(fd, rx, sizeof(rx));
        if (n > 0)
            return 0;
        if (n != -1 || errno != EAGAIN)
            return -1;
        (void) nanosleep(&retry, NULL);
    }
    return -1;
}

static int run_one(const char *mode)
{
    if (!strcmp(mode, "fs"))
        return read_file("/fs/root/hello.txt");
    if (!strcmp(mode, "net"))
        return ping_host();
    if (!strcmp(mode, "net-blocked"))
        return ping_unreachable();
    if (!strcmp(mode, "net-revoke"))
        return ping_revoke();
    errno = EINVAL;
    return -1;
}

static int open_measure_fd(const char *mode)
{
    if (!strcmp(mode, "fs"))
        return open("/fs/root/hello.txt", O_RDONLY);
    if (!strcmp(mode, "net"))
        return open_udp_echo();
    errno = EINVAL;
    return -1;
}

static int run_one_fd(const char *mode, int fd)
{
    if (!strcmp(mode, "fs"))
        return read_fd(fd);
    if (!strcmp(mode, "net"))
        return udp_echo_fd(fd);
    errno = EINVAL;
    return -1;
}

static void pace_load_thread(void)
{
    struct timespec wait = {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };

    (void) nanosleep(&wait, NULL);
}

static void close_measure_fd(const char *mode, int fd)
{
    (void) close(fd);
    if (!strcmp(mode, "net"))
        close_net_clone();
}

static void *load_thread(void *arg)
{
    const char *mode = arg;
    int fd;

    if (split_cpus)
        (void) pin_to_cpu(1);
    fd = open_measure_fd(mode);
    /* Measure steady interference, not the load thread's own fd/setup warmup.
     */
    if (fd >= 0)
        (void) run_one_fd(mode, fd);
    else
        (void) run_one(mode);
    __atomic_store_n(&load_ready, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&load_stop, __ATOMIC_RELAXED)) {
        if (fd >= 0)
            (void) run_one_fd(mode, fd);
        else
            (void) run_one(mode);
        pace_load_thread();
    }
    if (fd >= 0)
        close_measure_fd(mode, fd);
    __atomic_store_n(&load_done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void stop_load_thread(void)
{
    struct timespec wait = {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };

    /* No pthread_join in this libc, so wait for the detached load thread to
     * actually observe the stop flag and exit before the caller frees state and
     * the process tears down. The thread checks load_stop between every
     * run_one(), so this returns within one in-flight request.
     */
    __atomic_store_n(&load_stop, true, __ATOMIC_RELAXED);
    while (!__atomic_load_n(&load_done, __ATOMIC_ACQUIRE))
        (void) nanosleep(&wait, NULL);
}

/* Nearest-rank percentile: ceil(samples * pct / 100) - 1 (values sorted). */
static uint64_t percentile(const uint64_t *values, int samples, unsigned pct)
{
    return values[((size_t) samples * pct + 99U) / 100U - 1U];
}

/* Sort the sample set, then print the min/median/p99/max latency spread. min_ns
 * and p50_ns expose whether a class is uniformly slow (structural cost) or
 * fast-with-a-tail (jitter); max_ns/p99_ns stay first for the budget rows.
 */
static void report(const char *label,
                   uint64_t *values,
                   int samples,
                   uint64_t max)
{
    qsort(values, (size_t) samples, sizeof(*values), cmp_u64);
    printf(
        "lkl-measure: mode=%s samples=%d max_ns=%llu p99_ns=%llu "
        "p50_ns=%llu min_ns=%llu\n",
        label, samples, (unsigned long long) max,
        (unsigned long long) percentile(values, samples, 99),
        (unsigned long long) percentile(values, samples, 50),
        (unsigned long long) values[0]);
}

static void trace_sample(const char *mode,
                         int sample,
                         uint64_t before,
                         uint64_t after)
{
    if (!trace_samples)
        return;
    printf(
        "lkl-trace: mode=%s sample=%d pid=%d tid=%d before_ns=%llu "
        "after_ns=%llu delta_ns=%llu\n",
        mode, sample, (int) getpid(), (int) pthread_self(),
        (unsigned long long) before,
        (unsigned long long) after,
        (unsigned long long) (after >= before ? after - before : 0));
    if (emit_json)
        chrome_emit_span(mode, chrome_tid(mode), before,
                         after >= before ? after - before : 0);
}

static int run_interference(const char *load_mode, int samples)
{
    char label[64];
    char rt_name[64];
    uint64_t *values, *step_values, *trace_before, *trace_after;
    uint64_t max = 0, max_step = 0, last = 0;
    uint64_t max_step_late = 0;
    uint64_t deadline;
    pthread_t thread;

    if (split_cpus && pin_to_cpu(1) == -1) {
        printf("lkl-measure-split: mode=%s-rt using cpu0 errno=%d\n",
               load_mode, errno);
        split_cpus = false;
    }
    if (split_cpus && pin_to_cpu(0) == -1) {
        printf("lkl-measure-split: mode=%s-rt unavailable errno=%d\n",
               load_mode, errno);
        return 0;
    }
    values = calloc((size_t) samples, sizeof(*values));
    if (!values)
        return -1;
    step_values = calloc((size_t) samples, sizeof(*step_values));
    if (!step_values) {
        free(values);
        return -1;
    }
    trace_before =
        trace_samples ? calloc((size_t) samples, sizeof(*trace_before)) : NULL;
    trace_after =
        trace_samples ? calloc((size_t) samples, sizeof(*trace_after)) : NULL;
    if (trace_samples && (!trace_before || !trace_after)) {
        free(values);
        free(step_values);
        free(trace_before);
        free(trace_after);
        return -1;
    }
    __atomic_store_n(&load_stop, false, __ATOMIC_RELAXED);
    __atomic_store_n(&load_done, false, __ATOMIC_RELAXED);
    __atomic_store_n(&load_ready, false, __ATOMIC_RELAXED);
    if (pthread_create(&thread, NULL, load_thread, (void *) load_mode) != 0) {
        free(values);
        free(step_values);
        free(trace_before);
        free(trace_after);
        return -1;
    }
    (void) pthread_detach(thread);
    while (!__atomic_load_n(&load_ready, __ATOMIC_ACQUIRE))
        pace_load_thread();
    if (raise_to_rt() == -1 || monotonic_ns(&deadline) == -1) {
        stop_load_thread();
        free(values);
        free(step_values);
        free(trace_before);
        free(trace_after);
        return -1;
    }
    last = deadline;
    /* Spans on this path are the native RT wake, not the load's fs/net op, so
     * label them <mode>-rt to match the reports and land on their own track.
     */
    snprintf(rt_name, sizeof(rt_name), "%s-rt", load_mode);
    for (int i = 0; i < samples; i++) {
        uint64_t now;
        uint64_t late;
        uint64_t step_late;
        struct timespec wait;

        deadline += 1000000ULL;
        if (monotonic_ns(&now) == -1)
            goto fail;
        trace_measure_event(RTMUX_TRACE_USER_LKL_MEASURE_BEGIN, rt_name, i);
        trace_measure_mono(rt_name, i, TRACE_PHASE_BEGIN, now);
        if (now < deadline) {
            ns_to_ts(deadline - now, &wait);
            if (nanosleep(&wait, NULL) == -1 && errno != EINTR)
                goto fail;
        }
        trace_measure_event(RTMUX_TRACE_USER_LKL_MEASURE_WAKE, rt_name, i);
        if (monotonic_ns(&now) == -1)
            goto fail;
        trace_measure_mono(rt_name, i, TRACE_PHASE_WAKE, now);
        late = now > deadline ? now - deadline : 0;
        if (now >= last && now - last > max_step)
            max_step = now - last;
        step_late = now - last > 1000000ULL ? now - last - 1000000ULL : 0;
        last = now;
        trace_measure_mono(rt_name, i, TRACE_PHASE_END, now);
        trace_measure_event(RTMUX_TRACE_USER_LKL_MEASURE_END, rt_name, i);
        if (trace_samples) {
            trace_before[i] = deadline;
            trace_after[i] = now;
        }
        values[i] = late;
        step_values[i] = step_late;
        if (late > max)
            max = late;
        if (step_late > max_step_late)
            max_step_late = step_late;
    }
    stop_load_thread();
    for (int i = 0; trace_samples && i < samples; i++)
        trace_sample(rt_name, i, trace_before[i], trace_after[i]);
    printf("lkl-measure-step: mode=%s-rt max_step_ns=%llu\n", load_mode,
           (unsigned long long) max_step);
    snprintf(label, sizeof(label), "%s-rt", load_mode);
    report(label, values, samples, max);
    snprintf(label, sizeof(label), "%s-rt-step", load_mode);
    report(label, step_values, samples, max_step_late);
    free(values);
    free(step_values);
    free(trace_before);
    free(trace_after);
    return 0;

fail:
    stop_load_thread();
    free(values);
    free(step_values);
    free(trace_before);
    free(trace_after);
    return -1;
}

int main(int argc, char **argv)
{
    const char *mode;
    int samples = DEFAULT_SAMPLES;
    int measure_fd = -1;
    uint64_t *values, *trace_before, *trace_after;
    uint64_t max = 0;

    if (argc < 2 || argc > 6) {
        fprintf(stderr,
                "usage: lkl-measure "
                "<fs|net|net-blocked|net-revoke|fs-rt|net-rt> "
                "[samples [trace] [split] [json]]\n");
        return EXIT_FAILURE;
    }
    mode = argv[1];
    if (argc >= 3) {
        char *end;
        long v = strtol(argv[2], &end, 10);

        if (*end != '\0' || v <= 0 || v > MAX_SAMPLES) {
            fprintf(stderr, "lkl-measure: bad sample count: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
        samples = (int) v;
    }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "trace")) {
            trace_samples = true;
        } else if (!strcmp(argv[i], "split")) {
            split_cpus = true;
        } else if (!strcmp(argv[i], "json")) {
            emit_json = true;
            trace_samples = true; /* Chrome events need the per-sample spans */
        } else {
            fprintf(stderr, "lkl-measure: bad option: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    if (!strcmp(mode, "fs-rt"))
        return run_interference("fs", samples) == 0 ? EXIT_SUCCESS
                                                    : EXIT_FAILURE;
    if (!strcmp(mode, "net-rt"))
        return run_interference("net", samples) == 0 ? EXIT_SUCCESS
                                                     : EXIT_FAILURE;
    values = calloc((size_t) samples, sizeof(*values));
    if (!values) {
        fprintf(stderr, "calloc errno=%d\n", errno);
        return EXIT_FAILURE;
    }
    trace_before =
        trace_samples ? calloc((size_t) samples, sizeof(*trace_before)) : NULL;
    trace_after =
        trace_samples ? calloc((size_t) samples, sizeof(*trace_after)) : NULL;
    if (trace_samples && (!trace_before || !trace_after)) {
        fprintf(stderr, "calloc errno=%d\n", errno);
        free(values);
        free(trace_before);
        free(trace_after);
        return EXIT_FAILURE;
    }
    if (!strcmp(mode, "fs") || !strcmp(mode, "net")) {
        measure_fd = open_measure_fd(mode);
        if (measure_fd == -1) {
            fprintf(stderr, "lkl-measure: open %s errno=%d\n", mode, errno);
            free(values);
            free(trace_before);
            free(trace_after);
            return EXIT_FAILURE;
        }
        bool saved_trace = trace_samples;
        trace_samples = false;
        if (run_one_fd(mode, measure_fd) == -1) {
            fprintf(stderr, "lkl-measure: warmup %s errno=%d\n", mode, errno);
            close_measure_fd(mode, measure_fd);
            free(values);
            free(trace_before);
            free(trace_after);
            return EXIT_FAILURE;
        }
        trace_samples = saved_trace;
    }
    if (raise_to_rt() == -1) {
        fprintf(stderr, "lkl-measure: sched errno=%d\n", errno);
        if (measure_fd >= 0)
            close_measure_fd(mode, measure_fd);
        free(values);
        free(trace_before);
        free(trace_after);
        return EXIT_FAILURE;
    }
    if (split_cpus && pin_to_cpu(0) == -1) {
        fprintf(stderr, "lkl-measure: split pin errno=%d\n", errno);
        if (measure_fd >= 0)
            close_measure_fd(mode, measure_fd);
        free(values);
        free(trace_before);
        free(trace_after);
        return EXIT_FAILURE;
    }
    for (int i = 0; i < samples; i++) {
        uint64_t before;
        uint64_t after;

        if (monotonic_ns(&before) == -1)
            goto sample_fail;
        trace_measure_event(RTMUX_TRACE_USER_LKL_MEASURE_BEGIN, mode, i);
        if ((measure_fd >= 0 ? run_one_fd(mode, measure_fd) : run_one(mode)) ==
                -1 ||
            monotonic_ns(&after) == -1) {
sample_fail:
            fprintf(stderr, "lkl-measure: %s sample %d failed errno=%d\n", mode,
                    i, errno);
            if (measure_fd >= 0)
                close_measure_fd(mode, measure_fd);
            free(values);
            free(trace_before);
            free(trace_after);
            return EXIT_FAILURE;
        }
        trace_measure_event(RTMUX_TRACE_USER_LKL_MEASURE_END, mode, i);
        values[i] = after - before;
        if (trace_samples) {
            trace_before[i] = before;
            trace_after[i] = after;
        }
        if (values[i] > max)
            max = values[i];
    }
    if (measure_fd >= 0)
        close_measure_fd(mode, measure_fd);
    for (int i = 0; trace_samples && i < samples; i++)
        trace_sample(mode, i, trace_before[i], trace_after[i]);
    report(mode, values, samples, max);
    free(values);
    free(trace_before);
    free(trace_after);
    return EXIT_SUCCESS;
}
