/*
 * RTMux delegated virtio-mmio transport helpers for LKL domains.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/riscv-iommu.h>
#include <sys/rtmux-core.h>
#include <sys/virtio-mmio-msg.h>
#include <time.h>
#include <unistd.h>

#include <align.h>

#include "domain.h"
#include "virtio.h"

#include <arch/plic-vector.h>

#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c
#define VIRTIO_MMIO_QUEUE_PFN 0x040
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4
#define VIRTIO_DELEGATE_MAP_RETRIES 200
#define VIRTIO_IOMMU_PT_PAGES 160U
#define VIRTIO_IOMMU_PT_BYTES (VIRTIO_IOMMU_PT_PAGES * 4096U)
static bool virtio_mmio_delegated;

struct virtio_mmio_candidate {
    uint64_t paddr;
    size_t size;
    uint32_t iommu_phandle;
    uint32_t iommu_stream;
    int irq_vector;
    bool claimed;
};

struct virtio_iommu_pt {
    void *base;
    off_t pa;
    size_t root_off;
    unsigned used_pages;
};

static uint64_t mmio_read64(volatile void *base, size_t off)
{
    return *(volatile uint64_t *) ((volatile char *) base + off);
}

static uint32_t mmio_read32(volatile void *base, size_t off)
{
    return *(volatile uint32_t *) ((volatile char *) base + off);
}

static void mmio_write64(volatile void *base, size_t off, uint64_t value)
{
    *(volatile uint64_t *) ((volatile char *) base + off) = value;
}

static void mmio_write32(volatile void *base, size_t off, uint32_t value)
{
    *(volatile uint32_t *) ((volatile char *) base + off) = value;
}

static void virtio_mmio_release_candidates(struct virtio_mmio_candidate *cand,
                                           unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (cand[i].claimed)
            rtmux_hwres_release_range(cand[i].paddr, cand[i].size,
                                      RTMUX_HWRES_MEMORY);
    }
}

static int virtio_mmio_query(uint32_t device_id,
                             struct virtio_mmio_candidate *cand,
                             unsigned *out_count)
{
    struct virtio_mmio_message msg;
    uint32_t count;
    int fd;
    int rc;

    *out_count = 0;
    /* ipc-send: lkl-netd,lkl-fsd -> virtio-mmio */
    fd = open(VIRTIO_MMIO_SERVICE_PATH, O_RDWR);
    if (fd == -1)
        return errno ? errno : EIO;
    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_QUERY;
    msg.msg.delegate.device_id = device_id;
    msg.msg.delegate.max_handles = VIRTIO_MMIO_CAND_MAX;
    /* ipc-send: lkl-netd,lkl-fsd -> virtio-mmio */
    rc = msg_send(fd, &msg, sizeof(msg), &msg, sizeof(msg));
    close(fd);
    if (rc == -1)
        return errno ? errno : EIO;
    if (msg.rep_hdr.reply_status != EOK)
        return (int) msg.rep_hdr.reply_status;
    count = msg.rep_hdr.reply_count;
    if (count == 0 || count > VIRTIO_MMIO_CAND_MAX)
        return ENODEV;
    for (uint32_t i = 0; i < count; i++) {
        if (msg.handles[i].kind != RTMUX_RESOURCE_MMIO ||
            (msg.handles[i].flags & RTMUX_RESOURCE_HANDLE_F_REVOKED) != 0 ||
            msg.handles[i].owner_pid != getpid() ||
            msg.handles[i].device_id != device_id ||
            msg.handles[i].generation == 0 ||
            msg.handles[i].base > (uint64_t) INT64_MAX ||
            msg.handles[i].length < 0x200 ||
            msg.handles[i].length > VIRTIO_MMIO_MAP_MAX ||
            msg.handles[i].irq_vector < 0 ||
            msg.handles[i].irq_vector > INT_MAX - PLIC_VECTOR_BASE)
            return EINVAL;
        cand[i].paddr = msg.handles[i].base;
        cand[i].size = msg.handles[i].length;
        cand[i].iommu_phandle = msg.handles[i].iommu_phandle;
        cand[i].iommu_stream = msg.handles[i].reserved;
        cand[i].irq_vector = PLIC_VECTOR_BASE + msg.handles[i].irq_vector;
        cand[i].claimed = true;
    }
    *out_count = count;
    return EOK;
}

uint32_t virtio_mmio_rd32(struct rtmux_virtio_mmio *vio, unsigned off)
{
    return *(volatile uint32_t *) (vio->base + off);
}

void virtio_mmio_wr32(struct rtmux_virtio_mmio *vio, unsigned off, uint32_t val)
{
    *(volatile uint32_t *) (vio->base + off) = val;
}

