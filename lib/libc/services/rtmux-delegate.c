/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * rtmux_delegate_device - delegate one MMIO/IRQ device from the virtio-mmio
 * resource server and return its validated single resource handle.
 *
 * The open, DELEGATE, QUERY, validate handshake for a single-handle device was
 * copy-pasted across every such client (the two serial drivers, the PCI ECAM
 * bring-up, and the IRQ-notify test). This centralizes it behind one strict
 * validation so those callers cannot drift apart. Multi-handle clients (the LKL
 * virtio path) query a candidate array and keep their own code.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include <sys/virtio-mmio-msg.h>
#include <arch/plic-vector.h>

int rtmux_delegate_device(uint32_t device_id,
                          size_t min_size,
                          uint64_t expected_base,
                          struct rtmux_resource_handle *out)
{
    struct virtio_mmio_message msg;
    int fd, rc;

    if (!min_size || !out)
        return EINVAL;

    fd = open(VIRTIO_MMIO_SERVICE_PATH, O_RDWR);
    if (fd == -1)
        return errno ? errno : EIO;

    memset(&msg, 0, sizeof msg);
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof msg;
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_DELEGATE;
    msg.msg.delegate.device_id = device_id;
    msg.msg.delegate.max_handles = 1;
    msg.msg.delegate.pid = getpid();
    rc = msg_send(fd, &msg, sizeof msg, &msg, sizeof msg);
    if (rc == -1) {
        close(fd);
        return errno ? errno : EIO;
    }
    if (msg.rep_hdr.reply_status != EOK) {
        close(fd);
        return (int) msg.rep_hdr.reply_status;
    }

    memset(&msg, 0, sizeof msg);
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof msg;
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_QUERY;
    msg.msg.delegate.device_id = device_id;
    msg.msg.delegate.max_handles = 1;
    rc = msg_send(fd, &msg, sizeof msg, &msg, sizeof msg);
    close(fd);
    if (rc == -1)
        return errno ? errno : EIO;
    if (msg.rep_hdr.reply_status != EOK)
        return (int) msg.rep_hdr.reply_status;
    if (msg.rep_hdr.reply_count != 1)
        return EINVAL;

    *out = msg.handles[0];

    /* Strict single-handle validation, the union of what the callers checked:
     * the handle is an MMIO resource owned by us for the requested device, live
     * (nonzero generation, not revoked), at least min_size, a base that fits a
     * signed offset when the caller mmaps it, and a raw PLIC IRQ number that is
     * nonnegative and small enough that PLIC_VECTOR_BASE + irq_vector cannot
     * overflow at the caller. expected_base pins the base for a caller that
     * already knows it (the PCI ECAM bring-up reads it from the syspage); pass
     * RTMUX_DELEGATE_ANY_BASE to accept whatever the server grants.
     */
    if (out->kind != RTMUX_RESOURCE_MMIO || out->owner_pid != getpid() ||
        out->device_id != device_id || out->generation == 0 ||
        (out->flags & RTMUX_RESOURCE_HANDLE_F_REVOKED) != 0 ||
        out->length < min_size || out->base > (uint64_t) INT64_MAX ||
        out->irq_vector < 0 || out->irq_vector > INT_MAX - PLIC_VECTOR_BASE ||
        (expected_base != RTMUX_DELEGATE_ANY_BASE &&
         out->base != expected_base))
        return EINVAL;
    return EOK;
}
