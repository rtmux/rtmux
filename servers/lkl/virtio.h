/*
 * RTMux delegated virtio-mmio transport helpers for LKL domains.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_LKL_VIRTIO_H
#define RTMUX_LKL_VIRTIO_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_STATUS 0x070

#define VIRTIO_ID_NET 1
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_F_VERSION_1 32

#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

#define VIRTIO_QUEUE_MAX_SIZE 32U
#define VIRTIO_MMIO_CAND_MAX 8U
#define VIRTIO_MMIO_MAP_MAX 0x1000U

/* Current virtio-net path needs tiny contiguous DMA buffers; widen only with a
 * measured larger-device user and a matching containment story.
 */
#define VIRTIO_DMA_ALLOC_MAX (64U * 1024U)
#define VIRTIO_DMA_POOL_MAX (256U * 1024U)

struct rtmux_virtio_irq {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int iid;
    bool started;
    bool ready;
    int error;
    void (*notify)(void *);
    void *notify_arg;
};

struct rtmux_virtio_mmio {
    volatile uint8_t *base;
    size_t size;
    uint64_t paddr;
    uint32_t iommu_phandle;
    uint32_t iommu_stream;
    int irq_vector;
    struct rtmux_virtio_irq irq;
    bool claimed;
};

struct rtmux_virtio_dma_buf {
    void *addr;
    uint64_t paddr;
};

struct rtmux_virtio_dma_pool {
    struct rtmux_virtio_dma_buf buf[VIRTIO_QUEUE_MAX_SIZE];
    unsigned count;
    size_t size;
};

struct rtmux_virtio_dma_range {
    uint64_t paddr;
    size_t size;
};

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_MAX_SIZE];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VIRTIO_QUEUE_MAX_SIZE];
};

struct rtmux_virtio_queue {
    unsigned qid;
    unsigned num;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
    void *mem;
    size_t mem_size;
    uint64_t mem_pa;
    uint64_t desc_pa;
    uint64_t avail_pa;
    uint64_t used_pa;
    uint16_t last_used;
};

uint32_t virtio_mmio_rd32(struct rtmux_virtio_mmio *vio, unsigned off);
void virtio_mmio_wr32(struct rtmux_virtio_mmio *vio,
                      unsigned off,
                      uint32_t val);
void virtio_mb(void); /* memory-vs-memory full fence (not ordered against I/O) */
void virtio_fence_w_o(void); /* prior memory stores before a later MMIO write */
void virtio_fence_o_o(void); /* earlier MMIO writes before later MMIO writes */
/* Scoped I/O fences for queue control (RISC-V fence io,o / o,i; w,o and o,o above).
 * They order CPU-issued MMIO accesses only; no posted-write completion is implied.
 */
void virtio_io_fence_before_sel(void);
void virtio_io_fence_oi(void);
void virtio_mmio_set_delegated(bool enabled);
void virtio_mmio_unmap(struct rtmux_virtio_mmio *vio);
int virtio_mmio_discover(struct rtmux_virtio_mmio *vio,
                         uint32_t device_id,
                         int *version);
int virtio_mmio_require_iommu_domain(
    struct rtmux_virtio_mmio *vio,
    const struct rtmux_virtio_dma_range *ranges,
    unsigned count);
int virtio_dma_pool_alloc(struct rtmux_virtio_dma_pool *pool,
                          unsigned count,
                          size_t size);
void virtio_dma_pool_free(struct rtmux_virtio_dma_pool *pool);
/* Queue setup is split so DMA ranges can be authorized before the device sees
 * them:
 *   virtio_queue_alloc()   selects the queue, reads QUEUE_NUM_MAX, allocates the
 *                          ring memory and validates it (v1 PFN must fit 32
 *                          bits). It writes only QUEUE_SEL; the device is not
 *                          given any address, QUEUE_PFN or QUEUE_READY. Returns
 *                          NULL on failure with nothing leaked.
 *   virtio_queue_publish() programs QUEUE_NUM and the addresses (v1: ALIGN/PFN,
 *                          v2: DESC/DRIVER/DEVICE + QUEUE_READY) for the same
 *                          version passed to alloc, after virtio_fence_w_o(). It
 *                          re-selects the queue and cannot fail.
 * The caller must publish only after the domain decision, and must issue
 * virtio_fence_w_o() itself before the DRIVER_OK status write if it stored to the
 * rings after publishing.
 */
struct rtmux_virtio_queue *virtio_queue_alloc(struct rtmux_virtio_mmio *vio,
                                              int version,
                                              unsigned qid,
                                              unsigned max_entries);
void virtio_queue_publish(struct rtmux_virtio_mmio *vio,
                          int version,
                          struct rtmux_virtio_queue *q);
void virtio_queue_free(struct rtmux_virtio_queue *q);
void virtio_queue_kick(struct rtmux_virtio_mmio *vio,
                       struct rtmux_virtio_queue *q);
int virtio_irq_start(struct rtmux_virtio_mmio *vio,
                     void (*notify)(void *),
                     void *notify_arg);
void virtio_irq_stop(struct rtmux_virtio_mmio *vio);

void rtmux_lkl_host_stats_dma_add(size_t size);
void rtmux_lkl_host_stats_dma_sub(size_t size);

#endif
