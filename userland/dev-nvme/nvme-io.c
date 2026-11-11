/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * command.
 *
 * The driver runs with exactly one I/O queue pair (qid = 1), shared by any
 * future dispatch threads. The Create-IO-CQ / Create-IO-SQ admin commands
 * allocate the queue on the controller side; the matching host-side rings live
 * in MAP_ANON'd DMA-visible pages.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

#include "nvme.h"

/* Allocate one 4 KiB DMA-visible page and return its VA / PA. A page is enough
 * for either NVME_IO_QDEPTH × 64 B SQ entries or NVME_IO_QDEPTH × 16 B CQ
 * entries.
 */
static void *alloc_queue_page(uint64_t *phys_out, const char *what)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                   MAP_ANON | MAP_SHARED, NOFD, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "nvme: mmap %s: %s\n", what, strerror(errno));
        return NULL;
    }

    /* A DMA ring must sit at a page boundary so its whole extent is one PRP
     * entry and one contiguous physical page. mmap already guarantees page
     * alignment; check it so a broken VM surfaces here rather than as silent
     * controller corruption.
     */
    if (((uintptr_t) p & 0xfffu) != 0) {
        fprintf(stderr, "nvme: %s not page-aligned: %p\n", what, p);
        munmap(p, 4096);
        return NULL;
    }
    off_t pa;
    if (mem_offset(p, NOFD, 4096, &pa, NULL) == -1) {
        fprintf(stderr, "nvme: mem_offset %s: %s\n", what, strerror(errno));
        munmap(p, 4096);
        return NULL;
    }
    memset(p, 0, 4096);
    *phys_out = (uint64_t) pa;
    return p;
}

/* Create the single I/O queue pair (CQ then SQ).
 *
 * Allocates the host-side rings, then issues Create-IO-CQ and Create-IO-SQ
 * admin commands. The CQ must be created first so the SQ can reference a live
 * CQ. Parameters:
 *   c : controller instance
 * Return value: 0 on success, -1 on allocation or command failure.
 */
int nvme_create_io_queues(nvme_ctrl_t *c)
{
    /* Allocate host-side rings first. */
    c->iocq = alloc_queue_page(&c->iocq_phys, "IOCQ");
    if (!c->iocq)
        return -1;
    c->iosq = alloc_queue_page(&c->iosq_phys, "IOSQ");
    if (!c->iosq) {
        munmap(c->iocq, 4096);
        c->iocq = NULL;
        return -1;
    }
    c->iocq_head = 0;
    c->iocq_phase = 1;
    c->iosq_tail = 0;

    nvme_sqe_t cmd;
    int sf;

    /* Create I/O CQ (admin opcode 0x05).
     *   CDW10[31:16] = QSIZE-1 (0-based)
     *   CDW10[15:0]  = QID
     *   CDW11[31:16] = IV (interrupt vector — 0 for the single INTx
     *                      we use today; still 0 when MSI-X lands,
     *                      since the admin queue sits on vector 0 and
     *                      the I/O queue on vector 1 in that future)
     *   CDW11[1]     = IEN (interrupt enable)
     *   CDW11[0]     = PC  (physically contiguous)
     * Must come before Create SQ so the SQ can reference a live CQ.
     */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = NVME_ADMIN_OP_CREATE_IO_CQ;
    cmd.prp1 = c->iocq_phys;
    cmd.cdw10 = ((NVME_IO_QDEPTH - 1u) << 16) | NVME_IO_QID;
    cmd.cdw11 = 0x3u; /* IV=0, IEN=1, PC=1 */
    sf = nvme_exec_cmd(c, 0, &cmd);
    if (sf != 0) {
        fprintf(stderr, "nvme: Create IO CQ failed, sf=0x%x\n", (unsigned) sf);
        nvme_teardown_io_queues(c);
        return -1;
    }

    /* Create I/O SQ (admin opcode 0x01).
     *   CDW11[31:16] = CQID (which CQ this SQ's completions go to)
     *   CDW11[2:1]   = QPRIO (ignored when CC.AMS = round-robin)
     *   CDW11[0]     = PC
     */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = NVME_ADMIN_OP_CREATE_IO_SQ;
    cmd.prp1 = c->iosq_phys;
    cmd.cdw10 = ((NVME_IO_QDEPTH - 1u) << 16) | NVME_IO_QID;
    cmd.cdw11 = (NVME_IO_QID << 16) | 0x1u; /* CQID=IO_QID, PC=1 */
    sf = nvme_exec_cmd(c, 0, &cmd);
    if (sf != 0) {
        fprintf(stderr, "nvme: Create IO SQ failed, sf=0x%x\n", (unsigned) sf);
        nvme_teardown_io_queues(c);
        return -1;
    }

    /* The I/O completion queue, like the admin one, is host-read-only once
     * created: the IST reads CQEs and rings the CQ-head doorbell, the
     * controller DMA-writes completions, and nothing writes the ring after the
     * zero in alloc_queue_page. Drop host write permission, keeping
     * PROT_NOCACHE. The I/O submission queue stays writable for command
     * entries. See the ACQ comment in nvme_setup_admin_queues for the reset
     * caveat. Failure is fatal for the same reason as the ACQ: the isolation is
     * the point.
     */
    if (mprotect(c->iocq, 4096, PROT_READ | PROT_NOCACHE) == -1) {
        fprintf(stderr, "nvme: mprotect IOCQ read-only: %s\n", strerror(errno));
        nvme_teardown_io_queues(c);
        return -1;
    }

    return 0;
}

