/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* irq-latency - measure the hard-IRQ entry to IST-dispatch span with a real
 * IRQ.
 *
 * The kernel emits a trap-entry tracepoint when it claims a hardware interrupt
 * (_TRACE_IRQ_CLAIM in the RISC-V trap handler) and a dispatch tracepoint when
 * it makes the owning interrupt service thread ready (_TRACE_IST_DISPATCH in
 * the interrupt path), both cycle-timestamped by the CONFIG_INSTRUMENTATION
 * ring. scripts/rt-trace-irq.py pairs the two by the receiving IST process and
 * reports the span in nanoseconds (min/p50/p99/max) into a .irq.csv.
 *
 * This fixture drives that path with a genuine hardware interrupt so the span
 * is measured, not claimed. It merges two existing patterns: the tracelogger
 * ring drain (allocate, map, start, then flush and print the raw records so the
 * host parser can read them) and the irq-notify dedicated IST loop (delegate
 * the QEMU virt goldfish-rtc alarm, attach its IRQ as an IST, then repeatedly
 * arm the alarm and block in interrupt_wait). Each iteration fires one
 * IRQ_CLAIM plus one IST_DISPATCH into the ring.
 *
 * Absolute latency under QEMU/TCG is inflated by host descheduling, so the
 * numbers are reported for the relative/class comparison and the hardware
 * absolute is FU740-gated, matching the other rt-latency probes.
 */

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/siginfo.h>
#include <sys/trace.h>
#include <sys/virtio-mmio-msg.h>

#include <arch/plic-vector.h>

#include "rtc-probe.h"

#define LAT_ITERS 50
#define IRQ_PRIORITY 60

#define NBUF 16

static struct rtc_probe probe; /* shared RTC state (rtc-probe.h) */

static int rtc_delegate(unsigned long *phys, int *irq)
{
    struct rtmux_resource_handle h;
    int rc = rtmux_delegate_device(RTMUX_DEVICE_GOLDFISH_RTC, 0x1000,
                                   RTMUX_DELEGATE_ANY_BASE, &h);

    if (rc != EOK)
        return rc;
    *phys = (unsigned long) h.base;
    *irq = h.irq_vector;
    return EOK;
}

static int trace_cmd(unsigned opcode)
{
    unsigned cmd = opcode;
    return trace_event(&cmd);
}

/* INT trace class and the IST-dispatch event id, mirrored from the header
 * encoding scripts/rt-trace-irq.py decodes (class in bits 10..14, event in the
 * low 10 bits). Used to gate the PASS on real IST dispatches being captured.
 */
#define TRACE_INT_CLASS 3
#define TRACE_IST_DISPATCH_EVENT 6

/* Print one raw ring record. The "tracelogger:" prefix is the token both
 * scripts/rt-trace-irq.py and scripts/rt-trace-sched.py anchor their event
 * regex on, so it must stay verbatim; data[0] is the cycle timestamp,
 * data[1]/data[2] the event payload.
 */
static void print_raw_event(int buf,
                            unsigned e,
                            const volatile traceevent_t *ev)
{
    printf("tracelogger: buf %d ev %u header=%#x d=%#x,%#x,%#x\n", buf, e,
           ev->header, ev->data[0], ev->data[1], ev->data[2]);
}

/* Scan the mapped ring for FLUSH'd buffers, print their records, and release
 * them.
 *
 * Returns the number of records printed and, via *dispatches, how many were
 * IST-dispatch events (the ones that carry the measured span).
 */
static unsigned long drain_ring(volatile tracebuf_t *ring,
                                uint32_t *seen_seq,
                                unsigned *seen_events,
                                unsigned long *dispatches)
{
    unsigned long printed = 0;

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
        for (unsigned e = start; e < n; e++) {
            uint32_t header = tb->data[e].header;

            print_raw_event(b, e, &tb->data[e]);
            printed++;
            if (((header >> 10) & 0x1fU) == TRACE_INT_CLASS &&
                (header & 0x3ffU) == TRACE_IST_DISPATCH_EVENT)
                (*dispatches)++;
        }
        tb->h.flags &= ~(uint32_t) _TRACE_FLAGS_FLUSH;
    }
    return printed;
}

static const struct rtc_probe_ops ist_ops = {rtc_irq_prepare, rtc_ist_wait};