void virtio_mb(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* virtio_mb() is fence rw,rw: it orders normal memory, not MMIO (on RISC-V it
 * does not cover the I/O space). These helpers are scoped to the init handshake
 * and queue control, including kick. Reset and hardware completion ordering are
 * not covered. They order CPU-issued accesses only; no posted-write completion
 * is implied.
 *   w,o: staged ring/descriptor stores before an MMIO write that lets the
 *        device fetch them (Linux __io_bw).
 *   o,o: earlier MMIO writes before later MMIO writes (SEL->NUM, config->
 *        PFN/READY, READY->next SEL/STATUS, STATUS->first NOTIFY).
 *   before_sel (io,o): earlier MMIO reads/writes before a QUEUE_SEL write, so
 * the previous queue's NUM_MAX read/config/READY cannot land on the new queue.
 *   o,i: QUEUE_SEL write before the QUEUE_NUM_MAX read.
 */
void virtio_fence_w_o(void)
{
#if defined(__riscv)
    __asm__ volatile("fence w,o" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

void virtio_fence_o_o(void)
{
#if defined(__riscv)
    __asm__ volatile("fence o,o" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

void virtio_io_fence_before_sel(void)
{
#if defined(__riscv)
    __asm__ volatile("fence io,o" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

void virtio_io_fence_oi(void)
{
#if defined(__riscv)
    __asm__ volatile("fence o,i" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

void virtio_mmio_set_delegated(bool enabled)
{
    virtio_mmio_delegated = enabled;
}

static int virtio_mmio_map_claimed(struct rtmux_virtio_mmio *vio,
                                   struct virtio_mmio_candidate *cand)
{
    volatile uint8_t *base;

    if (vio->base || vio->claimed)
        return EBUSY;
    if (!cand->claimed)
        return EINVAL;
    base = mmap(NULL, cand->size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                MAP_PHYS | MAP_SHARED, NOFD, (off_t) cand->paddr);
    if (base == MAP_FAILED) {
        return errno ? errno : EIO;
    }
    vio->base = base;
    vio->size = cand->size;
    vio->paddr = cand->paddr;
    vio->iommu_phandle = cand->iommu_phandle;
    vio->iommu_stream = cand->iommu_stream;
    vio->irq_vector = cand->irq_vector;
    vio->claimed = true;
    cand->claimed = false;
    return EOK;
}

static int virtio_iommu_wait_cqcsr(volatile void *mmio,
                                   uint32_t mask,
                                   uint32_t value,
                                   uint32_t *out)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint32_t cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);

        if ((cqcsr & RISCV_IOMMU_CQCSR_ERR) != 0) {
            *out = cqcsr;
            return EIO;
        }
        if ((cqcsr & mask) == value) {
            *out = cqcsr;
            return EOK;
        }
    }
    *out = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
    return ETIMEDOUT;
}

static int virtio_iommu_wait_cqh(volatile void *mmio,
                                 uint32_t value,
                                 uint32_t *out_cqh,
                                 uint32_t *out_cqcsr)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint32_t cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
        uint32_t cqh = mmio_read32(mmio, RISCV_IOMMU_CQH);

        if ((cqcsr & RISCV_IOMMU_CQCSR_ERR) != 0) {
            *out_cqh = cqh;
            *out_cqcsr = cqcsr;
            return EIO;
        }
        if (cqh == value) {
            *out_cqh = cqh;
            *out_cqcsr = cqcsr;
            return EOK;
        }
    }
    *out_cqh = mmio_read32(mmio, RISCV_IOMMU_CQH);
    *out_cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
    return ETIMEDOUT;
}

static int virtio_iommu_submit(volatile void *mmio,
                               volatile struct riscv_iommu_cmd *cq,
                               uint32_t *tail,
                               struct riscv_iommu_cmd cmd,
                               uint32_t *out_cqh,
                               uint32_t *out_cqcsr)
{
    uint32_t next = (*tail + 1U) & 1U;

    cq[*tail] = cmd;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mmio_write32(mmio, RISCV_IOMMU_CQT, next);
    int rc = virtio_iommu_wait_cqh(mmio, next, out_cqh, out_cqcsr);

    if (rc != EOK)
        return rc;
    *tail = next;
    return EOK;
}

static int virtio_iommu_wait_ddtp(volatile void *mmio, uint64_t *out)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint64_t ddtp = mmio_read64(mmio, RISCV_IOMMU_DDTP);

        if ((ddtp & RISCV_IOMMU_DDTP_BUSY) == 0) {
            *out = ddtp;
            return EOK;
        }
    }
    *out = mmio_read64(mmio, RISCV_IOMMU_DDTP);
    return ETIMEDOUT;
}

static int virtio_iommu_set_ddtp(volatile void *mmio,
                                 uint64_t ppn,
                                 unsigned mode,
                                 uint64_t *out)
{
    uint64_t ddtp;
    int rc = virtio_iommu_wait_ddtp(mmio, &ddtp);

    if (rc != EOK)
        goto fail;
    uint64_t value = (ppn << 10) | mode;

    if (mode == RISCV_IOMMU_DDTP_MODE_OFF)
        value = (ddtp & RISCV_IOMMU_DDTP_PPN_MASK) | mode;
    mmio_write64(mmio, RISCV_IOMMU_DDTP, value);
    rc = virtio_iommu_wait_ddtp(mmio, out);
    if (rc != EOK)
        goto fail;
    return EOK;

fail:
    *out = mmio_read64(mmio, RISCV_IOMMU_DDTP);
    return rc;
}

static int virtio_iommu_restore_ddtp(volatile void *mmio, unsigned mode)
{
    uint64_t ddtp;
    int rc = virtio_iommu_set_ddtp(mmio, 0, RISCV_IOMMU_DDTP_MODE_OFF, &ddtp);

    if (rc != EOK)
        return rc;
    if (mode != RISCV_IOMMU_DDTP_MODE_OFF) {
        rc = virtio_iommu_set_ddtp(mmio, 0, mode, &ddtp);
        if (rc != EOK)
            return rc;
    }
    return ((ddtp & RISCV_IOMMU_MODE_MASK) == mode) ? EOK : EIO;
}

static int virtio_iommu_debug_translate(volatile void *mmio,
                                        uint32_t did,
                                        uint64_t iova,
                                        uint64_t *out_ctl,
                                        uint64_t *out_response)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint64_t ctl = mmio_read64(mmio, RISCV_IOMMU_TR_REQ_CTL);

        if ((ctl & RISCV_IOMMU_TR_REQ_GO) == 0)
            break;
        if (i == 99999U) {
            *out_ctl = ctl;
            *out_response = mmio_read64(mmio, RISCV_IOMMU_TR_RESPONSE);
            return ETIMEDOUT;
        }
    }
    mmio_write64(mmio, RISCV_IOMMU_TR_REQ_IOVA, iova);
    mmio_write64(mmio, RISCV_IOMMU_TR_REQ_CTL,
                 RISCV_IOMMU_TR_REQ_GO | RISCV_IOMMU_TR_REQ_NW |
                     ((uint64_t) did << RISCV_IOMMU_DID_SHIFT));
    for (unsigned i = 0; i < 100000U; i++) {
        uint64_t ctl = mmio_read64(mmio, RISCV_IOMMU_TR_REQ_CTL);

        if ((ctl & RISCV_IOMMU_TR_REQ_GO) == 0) {
            *out_ctl = ctl;
            *out_response = mmio_read64(mmio, RISCV_IOMMU_TR_RESPONSE);
            return EOK;
        }
    }
    *out_ctl = mmio_read64(mmio, RISCV_IOMMU_TR_REQ_CTL);
    *out_response = mmio_read64(mmio, RISCV_IOMMU_TR_RESPONSE);
    return ETIMEDOUT;
}

