/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* lkl-contain-test — prove _PROC_PHYS_CONTAIN seals specific-address MAP_PHYS.
 *
 * This is the roottask-side containment mechanism that lkl-netd uses to confine
 * itself after mapping its device MMIO. The test exercises it directly (no LKL
 * needed): it finds a real device MMIO window from the hwinfo manifest, maps it
 * as a baseline, self-restricts via rtmux_phys_contain(), then asserts that
 *   - an anonymous MAP_PHYS (a DMA-style buffer) is STILL allowed, and
 *   - the same specific-address MAP_PHYS is now refused with EPERM.
 *
 * Containment is irreversible and process-scoped, so this runs in its own
 * process. Prints "lkl-contain: PASS" only when every assertion holds.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/virtio-mmio-msg.h>
#include <sys/wait.h>
#include "../../../servers/roottask/include/hwres-msg.h"
#include "../../../servers/roottask/include/hwres-types.h"
#include <hw/hwinfo-lookup.h>
#include <hw/sysinfo.h>

#define CONTAIN_DELEGATE_DEVICE RTMUX_DEVICE_NS16550
#define VIRTIO_ID_NET 1U

extern char **environ;

/* First device in the hwinfo manifest that carries a location tag. The QEMU
 * virt console UART ("ns16550") is always present; the typed delegation probe
 * below also uses the UART handle so this regression does not require a QEMU
 * virtio-net device.
 */
static int find_mmio(uint64_t *base, size_t *len)
{
    static const char *const names[] = {"ns16550", "virtio", "8250", NULL};

    for (int i = 0; names[i]; i++) {
        hwi_tag *dev = hwinfo_find_device(names[i], NULL);
        while (dev) {
            hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
            if (loc && loc->location.len > 0 &&
                loc->location.base <= (uint64_t) INT64_MAX) {
                *base = loc->location.base;
                *len = (size_t) loc->location.len;
                return 0;
            }
            dev = hwinfo_find_device(names[i], dev);
        }
    }
    return -1;
}

static int hwres_send(void *header,
                      size_t hbytes,
                      size_t nbytes_off,
                      void *data,
                      size_t dwidth,
                      size_t name_off,
                      void *reply,
                      size_t rbytes)
{
    iov_t out[3];
    iov_t in;
    char **namep = (char **) ((char *) data + name_off);
    size_t nbytes = dwidth + strlen(*namep) + 1;

    *((uint32_t *) ((char *) header + nbytes_off)) = (uint32_t) nbytes;
    SETIOV(&out[0], header, hbytes);
    SETIOV(&out[1], data, dwidth);
    SETIOV(&out[2], *namep, strlen(*namep) + 1);
    SETIOV(&in, reply, rbytes);
    return msg_sendv(HWRES_COID, out, 3, reply ? &in : NULL, reply ? 1 : 0);
}

static int hwres_query_free(hwres_range_t *list, int count, int type)
{
    iov_t out[1], in[1];
    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = (uint16_t) (HWRES_REQ_QUERY_NAME | type),
        .i.pid = 0,
        .i.count = (uint32_t) count,
        .i.nbytes = 0,
    };

    SETIOV(&out[0], &request, sizeof(request));
    SETIOV(&in[0], list, (size_t) count * sizeof(*list));
    return msg_sendv(HWRES_COID, out, 1, in, 1);
}

static int hwres_claim_one(hwres_claim_t *claim)
{
    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = HWRES_REQ_CLAIM,
        .i.pid = 0,
        .i.count = 1,
    };

    return hwres_send(&request, sizeof(request),
                      offsetof(struct hwres_msg, nbytes), claim,
                      sizeof(*claim), offsetof(hwres_claim_t, name), claim,
                      sizeof(*claim));
}

static int hwres_create_one(hwres_claim_t *claim)
{
    hwres_msg_t request = {
        .i.type = HWRES_CMD,
        .i.subtype = HWRES_REQ_CREATE,
        .i.pid = 0,
        .i.count = 1,
    };

    return hwres_send(&request, sizeof(request),
                      offsetof(struct hwres_msg, nbytes), claim,
                      sizeof(*claim), offsetof(hwres_claim_t, name), claim,
                      sizeof(*claim));
}

