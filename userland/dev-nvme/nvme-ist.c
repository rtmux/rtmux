/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * INTx today: one wire carries both admin-CQ and I/O-CQ completions, and we
 * drain both CQs on every wake. The submitter side uses a per-queue
 * mutex/condvar gate (c->gate[0] for admin, c->gate[1] for I/O) and issues at
 * most one command per queue at a time, so the IST side just latches the most
 * recent CQE into the matching gate and signals. MSI-X will split admin onto
 * vector 0 and I/O onto vector 1; gate[] bookkeeping stays the same.
 *
 * A full hardware fence is required after the phase-bit check: on RISC-V, plain
 * loads of the CQE tail fields are NOT ordered with the phase-bit read even
 * with volatile, and __atomic_thread_fence isn't enough either.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <sys/rtmux-core.h>

#include "nvme.h"

#ifdef CONFIG_MEASURE_IST_DRAIN
#define IST_DRAIN_BUCKETS 32        /* log2-ns histogram width */
#define IST_DRAIN_REPORT_EVERY 1024 /* snapshot cadence, in samples */

static unsigned ist_drain_ilog2(uint64_t v)
{
    unsigned b = 0;

    while (v > 1) {
        v >>= 1;
        b++;
    }
    return b;
}

/* Accumulate the drain-before-unmask span into a log2-ns histogram and print a
 * running snapshot (samples, exact max, histogram-derived p99, and the nonzero
 * buckets) every IST_DRAIN_REPORT_EVERY samples. The IST loops forever, so the
 * measurement is a periodic snapshot rather than a single end-of-run report; a
 * drive-exercising workload drives the sample count up and the last snapshot is
 * the one to record. Measurement build only. A static mutex guards the shared
 * stats against the rare multi-controller case; the print runs after the sample
 * is recorded, so it never inflates the span it just measured.
 */
static void nvme_ist_drain_measure(uint64_t start, uint64_t end)
{
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t hist[IST_DRAIN_BUCKETS];
    static uint64_t count;
    static uint64_t max_ns;
    uint64_t delta = end - start;
    unsigned b = ist_drain_ilog2(delta);

    if (b >= IST_DRAIN_BUCKETS)
        b = IST_DRAIN_BUCKETS - 1;

    pthread_mutex_lock(&mtx);
    hist[b]++;
    count++;
    if (delta > max_ns)
        max_ns = delta;
    if (count % IST_DRAIN_REPORT_EVERY == 0) {
        uint64_t target = count - count / 100; /* rank of the 99th percentile */
        uint64_t acc = 0;
        unsigned p99b = 0;

        for (unsigned i = 0; i < IST_DRAIN_BUCKETS; i++) {
            acc += hist[i];
            if (acc >= target) {
                p99b = i;
                break;
            }
        }
        fprintf(stderr,
                "dev-nvme: ist-drain samples=%llu max_ns=%llu p99_ns=%llu\n",
                (unsigned long long) count, (unsigned long long) max_ns,
                (unsigned long long) (1ull << (p99b + 1)));
        for (unsigned i = 0; i < IST_DRAIN_BUCKETS; i++) {
            if (hist[i])
                fprintf(stderr,
                        "dev-nvme: ist-drain hist 2^%u..2^%u ns: %llu\n", i,
                        i + 1, (unsigned long long) hist[i]);
        }
    }
    pthread_mutex_unlock(&mtx);
}
#endif

/* Drain one CQ. If a new completion is present, advance the head, ring the CQ
 * doorbell, latch (status, cid) into the matching gate and signal.
 *
 * Returns the number of CQEs consumed (0 or 1 in the
 * one-outstanding-command-per-queue model).
 */
static unsigned drain_cq(nvme_ctrl_t *c,
                         uint32_t qid,
                         nvme_cqe_t *cq,
                         uint32_t *head,
                         uint8_t *phase,
                         uint32_t qdepth)
{
    volatile nvme_cqe_t *cqe = &cq[*head];
    uint16_t status = cqe->status;
    if ((status & 1u) != *phase)
        return 0;

    /* Full hardware fence between the phase-bit check and the CQE-tail /
     * payload reads.
     */

#ifdef __aarch64__
    __asm__ volatile("dmb osh" ::: "memory");
#else
    __asm__ __volatile__("fence rw,rw" ::: "memory");
#endif

    uint16_t done_cid = cqe->cid;
    uint16_t sf = (uint16_t) (status >> 1);

    *head = (*head + 1u) % qdepth;
    if (*head == 0)
        *phase ^= 1u;
    nvme_wr32(c, nvme_cq_dbl(c, qid), *head);

    unsigned gi = qid == 0 ? 0u : 1u;
    pthread_mutex_lock(&c->gate[gi].mtx);
    c->gate[gi].status = (uint16_t) (sf & 0x7FFFu);
    c->gate[gi].cid = done_cid;
    c->gate[gi].done = 1;
    pthread_cond_signal(&c->gate[gi].cv);
    pthread_mutex_unlock(&c->gate[gi].mtx);

    return 1;
}