static volatile uint64_t *virtio_iommu_pt_page(struct virtio_iommu_pt *pt,
                                               uint64_t pa)
{
    return (volatile uint64_t *) ((char *) pt->base + (pa - (uint64_t) pt->pa));
}

static int virtio_iommu_alloc_pt_page(struct virtio_iommu_pt *pt,
                                      volatile uint64_t **out,
                                      uint64_t *out_pa)
{
    if (pt->used_pages >= VIRTIO_IOMMU_PT_PAGES)
        return ENOMEM;
    *out = (volatile uint64_t *) ((char *) pt->base + pt->used_pages * 4096U);
    *out_pa = (uint64_t) pt->pa + pt->used_pages * 4096ULL;
    memset((void *) *out, 0, 4096);
    pt->used_pages++;
    return EOK;
}

static int virtio_iommu_map_page(struct virtio_iommu_pt *pt,
                                 uint64_t iova,
                                 uint64_t pa)
{
    volatile uint64_t *root =
        (volatile uint64_t *) ((char *) pt->base + pt->root_off);
    unsigned root_index = (unsigned) ((iova >> 30) & 0x7ffU);
    unsigned l1_index = (unsigned) ((iova >> 21) & 0x1ffU);
    unsigned l0_index = (unsigned) ((iova >> 12) & 0x1ffU);
    uint64_t l1_pa;
    uint64_t l0_pa;
    volatile uint64_t *l1;
    volatile uint64_t *l0;
    int rc;

    if ((root[root_index] & RISCV_IOMMU_PTE_V) == 0) {
        rc = virtio_iommu_alloc_pt_page(pt, &l1, &l1_pa);
        if (rc != EOK)
            return rc;
        root[root_index] = riscv_iommu_pte(l1_pa, RISCV_IOMMU_PTE_V);
    } else {
        l1_pa = ((root[root_index] & RISCV_IOMMU_TR_RESPONSE_PPN_MASK) >> 10)
                << 12;
        l1 = virtio_iommu_pt_page(pt, l1_pa);
    }
    if ((l1[l1_index] & RISCV_IOMMU_PTE_V) == 0) {
        rc = virtio_iommu_alloc_pt_page(pt, &l0, &l0_pa);
        if (rc != EOK)
            return rc;
        l1[l1_index] = riscv_iommu_pte(l0_pa, RISCV_IOMMU_PTE_V);
    } else {
        l0_pa = ((l1[l1_index] & RISCV_IOMMU_TR_RESPONSE_PPN_MASK) >> 10) << 12;
        l0 = virtio_iommu_pt_page(pt, l0_pa);
    }
    l0[l0_index] = riscv_iommu_pte(
        pa, RISCV_IOMMU_PTE_V | RISCV_IOMMU_PTE_R | RISCV_IOMMU_PTE_W |
                RISCV_IOMMU_PTE_U | RISCV_IOMMU_PTE_A | RISCV_IOMMU_PTE_D);
    return EOK;
}

static bool virtio_iommu_iova_in_ranges(
    const struct rtmux_virtio_dma_range *ranges,
    unsigned count,
    uint64_t iova)
{
    for (unsigned i = 0; i < count; i++) {
        uint64_t start = ranges[i].paddr & ~0xfffULL;
        uint64_t end =
            (ranges[i].paddr + ranges[i].size + 0xfffULL) & ~0xfffULL;

        if (iova >= start && iova < end)
            return true;
    }
    return false;
}

static int virtio_iommu_pick_denied_iova(
    const struct rtmux_virtio_dma_range *ranges,
    unsigned count,
    uint64_t *out)
{
    uint64_t iova = (ranges[0].paddr + ranges[0].size + 0xfffULL) & ~0xfffULL;

    for (unsigned i = 0; i < 1024U; i++, iova += 4096U) {
        if (!virtio_iommu_iova_in_ranges(ranges, count, iova)) {
            *out = iova;
            return EOK;
        }
    }
    return ENOSPC;
}