static int find_claimable_ranges(uint64_t *claimed,
                                 uint64_t *delegated,
                                 uint64_t *unclaimed)
{
    hwres_range_t list[16];
    int num = hwres_query_free(list, 16, HWRES_MEMORY);

    if (num < 0)
        return -1;
    if (num > 16)
        num = 16;
    for (int i = 0; i < num; i++) {
        uint64_t start = (list[i].start + 4095U) & ~4095ULL;
        if (start + 12287U > list[i].end)
            continue;
        hwres_claim_t claim = {
            .length = 4096,
            .align = 4096,
            .start = start,
            .end = start + 4095U,
            .flags = HWRES_MEMORY | HWRES_FLAG_RANGE,
            .name = "",
        };
        if (hwres_claim_one(&claim) == EOK) {
            *claimed = claim.start;
            *delegated = claim.end + 1;
            *unclaimed = claim.end + 4097U;
            return 0;
        }
    }
    return -1;
}

static int virtio_msg(struct virtio_mmio_message *msg)
{
    int fd;
    int rc;

    fd = open(VIRTIO_MMIO_SERVICE_PATH, O_RDWR);
    if (fd == -1)
        return -1;
    rc = msg_send(fd, msg, sizeof(*msg), msg, sizeof(*msg));
    close(fd);
    return rc;
}

static int virtio_query_only(void)
{
    struct virtio_mmio_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_QUERY;
    msg.msg.delegate.device_id = CONTAIN_DELEGATE_DEVICE;
    msg.msg.delegate.max_handles = 1;
    if (virtio_msg(&msg) == -1)
        return 1;
    if (msg.rep_hdr.reply_status != ENODEV) {
        printf("lkl-contain: FAIL wrong-pid query status=%u\n",
               msg.rep_hdr.reply_status);
        return 1;
    }
    return 0;
}