/* Release the I/O queue host-side DMA pages.
 *
 * Parameters:
 *   c : controller instance
 */
void nvme_teardown_io_queues(nvme_ctrl_t *c)
{
    /* NVMe Delete-IO-SQ / Delete-IO-CQ are the proper cleanup, but on process
     * exit the controller will also be reset by the next driver to attach, so
     * skipping them is safe. Just release the host-side DMA pages.
     */
    if (c->iosq && c->iosq != MAP_FAILED)
        munmap(c->iosq, 4096);
    if (c->iocq && c->iocq != MAP_FAILED)
        munmap(c->iocq, 4096);
    c->iosq = NULL;
    c->iocq = NULL;
}

/* Issue a single NVM Read into a page-aligned single-page buffer.
 *
 * Parameters:
 *   c       : controller instance
 *   nsid    : namespace id
 *   slba    : starting logical block address
 *   nblocks : number of logical blocks to read
 *   buf     : page-aligned destination (single PRP page)
 * Return value: 0 on success, -1 on submission failure or non-zero
 *   completion status.
 */
int nvme_cmd_read(nvme_ctrl_t *c,
                  uint32_t nsid,
                  uint64_t slba,
                  uint16_t nblocks,
                  void *buf)
{
    /* Caller guarantees <buf> is a page-aligned single-page region. That means
     * PRP1 covers the whole transfer and PRP2 stays zero, which in turn caps
     * nblocks × LBA-size at 4 KiB.
     */
    off_t pa;
    if (mem_offset(buf, NOFD, 4096, &pa, NULL) == -1) {
        fprintf(stderr, "nvme: mem_offset read buf: %s\n", strerror(errno));
        return -1;
    }

    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = NVME_NVM_OP_READ;
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t) pa;
    cmd.cdw10 = (uint32_t) (slba & 0xFFFFFFFFu);
    cmd.cdw11 = (uint32_t) (slba >> 32);
    cmd.cdw12 = (uint32_t) (nblocks - 1u); /* NLB is 0-based */

    int sf = nvme_exec_cmd(c, NVME_IO_QID, &cmd);
    if (sf < 0)
        return -1;
    if (sf != 0) {
        fprintf(stderr, "nvme: Read nsid=%u slba=%lu returned status 0x%x\n",
                nsid, (unsigned long) slba, (unsigned) sf);
        return -1;
    }
    return 0;
}

int nvme_cmd_write(nvme_ctrl_t *c,
                   uint32_t nsid,
                   uint64_t slba,
                   uint16_t nblocks,
                   void *buf)
{
    off_t pa;
    nvme_sqe_t cmd;
    int sf;

    if (mem_offset(buf, NOFD, 4096, &pa, NULL) == -1) {
        fprintf(stderr, "nvme: mem_offset write buf: %s\n", strerror(errno));
        return -1;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.opc = NVME_NVM_OP_WRITE;
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t) pa;
    cmd.cdw10 = (uint32_t) (slba & 0xFFFFFFFFu);
    cmd.cdw11 = (uint32_t) (slba >> 32);
    cmd.cdw12 = (uint32_t) (nblocks - 1u);

    sf = nvme_exec_cmd(c, NVME_IO_QID, &cmd);
    if (sf < 0)
        return -1;
    if (sf != 0) {
        fprintf(stderr, "nvme: Write nsid=%u slba=%lu returned status 0x%x\n",
                nsid, (unsigned long) slba, (unsigned) sf);
        return -1;
    }
    return 0;
}