/* Interrupt service thread.
 *
 * Attaches to the controller's INTx vector, then loops: wait for the interrupt,
 * drain both CQs until quiet, unmask. See docs/irq-contract.md for the hard-IRQ
 * versus IST split this follows. Parameters:
 *   arg : controller instance (nvme_ctrl_t *)
 * Return value: NULL (only reached on attach failure).
 */
static void *ist_thread(void *arg)
{
    nvme_ctrl_t *c = arg;

    /* Match serial ISTs: elevate above worker priority so the IST preempts the
     * dispatch thread when the device fires.
     */
    {
        struct sched_param sp = {.sched_priority = 21};
        (void) __sched_set(0, 0, SCHED_RR, &sp);
    }

    thread_ctl(RTMUX_TCTL_IO, 0);
    c->iid = interrupt_attach_thread((int) c->vector, 0);
    if (c->iid == -1) {
        fprintf(stderr,
                "dev-nvme: interrupt_attach_thread(vector=%u) failed: %s\n",
                c->vector, strerror(errno));
        return NULL;
    }

    for (;;) {
        interrupt_wait(0, NULL);
#ifdef CONFIG_MEASURE_IST_DRAIN
        uint64_t drain_start = 0;
        uint64_t drain_end = 0;
        int drain_timed =
            __clock_time(CLOCK_MONOTONIC, NULL, &drain_start) == 0;
#endif

        /* Drain both CQs — INTx is shared. Loop until both are quiet so a burst
         * of completions doesn't leave entries behind between unmask/wait
         * cycles.
         */
        unsigned drained;
        do {
            drained = drain_cq(c, 0, c->acq, &c->acq_head, &c->acq_phase,
                               NVME_ADMIN_QDEPTH);
            drained += drain_cq(c, NVME_IO_QID, c->iocq, &c->iocq_head,
                                &c->iocq_phase, NVME_IO_QDEPTH);
        } while (drained > 0);
        __interrupt_unmask((int) c->vector, c->iid);
#ifdef CONFIG_MEASURE_IST_DRAIN
        if (drain_timed &&
            __clock_time(CLOCK_MONOTONIC, NULL, &drain_end) == 0) {
            nvme_ist_drain_measure(drain_start, drain_end);
        }
#endif
    }
    return NULL;
}

/* Initialize the per-queue gates and spawn the interrupt service thread.
 *
 * Parameters:
 *   c : controller instance
 * Return value: 0 on success, -1 on gate-init or thread-create failure.
 */
int nvme_ist_start(nvme_ctrl_t *c)
{
    enum { NVME_IST_ATTACHING = -2 };

    for (unsigned i = 0; i < 2; i++) {
        if (pthread_mutex_init(&c->gate[i].mtx, NULL) != 0 ||
            pthread_cond_init(&c->gate[i].cv, NULL) != 0) {
            fprintf(stderr, "dev-nvme: gate[%u] init failed\n", i);
            return -1;
        }
        c->gate[i].done = 0;
    }
    c->iid = NVME_IST_ATTACHING;
    {
        int rc = pthread_create(&c->ist_tid, NULL, ist_thread, c);
        if (rc != 0) {
            fprintf(
                stderr,
                "dev-nvme: pthread_create(ist) failed: rc=%d (%s) errno=%d\n",
                rc, strerror(rc), errno);
            return -1;
        }
    }
    for (unsigned spins = 0; c->iid == NVME_IST_ATTACHING; spins++) {
        if (spins >= 100000u) {
            fprintf(stderr, "dev-nvme: IST attach timed out\n");
            return -1;
        }
        sched_yield();
    }
    if (c->iid == -1)
        return -1;
    return 0;
}