static int expect_bad_register(struct rtmux_resource_handle handle,
                               const char *what)
{
    struct virtio_mmio_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_REGISTER;
    msg.msg.delegate.max_handles = 1;
    msg.handles[0] = handle;
    if (virtio_msg(&msg) == -1) {
        printf("lkl-contain: FAIL %s register send: %s\n", what,
               strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_status != EINVAL) {
        printf("lkl-contain: FAIL %s register status=%u\n", what,
               msg.rep_hdr.reply_status);
        return -1;
    }
    return 0;
}

static int register_handles(const struct rtmux_resource_handle *handles,
                            uint32_t count)
{
    struct virtio_mmio_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_REGISTER;
    msg.msg.delegate.max_handles = count;
    memcpy(msg.handles, handles, (size_t) count * sizeof(handles[0]));
    if (virtio_msg(&msg) == -1) {
        printf("lkl-contain: FAIL register handles send: %s\n",
               strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_status != EOK) {
        printf("lkl-contain: FAIL register handles status=%u\n",
               msg.rep_hdr.reply_status);
        return -1;
    }
    return 0;
}

static int expect_delegate(uint32_t max_handles, int want_status)
{
    struct virtio_mmio_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_DELEGATE;
    msg.msg.delegate.device_id = CONTAIN_DELEGATE_DEVICE;
    msg.msg.delegate.max_handles = max_handles;
    msg.msg.delegate.pid = getpid();
    if (virtio_msg(&msg) == -1) {
        printf("lkl-contain: FAIL delegate send: %s\n", strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_status != (uint32_t) want_status) {
        printf("lkl-contain: FAIL delegate status=%u want=%d count=%u\n",
               msg.rep_hdr.reply_status, want_status, msg.rep_hdr.reply_count);
        return -1;
    }
    if ((want_status == EOK) != (msg.rep_hdr.reply_count != 0)) {
        printf("lkl-contain: FAIL delegate count=%u status=%u\n",
               msg.rep_hdr.reply_count, msg.rep_hdr.reply_status);
        return -1;
    }
    return 0;
}

static int expect_query_device(uint32_t device_id,
                               uint32_t max_handles,
                               int want_status,
                               uint32_t want_count,
                               struct rtmux_resource_handle *handles)
{
    struct virtio_mmio_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_QUERY;
    msg.msg.delegate.device_id = device_id;
    msg.msg.delegate.max_handles = max_handles;
    if (virtio_msg(&msg) == -1) {
        printf("lkl-contain: FAIL query send: %s\n", strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_status != (uint32_t) want_status ||
        msg.rep_hdr.reply_count != want_count) {
        printf("lkl-contain: FAIL query status=%u want=%d count=%u "
               "want_count=%u\n",
               msg.rep_hdr.reply_status, want_status, msg.rep_hdr.reply_count,
               want_count);
        return -1;
    }
    if (handles && want_count > 0)
        memcpy(handles, msg.handles,
               (size_t) want_count * sizeof(handles[0]));
    return 0;
}

static int expect_query(uint32_t max_handles,
                        int want_status,
                        uint32_t want_count,
                        struct rtmux_resource_handle *handles)
{
    return expect_query_device(CONTAIN_DELEGATE_DEVICE, max_handles,
                               want_status, want_count, handles);
}

static int delegate_device_to_self(uint32_t device_id,
                                   uint32_t max_handles,
                                   unsigned *out_count)
{
    struct virtio_mmio_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_DELEGATE;
    msg.msg.delegate.device_id = device_id;
    msg.msg.delegate.max_handles = max_handles;
    msg.msg.delegate.pid = getpid();
    if (virtio_msg(&msg) == -1) {
        printf("lkl-contain: FAIL delegate device %#x send: %s\n", device_id,
               strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_status != EOK || msg.rep_hdr.reply_count == 0) {
        printf("lkl-contain: FAIL delegate device %#x status=%u count=%u\n",
               device_id, msg.rep_hdr.reply_status, msg.rep_hdr.reply_count);
        return -1;
    }
    *out_count = msg.rep_hdr.reply_count;
    return 0;
}

static int virtio_iommu_metadata_stream(uint32_t *out_stream)
{
    struct rtmux_resource_handle handles[VIRTIO_MMIO_MAX_HANDLES];
    unsigned count = 0;
    uint32_t stream = RTMUX_RESOURCE_IOMMU_STREAM_NONE;
    bool found = false;

    if (delegate_device_to_self(VIRTIO_ID_NET, VIRTIO_MMIO_MAX_HANDLES,
                                &count) != 0)
        return 1;
    if (expect_query_device(VIRTIO_ID_NET, VIRTIO_MMIO_MAX_HANDLES, EOK,
                            count, handles) != 0)
        return 1;
    for (unsigned i = 0; i < count; i++) {
        if (handles[i].device_id == VIRTIO_ID_NET &&
            handles[i].iommu_phandle != RTMUX_RESOURCE_IOMMU_PHANDLE_NONE &&
            handles[i].reserved != RTMUX_RESOURCE_IOMMU_STREAM_NONE) {
            stream = RTMUX_RESOURCE_IOMMU_STREAM_DECODE(handles[i].reserved);
            found = true;
        }
        rtmux_hwres_release_range(handles[i].base, handles[i].length,
                                  RTMUX_HWRES_MEMORY);
    }
    if (!found) {
        puts("lkl-contain: FAIL no virtio-net IOMMU phandle/stream metadata");
        return 1;
    }
    *out_stream = stream;
    return 0;
}

static int virtio_iommu_metadata(void)
{
    uint32_t stream;

    if (virtio_iommu_metadata_stream(&stream) != 0)
        return 1;
    if (stream != 42) {
        printf("lkl-contain: FAIL virtio-net IOMMU stream=%u\n", stream);
        return 1;
    }
    puts("lkl-contain: virtio-net IOMMU metadata preserved");
    return 0;
}

static int virtio_iommu_domain_preflight(void)
{
    uint32_t stream;
    char did[16];
    char *argv[] = {
        "lkl-iommu-probe",
        "--domain",
        "--did",
        did,
        "--iova",
        "0x400000",
        NULL,
    };
    pid_t child;
    int status;

    if (virtio_iommu_metadata_stream(&stream) != 0)
        return 1;
    if (stream != 42) {
        printf("lkl-contain: FAIL virtio-net IOMMU stream=%u\n", stream);
        return 1;
    }
    snprintf(did, sizeof(did), "%u", stream);
    puts("lkl-contain: virtio-net IOMMU metadata preserved");
    int rc = posix_spawn(&child, "/bin/lkl-iommu-probe", NULL, NULL, argv,
                         environ);
    if (rc != EOK) {
        printf("lkl-contain: FAIL spawn IOMMU domain preflight: %s\n",
               strerror(rc));
        return 1;
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        puts("lkl-contain: FAIL IOMMU domain preflight child");
        return 1;
    }
    return 0;
}

#define BAD_REGISTER_CASES(X)                                                  \
    X(stale_generation, "stale-generation", { handle.generation = 0; })        \
    X(revoked, "revoked", { handle.flags = RTMUX_RESOURCE_HANDLE_F_REVOKED; }) \
    X(iommu_phandle_without_stream, "iommu-phandle-without-stream",            \
      { handle.iommu_phandle = 17; })                                          \
    X(iommu_stream_without_phandle, "iommu-stream-without-phandle",            \
      { handle.reserved = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(7); })

#define DEFINE_BAD_REGISTER_CASE(case_id, label, setup)           \
    static struct rtmux_resource_handle bad_register_##case_id(void) \
    {                                                             \
        struct rtmux_resource_handle handle = {                   \
            .kind = RTMUX_RESOURCE_MMIO,                          \
            .base = 0x1000,                                       \
            .length = 0x1000,                                     \
            .device_id = CONTAIN_DELEGATE_DEVICE,                 \
            .generation = 1,                                      \
            .irq_vector = 1,                                      \
        };                                                        \
                                                                  \
        setup;                                                    \
        return handle;                                            \
    }

BAD_REGISTER_CASES(DEFINE_BAD_REGISTER_CASE)

struct bad_register_case {
    const char *macro_name;
    const char *name;
    struct rtmux_resource_handle (*make)(void);
};

#define BAD_REGISTER_ENTRY(case_id, label, setup)                             \
    {                                                                         \
        .macro_name = #case_id,                                                \
        .name = label,                                                         \
        .make = bad_register_##case_id,                                        \
    },

static const struct bad_register_case bad_register_cases[] = {
    BAD_REGISTER_CASES(BAD_REGISTER_ENTRY)
};

static int test_typed_mmio_handles(uint64_t base,
                                   uint64_t extra_base,
                                   struct rtmux_resource_handle *stale)
{
    struct rtmux_resource_handle valid = {
        .kind = RTMUX_RESOURCE_MMIO,
        .base = base,
        .length = 0x1000,
        .device_id = CONTAIN_DELEGATE_DEVICE,
        .generation = 1,
        .irq_vector = 1,
        .reserved = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(7),
        .iommu_phandle = 17,
    };
    struct rtmux_resource_handle duplicate[] = {
        valid,
        {
            .kind = RTMUX_RESOURCE_MMIO,
            .base = base,
            .length = 0x1000,
            .device_id = CONTAIN_DELEGATE_DEVICE,
            .generation = 2,
            .irq_vector = 1,
            .reserved = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(8),
            .iommu_phandle = 18,
        },
    };
    struct rtmux_resource_handle pair[] = {
        valid,
        {
            .kind = RTMUX_RESOURCE_MMIO,
            .base = extra_base,
            .length = 0x1000,
            .device_id = CONTAIN_DELEGATE_DEVICE,
            .generation = 2,
            .irq_vector = 1,
            .reserved = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(8),
            .iommu_phandle = 18,
        },
    };
    char *argv[] = {"lkl-contain-test", "--virtio-query-only", NULL};
    struct rtmux_resource_handle returned[2];
    pid_t child;
    int status;

    for (size_t i = 0;
         i < sizeof(bad_register_cases) / sizeof(bad_register_cases[0]); i++) {
        if (expect_bad_register(bad_register_cases[i].make(),
                                bad_register_cases[i].name) != 0) {
            printf("lkl-contain: FAIL bad-register case %s (%s)\n",
                   bad_register_cases[i].name,
                   bad_register_cases[i].macro_name);
            return -1;
        }
    }

    if (register_handles(duplicate, 2) != 0)
        return -1;
    if (expect_delegate(2, EINVAL) != 0)
        return -1;
    if (register_handles(pair, 2) != 0)
        return -1;
    if (expect_delegate(2, EOK) != 0)
        return -1;

    if (posix_spawn(&child, "/bin/lkl-contain-test", NULL, NULL, argv,
                    environ) != EOK) {
        printf("lkl-contain: FAIL spawn wrong-pid query: %s\n",
               strerror(errno));
        return -1;
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        puts("lkl-contain: FAIL wrong-pid query child");
        return -1;
    }

    if (expect_query(1, EMSGSIZE, 0, NULL) != 0)
        return -1;
    if (expect_query(2, EOK, 2, returned) != 0)
        return -1;
    if (returned[0].reserved != RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(7) ||
        returned[1].reserved != RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(8) ||
        returned[0].iommu_phandle != 17 || returned[1].iommu_phandle != 18) {
        printf("lkl-contain: FAIL IOMMU metadata lost %#x/%u %#x/%u\n",
               returned[0].reserved, returned[0].iommu_phandle,
               returned[1].reserved, returned[1].iommu_phandle);
        rtmux_hwres_release_range(returned[0].base, returned[0].length,
                                  RTMUX_HWRES_MEMORY);
        rtmux_hwres_release_range(returned[1].base, returned[1].length,
                                  RTMUX_HWRES_MEMORY);
        return -1;
    }
    *stale = returned[0];

    if (expect_query(2, ENODEV, 0, NULL) != 0) {
        rtmux_hwres_release_range(returned[0].base, returned[0].length,
                                  RTMUX_HWRES_MEMORY);
        rtmux_hwres_release_range(returned[1].base, returned[1].length,
                                  RTMUX_HWRES_MEMORY);
        return -1;
    }
    rtmux_hwres_release_range(returned[0].base, returned[0].length,
                              RTMUX_HWRES_MEMORY);
    rtmux_hwres_release_range(returned[1].base, returned[1].length,
                              RTMUX_HWRES_MEMORY);
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t base;
    uint64_t claimed_base = 0;
    uint64_t delegated_base = 0;
    uint64_t unclaimed_base = 0;
    struct rtmux_resource_handle stale_handle;
    size_t len;
    void *p;
    int memfd;
    bool devmem_baseline = false;
    unsigned char byte;

    if (argc == 2 && !strcmp(argv[1], "--virtio-query-only"))
        return virtio_query_only();
    if (argc == 2 && !strcmp(argv[1], "--virtio-iommu-metadata"))
        return virtio_iommu_metadata();
    if (argc == 2 && !strcmp(argv[1], "--virtio-iommu-domain-preflight"))
        return virtio_iommu_domain_preflight();
    memset(&stale_handle, 0, sizeof(stale_handle));

    if (find_mmio(&base, &len) != 0) {
        puts("lkl-contain: FAIL no hwinfo device with a location tag");
        return 1;
    }
    if (len > 4096)
        len = 4096;

    /* Baseline: the mapping succeeds before containment. */
    p = mmap(NULL, len, PROT_READ | PROT_NOCACHE, MAP_PHYS | MAP_SHARED, NOFD,
             (off_t) base);
    if (p == MAP_FAILED) {
        printf("lkl-contain: FAIL baseline MAP_PHYS @%#lx: %s\n",
               (unsigned long) base, strerror(errno));
        return 1;
    }
    munmap(p, len);

    if (find_claimable_ranges(&claimed_base, &delegated_base,
                              &unclaimed_base) != 0) {
        puts("lkl-contain: FAIL no claimable hwres memory ranges");
        return 1;
    }
    if (test_typed_mmio_handles(delegated_base, unclaimed_base,
                                &stale_handle) != 0)
        return 1;

    /* Baseline the fd-backed /dev/mem mmap path if it is in this namespace. Do
     * not pre-read arbitrary physical addresses; after containment, raw
     * /dev/mem I/O must fail before roottask maps physical memory at all.
     */
    memfd = open("/dev/mem", O_RDONLY);
    if (memfd >= 0) {
        p = mmap(NULL, len, PROT_READ | PROT_NOCACHE, MAP_SHARED, memfd,
                 (off_t) base);
        if (p != MAP_FAILED) {
            devmem_baseline = true;
            munmap(p, len);
        }
    }

    if (rtmux_phys_contain() != EOK) {
        printf("lkl-contain: FAIL rtmux_phys_contain: %s\n", strerror(errno));
        return 1;
    }

    {
        hwres_claim_t claim = {
            .length = 4096,
            .align = 4096,
            .start = unclaimed_base,
            .end = unclaimed_base + 4095U,
            .flags = HWRES_MEMORY | HWRES_FLAG_RANGE,
            .name = "",
        };

        if (hwres_claim_one(&claim) != -1 || errno != EPERM) {
            printf("lkl-contain: FAIL self hwres claim after contain: %s\n",
                   strerror(errno));
            return 1;
        }
        if (hwres_create_one(&claim) != -1 || errno != EPERM) {
            printf("lkl-contain: FAIL self hwres create after contain: %s\n",
                   strerror(errno));
            return 1;
        }
    }

    /* Anonymous MAP_PHYS (DMA-style buffer) must remain allowed. */
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (p == MAP_FAILED) {
        printf("lkl-contain: FAIL anon MAP_PHYS refused after contain: %s\n",
               strerror(errno));
        return 1;
    }
    munmap(p, 4096);

    p = mmap(NULL, 4096, PROT_READ | PROT_NOCACHE, MAP_PHYS | MAP_SHARED, NOFD,
             (off_t) claimed_base);
    if (p == MAP_FAILED) {
        printf("lkl-contain: FAIL claimed MAP_PHYS refused after contain: %s\n",
               strerror(errno));
        return 1;
    }
    munmap(p, 4096);

    p = mmap(NULL, 4096, PROT_READ | PROT_NOCACHE, MAP_PHYS | MAP_SHARED, NOFD,
             (off_t) unclaimed_base);
    if (p != MAP_FAILED) {
        puts("lkl-contain: FAIL unclaimed MAP_PHYS allowed after contain");
        munmap(p, 4096);
        return 1;
    }
    if (errno != EPERM) {
        printf("lkl-contain: FAIL unclaimed MAP_PHYS errno=%d (want EPERM)\n",
               errno);
        return 1;
    }

    p = mmap(NULL, (size_t) stale_handle.length, PROT_READ | PROT_NOCACHE,
             MAP_PHYS | MAP_SHARED, NOFD, (off_t) stale_handle.base);
    if (p != MAP_FAILED) {
        puts("lkl-contain: FAIL consumed typed handle mapped after release");
        munmap(p, (size_t) stale_handle.length);
        return 1;
    }
    if (errno != EPERM) {
        printf("lkl-contain: FAIL consumed typed handle errno=%d "
               "(want EPERM)\n", errno);
        return 1;
    }

    /* The specific-address mapping that just worked must now be refused. */
    p = mmap(NULL, len, PROT_READ | PROT_NOCACHE, MAP_PHYS | MAP_SHARED, NOFD,
             (off_t) base);
    if (p != MAP_FAILED) {
        puts("lkl-contain: FAIL specific-address MAP_PHYS allowed after contain");
        munmap(p, len);
        return 1;
    }
    if (errno != EPERM) {
        printf("lkl-contain: FAIL contained MAP_PHYS errno=%d (want EPERM)\n",
               errno);
        return 1;
    }

    /* The fd-backed /dev/mem path must be sealed too (it acquires MAP_PHYS
     * inside devmem_check, after the explicit-flag branch).
     */
    if (devmem_baseline) {
        p = mmap(NULL, len, PROT_READ | PROT_NOCACHE, MAP_SHARED, memfd,
                 (off_t) base);
        if (p != MAP_FAILED) {
            puts("lkl-contain: FAIL /dev/mem mapping allowed after contain");
            munmap(p, len);
            close(memfd);
            return 1;
        }
        if (errno != EPERM) {
            printf("lkl-contain: FAIL /dev/mem map errno=%d (want EPERM)\n",
                   errno);
            close(memfd);
            return 1;
        }
    }
    if (memfd >= 0) {
        if (pread(memfd, &byte, 1, (off_t) base) != -1) {
            puts("lkl-contain: FAIL /dev/mem read allowed after contain");
            close(memfd);
            return 1;
        }
        if (errno != EPERM) {
            printf("lkl-contain: FAIL /dev/mem read errno=%d (want EPERM)\n",
                   errno);
            close(memfd);
            return 1;
        }
    }
    if (memfd >= 0)
        close(memfd);

    printf("lkl-contain: typed MMIO misuse sealed, claimed MAP_PHYS allowed, "
           "unclaimed sealed, /dev/mem mmap(%s), /dev/mem read(%s), "
           "anon exempt\n",
           devmem_baseline ? "checked" : "unavailable",
           memfd >= 0 ? "checked" : "unavailable");
    puts("lkl-contain: PASS");
    return 0;
}
