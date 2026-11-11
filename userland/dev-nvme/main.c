/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Read-only driver for QEMU virt + SiFive Unmatched. After PCI attach,
 * controller reset/enable, admin queue, Identify, and I/O queue creation, hand
 * control to the I/O service loop so /dev/nvme0n1 becomes usable by any POSIX
 * client (dd, mount, cat, ...).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/ioserv.h>
#include "pci.h"

#include "nvme.h"

#define NVME_CCODE_ANY_REGIF PCI_CCODE(0x01u, 0x08u, PCI_CCODE_REG_IF_ANY)
#define NVME_CCODE_CLASS_SUBCLASS PCI_CCODE(0x01u, 0x08u, 0)
#define NVME_FIND_RETRIES 50u
#define NVME_FIND_RETRY_NS 100000000L

/* Single driver-global controller; /dev/nvme0n1 binds to its first NS. */
static nvme_ctrl_t ctrl;

static int validate_bar0_handle(const pci_ba_t *bar)
{
    const struct rtmux_resource_handle *handle = &bar->handle;

    if (bar->bar_num != 0)
        return EINVAL;
    if (handle->kind != RTMUX_RESOURCE_PCI_BAR)
        return EINVAL;
    if (handle->owner_pid != getpid())
        return EINVAL;
    if ((handle->device_id & 0xffff00U) != NVME_CCODE_CLASS_SUBCLASS)
        return EINVAL;
    if (handle->generation == 0 ||
        (handle->flags & RTMUX_RESOURCE_HANDLE_F_REVOKED) != 0 ||
        handle->reserved != 0)
        return EINVAL;
    if (handle->length < NVME_CTRL_REG_SIZE || handle->base == 0)
        return EINVAL;
    if ((handle->base & ~(uint64_t) 0xfff) != (bar->addr & ~(uint64_t) 0xfff))
        return EINVAL;

    return EOK;
}

static int map_bar0(nvme_ctrl_t *c, pci_devhdl_t hdl)
{
    pci_ba_t bars[6];
    int nba = 6;
    pci_err_t err = pci_device_read_ba(hdl, &nba, bars, PCI_REQ_MANDATORY);
    if (err != PCI_ERR_OK || nba < 1) {
        fprintf(stderr, "dev-nvme: pci_device_read_ba: %s\n",
                pci_strerror(err));
        return -1;
    }
    if (bars[0].type != PCI_BAR_MEM) {
        fprintf(stderr, "dev-nvme: BAR0 is not a memory BAR\n");
        return -1;
    }
    if (validate_bar0_handle(&bars[0]) != EOK) {
        fprintf(stderr, "dev-nvme: BAR0 lacks delegated PCI BAR handle\n");
        return -1;
    }

    c->bar0_phys = bars[0].handle.base & ~(uint64_t) 0xfff;
    c->bar0_size = bars[0].handle.length;
    if (c->bar0_size < NVME_CTRL_REG_SIZE)
        c->bar0_size = NVME_CTRL_REG_SIZE;

    c->bar0 = mmap(NULL, c->bar0_size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                   MAP_PHYS | MAP_SHARED, NOFD, (off_t) c->bar0_phys);
    if (c->bar0 == MAP_FAILED) {
        fprintf(stderr, "dev-nvme: mmap BAR0: %s\n", strerror(errno));
        return -1;
    }

    uint64_t cap = nvme_rd64(c, NVME_REG_CAP);
    c->mqes = NVME_CAP_MQES(cap);
    c->dstrd = NVME_CAP_DSTRD(cap);
    c->mpsmin = NVME_CAP_MPSMIN(cap);
    c->doorbell_stride = 4u << c->dstrd;
    return 0;
}

static void trim_field(char *dst, const char *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0'))
        dst[--n] = '\0';
}