static int virtio_mmio_probe_iommu_domain(
    struct rtmux_virtio_mmio *vio,
    const struct rtmux_virtio_dma_range *ranges,
    unsigned count,
    size_t bytes)
{
    struct virtio_mmio_candidate cand[VIRTIO_MMIO_CAND_MAX];
    unsigned cand_count = 0;
    volatile void *mmio = MAP_FAILED;
    void *cq_map = MAP_FAILED;
    void *ddt_map = MAP_FAILED;
    void *pt_map = MAP_FAILED;
    volatile struct riscv_iommu_cmd *cq = NULL;
    volatile struct riscv_iommu_dc *dc = NULL;
    struct virtio_iommu_pt pt = {0};
    size_t mmio_size = 0;
    uint64_t mmio_paddr = 0;
    int mmio_irq = -1;
    uint64_t capabilities;
    uint64_t ddtp;
    uint64_t probed;
    uint64_t tr_ctl;
    uint64_t tr_response;
    uint64_t denied_response;
    uint64_t deny_iova;
    off_t cq_pa;
    off_t ddt_pa;
    off_t pt_pa;
    size_t cq_contig;
    size_t ddt_contig;
    size_t pt_contig;
    uint32_t cqh;
    uint32_t cqcsr;
    uint32_t cq_tail = 0;
    uint32_t did = RTMUX_RESOURCE_IOMMU_STREAM_DECODE(vio->iommu_stream);
    unsigned mode;
    bool cq_enabled = false;
    bool ddt_programmed = false;
    int rc;

    if (did >= RISCV_IOMMU_DDT_BYTES / sizeof(struct riscv_iommu_dc))
        return ERANGE;
    memset(cand, 0, sizeof(cand));
    rc = virtio_mmio_query(RTMUX_DEVICE_RISCV_IOMMU, cand, &cand_count);
    if (rc != EOK)
        return rc;
    for (unsigned i = 0; i < cand_count; i++) {
        mmio = mmap(NULL, cand[i].size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                    MAP_PHYS | MAP_SHARED, NOFD, (off_t) cand[i].paddr);
        if (mmio == MAP_FAILED) {
            rc = errno ? errno : EIO;
            goto out;
        }
        mmio_size = cand[i].size;
        mmio_paddr = cand[i].paddr;
        mmio_irq = cand[i].irq_vector;
        break;
    }
    if (mmio == MAP_FAILED) {
        rc = ENODEV;
        goto out;
    }

    capabilities = mmio_read64(mmio, RISCV_IOMMU_CAPABILITIES);
    ddtp = mmio_read64(mmio, RISCV_IOMMU_DDTP);
    mode = (unsigned) (ddtp & RISCV_IOMMU_MODE_MASK);
    fprintf(stderr,
            "lkl: system IOMMU MMIO @ %#lx irq %d caps=0x%lx ddtp=0x%lx "
            "mode=%u delegated\n",
            (unsigned long) mmio_paddr, mmio_irq, (unsigned long) capabilities,
            (unsigned long) ddtp, mode);
    if ((unsigned) (capabilities & 0xffU) != RISCV_IOMMU_VERSION_1_0 ||
        mode > RISCV_IOMMU_DDTP_MODE_BARE ||
        (capabilities & (RISCV_IOMMU_CAP_DBG | RISCV_IOMMU_CAP_SV39X4)) !=
            (RISCV_IOMMU_CAP_DBG | RISCV_IOMMU_CAP_SV39X4)) {
        fprintf(stderr,
                "lkl: virtio-mmio IOMMU domain prerequisites failed "
                "caps=0x%lx mode=%u\n",
                (unsigned long) capabilities, mode);
        rc = ENODEV;
        goto out;
    }
    for (unsigned i = 0; i < count; i++)
        fprintf(stderr,
                "lkl: virtio-mmio IOMMU domain range %u paddr=%#lx size=%#lx\n",
                i, (unsigned long) ranges[i].paddr,
                (unsigned long) ranges[i].size);
    fprintf(stderr,
            "lkl: virtio-mmio IOMMU domain allowlist ranges=%u bytes=%lu\n",
            count, (unsigned long) bytes);

    ddt_map =
        mmap(NULL, RISCV_IOMMU_DDT_BYTES, PROT_READ | PROT_WRITE | PROT_NOCACHE,
             MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (ddt_map == MAP_FAILED) {
        rc = errno ? errno : EIO;
        goto out;
    }
    if (mem_offset(ddt_map, NOFD, RISCV_IOMMU_DDT_BYTES, &ddt_pa,
                   &ddt_contig) == -1 ||
        ddt_contig < RISCV_IOMMU_DDT_BYTES ||
        ((uint64_t) ddt_pa & (RISCV_IOMMU_DDT_BYTES - 1U)) != 0) {
        rc = EINVAL;
        goto out;
    }
    memset(ddt_map, 0, RISCV_IOMMU_DDT_BYTES);

    cq_map =
        mmap(NULL, RISCV_IOMMU_CQ_BYTES, PROT_READ | PROT_WRITE | PROT_NOCACHE,
             MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (cq_map == MAP_FAILED) {
        rc = errno ? errno : EIO;
        goto out;
    }
    if (mem_offset(cq_map, NOFD, RISCV_IOMMU_CQ_BYTES, &cq_pa, &cq_contig) ==
            -1 ||
        cq_contig < RISCV_IOMMU_CQ_BYTES ||
        ((uint64_t) cq_pa & (RISCV_IOMMU_CQ_BYTES - 1U)) != 0) {
        rc = EINVAL;
        goto out;
    }
    memset(cq_map, 0, RISCV_IOMMU_CQ_BYTES);

    pt_map =
        mmap(NULL, VIRTIO_IOMMU_PT_BYTES, PROT_READ | PROT_WRITE | PROT_NOCACHE,
             MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (pt_map == MAP_FAILED) {
        rc = errno ? errno : EIO;
        goto out;
    }
    if (mem_offset(pt_map, NOFD, VIRTIO_IOMMU_PT_BYTES, &pt_pa, &pt_contig) ==
            -1 ||
        pt_contig < VIRTIO_IOMMU_PT_BYTES || ((uint64_t) pt_pa & 0xfffU) != 0) {
        rc = EINVAL;
        goto out;
    }
    memset(pt_map, 0, VIRTIO_IOMMU_PT_BYTES);
    pt = (struct virtio_iommu_pt){
        .base = pt_map,
        .pa = pt_pa,
        .root_off = (size_t) (((uint64_t) pt_pa + RISCV_IOMMU_ROOT_BYTES - 1U) &
                              ~(uint64_t) (RISCV_IOMMU_ROOT_BYTES - 1U)) -
                    (size_t) pt_pa,
        .used_pages = 0,
    };
    pt.used_pages = (unsigned) ((pt.root_off + RISCV_IOMMU_ROOT_BYTES) / 4096U);

    for (unsigned i = 0; i < count; i++) {
        uint64_t start = ranges[i].paddr & ~0xfffULL;
        uint64_t end =
            (ranges[i].paddr + ranges[i].size + 0xfffULL) & ~0xfffULL;

        for (uint64_t pa = start; pa < end; pa += 4096U) {
            rc = virtio_iommu_map_page(&pt, pa, pa);
            if (rc != EOK)
                goto out;
        }
    }
    rc = virtio_iommu_pick_denied_iova(ranges, count, &deny_iova);
    if (rc != EOK)
        goto out;

    cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
    if ((cqcsr & RISCV_IOMMU_CQCSR_BUSY) != 0) {
        rc = EBUSY;
        goto out;
    }
    mmio_write64(mmio, RISCV_IOMMU_CQB, ((uint64_t) cq_pa >> 12) << 10);
    mmio_write32(mmio, RISCV_IOMMU_CQT, 0);
    mmio_write32(mmio, RISCV_IOMMU_CQCSR, RISCV_IOMMU_CQCSR_CQEN);
    cq_enabled = true;
    rc = virtio_iommu_wait_cqcsr(mmio, RISCV_IOMMU_CQCSR_CQON,
                                 RISCV_IOMMU_CQCSR_CQON, &cqcsr);
    if (rc != EOK)
        goto out;
    cqh = mmio_read32(mmio, RISCV_IOMMU_CQH);
    if (cqh != 0) {
        rc = EIO;
        goto out;
    }

    cq = cq_map;
    dc = ddt_map;
    dc[did] = (struct riscv_iommu_dc){
        .tc = RISCV_IOMMU_DC_TC_V,
        .iohgatp =
            (RISCV_IOMMU_IOHGATP_MODE_SV39X4 << 60) |
            ((((uint64_t) pt_pa + pt.root_off) >> 12) & ((1ULL << 44) - 1ULL)),
    };
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rc = virtio_iommu_set_ddtp(mmio, (uint64_t) ddt_pa >> 12,
                               RISCV_IOMMU_DDTP_MODE_1LVL, &probed);
    ddt_programmed = rc == EOK;
    if (rc != EOK ||
        (probed & RISCV_IOMMU_MODE_MASK) != RISCV_IOMMU_DDTP_MODE_1LVL)
        goto out;

    struct riscv_iommu_cmd iodir = riscv_iommu_iodir_inval_ddt_cmd(did);
    struct riscv_iommu_cmd iofence = riscv_iommu_iofence_cmd();

    rc = virtio_iommu_submit(mmio, cq, &cq_tail, iodir, &cqh, &cqcsr);
    if (rc != EOK)
        goto out;
    rc = virtio_iommu_submit(mmio, cq, &cq_tail, iofence, &cqh, &cqcsr);
    if (rc != EOK)
        goto out;
    rc = virtio_iommu_debug_translate(mmio, did, ranges[0].paddr, &tr_ctl,
                                      &tr_response);
    if (rc != EOK || (tr_response & RISCV_IOMMU_TR_RESPONSE_FAULT) != 0 ||
        (tr_response & RISCV_IOMMU_TR_RESPONSE_PPN_MASK) !=
            ((ranges[0].paddr >> 12) << 10)) {
        rc = EIO;
        goto out;
    }
    rc = virtio_iommu_debug_translate(mmio, did, deny_iova, &tr_ctl,
                                      &denied_response);
    if (rc != EOK || (denied_response & RISCV_IOMMU_TR_RESPONSE_FAULT) == 0) {
        rc = EIO;
        goto out;
    }
    fprintf(stderr,
            "lkl: virtio-mmio IOMMU domain programmed stream=%u "
            "allow=%#lx deny=%#lx\n",
            did, (unsigned long) tr_response, (unsigned long) denied_response);
    rc = EOK;

out:
    if (ddt_programmed && mmio != MAP_FAILED) {
        if (dc)
            dc[did] = (struct riscv_iommu_dc){0};
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (cq_enabled && cq) {
            (void) virtio_iommu_submit(mmio, cq, &cq_tail,
                                       riscv_iommu_iodir_inval_ddt_cmd(did),
                                       &cqh, &cqcsr);
            (void) virtio_iommu_submit(mmio, cq, &cq_tail,
                                       riscv_iommu_iofence_cmd(), &cqh, &cqcsr);
        }
        if (virtio_iommu_restore_ddtp(mmio, mode) == EOK) {
            ddt_programmed = false;
            fprintf(stderr, "lkl: virtio-mmio IOMMU domain cleared stream=%u\n",
                    did);
        } else {
            rc = EIO;
        }
    }
    if (cq_enabled && mmio != MAP_FAILED) {
        uint32_t final_cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);

        if ((final_cqcsr & RISCV_IOMMU_CQCSR_BUSY) == 0 ||
            virtio_iommu_wait_cqcsr(mmio, RISCV_IOMMU_CQCSR_BUSY, 0,
                                    &final_cqcsr) == EOK) {
            mmio_write32(mmio, RISCV_IOMMU_CQCSR, 0);
            (void) virtio_iommu_wait_cqcsr(mmio, RISCV_IOMMU_CQCSR_CQON, 0,
                                           &final_cqcsr);
        }
    }
    if (pt_map != MAP_FAILED)
        munmap(pt_map, VIRTIO_IOMMU_PT_BYTES);
    if (cq_map != MAP_FAILED)
        munmap(cq_map, RISCV_IOMMU_CQ_BYTES);
    if (ddt_map != MAP_FAILED)
        munmap(ddt_map, RISCV_IOMMU_DDT_BYTES);
    if (mmio != MAP_FAILED)
        munmap((void *) mmio, mmio_size);
    virtio_mmio_release_candidates(cand, cand_count);
    return rc;
}

void virtio_mmio_unmap(struct rtmux_virtio_mmio *vio)
{
    if (vio->base)
        munmap((void *) vio->base, vio->size);
    if (vio->claimed)
        rtmux_hwres_release_range(vio->paddr, vio->size, RTMUX_HWRES_MEMORY);
    memset(vio, 0, sizeof(*vio));
}

int virtio_mmio_discover(struct rtmux_virtio_mmio *vio,
                         uint32_t device_id,
                         int *version)
{
    struct virtio_mmio_candidate cand[VIRTIO_MMIO_CAND_MAX];
    unsigned count = 0;
    int rc;

    if (!version)
        return -EINVAL;
    memset(cand, 0, sizeof(cand));
    if (!virtio_mmio_delegated) {
        fputs("lkl: virtio-mmio requires delegated hwres\n", stderr);
        return -ENODEV;
    }
    for (unsigned retry = 0;; retry++) {
        rc = virtio_mmio_query(device_id, cand, &count);
        if (rc == EOK || rc != ENODEV || retry == VIRTIO_DELEGATE_MAP_RETRIES)
            break;
        (void) nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000},
                         NULL);
    }
    if (rc != EOK)
        return -rc;
    if (rtmux_phys_contain() != EOK) {
        int error = errno ? errno : EIO;

        virtio_mmio_release_candidates(cand, count);
        return -error;
    }
    for (unsigned i = 0; i < count; i++) {
        int error;

        for (unsigned retry = 0;; retry++) {
            error = virtio_mmio_map_claimed(vio, &cand[i]);
            if (!virtio_mmio_delegated || error != EPERM ||
                retry == VIRTIO_DELEGATE_MAP_RETRIES)
                break;
            (void) nanosleep(
                &(struct timespec){.tv_sec = 0, .tv_nsec = 10000000}, NULL);
        }

        if (error != EOK) {
            fprintf(stderr, "lkl: virtio-mmio map @ %#lx size %#lx: %s\n",
                    (unsigned long) cand[i].paddr, (unsigned long) cand[i].size,
                    strerror(error));
            continue;
        }
        fprintf(stderr,
                "lkl: virtio-mmio @ %#lx magic=%#x version=%u device=%u\n",
                (unsigned long) cand[i].paddr,
                virtio_mmio_rd32(vio, VIRTIO_MMIO_MAGIC_VALUE),
                virtio_mmio_rd32(vio, VIRTIO_MMIO_VERSION),
                virtio_mmio_rd32(vio, VIRTIO_MMIO_DEVICE_ID));
        if (virtio_mmio_rd32(vio, VIRTIO_MMIO_MAGIC_VALUE) ==
                VIRTIO_MMIO_MAGIC &&
            virtio_mmio_rd32(vio, VIRTIO_MMIO_DEVICE_ID) == device_id) {
            *version = (int) virtio_mmio_rd32(vio, VIRTIO_MMIO_VERSION);
            if (*version != 1 && *version != 2) {
                virtio_mmio_unmap(vio);
                virtio_mmio_release_candidates(cand, count);
                return -ENODEV;
            }
            virtio_mmio_release_candidates(cand, count);
            return 0;
        }
        virtio_mmio_unmap(vio);
    }
    virtio_mmio_release_candidates(cand, count);
    return -ENODEV;
}

