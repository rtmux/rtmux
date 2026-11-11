/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* tracelogger — drain the kernel instrumentation ring buffers.
 *
 * First increment of the §17 CTF exporter: prove the kernel trace data reaches
 * a userland consumer. The kernel/trace framework (CONFIG_INSTRUMENTATION)
 * records IPC/scheduling/timer events into per-CPU ring buffers with
 * cycle-accurate timestamps; the buffers are physically contiguous and
 * RTMUX_TRACE_ALLOCBUFFER hands back their physical address, so a root consumer
 * maps them with MAP_PHYS and reads the events directly (the QNX tracelogger
 * pattern).
 *
 * Flow: allocate the ring, map it, enable all classes, start tracing, then
 * drain by POLLING: RTMUX_TRACE_FLUSHBUFFER marks the current buffer
 * _TRACE_FLAGS_FLUSH and advances the ring, and naturally-filled buffers get
 * the same flag, so we scan the mapped ring for FLUSH'd buffers, count their
 * events, dump a few raw records, and clear FLUSH to release them. Polling
 * (rather than waiting on the RTMUX_HOOK_TRACE notification) keeps this first
 * increment independent of the pseudo-interrupt attach path; a
 * notification-driven drain is a later optimization. A bounded loop keeps this
 * a terminating self-test rather than a resident logger. CTF/TSDL encoding for
 * Trace Compass is a later increment; here we only prove events flow.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/trace.h>

#define NBUF 16
#define MAX_WAITS 12
#define DUMP_EVENTS 1024 /* raw records to print per flushed buffer */
#define PRE_EVENTS 32

struct raw_event {
    int buf;
    unsigned ev;
    uint32_t header;
    uint32_t data[3];
};

static int trace_class(unsigned header)
{
    return (int) (header & (0x1fU << 10));
}

static unsigned trace_user_event(unsigned header)
{
    return header & RTMUX_TRACE_USERLAST;
}

static int trace_is_lkl_measure(unsigned header)
{
    unsigned event;

    if (trace_class(header) != _TRACE_USER_C)
        return 0;
    event = trace_user_event(header);
    return event == RTMUX_TRACE_USER_LKL_MEASURE_BEGIN ||
           event == RTMUX_TRACE_USER_LKL_MEASURE_END ||
           event == RTMUX_TRACE_USER_LKL_MEASURE_WAKE;
}

/* Priority-inheritance boost events are sparse, so windowed capture keeps them
 * unconditionally rather than dropping them with the rest of the non-measure
 * stream; rt-trace-sched pairs each boost with its restore into a span.
 */
static int trace_is_pi_boost(unsigned header)
{
    if (trace_class(header) != _TRACE_SYSTEM_C)
        return 0;
    return (header & RTMUX_TRACE_USERLAST) == RTMUX_TRACE_SYS_PI_BOOST;
}

static void print_raw_event(const struct raw_event *ev)
{
    printf("tracelogger: buf %d ev %u header=%#x d=%#x,%#x,%#x\n", ev->buf,
           ev->ev, ev->header, ev->data[0], ev->data[1], ev->data[2]);
}

/* Issue a single-opcode trace control command. */
static int trace_cmd(unsigned opcode)
{
    unsigned cmd = opcode;
    return trace_event(&cmd);
}

/* Enable one trace class. */
static int trace_add_class(unsigned class)
{
    unsigned cmd[2] = {RTMUX_TRACE_ADDCLASS, class};
    return trace_event(cmd);
}

static int pin_to_cpu0(void)
{
    struct {
        struct _thread_runmask hdr;
        unsigned mask[2];
    } runmask = {
        .hdr = {.size = 1U},
    };

    RMSK_SET(0, runmask.mask);
    RMSK_SET(0, runmask.mask + 1);
    return thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, &runmask);
}

