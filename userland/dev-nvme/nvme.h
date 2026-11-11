/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DEVB_NVME_H
#define _DEVB_NVME_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <arch/plic-vector.h>

/* Forward declaration — including <sys/dispatch.h> would pull in a lot of extra
 * I/O service API surface we don't need in this header.
 */
typedef struct _dispatch dispatch_t;

/* PCI class code for NVMe: base 0x01, sub 0x08, prog-if 0x02. */
#define NVME_PCI_CLASS 0x010802u

/* NVMe controller register offsets (NVMe base spec, rev 1.4). Doorbells start
 * at 0x1000 and are indexed by queue ID; stride is 4 << CAP.DSTRD bytes (4 for
 * our controller).
 */
#define NVME_REG_CAP 0x00        /* 64-bit Controller Capabilities   */
#define NVME_REG_VS 0x08         /* 32-bit Version                   */
#define NVME_REG_INTMS 0x0C      /* 32-bit Interrupt Mask Set        */
#define NVME_REG_INTMC 0x10      /* 32-bit Interrupt Mask Clear      */
#define NVME_REG_CC 0x14         /* 32-bit Controller Configuration  */
#define NVME_REG_CSTS 0x1C       /* 32-bit Controller Status         */
#define NVME_REG_AQA 0x24        /* 32-bit Admin Queue Attributes    */
#define NVME_REG_ASQ 0x28        /* 64-bit Admin SQ Base Address     */
#define NVME_REG_ACQ 0x30        /* 64-bit Admin CQ Base Address     */
#define NVME_REG_DBS_BASE 0x1000 /* Doorbell registers start here    */

/* CAP field extraction. */
#define NVME_CAP_MQES(c) ((uint16_t) (((c) >> 0) & 0xffff))
#define NVME_CAP_TO(c) ((uint8_t) (((c) >> 24) & 0xff)) /* 500 ms units */
#define NVME_CAP_DSTRD(c) ((uint8_t) (((c) >> 32) & 0x0f))
#define NVME_CAP_MPSMIN(c) ((uint8_t) (((c) >> 48) & 0x0f))
#define NVME_CAP_MPSMAX(c) ((uint8_t) (((c) >> 52) & 0x0f))

/* CC register bits. */
#define NVME_CC_EN (1u << 0)
#define NVME_CC_CSS_NVM (0u << 4)    /* NVM command set */
#define NVME_CC_MPS_SHIFT 7          /* memory page size (2^(12+MPS)) */
#define NVME_CC_AMS_RR (0u << 11)    /* round-robin arbitration */
#define NVME_CC_IOSQES_64 (6u << 16) /* 2^6 = 64-byte SQ entry */
#define NVME_CC_IOCQES_16 (4u << 20) /* 2^4 = 16-byte CQ entry */

/* CSTS register bits. */
#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)

/* Admin queue sizes (entries).  One page per queue is plenty. */
#define NVME_ADMIN_QDEPTH 64

/* Admin command opcodes (only the ones we use). */
#define NVME_ADMIN_OP_CREATE_IO_SQ 0x01
#define NVME_ADMIN_OP_CREATE_IO_CQ 0x05
#define NVME_ADMIN_OP_IDENTIFY 0x06

/* NVM command-set opcodes. */
#define NVME_NVM_OP_WRITE 0x01
#define NVME_NVM_OP_READ 0x02

/* CNS values for Identify. */
#define NVME_IDENT_CNS_NAMESPACE 0x00
#define NVME_IDENT_CNS_CONTROLLER 0x01
#define NVME_IDENT_CNS_ACTIVE_NS_LIST 0x02

/* I/O queue depth and the fixed queue ID we use (one pair only). */
#define NVME_IO_QDEPTH 64
#define NVME_IO_QID 1u

/* Generic Identify buffer size (NVMe spec: always 4 KiB). */
#define NVME_IDENT_BUF_SIZE 4096

/* Minimum BAR0 window defined by the spec: enough for 1 admin + 1 I/O pair. */
#define NVME_CTRL_REG_SIZE 0x2000

/* 64-byte Submission Queue Entry. Layout per NVMe base spec §4.2. We zero the
 * whole thing and fill only the fields each command needs.
 */