static void print_controller(const nvme_id_ctrl_t *id)
{
    char mn[41], sn[21], fr[9];

    trim_field(mn, id->mn, 40);
    trim_field(sn, id->sn, 20);
    trim_field(fr, id->fr, 8);

    fprintf(stderr,
            "nvme: controller VID:DID %04x:%04x  ctrl-id %u\n"
            "  Model:    \"%s\"\n"
            "  Serial:   \"%s\"\n"
            "  Firmware: \"%s\"\n",
            id->vid, id->ssvid, id->cntlid, mn, sn, fr);
}

static uint64_t ns_lba_size(const nvme_id_ns_t *ns)
{
    uint32_t fmt = ns->lbaf[ns->flbas & 0x0F];
    unsigned lbads = NVME_LBAF_LBADS(fmt);
    return (lbads >= 9 && lbads <= 20) ? (1ULL << lbads) : 0;
}

static void print_namespace(uint32_t nsid, const nvme_id_ns_t *ns)
{
    uint64_t lba_size = ns_lba_size(ns);
    uint64_t total_bytes = ns->nsze * lba_size;

    fprintf(stderr, "nvme: ns %u  size=%lu LBAs  lba=%lu B  capacity=%lu MiB\n",
            nsid, (unsigned long) ns->nsze, (unsigned long) lba_size,
            (unsigned long) (total_bytes / (1024 * 1024)));
}

static pci_bdf_t find_nvme_controller(void)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = NVME_FIND_RETRY_NS};
    pci_bdf_t bdf;

    for (unsigned attempt = 0; attempt < NVME_FIND_RETRIES; attempt++) {
        bdf = pci_device_find_class(0, NVME_CCODE_ANY_REGIF);
        if (bdf != PCI_BDF_NONE)
            return bdf;
        nanosleep(&delay, NULL);
    }
    return PCI_BDF_NONE;
}

/* Run the full bring-up sequence. On success the ctrl struct holds everything
 * the I/O service needs; return 0. Everything printed here is diagnostic — a
 * healthy boot on QEMU virt or U740 looks the same.
 */
static int bringup(nvme_ctrl_t *c, pci_devhdl_t hdl)
{
    if (map_bar0(c, hdl) != 0)
        return -1;

    fprintf(stderr, "dev-nvme: controller at %02x:%02x.%x BAR0 %#lx\n", c->bus,
            c->devfn >> 3, c->devfn & 7, (unsigned long) c->bar0_phys);

    if (nvme_reset(c) != 0) {
        fprintf(stderr, "dev-nvme: reset failed\n");
        return -1;
    }
    if (nvme_setup_admin_queues(c) != 0)
        return -1;
    if (nvme_enable(c) != 0) {
        fprintf(stderr, "dev-nvme: enable failed\n");
        return -1;
    }
    fprintf(stderr, "dev-nvme: controller enabled (CSTS.RDY=1)\n");

    nvme_id_ctrl_t *id_ctrl = malloc(sizeof(*id_ctrl));
    if (!id_ctrl)
        return -1;
    if (nvme_identify_controller(c, id_ctrl) != 0) {
        free(id_ctrl);
        return -1;
    }
    print_controller(id_ctrl);
    free(id_ctrl);

    uint32_t *nsids = calloc(1024, sizeof(uint32_t));
    if (!nsids)
        return -1;
    if (nvme_identify_active_ns_list(c, nsids) != 0) {
        free(nsids);
        return -1;
    }

    nvme_id_ns_t *id_ns = malloc(sizeof(*id_ns));
    if (!id_ns) {
        free(nsids);
        return -1;
    }

    /* Bind /dev/nvme0n1 to the first active namespace. Any further namespaces
     * are still Identified so they show in the log.
     */
    unsigned active = 0;
    for (unsigned i = 0; i < 1024 && nsids[i] != 0; i++) {
        if (nvme_identify_namespace(c, nsids[i], id_ns) != 0)
            continue;
        print_namespace(nsids[i], id_ns);
        if (active == 0) {
            c->ns_nsid = nsids[i];
            c->ns_lba_size = ns_lba_size(id_ns);
            c->ns_nlba = id_ns->nsze;
            c->ns_size_bytes = c->ns_nlba * c->ns_lba_size;
        }
        active++;
    }
    free(id_ns);
    free(nsids);

    if (active == 0 || c->ns_lba_size == 0) {
        fprintf(stderr, "dev-nvme: no usable namespace\n");
        return -1;
    }

    if (nvme_create_io_queues(c) != 0)
        return -1;

    return 0;
}

