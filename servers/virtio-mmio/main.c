/*
 * RTMux virtio-mmio delegation service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hw/hwinfo-lookup.h>
#include <sys/dispatch.h>
#include <sys/ioserv.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <sys/virtio-mmio-msg.h>

#define VIRTIO_MMIO_MODE (S_IFCHR | 0600)
#define VIRTIO_MMIO_DELEGATION_MAX 32U
#define PCI_ECAM_MMAP_MAX (256UL << 20)
#define PCI_DBI_MMAP_MAX (4UL << 20)

static time_t service_created;
static ioserv_ops_t io_funcs;

struct virtio_mmio_delegation {
    struct rtmux_resource_handle handle;
};

static struct virtio_mmio_delegation delegations[VIRTIO_MMIO_DELEGATION_MAX];
static unsigned delegation_count;
static struct rtmux_resource_handle devices[VIRTIO_MMIO_MAX_HANDLES];
static unsigned device_count;
static uint64_t next_generation = 1;

static int virtio_open(ioserv_context_t *ctp,
                       io_open_t *msg,
                       void *handle,
                       void *extra)
{
    struct _client_info info;
    void *ocb;
    unsigned want;

    (void) handle;
    (void) extra;
    if (msg->connect.subtype != _IO_CONNECT_OPEN &&
        msg->connect.subtype != _IO_CONNECT_COMBINE &&
        msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)
        return ENOSYS;
    if (msg->connect.file_type != _FTYPE_ANY)
        return ENOSYS;
    want = msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR);
    if (want != 0) {
        if (connect_client_info_r(ctp->info.scoid, &info, 0) != EOK)
            return errno;
        if (info.cred.euid != 0)
            return EACCES;
    }
    ocb = calloc(1, 1);
    if (!ocb)
        return ENFILE;
    if (ioserv_open_bind(ctp, ocb, &io_funcs) == -1) {
        int rc = errno;

        free(ocb);
        return rc;
    }
    return EOK;
}

static int virtio_close_ocb(ioserv_context_t *ctp, void *reserved, void *ocb)
{
    (void) ctp;
    (void) reserved;
    free(ocb);
    return EOK;
}

static int virtio_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    (void) ocb;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_ino = 1;
    msg->o.st_mode = VIRTIO_MMIO_MODE;
    msg->o.st_nlink = 1;
    msg->o.st_uid = 0;
    msg->o.st_gid = 0;
    msg->o.st_atime = msg->o.st_mtime = msg->o.st_ctime = service_created;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int store_device_records(const struct rtmux_resource_handle *handles,
                                unsigned count)
{
    if (count == 0 || count > VIRTIO_MMIO_MAX_HANDLES)
        return EINVAL;
    for (unsigned i = 0; i < count; i++) {
        const struct rtmux_resource_handle *h = &handles[i];

        if (h->kind != RTMUX_RESOURCE_MMIO || h->flags != 0 ||
            h->base > (uint64_t) INT64_MAX || h->length == 0 ||
            h->owner_pid != 0 || h->device_id == 0 || h->generation == 0 ||
            h->irq_vector < 0 || h->irq_vector > INT_MAX)
            return EINVAL;
        if ((h->iommu_phandle == RTMUX_RESOURCE_IOMMU_PHANDLE_NONE) !=
            (h->reserved == RTMUX_RESOURCE_IOMMU_STREAM_NONE))
            return EINVAL;
    }
    device_count = count;
    memcpy(devices, handles, count * sizeof(devices[0]));
    return EOK;
}

static void release_delegated_handles(
    const struct rtmux_resource_handle *handles,
    unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        if (handles[i].kind == RTMUX_RESOURCE_MMIO && handles[i].owner_pid > 0)
            rtmux_hwres_release_range_for_pid(
                handles[i].base, handles[i].length, RTMUX_HWRES_MEMORY,
                handles[i].owner_pid);
    }
}

static int delegate_device(uint32_t device_id,
                           struct rtmux_resource_handle *handles,
                           uint32_t max_handles,
                           pid_t claim_pid,
                           unsigned *out_found)
{
    unsigned found = 0;

    *out_found = 0;
    if (max_handles == 0 || max_handles > VIRTIO_MMIO_MAX_HANDLES)
        return EINVAL;
    if (claim_pid < 0)
        return EINVAL;

    for (unsigned i = 0; i < device_count; i++) {
        const struct rtmux_resource_handle *dev = &devices[i];

        if (dev->device_id != device_id)
            continue;
        if (claim_pid > 0 &&
            rtmux_hwres_claim_range_for_pid(
                dev->base, dev->length, RTMUX_HWRES_MEMORY, claim_pid) == -1) {
            int rc = errno ? errno : EIO;

            release_delegated_handles(handles, found);
            return rc;
        }
        handles[found] = *dev;
        handles[found].flags = RTMUX_RESOURCE_HANDLE_F_ONESHOT;
        handles[found].owner_pid = claim_pid;
        handles[found].generation = next_generation++;
        found++;
        if (found >= max_handles)
            break;
    }
    *out_found = found;
    return found ? EOK : ENODEV;
}

static int remember_delegation(pid_t pid,
                               uint32_t device_id,
                               const struct rtmux_resource_handle *handles,
                               unsigned count)
{
    if (count > VIRTIO_MMIO_MAX_HANDLES ||
        delegation_count > VIRTIO_MMIO_DELEGATION_MAX - count)
        return ENOSPC;
    for (unsigned i = 0; i < count; i++) {
        if (handles[i].owner_pid != pid || handles[i].device_id != device_id ||
            handles[i].kind != RTMUX_RESOURCE_MMIO)
            return EINVAL;
        delegations[delegation_count++] = (struct virtio_mmio_delegation) {
            .handle = handles[i],
        };
    }
    return EOK;
}

static int query_delegation(pid_t pid,
                            uint32_t device_id,
                            struct rtmux_resource_handle *handles,
                            uint32_t max_handles,
                            unsigned *out_found)
{
    unsigned found = 0;
    unsigned out = 0;

    *out_found = 0;
    if (pid <= 0 || max_handles == 0 || max_handles > VIRTIO_MMIO_MAX_HANDLES)
        return EINVAL;
    for (unsigned i = 0; i < delegation_count; i++) {
        if (delegations[i].handle.owner_pid != pid ||
            delegations[i].handle.device_id != device_id ||
            (delegations[i].handle.flags & RTMUX_RESOURCE_HANDLE_F_REVOKED) !=
                0)
            continue;
        found++;
    }
    if (found == 0)
        return ENODEV;
    if (found > max_handles)
        return EMSGSIZE;

    for (unsigned i = 0; i < delegation_count; i++) {
        if (delegations[i].handle.owner_pid != pid ||
            delegations[i].handle.device_id != device_id ||
            (delegations[i].handle.flags & RTMUX_RESOURCE_HANDLE_F_REVOKED) !=
                0)
            continue;
        handles[out++] = delegations[i].handle;
    }

    unsigned dst = 0;
    for (unsigned i = 0; i < delegation_count; i++) {
        if (delegations[i].handle.owner_pid == pid &&
            delegations[i].handle.device_id == device_id)
            continue;
        delegations[dst++] = delegations[i];
    }
    delegation_count = dst;
    *out_found = out;
    return out ? EOK : EMSGSIZE;
}

static int virtio_io_msg(ioserv_context_t *ctp, io_msg_t *msg, void *ocb)
{
    struct virtio_mmio_message *req = (struct virtio_mmio_message *) msg;
    struct virtio_mmio_message rep;
    unsigned claimed = 0;
    int rc;

    (void) ocb;
    if (msg->i.protocol != _IOMSG_VIRTIO_MMIO)
        return ENOSYS;
    if (msg->i.combine_len < sizeof(*req))
        return EMSGSIZE;

    memset(&rep, 0, sizeof(rep));
    memcpy(&rep.msg_hdr.i, &msg->i, sizeof(msg->i));
    switch (msg->i.subtype) {
    case IOM_VIRTIO_MMIO_REGISTER:
        rc = store_device_records(req->handles, req->msg.delegate.max_handles);
        rep.rep_hdr.reply_status = (uint32_t) rc;
        rep.rep_hdr.reply_count = rc == EOK ? req->msg.delegate.max_handles : 0;
        break;
    case IOM_VIRTIO_MMIO_DELEGATE:
        if (req->msg.delegate.pid <= 0) {
            rc = EINVAL;
        } else if (req->msg.delegate.max_handles >
                   VIRTIO_MMIO_DELEGATION_MAX - delegation_count) {
            rc = ENOSPC;
        } else {
            rc = delegate_device(req->msg.delegate.device_id, rep.handles,
                                 req->msg.delegate.max_handles,
                                 req->msg.delegate.pid, &claimed);
            if (rc == EOK) {
                rc = remember_delegation(req->msg.delegate.pid,
                                         req->msg.delegate.device_id,
                                         rep.handles, claimed);
                if (rc != EOK) {
                    release_delegated_handles(rep.handles, claimed);
                    claimed = 0;
                }
            }
        }
        rep.rep_hdr.reply_status = (uint32_t) rc;
        rep.rep_hdr.reply_count = claimed;
        break;
    case IOM_VIRTIO_MMIO_QUERY:
        rc = query_delegation(ctp->info.pid, req->msg.delegate.device_id,
                              rep.handles, req->msg.delegate.max_handles,
                              &claimed);
        rep.rep_hdr.reply_status = (uint32_t) rc;
        rep.rep_hdr.reply_count = claimed;
        break;
    default:
        rc = ENOSYS;
        rep.rep_hdr.reply_status = (uint32_t) rc;
        break;
    }
    rep.msg_hdr.i.combine_len = sizeof(rep);
    SETIOV(ctp->iov, &rep, sizeof(rep));
    msg_replyv(ctp->rcvid, EOK, ctp->iov, 1);
    return IOSERV_NOREPLY;
}

static void iommu_metadata_from_hwinfo(hwi_tag *dev,
                                       uint32_t *out_phandle,
                                       uint32_t *out_stream)
{
    hwi_tag *tag = hwinfo_next(dev);
    uint8_t *base = hwinfo_base();
    char *strings = hwinfo_strings();
    unsigned section_size = _syspage_ptr->hwinfo.entry_size;

    *out_phandle = RTMUX_RESOURCE_IOMMU_PHANDLE_NONE;
    *out_stream = RTMUX_RESOURCE_IOMMU_STREAM_NONE;
    while ((uintptr_t) tag < (uintptr_t) base + section_size) {
        const char *name;

        if (tag->prefix.size == 0)
            break;
        name = strings + tag->prefix.name;
        if (!strcmp(name, HWI_TAG_NAME_device) ||
            !strcmp(name, HWI_TAG_NAME_bus) ||
            !strcmp(name, HWI_TAG_NAME_group))
            break;
        if (!strcmp(name, HWI_TAG_NAME_optstr)) {
            const char *opt = strings + tag->optstr.string;
            unsigned long val;
            char *end;

            if (strncmp(opt, "iommu-phandle=", 14) == 0) {
                errno = 0;
                val = strtoul(opt + 14, &end, 0);
                if (errno == 0 && *end == '\0' && val <= UINT32_MAX)
                    *out_phandle = (uint32_t) val;
            } else if (strncmp(opt, "iommu-stream=", 13) == 0) {
                errno = 0;
                val = strtoul(opt + 13, &end, 0);
                if (errno == 0 && *end == '\0' && val < UINT32_MAX)
                    *out_stream = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(val);
            }
        }
        tag = hwinfo_next(tag);
    }
}

static int find_boot_devices(struct rtmux_resource_handle *handles,
                             unsigned max_handles,
                             unsigned *out_found)
{
    hwi_tag *dev = NULL;
    unsigned found = 0;

#define ADD_BOOT_HANDLE(_base, _size, _device_id, _irq)   \
    do {                                                  \
        handles[found] = (struct rtmux_resource_handle) { \
            .kind = RTMUX_RESOURCE_MMIO,                  \
            .base = (_base),                              \
            .length = (_size),                            \
            .device_id = (_device_id),                    \
            .generation = found + 1,                      \
            .irq_vector = (int32_t) (_irq),               \
        };                                                \
        found++;                                          \
    } while (0)

    dev = hwinfo_find_device("ns16550", NULL);
    if (dev && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            irq->irq.vector <= (uint32_t) INT_MAX)
            ADD_BOOT_HANDLE(loc->location.base, 0x1000, RTMUX_DEVICE_NS16550,
                            irq->irq.vector);
    }

    dev = hwinfo_find_device("sifive-uart", NULL);
    if (dev && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            irq->irq.vector <= (uint32_t) INT_MAX)
            ADD_BOOT_HANDLE(loc->location.base, 0x1000,
                            RTMUX_DEVICE_SIFIVE_UART, irq->irq.vector);
    }

    dev = hwinfo_find_device("goldfish-rtc", NULL);
    if (dev && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            irq->irq.vector <= (uint32_t) INT_MAX)
            ADD_BOOT_HANDLE(loc->location.base, 0x1000,
                            RTMUX_DEVICE_GOLDFISH_RTC, irq->irq.vector);
    }

    dev = hwinfo_find_device("sifive-gpio", NULL);
    if (dev && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            irq->irq.vector <= (uint32_t) INT_MAX)
            ADD_BOOT_HANDLE(loc->location.base, 0x1000,
                            RTMUX_DEVICE_SIFIVE_GPIO, irq->irq.vector);
    }

    dev = hwinfo_find_device("riscv-iommu", NULL);
    if (dev && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            irq->irq.vector <= (uint32_t) INT_MAX)
            ADD_BOOT_HANDLE(loc->location.base, 0x1000,
                            RTMUX_DEVICE_RISCV_IOMMU, irq->irq.vector);
    }

    hwi_tag *bus = hwinfo_find_bus(HWI_ITEM_BUS_PCI, NULL);
    if (bus && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(bus, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(bus, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            loc->location.len > 0 && irq->irq.vector <= (uint32_t) INT_MAX) {
            uint64_t size = loc->location.len > PCI_ECAM_MMAP_MAX
                                ? PCI_ECAM_MMAP_MAX
                                : loc->location.len;

            ADD_BOOT_HANDLE(loc->location.base, size, RTMUX_DEVICE_PCI_ECAM,
                            irq->irq.vector);
        }
    }

    dev = hwinfo_find_device("dw_pcie_msi", NULL);
    if (dev && found < max_handles) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);

        if (loc && irq && loc->location.base <= (uint64_t) INT64_MAX &&
            loc->location.len > 0 && irq->irq.vector <= (uint32_t) INT_MAX) {
            uint64_t size = loc->location.len > PCI_DBI_MMAP_MAX
                                ? PCI_DBI_MMAP_MAX
                                : loc->location.len;

            ADD_BOOT_HANDLE(loc->location.base, size, RTMUX_DEVICE_PCI_DBI,
                            irq->irq.vector);
        }
    }

    dev = NULL;
    while ((dev = hwinfo_find_device("virtio", dev))) {
        hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
        hwi_tag *irq = hwinfo_find_tag_after(dev, HWI_TAG_NAME_irq);
        size_t size;
        void *map;
        uint32_t device_id;

        if (!loc || !irq)
            continue;
        if (loc->location.base > (uint64_t) INT64_MAX)
            continue;
        if (irq->irq.vector > (uint32_t) INT_MAX)
            continue;
        size = loc->location.len > VIRTIO_MMIO_MAP_MAX
                   ? VIRTIO_MMIO_MAP_MAX
                   : (size_t) loc->location.len;
        if (size < 0x200)
            size = 0x200;

        map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_PHYS,
                   NOFD, (off_t) loc->location.base);
        if (map == MAP_FAILED)
            continue;
        if (*(volatile uint32_t *) ((char *) map + VIRTIO_MMIO_MAGIC_VALUE) !=
            VIRTIO_MMIO_MAGIC) {
            munmap(map, size);
            continue;
        }
        device_id =
            *(volatile uint32_t *) ((char *) map + VIRTIO_MMIO_DEVICE_ID);
        munmap(map, size);
        if (device_id == 0)
            continue;

        ADD_BOOT_HANDLE(loc->location.base, size, device_id, irq->irq.vector);
        iommu_metadata_from_hwinfo(dev, &handles[found - 1].iommu_phandle,
                                   &handles[found - 1].reserved);
        if (found >= max_handles)
            break;
    }
#undef ADD_BOOT_HANDLE
    *out_found = found;
    return found ? EOK : ENODEV;
}

static int register_boot_devices(void)
{
    struct virtio_mmio_message msg;
    unsigned found = 0;
    int fd;
    int rc;

    memset(&msg, 0, sizeof(msg));
    rc = find_boot_devices(msg.handles, VIRTIO_MMIO_MAX_HANDLES, &found);
    if (rc != EOK)
        return rc;

    /* ipc-send: virtio-mmio-register -> virtio-mmio */
    fd = open(VIRTIO_MMIO_SERVICE_PATH, O_RDWR);
    if (fd == -1)
        return errno ? errno : EIO;
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_REGISTER;
    msg.msg.delegate.max_handles = found;
    /* ipc-send: virtio-mmio-register -> virtio-mmio */
    rc = msg_send(fd, &msg, sizeof(msg), &msg, sizeof(msg));
    close(fd);
    if (rc == -1)
        return errno ? errno : EIO;
    return (int) msg.rep_hdr.reply_status;
}