typedef struct __attribute__((packed)) {
    uint8_t opc;
    uint8_t fuse_psdt; /* fuse[1:0] | reserved[5:2] | psdt[7:6] */
    uint16_t cid;
    uint32_t nsid;
    uint32_t rsvd2[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

_Static_assert(sizeof(nvme_sqe_t) == 64, "nvme_sqe_t size");

/* 16-byte Completion Queue Entry. Layout per NVMe base spec §4.6. */
typedef struct __attribute__((packed)) {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* bit 0: Phase Tag; bits 15:1: Status Field */
} nvme_cqe_t;

_Static_assert(sizeof(nvme_cqe_t) == 16, "nvme_cqe_t size");

/* Minimal view of Identify Controller data (spec §5.15). We only pull out the
 * fields we actually print; the rest of the 4 KiB buffer is left opaque. Fields
 * are little-endian.
 */
typedef struct __attribute__((packed)) {
    uint16_t vid;
    uint16_t ssvid;
    char sn[20]; /* serial number, ASCII, space-padded */
    char mn[40]; /* model number */
    char fr[8];  /* firmware revision */
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint8_t pad1[172];  /* carries to offset 256 */
    uint8_t rest[3840]; /* through end of 4 KiB */
} nvme_id_ctrl_t;

_Static_assert(sizeof(nvme_id_ctrl_t) == 4096, "id_ctrl size");

/* Minimal view of Identify Namespace data (spec §5.15.2.2). */
typedef struct __attribute__((packed)) {
    uint64_t nsze; /* namespace size in LBAs */
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf; /* number of LBA formats - 1 */
    uint8_t flbas; /* formatted LBA size (index into lbaf[]) */
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t pad1[98]; /* carries to offset 128 */
    uint32_t
        lbaf[16]; /* LBA Format table (bits [23:16] = LBA data size, 2^x) */
    uint8_t rest[3904]; /* through end of 4 KiB */
} nvme_id_ns_t;

_Static_assert(sizeof(nvme_id_ns_t) == 4096, "id_ns size");

#define NVME_LBAF_LBADS(f)                                           \
    (((f) >> 16) & 0xffu)               /* LBA data size, power of 2 \
                                         */
#define NVME_LBAF_MS(f) ((f) & 0xffffu) /* metadata size */

/* Controller handle: PCI mapping, admin SQ/CQ + doorbell offsets + CID counter,
 * and the I/O queue pair.
 */
typedef struct {
    /* PCI */
    int pci_fd;
    uint8_t bus;
    uint8_t devfn;

    /* BAR0 */
    uint64_t bar0_phys;
    size_t bar0_size;
    volatile uint8_t *bar0;

    /* Controller parameters captured from CAP. */
    uint16_t mqes;
    uint8_t dstrd; /* doorbell stride log2 */
    uint8_t mpsmin;
    uint32_t doorbell_stride; /* bytes */

    /* Admin queue pair. */
    nvme_sqe_t *asq; /* SQ base (VA) */
    uint64_t asq_phys;
    uint32_t asq_tail; /* next slot to fill */

    nvme_cqe_t *acq; /* CQ base (VA) */
    uint64_t acq_phys;
    uint32_t acq_head; /* next slot to read */
    uint8_t acq_phase; /* expected Phase Tag for next completion */

    /* Monotonically increasing command-ID (per process, shared across queues).
     */
    uint16_t next_cid;

    /* I/O queue pair (a single pair at NVME_IO_QID). */
    nvme_sqe_t *iosq;
    uint64_t iosq_phys;
    uint32_t iosq_tail;

    nvme_cqe_t *iocq;
    uint64_t iocq_phys;
    uint32_t iocq_head;
    uint8_t iocq_phase;

    /* First active namespace — what /dev/nvme0n1 binds to. */
    uint32_t ns_nsid;
    uint64_t ns_lba_size;   /* bytes per LBA */
    uint64_t ns_nlba;       /* total LBAs */
    uint64_t ns_size_bytes; /* nlba * lba_size */

    /* Interrupt / IST. INTx today (one wire shared by admin + I/O queue
     * completion). MSI-X will land the admin queue on vector 0 and the I/O
     * queue on vector 1, but the gate[] bookkeeping below stays identical.
     *
     * ist_ready is the "mode switch": exec_cmd polls while it's 0
     * (controller-bringup path, before the IST exists) and waits on gate[qid]
     * once it's 1.
     */
    uint32_t irq;    /* raw PLIC IRQ from the PCI server */
    uint32_t vector; /* PLIC_VECTOR_BASE + irq */
    volatile int
        iid; /* interrupt_attach_thread id; -2 while IST is attaching */
    pthread_t ist_tid;
    volatile int ist_ready;

    /* Per-queue completion gate. Submitter clears done, rings the SQ doorbell,
     * pthread_cond_wait()s. IST drains the CQ, copies status/cid, sets done,
     * signals. One outstanding command per queue is all we issue today.
     */
    struct {
        pthread_mutex_t mtx;
        pthread_cond_t cv;
        int done;
        uint16_t status; /* SF from CQE, 0 on success */
        uint16_t cid;    /* matching command ID */
    } gate[2];           /* [0] = admin, [1] = I/O */
} nvme_ctrl_t;

/* Doorbell register offsets. */
static inline uint32_t nvme_sq_dbl(const nvme_ctrl_t *c, uint32_t qid)
{
    return NVME_REG_DBS_BASE + (2u * qid) * c->doorbell_stride;
}
static inline uint32_t nvme_cq_dbl(const nvme_ctrl_t *c, uint32_t qid)
{
    return NVME_REG_DBS_BASE + (2u * qid + 1u) * c->doorbell_stride;
}

/* MMIO helpers — 32-bit is the basic unit; 64-bit writes are split. */
static inline uint32_t nvme_rd32(const nvme_ctrl_t *c, uint32_t off)
{
    return *(volatile uint32_t *) (c->bar0 + off);
}
static inline void nvme_wr32(const nvme_ctrl_t *c, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *) (c->bar0 + off) = v;
}
static inline uint64_t nvme_rd64(const nvme_ctrl_t *c, uint32_t off)
{
    uint32_t lo = *(volatile uint32_t *) (c->bar0 + off);
    uint32_t hi = *(volatile uint32_t *) (c->bar0 + off + 4);
    return ((uint64_t) hi << 32) | lo;
}
static inline void nvme_wr64(const nvme_ctrl_t *c, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *) (c->bar0 + off) = (uint32_t) v;
    *(volatile uint32_t *) (c->bar0 + off + 4) = (uint32_t) (v >> 32);
}