int main(int argc, char **argv)
{
    pci_bdf_t bdf;
    pci_devhdl_t hdl;
    pci_err_t err;
    dispatch_t *dpp;

    (void) argc;
    (void) argv;
    memset(&ctrl, 0, sizeof(ctrl));
    setvbuf(stderr, NULL, _IONBF, 0);

    /* dispatch_create() and the I/O service setup call must run BEFORE any
     * /dev/pci activity. If we call pci_device_attach() first, a later
     * ioserv_attach() against /dev/nvme0n1 fails with errno = -16 (-EBUSY). The
     * suspected cause is that the PCI client's msg_send to /dev/pci takes a
     * channel/connection slot that the I/O service layer expects to allocate
     * itself on its first call.
     *
     * Calling ioserv_attach() with a null path just initializes the internals —
     * no path is registered yet — which is exactly what we need to run before
     * /dev/pci activity.
     */
    dpp = dispatch_create();
    if (!dpp) {
        fprintf(stderr, "dev-nvme: dispatch_create failed\n");
        return 1;
    }
    {
        static const ioserv_connect_ops_t _cf = {
            .nfuncs = IOSERV_CONNECT_NFUNCS,
        };
        static const ioserv_ops_t _iof = {
            .nfuncs = IOSERV_IO_NFUNCS,
        };
        if (ioserv_attach(dpp, NULL, NULL, _FTYPE_ANY, 0, &_cf, &_iof, NULL) ==
            -1) {
            fprintf(stderr, "dev-nvme: ioserv_attach(NULL) failed: %s\n",
                    strerror(errno));
            return 1;
        }
    }

    bdf = find_nvme_controller();
    if (bdf == PCI_BDF_NONE) {
        fprintf(stderr, "dev-nvme: no NVMe controller found after retry\n");
        return 0;
    }
    ctrl.bus = (uint8_t) PCI_BUS(bdf);
    ctrl.devfn = (uint8_t) ((PCI_DEV(bdf) << 3) | PCI_FUNC(bdf));

    hdl = pci_device_attach(bdf, PCI_ATTACH_EXCLUSIVE_OWNER, &err);
    if (!hdl) {
        fprintf(stderr, "dev-nvme: pci_device_attach: %s\n", pci_strerror(err));
        return 1;
    }
    if (rtmux_phys_contain() != EOK) {
        fprintf(stderr, "dev-nvme: rtmux_phys_contain: %s\n", strerror(errno));
        pci_device_detach(hdl);
        return 1;
    }

    /* Fetch the INTx IRQ the server assigned during attach. */
    {
        int nirq = 1;
        pci_irq_t irq;
        pci_err_t rc = pci_device_read_irq(hdl, &nirq, &irq);
        if (rc != PCI_ERR_OK || nirq < 1 || irq < 0) {
            fprintf(stderr, "dev-nvme: pci_device_read_irq: %s\n",
                    pci_strerror(rc));
            pci_device_detach(hdl);
            return 1;
        }
        ctrl.irq = (uint32_t) irq;
        ctrl.vector = PLIC_VECTOR_BASE + ctrl.irq;
    }

    if (bringup(&ctrl, hdl) != 0) {
        nvme_teardown_io_queues(&ctrl);
        nvme_teardown_admin_queues(&ctrl);
        if (ctrl.bar0 && ctrl.bar0 != MAP_FAILED)
            munmap((void *) ctrl.bar0, ctrl.bar0_size);
        pci_device_detach(hdl);
        return 1;
    }

    /* Hand off to the I/O service — never returns unless something fatal. The
     * service publishes GPT-derived paths using the polling command path first,
     * then starts the IST before servicing client reads.
     */
    return nvme_ioserv_run(&ctrl, dpp);
}
