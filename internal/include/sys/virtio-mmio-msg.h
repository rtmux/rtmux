/*
 * RTMux virtio-mmio device delegation protocol.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_VIRTIO_MMIO_MSG_H
#define _SYS_VIRTIO_MMIO_MSG_H

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/types.h>

#define VIRTIO_MMIO_SERVICE_PATH "/dev/virtio-mmio"

#define VIRTIO_MMIO_MAP_MAX      0x1000U
#define VIRTIO_MMIO_MAGIC_VALUE  0x000U
#define VIRTIO_MMIO_VERSION      0x004U
#define VIRTIO_MMIO_DEVICE_ID    0x008U
#define VIRTIO_MMIO_MAGIC        0x74726976U

#define RTMUX_DEVICE_NS16550      0x52540001U
#define RTMUX_DEVICE_SIFIVE_UART  0x52540002U
#define RTMUX_DEVICE_PCI_ECAM     0x52540003U
#define RTMUX_DEVICE_PCI_DBI      0x52540004U
#define RTMUX_DEVICE_GOLDFISH_RTC 0x52540005U
#define RTMUX_DEVICE_SIFIVE_GPIO  0x52540006U
#define RTMUX_DEVICE_RISCV_IOMMU  0x52540007U

typedef enum {
    IOM_VIRTIO_MMIO_DELEGATE = 1,
    IOM_VIRTIO_MMIO_QUERY = 2,
    IOM_VIRTIO_MMIO_REGISTER = 3,
} virtio_mmio_func_t;

#define VIRTIO_MMIO_MAX_HANDLES 16U

typedef enum {
    RTMUX_RESOURCE_MMIO = 1,
    RTMUX_RESOURCE_PCI_BAR = 2,
} rtmux_resource_kind_t;

#define RTMUX_RESOURCE_HANDLE_F_ONESHOT  (1U << 0)
#define RTMUX_RESOURCE_HANDLE_F_REVOKED  (1U << 1)
#define RTMUX_RESOURCE_HANDLE_F_CONSUMED (1U << 2)

/* For RTMUX_RESOURCE_MMIO handles, reserved == 0 means no IOMMU stream was
 * described by firmware. A nonzero value stores stream-id + 1 so stream ID 0
 * remains representable.
 */
#define RTMUX_RESOURCE_IOMMU_STREAM_NONE 0U
#define RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(_id) ((uint32_t) (_id) + 1U)
#define RTMUX_RESOURCE_IOMMU_STREAM_DECODE(_reserved) \
    ((uint32_t) (_reserved) - 1U)
#define RTMUX_RESOURCE_IOMMU_PHANDLE_NONE 0U

struct virtio_mmio_delegate {
    uint32_t device_id;
    uint32_t max_handles;
    pid_t pid;
    uint32_t reserved;
};

struct rtmux_resource_handle {
    uint32_t kind;
    uint32_t flags;
    uint64_t base;
    uint64_t length;
    pid_t owner_pid;
    uint32_t device_id;
    uint64_t generation;
    int32_t irq_vector;
    uint32_t reserved;
    uint32_t iommu_phandle;
};

struct virtio_mmio_reply_hdr {
    uint32_t reply_status;
    uint32_t reply_count;
};

struct virtio_mmio_message {
    io_msg_t msg_hdr;
    struct virtio_mmio_reply_hdr rep_hdr;
    union {
        struct virtio_mmio_delegate delegate;
    } msg;
    struct rtmux_resource_handle handles[VIRTIO_MMIO_MAX_HANDLES];
};

#endif /* _SYS_VIRTIO_MMIO_MSG_H */