/* Prototypes implemented in nvme-ctrl.c / nvme-admin.c / nvme-io.c. */
int nvme_reset(nvme_ctrl_t *c);
int nvme_enable(nvme_ctrl_t *c);
int nvme_setup_admin_queues(nvme_ctrl_t *c);
void nvme_teardown_admin_queues(nvme_ctrl_t *c);

/* Submit one SQE on queue <qid> (0 = admin) and poll its CQ for the matching
 * completion. Assigns cmd->cid; returns the 15-bit Status Field (0 on success)
 * or -1 on timeout / malformed reply.
 */
int nvme_exec_cmd(nvme_ctrl_t *c, uint32_t qid, nvme_sqe_t *cmd);

int nvme_identify_controller(nvme_ctrl_t *c, nvme_id_ctrl_t *out);
int nvme_identify_active_ns_list(nvme_ctrl_t *c, uint32_t *nsids /* [1024] */);
int nvme_identify_namespace(nvme_ctrl_t *c, uint32_t nsid, nvme_id_ns_t *out);

int nvme_create_io_queues(nvme_ctrl_t *c);
void nvme_teardown_io_queues(nvme_ctrl_t *c);

/* Read <nblocks> LBAs starting at <slba> from <nsid> into <buf>. <buf> must be
 * page-aligned; caller passes a single-page MAP_ANON region so PRP2 stays zero.
 * Named nvme_cmd_read rather than nvme_read so it doesn't collide with
 * file-static I/O service handlers.
 */
int nvme_cmd_read(nvme_ctrl_t *c,
                  uint32_t nsid,
                  uint64_t slba,
                  uint16_t nblocks,
                  void *buf);
int nvme_cmd_write(nvme_ctrl_t *c,
                   uint32_t nsid,
                   uint64_t slba,
                   uint16_t nblocks,
                   void *buf);

/* Register /dev/nvme0n1 on the caller-supplied dispatch handle and enter the
 * dispatch loop. The dispatch MUST have been created before any /dev/pci
 * activity in this process, otherwise ioserv_attach fails with a stale errno.
 * Uses c's ns_nsid / ns_lba_size / ns_nlba fields; never returns.
 */
int nvme_ioserv_run(nvme_ctrl_t *c, dispatch_t *dpp);

/* Start the IST thread. Initializes gate[]'s mutex/cond, attaches the IST to
 * c->vector via interrupt_attach_thread, and starts the thread body that drains
 * admin + I/O CQs on each wake. Must be called after nvme_create_io_queues() so
 * iocq_phase is set.
 */
int nvme_ist_start(nvme_ctrl_t *c);

#endif /* _DEVB_NVME_H */