int main(int argc, char **argv)
{
    uint64_t paddr = 0;
    unsigned alloc_cmd[4];
    volatile tracebuf_t *ring;
    size_t maplen = (size_t) NBUF * _TRACEBUFSIZE;
    unsigned long total_events = 0;
    unsigned long min_events = 32;
    unsigned long max_waits = MAX_WAITS;
    uint32_t seen_seq[NBUF] = {0};
    unsigned seen_events[NBUF] = {0};
    int windowed = 0;
    int kcall_only = 0;
    int in_measure = 0;
    struct raw_event pre[PRE_EVENTS];
    unsigned pre_head = 0;
    unsigned pre_count = 0;
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000}; /* 20 ms */

    if (argc > 1) {
        min_events = strtoul(argv[1], NULL, 0);
    }
    if (argc > 2) {
        max_waits = strtoul(argv[2], NULL, 0);
        if (max_waits == 0)
            max_waits = MAX_WAITS;
    }
    if (argc > 3 && !strcmp(argv[3], "windowed"))
        windowed = 1;
    /* "kcall" enables only the kernel-call exit class, so no other class emits a
     * ring event inside the window the trap dispatcher measures. Every other
     * mode uses ADDALLCLASSES, where the message handlers emit COMM and
     * thread-state events mid-call and the per-call cost rows would be measuring
     * the ring as much as the handler.
     */
    if (argc > 3 && !strcmp(argv[3], "kcall"))
        kcall_only = 1;
    if (argc > 4) {
        unsigned long poll_ns = strtoul(argv[4], NULL, 0);

        if (poll_ns > 0 && poll_ns < 1000000000UL)
            nap.tv_nsec = (long) poll_ns;
    }
    if (pin_to_cpu0() == -1) {
        printf("tracelogger: FAIL pin cpu0: %s\n", strerror(errno));
        return 1;
    }

    /* 1. Allocate the kernel trace ring; the kernel writes its physical address
     * into paddr. The pointer &paddr occupies alloc_cmd[2..3] on LP64.
     */
    alloc_cmd[0] = RTMUX_TRACE_ALLOCBUFFER;
    alloc_cmd[1] = NBUF;
    *(uint64_t **) &alloc_cmd[2] = &paddr;
    if (trace_event(alloc_cmd) == -1) {
        printf("tracelogger: FAIL ALLOCBUFFER: %s\n", strerror(errno));
        return 1;
    }
    if (paddr == 0) {
        puts("tracelogger: FAIL ALLOCBUFFER returned no physical address");
        return 1;
    }

    /* 3. Map the ring's physical pages into our address space. */
    ring = mmap(NULL, maplen, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                MAP_PHYS | MAP_SHARED, NOFD, (off_t) paddr);
    if (ring == MAP_FAILED) {
        printf("tracelogger: FAIL mmap ring @%#lx: %s\n", (unsigned long) paddr,
               strerror(errno));
        return 1;
    }

    /* 3. Enable the trace classes and start recording. */
    if ((kcall_only ? trace_add_class(RTMUX_TRACE_KCALLEXIT)
                    : trace_cmd(RTMUX_TRACE_ADDALLCLASSES)) == -1 ||
        trace_cmd(RTMUX_TRACE_START) == -1) {
        printf("tracelogger: FAIL start tracing: %s\n", strerror(errno));
        return 1;
    }

    /* 4. Drain by polling: force the current buffer out, nap to let the boot
     * workload generate IPC, then scan the ring for FLUSH'd buffers and read
     * the events the kernel wrote.
     */
    for (unsigned long w = 0;
         w < max_waits && (min_events == 0 || total_events < min_events); w++) {
        /* Force any partially-filled buffer out so the self-test does not
         * depend on ~1000 events accumulating to fill a whole 16 KiB buffer.
         */
        (void) trace_cmd(RTMUX_TRACE_FLUSHBUFFER);
        (void) nanosleep(&nap, NULL);
        for (int b = 0; b < NBUF; b++) {
            volatile tracebuf_t *tb = &ring[b];
            unsigned start = 0;
            unsigned n;

            if (!(tb->h.flags & _TRACE_FLAGS_FLUSH))
                continue;
            n = tb->h.num_events;
            if (tb->h.seq_buff_num == seen_seq[b])
                start = seen_events[b];
            if (start >= n) {
                tb->h.flags &= ~(uint32_t) _TRACE_FLAGS_FLUSH;
                continue;
            }
            seen_seq[b] = tb->h.seq_buff_num;
            seen_events[b] = n;
            total_events += n - start;
            for (unsigned e = start; e < n && (windowed || e < DUMP_EVENTS); e++) {
                volatile traceevent_t *ev = &tb->data[e];
                struct raw_event raw = {
                    .buf = b,
                    .ev = e,
                    .header = ev->header,
                    .data = {ev->data[0], ev->data[1], ev->data[2]},
                };
                int print = !windowed || trace_is_lkl_measure(ev->header) ||
                            trace_is_pi_boost(ev->header) || in_measure;

                if (windowed && trace_is_lkl_measure(ev->header)) {
                    unsigned event = trace_user_event(ev->header);

                    if (event == RTMUX_TRACE_USER_LKL_MEASURE_BEGIN) {
                        unsigned first = pre_head + PRE_EVENTS - pre_count;
                        for (unsigned i = 0; i < pre_count; i++)
                            print_raw_event(&pre[(first + i) % PRE_EVENTS]);
                        in_measure++;
                    } else if (event == RTMUX_TRACE_USER_LKL_MEASURE_END &&
                               in_measure > 0) {
                        in_measure--;
                    }
                }
                if (!print) {
                    if (windowed) {
                        pre[pre_head] = raw;
                        pre_head = (pre_head + 1) % PRE_EVENTS;
                        if (pre_count < PRE_EVENTS)
                            pre_count++;
                    }
                    continue;
                }
                print_raw_event(&raw);
                if (windowed) {
                    pre[pre_head] = raw;
                    pre_head = (pre_head + 1) % PRE_EVENTS;
                    if (pre_count < PRE_EVENTS)
                        pre_count++;
                }
            }
            tb->h.flags &= ~(uint32_t) _TRACE_FLAGS_FLUSH; /* release */
        }
    }

    (void) trace_cmd(RTMUX_TRACE_STOP);
    (void) trace_cmd(RTMUX_TRACE_DEALLOCBUFFER);

    if (total_events == 0) {
        puts("tracelogger: FAIL no events captured");
        return 1;
    }
    printf("tracelogger: captured %lu events\n", total_events);
    puts("tracelogger: PASS");
    return 0;
}