int virtio_mmio_require_iommu_domain(
    struct rtmux_virtio_mmio *vio,
    const struct rtmux_virtio_dma_range *ranges,
    unsigned count)
{
    size_t bytes = 0;
    int rc;

    if (!vio || !ranges || count == 0)
        return EINVAL;
    if (vio->iommu_stream == RTMUX_RESOURCE_IOMMU_STREAM_NONE)
        return EOK;
    for (unsigned i = 0; i < count; i++) {
        if (ranges[i].size == 0 || bytes > SIZE_MAX - ranges[i].size)
            return EINVAL;
        uint64_t size_last = (uint64_t) ranges[i].size - 1U;

        if (ranges[i].paddr > UINT64_MAX - size_last)
            return EINVAL;
        uint64_t last = ranges[i].paddr + size_last;

        if (last > UINT64_MAX - 0xfffULL)
            return EINVAL;
        bytes += ranges[i].size;
    }
    fprintf(stderr, "lkl: virtio-mmio IOMMU phandle=%u stream=%u\n",
            vio->iommu_phandle,
            RTMUX_RESOURCE_IOMMU_STREAM_DECODE(vio->iommu_stream));
    rc = virtio_mmio_probe_iommu_domain(vio, ranges, count, bytes);
    if (rc != EOK) {
        fprintf(stderr, "lkl: virtio-mmio IOMMU domain probe failed: %s\n",
                strerror(rc));
        return rc;
    }
    fputs("lkl: virtio-mmio IOMMU domain enforcement unavailable\n", stderr);
    return ENOTSUP;
}