int main(int argc, char **argv)
{
    ioserv_connect_ops_t connect_funcs;
    dispatch_t *dpp;
    ioserv_context_t *ctp;
    ioserv_attr_t attr = {
        .nparts_max = 2,
        .msg_max_size = sizeof(struct virtio_mmio_message),
    };

    if (argc == 2 && strcmp(argv[1], "--register-boot") == 0) {
        int rc = register_boot_devices();

        if (rc != EOK) {
            fprintf(stderr, "virtio-mmio: register boot devices: %s\n",
                    strerror(rc));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    dpp = dispatch_create();
    if (!dpp) {
        fprintf(stderr, "virtio-mmio: dispatch_create: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    memset(&connect_funcs, 0, sizeof(connect_funcs));
    memset(&io_funcs, 0, sizeof(io_funcs));
    connect_funcs.nfuncs = IOSERV_CONNECT_NFUNCS;
    io_funcs.nfuncs = IOSERV_IO_NFUNCS;
    connect_funcs.open = virtio_open;
    io_funcs.close_ocb = virtio_close_ocb;
    io_funcs.stat = virtio_stat;
    io_funcs.msg = virtio_io_msg;
    service_created = time(NULL);
    if (ioserv_attach(dpp, &attr, VIRTIO_MMIO_SERVICE_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, NULL) == -1) {
        fprintf(stderr, "virtio-mmio: attach %s: %s\n",
                VIRTIO_MMIO_SERVICE_PATH, strerror(errno));
        return EXIT_FAILURE;
    }
    ctp = ioserv_context_alloc(dpp);
    if (!ctp) {
        fprintf(stderr, "virtio-mmio: context: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (rtmux_detach(EXIT_SUCCESS) != EOK) {
        fprintf(stderr, "virtio-mmio: detach: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    for (;;) {
        ctp = ioserv_block(ctp);
        if (!ctp)
            return EXIT_FAILURE;
        ioserv_handler(ctp);
    }
}