int main(int argc, char **argv)
{
    unsigned long phys = 0;
    int irq = -1, vector, iid;
    uint64_t paddr = 0, seed = 0, ist_p50_ns, ist_max_ns;
    uint64_t lat[LAT_ITERS];
    unsigned nok = 0;
    struct rtc_irq_ctx ic;
    volatile unsigned char *rtc;
    unsigned alloc_cmd[4];
    volatile tracebuf_t *ring;
    size_t maplen = (size_t) NBUF * _TRACEBUFSIZE;
    uint32_t seen_seq[NBUF] = {0};
    unsigned seen_events[NBUF] = {0};
    unsigned long total = 0, dispatches = 0;
    struct sched_param sp = {.sched_priority = IRQ_PRIORITY};
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--seed=", 7)) {
            seed = strtoull(argv[i] + 7, NULL, 0);
        } else {
            printf("irq-latency: usage: irq-latency [--seed=N]\n");
            return 1;
        }
    }

    /* Run at a realtime priority so the wake is not queued behind other
     * threads, and take the IO privilege the IST attach needs.
     */
    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1) {
        printf("irq-latency: FAIL sched_set: %s\n", strerror(errno));
        return 1;
    }
    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        printf("irq-latency: FAIL thread_ctl IO: %s\n", strerror(errno));
        return 1;
    }

    if (rtc_delegate(&phys, &irq) != EOK || phys == 0 || irq < 0) {
        printf("irq-latency: FAIL no delegated goldfish-rtc MMIO\n");
        return 1;
    }
    vector = PLIC_VECTOR_BASE + irq;
    rtc = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
               MAP_PHYS | MAP_SHARED, NOFD, (off_t) phys);
    if (rtc == MAP_FAILED) {
        printf("irq-latency: FAIL mmap RTC: %s\n", strerror(errno));
        return 1;
    }
    rtc_probe_init(&probe, rtc, seed, "irq-latency");
    rtc_reset(&probe);

    /* Allocate and map the kernel trace ring; the interrupt class is enabled
     * and recording started below, before the IRQ workload, so
     * IRQ_CLAIM/IST_DISPATCH land in it.
     */
    alloc_cmd[0] = RTMUX_TRACE_ALLOCBUFFER;
    alloc_cmd[1] = NBUF;
    *(uint64_t **) &alloc_cmd[2] = &paddr;
    if (trace_event(alloc_cmd) == -1 || paddr == 0) {
        printf("irq-latency: FAIL ALLOCBUFFER: %s\n", strerror(errno));
        return 1;
    }
    ring = mmap(NULL, maplen, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                MAP_PHYS | MAP_SHARED, NOFD, (off_t) paddr);
    if (ring == MAP_FAILED) {
        printf("irq-latency: FAIL mmap ring: %s\n", strerror(errno));
        return 1;
    }
    /* Trace only the interrupt class. Every class (ADDALLCLASSES) would flood
     * the ring with IPC/scheduler events across all harts and overwrite the
     * sparse IRQ_CLAIM/IST_DISPATCH records before the drain below reads them.
     */
    {
        unsigned addint[2] = {RTMUX_TRACE_ADDCLASS, RTMUX_TRACE_INT};

        if (trace_event(addint) == -1 || trace_cmd(RTMUX_TRACE_START) == -1) {
            printf("irq-latency: FAIL start tracing: %s\n", strerror(errno));
            return 1;
        }
    }

    /* Dedicated IST path: attach the goldfish IRQ, then repeatedly arm the
     * alarm and block in interrupt_wait. Each wake is one hard IRQ dispatched
     * to this IST, recorded as an IRQ_CLAIM/IST_DISPATCH pair.
     */
    iid = interrupt_attach_thread(vector, 0);
    if (iid == -1) {
        printf("irq-latency: FAIL attach_thread: %s\n", strerror(errno));
        return 1;
    }
    ic.vector = vector;
    ic.id = iid;

    /* Every attempt is one seeded alarm, one bounded wait, exactly one outcome
     * (rtc-probe.h); a bad attempt is counted, never retaken.
     */
    for (int i = 0; i < LAT_ITERS; i++) {
        struct rtc_attempt a;

        if (rtc_probe_attempt(&probe, &ist_ops, &ic, 0, "irq-latency", "ist",
                              &a) != 0) {
            printf("irq-latency: FAIL wait failed\n");
            rtc_reset(&probe);
            (void) interrupt_detach(iid);
            (void) trace_cmd(RTMUX_TRACE_STOP);
            (void) trace_cmd(RTMUX_TRACE_DEALLOCBUFFER);
            return 1;
        }
        if (a.outcome == RTC_OUTCOME_OK)
            lat[nok++] = a.wake_ns - a.alarm_ns;
    }

    (void) interrupt_detach(iid);
    rtc_reset(&probe);

    /* Flush and drain: the printed records feed rt-trace-irq.py, which pairs
     * IRQ_CLAIM with IST_DISPATCH by process and emits the .irq.csv span.
     */
    (void) trace_cmd(RTMUX_TRACE_FLUSHBUFFER);
    (void) nanosleep(&nap, NULL);
    total = drain_ring(ring, seen_seq, seen_events, &dispatches);
    (void) trace_cmd(RTMUX_TRACE_STOP);
    (void) trace_cmd(RTMUX_TRACE_DEALLOCBUFFER);

    printf("irq-latency: outcomes ok=%u early=%u timeout=%u attempts=%u\n", nok,
           probe.counts[RTC_OUTCOME_EARLY], probe.counts[RTC_OUTCOME_TIMEOUT],
           probe.attempts);
    if (nok != LAT_ITERS) {
        printf("irq-latency: FAIL early=%u timeout=%u (expected 0 and 0)\n",
               probe.counts[RTC_OUTCOME_EARLY],
               probe.counts[RTC_OUTCOME_TIMEOUT]);
        return 1;
    }
    rtc_probe_stats(lat, nok, &ist_p50_ns, &ist_max_ns);

    /* Require the IST-dispatch records the measurement actually needs, not just
     * any trace record, so a broken pairing path cannot report a false PASS.
     */
    if (dispatches < LAT_ITERS - 1) {
        printf("irq-latency: FAIL captured %lu IST dispatches, need >= %d\n",
               dispatches, LAT_ITERS - 1);
        return 1;
    }
    printf(
        "irq-latency: rtc phys=%#lx irq=%d vector=%d iters=%d seed=%llu "
        "records=%lu dispatches=%lu wake_p50_ns=%llu wake_max_ns=%llu\n",
        phys, irq, vector, LAT_ITERS, (unsigned long long) probe.seed, total,
        dispatches, (unsigned long long) ist_p50_ns,
        (unsigned long long) ist_max_ns);
    printf(
        "irq-latency: PASS hard IRQ dispatched to IST, span traced for "
        "rt-trace-irq\n");
    return 0;
}