static void *virtio_dma_alloc(size_t size, uint64_t *pa_out)
{
    void *p;
    off_t pa;
    size_t contig;

    if (size == 0 || size > VIRTIO_DMA_ALLOC_MAX)
        return NULL;

    /* virtio descriptor rings and buffers must be physically contiguous;
     * MAP_PHYS forces that, and the contig check below rejects the mapping if
     * the allocator still handed back a fragmented multi-page region.
     */
    p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
             MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (mem_offset(p, NOFD, size, &pa, &contig) == -1 || contig < size) {
        munmap(p, size);
        return NULL;
    }
    memset(p, 0, size);
    rtmux_lkl_host_stats_dma_add(size);
    *pa_out = (uint64_t) pa;
    return p;
}

static void virtio_dma_free(void *p, size_t size)
{
    if (p && p != MAP_FAILED) {
        munmap(p, size);
        rtmux_lkl_host_stats_dma_sub(size);
    }
}

int virtio_dma_pool_alloc(struct rtmux_virtio_dma_pool *pool,
                          unsigned count,
                          size_t size)
{
    if (size == 0 || size > VIRTIO_DMA_ALLOC_MAX)
        return EINVAL;
    if (count > VIRTIO_QUEUE_MAX_SIZE || count > VIRTIO_DMA_POOL_MAX / size)
        return EINVAL;
    memset(pool, 0, sizeof(*pool));
    pool->count = count;
    pool->size = size;
    for (unsigned i = 0; i < count; i++) {
        pool->buf[i].addr = virtio_dma_alloc(size, &pool->buf[i].paddr);
        if (!pool->buf[i].addr) {
            for (unsigned j = 0; j < i; j++) {
                virtio_dma_free(pool->buf[j].addr, size);
                pool->buf[j].addr = NULL;
                pool->buf[j].paddr = 0;
            }
            memset(pool, 0, sizeof(*pool));
            return ENOMEM;
        }
    }
    return EOK;
}

