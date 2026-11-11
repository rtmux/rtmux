/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * setup.
 *
 * The sequence is lifted almost verbatim from NVMe base spec §7.6.1,
 * "Initialization". We pick minimum-viable parameters: one 64-entry admin queue
 * pair, NVM command set, memory page size = MPSMIN reported by the controller.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

#include "nvme.h"

/* Spin until (nvme_rd32(off) & mask) == want, or until CAP.TO expires (timeout
 * units are 500 ms per the spec).
 *
 * Returns 0 on success, -1 on timeout.
 */
static int wait_csts_bit(nvme_ctrl_t *c,
                         uint32_t mask,
                         uint32_t want,
                         unsigned timeout_units)
{
    struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = 1000000 /* 1 ms */};
    unsigned total_ms = timeout_units * 500u;

    for (unsigned i = 0; i < total_ms; i++) {
        uint32_t csts = nvme_rd32(c, NVME_REG_CSTS);
        if ((csts & mask) == want)
            return 0;
        if (csts & NVME_CSTS_CFS) {
            fprintf(stderr, "nvme: controller fatal status (CSTS=0x%x)\n",
                    csts);
            return -1;
        }
        nanosleep(&sleep_ts, NULL);
    }
    fprintf(stderr, "nvme: timeout waiting for CSTS mask=0x%x want=0x%x\n",
            mask, want);
    return -1;
}

/* Disable the controller and wait for it to report not-ready.
 *
 * Clears CC.EN and spins on CSTS.RDY going to 0 within CAP.TO. Parameters:
 *   c : controller instance
 * Return value: 0 on success, -1 on timeout or fatal status.
 */
int nvme_reset(nvme_ctrl_t *c)
{
    uint64_t cap = nvme_rd64(c, NVME_REG_CAP);
    uint32_t cc = nvme_rd32(c, NVME_REG_CC);
    unsigned to = NVME_CAP_TO(cap);
    if (to == 0)
        to = 1; /* avoid a zero-length wait if CAP.TO reports 0 */

    /* If already disabled, still wait for RDY to observe 0. */
    nvme_wr32(c, NVME_REG_CC, cc & ~NVME_CC_EN);
    return wait_csts_bit(c, NVME_CSTS_RDY, 0, to);
}

/* Allocate the admin submission/completion queues and program the controller's
 * AQA/ASQ/ACQ registers to point at them.
 *
 * Parameters:
 *   c : controller instance (queue VAs/PAs and indices are filled in)
 * Return value: 0 on success, -1 on allocation or mapping failure.
 */