void virtio_dma_pool_free(struct rtmux_virtio_dma_pool *pool)
{
    for (unsigned i = 0; i < pool->count; i++)
        virtio_dma_free(pool->buf[i].addr, pool->size);
    memset(pool, 0, sizeof(*pool));
}

struct rtmux_virtio_queue *virtio_queue_alloc(struct rtmux_virtio_mmio *vio,
                                              int version,
                                              unsigned qid,
                                              unsigned max_entries)
{
    struct rtmux_virtio_queue *q = calloc(1, sizeof(*q));
    size_t avail_off;
    size_t used_off;
    uint32_t max;

    if (!q)
        return NULL;
    if (max_entries == 0 || max_entries > VIRTIO_QUEUE_MAX_SIZE) {
        free(q);
        return NULL;
    }
    q->qid = qid;
    virtio_io_fence_before_sel();
    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_SEL, qid);
    virtio_io_fence_oi();
    max = virtio_mmio_rd32(vio, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0) {
        free(q);
        return NULL;
    }
    q->num = max < max_entries ? max : max_entries;
    if ((q->num & (q->num - 1U)) != 0 || q->num > UINT16_MAX) {
        free(q);
        return NULL;
    }
    if (version == 1) {
        avail_off = sizeof(struct vring_desc) * q->num;
        used_off = lib_align_up_size(
            avail_off + sizeof(uint16_t) * (3U + q->num), 4096);
        q->mem_size =
            lib_align_up_size(used_off + sizeof(uint16_t) * 3U +
                                  sizeof(struct vring_used_elem) * q->num,
                              4096);
        q->mem = virtio_dma_alloc(q->mem_size, &q->mem_pa);
        if (!q->mem) {
            free(q);
            return NULL;
        }
        q->desc = q->mem;
        q->avail = (struct vring_avail *) ((uint8_t *) q->mem + avail_off);
        q->used = (struct vring_used *) ((uint8_t *) q->mem + used_off);
    } else {
        q->desc = virtio_dma_alloc(4096, &q->desc_pa);
        q->avail = virtio_dma_alloc(4096, &q->avail_pa);
        q->used = virtio_dma_alloc(4096, &q->used_pa);
        if (!q->desc || !q->avail || !q->used) {
            virtio_dma_free(q->desc, 4096);
            virtio_dma_free(q->avail, 4096);
            virtio_dma_free(q->used, 4096);
            free(q);
            return NULL;
        }
        if ((q->desc_pa & 0xfU) != 0 || (q->avail_pa & 0x1U) != 0 ||
            (q->used_pa & 0x3U) != 0) {
            virtio_dma_free(q->desc, 4096);
            virtio_dma_free(q->avail, 4096);
            virtio_dma_free(q->used, 4096);
            free(q);
            return NULL;
        }
    }

    if (version == 1 && (q->mem_pa >> 12) > UINT32_MAX) {
        virtio_dma_free(q->mem, q->mem_size);
        free(q);
        return NULL;
    }
    return q;
}

/* Program queue addresses and mark the queue live. Kept apart from allocation
 * so the caller can authorize the DMA ranges first; QUEUE_SEL is re-written
 * because a later alloc may have selected another queue. Cannot fail. The
 * leading virtio_fence_w_o() orders all prior ring/descriptor stores before the
 * first MMIO write that exposes the queue (QUEUE_PFN / QUEUE_READY); the o,o
 * fences order SEL -> config -> PFN/READY -> later MMIO writes.
 */
void virtio_queue_publish(struct rtmux_virtio_mmio *vio,
                          int version,
                          struct rtmux_virtio_queue *q)
{
    virtio_fence_w_o();
    virtio_io_fence_before_sel();
    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_SEL, q->qid);
    virtio_fence_o_o();
    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_NUM, q->num);
    if (version == 1) {
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_ALIGN, 4096);
        virtio_fence_o_o();
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_PFN,
                         (uint32_t) (q->mem_pa >> 12));
    } else {
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_DESC_LOW,
                         (uint32_t) q->desc_pa);
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_DESC_HIGH,
                         (uint32_t) (q->desc_pa >> 32));
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_DRIVER_LOW,
                         (uint32_t) q->avail_pa);
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
                         (uint32_t) (q->avail_pa >> 32));
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_DEVICE_LOW,
                         (uint32_t) q->used_pa);
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
                         (uint32_t) (q->used_pa >> 32));
        virtio_fence_o_o();
        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);
    }

    /* Publication of this queue precedes every later MMIO write (the next
     * queue's QUEUE_SEL, NOTIFY, STATUS/DRIVER_OK).
     */
    virtio_fence_o_o();
}

void virtio_queue_free(struct rtmux_virtio_queue *q)
{
    if (!q)
        return;
    if (q->mem) {
        virtio_dma_free(q->mem, q->mem_size);
    } else {
        virtio_dma_free(q->desc, 4096);
        virtio_dma_free(q->avail, 4096);
        virtio_dma_free(q->used, 4096);
    }
    free(q);
}

void virtio_queue_kick(struct rtmux_virtio_mmio *vio,
                       struct rtmux_virtio_queue *q)
{
    /* The memory barrier does not order stores before MMIO on RISC-V. */
    virtio_mb();
    virtio_fence_w_o();
    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_NOTIFY, q->qid);
}

static void *virtio_irq_thread(void *arg)
{
    struct rtmux_virtio_mmio *vio = arg;
    struct rtmux_virtio_irq *irq = &vio->irq;
    struct sched_param sp = {.sched_priority = RTMUX_LKL_IST_PRIORITY};
    int error = 0;
    int sched_rc;

    sched_rc = __sched_set(0, 0, SCHED_RR, &sp);
    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        error = errno;
        goto fail;
    }
    irq->iid = interrupt_attach_thread(vio->irq_vector, 0);
    if (irq->iid == -1) {
        error = errno;
        goto fail;
    }
    pthread_mutex_lock(&irq->lock);
    irq->ready = true;
    pthread_cond_broadcast(&irq->cond);
    pthread_mutex_unlock(&irq->lock);

    /* Report the RT band only if __sched_set actually took, so the line cannot
     * claim a priority the IST is not running at.
     */
    if (sched_rc == 0)
        fprintf(stderr, "lkl: IST SCHED_RR/%d, dispatch worker receive-PI\n",
                RTMUX_LKL_IST_PRIORITY);
    else
        fprintf(
            stderr,
            "lkl: IST RT priority unavailable, dispatch worker receive-PI\n");

    for (;;) {
        interrupt_wait(
            RTMUX_INTR_WAIT_FLAGS_UNMASK | RTMUX_INTR_WAIT_FLAGS_FAST, NULL);
        if (vio->base)
            virtio_mmio_wr32(vio, VIRTIO_MMIO_INTERRUPT_ACK, UINT32_MAX);
        if (irq->notify)
            irq->notify(irq->notify_arg);
    }

fail:
    pthread_mutex_lock(&irq->lock);
    irq->error = error != 0 ? error : EIO;
    pthread_cond_broadcast(&irq->cond);
    pthread_mutex_unlock(&irq->lock);
    return NULL;
}

int virtio_irq_start(struct rtmux_virtio_mmio *vio,
                     void (*notify)(void *),
                     void *notify_arg)
{
    struct rtmux_virtio_irq *irq = &vio->irq;
    int ret;

    if (vio->irq_vector <= 0)
        return -ENODEV;
    if (irq->started)
        return -EBUSY;
    memset(irq, 0, sizeof(*irq));
    irq->iid = -1;
    irq->notify = notify;
    irq->notify_arg = notify_arg;
    ret = pthread_mutex_init(&irq->lock, NULL);
    if (ret != 0)
        return -ret;
    ret = pthread_cond_init(&irq->cond, NULL);
    if (ret != 0) {
        (void) pthread_mutex_destroy(&irq->lock);
        return -ret;
    }
    ret = pthread_create(&irq->thread, NULL, virtio_irq_thread, vio);
    if (ret != 0) {
        (void) pthread_cond_destroy(&irq->cond);
        (void) pthread_mutex_destroy(&irq->lock);
        return -ret;
    }
    irq->started = true;
    pthread_mutex_lock(&irq->lock);
    while (!irq->ready && irq->error == 0)
        pthread_cond_wait(&irq->cond, &irq->lock);
    ret = irq->error;
    pthread_mutex_unlock(&irq->lock);
    if (ret != 0) {
        (void) thread_join(irq->thread, NULL);
        irq->started = false;
        (void) pthread_cond_destroy(&irq->cond);
        (void) pthread_mutex_destroy(&irq->lock);
        return -ret;
    }
    return 0;
}

void virtio_irq_stop(struct rtmux_virtio_mmio *vio)
{
    struct rtmux_virtio_irq *irq = &vio->irq;

    if (!irq->started)
        return;
    (void) thread_ctl(RTMUX_TCTL_IO, 0);
    if (irq->iid >= 0)
        interrupt_detach(irq->iid);
    (void) __thread_cancel(irq->thread, NULL);
    (void) thread_join(irq->thread, NULL);
    (void) pthread_cond_destroy(&irq->cond);
    (void) pthread_mutex_destroy(&irq->lock);
    memset(irq, 0, sizeof(*irq));
}