int nvme_setup_admin_queues(nvme_ctrl_t *c)
{
    off_t pa;

    /* Each queue gets a full 4 KiB page even though 64 × 16 = 1 KiB would be
     * enough for the CQ. MAP_ANON on a single page is guaranteed physically
     * contiguous under RTMux's no-lazy-alloc policy, and a page is the natural
     * unit to hand to the controller via ASQ/ACQ.
     */
    c->asq = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                  MAP_ANON | MAP_SHARED, NOFD, 0);
    if (c->asq == MAP_FAILED) {
        fprintf(stderr, "nvme: mmap ASQ: %s\n", strerror(errno));
        return -1;
    }
    if (mem_offset(c->asq, NOFD, 4096, &pa, NULL) == -1) {
        fprintf(stderr, "nvme: mem_offset ASQ: %s\n", strerror(errno));
        return -1;
    }
    c->asq_phys = (uint64_t) pa;

    c->acq = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                  MAP_ANON | MAP_SHARED, NOFD, 0);
    if (c->acq == MAP_FAILED) {
        fprintf(stderr, "nvme: mmap ACQ: %s\n", strerror(errno));
        return -1;
    }
    if (mem_offset(c->acq, NOFD, 4096, &pa, NULL) == -1) {
        fprintf(stderr, "nvme: mem_offset ACQ: %s\n", strerror(errno));
        return -1;
    }
    c->acq_phys = (uint64_t) pa;

    /* Zero both queues — CQ entries' Phase Tag starts at 0, so the first
     * completion (written as phase=1) is distinguishable.
     */
    memset(c->asq, 0, 4096);
    memset(c->acq, 0, 4096);

    c->asq_tail = 0;
    c->acq_head = 0;
    c->acq_phase = 1;
    c->next_cid = 0;

    /* Program AQA: both queue sizes are (QDEPTH - 1). */
    uint32_t aqa =
        ((NVME_ADMIN_QDEPTH - 1u) << 16) | ((NVME_ADMIN_QDEPTH - 1u) << 0);
    nvme_wr32(c, NVME_REG_AQA, aqa);
    nvme_wr64(c, NVME_REG_ASQ, c->asq_phys);
    nvme_wr64(c, NVME_REG_ACQ, c->acq_phys);

    /* The admin completion queue is host-read-only for its whole operational
     * life: the driver only reads CQEs and rings the CQ-head doorbell in BAR0
     * MMIO, while the controller DMA-writes completions. Drop host write
     * permission now, after the one-time zero above, so a stray CPU write into
     * the completion ring faults instead of silently corrupting completion
     * state. Keep PROT_NOCACHE so the mapping's cache attribute is unchanged.
     * Device DMA bypasses the CPU page table, so this guards against host bugs,
     * not the controller. The admin submission queue stays writable because the
     * driver writes command entries into it. If a future reset path re-zeroes
     * this page in place it must first mprotect it writable again or
     * reallocate. Failure is fatal: the isolation is the point, so a controller
     * that cannot lock its completion queue does not come up. The caller frees
     * the queues.
     */
    if (mprotect(c->acq, 4096, PROT_READ | PROT_NOCACHE) == -1) {
        fprintf(stderr, "nvme: mprotect ACQ read-only: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* Release the admin queue DMA pages.
 *
 * Parameters:
 *   c : controller instance
 */
void nvme_teardown_admin_queues(nvme_ctrl_t *c)
{
    if (c->asq && c->asq != MAP_FAILED)
        munmap(c->asq, 4096);
    if (c->acq && c->acq != MAP_FAILED)
        munmap(c->acq, 4096);
    c->asq = NULL;
    c->acq = NULL;
}

/* 30-second poll cap on any single command — comfortably above NVMe 1.4's
 * worst-case administrative timings, and fine for the short synchronous I/O
 * commands we issue.
 */
#define NVME_CMD_POLL_US (30u * 1000u * 1000u)

/* Submit one command on the given queue and wait for its completion.
 *
 * Assigns a CID, places the SQE, rings the doorbell, then either waits on the
 * per-queue gate (interrupt-driven, once the IST is up) or polls the CQ (during
 * bringup). At most one command per queue is outstanding at a time. Parameters:
 *   c   : controller instance
 *   qid : queue id (0 = admin, NVME_IO_QID = I/O)
 *   cmd : command to submit (cid is overwritten)
 * Return value: NVMe status field (>= 0) on completion, -1 on
 *   timeout or CID mismatch.
 */
int nvme_exec_cmd(nvme_ctrl_t *c, uint32_t qid, nvme_sqe_t *cmd)
{
    uint16_t cid = c->next_cid++;

    /* Pick the SQ and its tail pointer. */
    nvme_sqe_t *sq;
    uint32_t *tail;
    uint32_t qdepth;
    unsigned gi;

    if (qid == 0) {
        sq = c->asq;
        tail = &c->asq_tail;
        qdepth = NVME_ADMIN_QDEPTH;
        gi = 0;
    } else {
        sq = c->iosq;
        tail = &c->iosq_tail;
        qdepth = NVME_IO_QDEPTH;
        gi = 1;
    }

    /* Arm the gate BEFORE the doorbell so the IST can't signal completion into
     * a stale "done" state.
     */
    if (c->ist_ready) {
        pthread_mutex_lock(&c->gate[gi].mtx);
        c->gate[gi].done = 0;
        pthread_mutex_unlock(&c->gate[gi].mtx);
    }

    cmd->cid = cid;
    sq[*tail] = *cmd;
    *tail = (*tail + 1u) % qdepth;

    /* Make sure the SQ-entry stores commit BEFORE the doorbell store. Without
     * this, RISC-V may reorder the regular-memory writes (SQ entry in DRAM)
     * past the MMIO doorbell write — the controller then sees the doorbell,
     * reads a stale SQ slot, and silently does nothing. No CQE, no IRQ, IST
     * never wakes, main hangs in cond_wait.
     */
    __asm__ __volatile__("fence w,o" ::: "memory");
    nvme_wr32(c, nvme_sq_dbl(c, qid), *tail);

    if (c->ist_ready) {
        /* Interrupt-driven wait: IST drains the CQ and signals. */
        pthread_mutex_lock(&c->gate[gi].mtx);
        while (!c->gate[gi].done)
            pthread_cond_wait(&c->gate[gi].cv, &c->gate[gi].mtx);
        uint16_t done_cid = c->gate[gi].cid;
        uint16_t sf = c->gate[gi].status;
        pthread_mutex_unlock(&c->gate[gi].mtx);

        if (done_cid != cid) {
            fprintf(stderr, "nvme: qid=%u cqe cid mismatch (got %u want %u)\n",
                    qid, done_cid, cid);
            return -1;
        }
        return (int) sf;
    }

    /* Polling fallback — used during bringup before nvme_ist_start(). The rw,rw
     * fence orders the phase-bit check against the CQE-tail reads (volatile
     * alone is insufficient on RISC-V).
     */
    nvme_cqe_t *cq = (qid == 0) ? c->acq : c->iocq;
    uint32_t *head = (qid == 0) ? &c->acq_head : &c->iocq_head;
    uint8_t *phase = (qid == 0) ? &c->acq_phase : &c->iocq_phase;
    struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = 1000 /* 1 us */};

    for (unsigned us = 0; us < NVME_CMD_POLL_US; us++) {
        volatile nvme_cqe_t *cqe = &cq[*head];
        uint16_t status = cqe->status;
        if ((status & 1u) == *phase) {
            __asm__ __volatile__("fence rw,rw" ::: "memory");
            uint16_t done_cid = cqe->cid;
            uint16_t sf = (uint16_t) (status >> 1);

            *head = (*head + 1u) % qdepth;
            if (*head == 0)
                *phase ^= 1u;
            nvme_wr32(c, nvme_cq_dbl(c, qid), *head);

            if (done_cid != cid) {
                fprintf(stderr,
                        "nvme: qid=%u cqe cid mismatch (got %u want %u)\n", qid,
                        done_cid, cid);
                return -1;
            }
            return (int) (sf & 0x7FFFu);
        }
        nanosleep(&sleep_ts, NULL);
    }

    fprintf(stderr, "nvme: qid=%u command cid=%u timed out\n", qid, cid);
    return -1;
}

/* Program CC and wait for the controller to report ready.
 *
 * Parameters:
 *   c : controller instance
 * Return value: 0 on success, -1 on timeout or fatal status.
 */
int nvme_enable(nvme_ctrl_t *c)
{
    uint64_t cap = nvme_rd64(c, NVME_REG_CAP);
    unsigned to = NVME_CAP_TO(cap);
    uint8_t mpsmin = NVME_CAP_MPSMIN(cap);
    if (to == 0)
        to = 1;

    /* Program CC:
     *   CSS   = NVM command set (0)
     *   MPS   = MPSMIN (host page size = 2^(12+MPSMIN)); on our
     *           targets MPSMIN is 0 (4 KiB), matching our allocations.
     *   AMS   = round-robin
     *   IOSQES/IOCQES = native entry sizes (6, 4)
     *   EN    = 1
     */
    uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM |
                  ((uint32_t) mpsmin << NVME_CC_MPS_SHIFT) | NVME_CC_AMS_RR |
                  NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    nvme_wr32(c, NVME_REG_CC, cc);

    return wait_csts_bit(c, NVME_CSTS_RDY, NVME_CSTS_RDY, to);
}
