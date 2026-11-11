/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Implements pdrvr_entry_t for RISC-V platforms using standard ECAM (Enhanced
 * Configuration Access Mechanism) for PCIe config space.
 *
 * Platform geometry (ECAM base/size, address windows, INTA IRQ) is taken from
 * the syspage hwinfo section, populated by the kernel at boot from the FDT PCIe
 * host-bridge node. No hardcoded platform table.
 *
 * ECAM address formula: base + (bus << 20) | (devfn << 12) | offset
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/slog.h>
#include <sys/rtmux-core.h>
#include <sys/virtio-mmio-msg.h>
#include <pci.h>
#include <hw/hwinfo-lookup.h>
#include <pcidrvr.h>
#include "hwres-client.h"
#include "slogcodes.h"
#include "globals.h"

/* Per-platform PCIe address windows, discovered from syspage or command line.
 *
 * QEMU virt:
 *   IO:   CPU 0x03000000 → PCI 0x00000000, size 0x10000
 *   MEM:  CPU 0x40000000 → PCI 0x40000000, size 0x40000000
 *   PREF: CPU 0x400000000 → PCI 0x400000000, size 0x400000000
 *
 * FU740 Unmatched:
 *   IO:   CPU 0x60080000 → PCI 0x60080000, size 0x10000
 *   MEM:  CPU 0x60090000 → PCI 0x60090000, size 0xff70000
 *         CPU 0x70000000 → PCI 0x70000000, size 0x10000000
 *   PREF: CPU 0x2000000000 → PCI 0x2000000000, size 0x2000000000
 */

#define MAX_ADDR_WINDOWS 8
#define ECAM_MMAP_MAX (256UL << 20)
#define DW_DBI_MMAP_MAX (4UL << 20)

typedef struct {
    uint64_t cpu_base;
    uint64_t pci_base;
    uint64_t size;
    uint32_t type; /* PCI_MEM_SPACE, PCI_IO_SPACE, PCI_MEM_PREFETCH */
} pci_window_t;

typedef struct _ecam_dev {
    uint8_t *ecam_vaddr; /* mmap'd ECAM config space */
    uint64_t ecam_phys;  /* physical base of ECAM region */
    uint32_t ecam_size;  /* size of ECAM region */
    uint8_t lastbus;     /* highest bus number */
    uint8_t num_windows;
    uint8_t rsvd[2];
    pci_window_t windows[MAX_ADDR_WINDOWS];
    /* PLIC IRQ numbers for INTA..INTD */
    uint32_t irq_base; /* first PLIC IRQ for INTA */
    /* DesignWare PCIe MSI controller (FU740 / Unmatched). dbi_base / dbi_size
     * describe the DBI register window (the MSI regs live at DBI +
     * 0x820..0x82F). msi_plic_irq is the aggregate PLIC IRQ raised when any MSI
     * vector fires. Zero on platforms without a DW MSI controller.
     */
    paddr_t dbi_base;
    size_t dbi_size;
    uint32_t msi_plic_irq;
    uint32_t msi_irq; /* PLIC IRQ for MSI (FU740 only) */
    hwres_claim_t ecam_claim;
    hwres_claim_t dbi_claim;
    int ecam_claimed;
    int dbi_claimed;
} ecam_dev_t;

/* Forward declarations */
static int ecam_attach(char *options, void **hdl);
static int ecam_detach(void *hdl);
static int ecam_cnfg_bridge(void *hdl,
                            uint32_t bus,
                            uint32_t devfunc,
                            pci_bus_t *pbus);
static int ecam_read_cnfg(void *hdl,
                          uint32_t bus,
                          uint32_t devfunc,
                          uint32_t offset,
                          uint32_t width,
                          void *data);
static int ecam_write_cnfg(void *hdl,
                           uint32_t bus,
                           uint32_t devfunc,
                           uint32_t offset,
                           uint32_t width,
                           const uint32_t data);
static int ecam_special_cycle(void *hdl, uint32_t bus, uint32_t data);
static int ecam_map_irq(void *hdl,
                        uint32_t bus,
                        uint32_t devfunc,
                        uint32_t line,
                        uint32_t pin,
                        uint32_t flags);
static int ecam_avail_irq(void *hdl,
                          uint32_t bus,
                          uint32_t devfunc,
                          uint32_t *list,
                          uint32_t *nelm,
                          int *msi_info);
static int ecam_map_addr(void *hdl,
                         uint64_t iaddr,
                         uint64_t *oaddr,
                         uint32_t type);

static int delegate_boot_mmio(uint32_t device_id,
                              uint64_t base,
                              size_t size,
                              struct rtmux_resource_handle *handle)
{
    return rtmux_delegate_device(device_id, size, base, handle);
}

static void claim_from_handle(hwres_claim_t *claim,
                              const struct rtmux_resource_handle *handle,
                              size_t size)
{
    *claim = (hwres_claim_t){
        .start = handle->base,
        .end = handle->base + size - 1,
        .length = size,
        .flags = HWRES_MEMORY | HWRES_FLAG_RANGE,
    };
}

/* The driver entry table — exported for the PCI server to use. */
pdrvr_entry_t ecam_entry = {
    .attach = ecam_attach,
    .detach = ecam_detach,
    .cnfg_bridge = ecam_cnfg_bridge,
    .read_cnfg = ecam_read_cnfg,
    .write_cnfg = ecam_write_cnfg,
    .special_cycle = ecam_special_cycle,
    .map_irq = ecam_map_irq,
    .avail_irq = ecam_avail_irq,
    .map_addr = ecam_map_addr,
};

/* Read PCIe host-bridge geometry from syspage hwinfo.
 *
 * The kernel (board.c → arch-init.c:init_hwinfo) publishes a "pci" bus
 * carrying:
 *   - one "location" tag  : ECAM physical base + size
 *   - one "irq" tag       : PLIC vector for INTA (INTB..INTD follow)
 *   - N "pci_window" tags : host-bridge address windows (IO / MEM / prefetch)
 *   - one "optstr" tag    : FDT compatible string ("pci-host-ecam-generic"
 * etc.)
 *
 * Returns 0 on success, -1 if the kernel didn't publish a PCIe bus (which is a
 * kernel bug, not a runtime condition we try to paper over).
 */
static int discover_from_hwinfo(ecam_dev_t *dev, char platform_out[64])
{
    hwi_tag *bus, *tag;

    bus = hwinfo_find_bus(HWI_ITEM_BUS_PCI, NULL);
    if (!bus)
        return -1;

    tag = hwinfo_find_tag_after(bus, HWI_TAG_NAME_location);
    if (!tag)
        return -1;
    dev->ecam_phys = tag->location.base;
    dev->ecam_size = tag->location.len;
    dev->lastbus = 255;

    tag = hwinfo_find_tag_after(bus, HWI_TAG_NAME_irq);
    dev->irq_base = tag ? tag->irq.vector : 0;

    /* MSI aggregate IRQ discovery: the kernel publishes the DW PCIe MSI
     * controller (when present) as a child device "dw_pcie_msi" under the PCI
     * bus, carrying its DBI window as "location" and its PLIC IRQ as "irq".
     * QEMU virt doesn't have one, so these stay zero.
     */
    dev->msi_irq = 0;
    dev->dbi_base = 0;
    dev->dbi_size = 0;
    dev->msi_plic_irq = 0;
    hwi_tag *msi_dev = NULL;
    /* Scan forward from the pci bus for any child devices. */
    while ((msi_dev = hwinfo_find_device("dw_pcie_msi", msi_dev))) {
        hwi_tag *loc = hwinfo_find_tag_after(msi_dev, HWI_TAG_NAME_location);
        hwi_tag *irqt = hwinfo_find_tag_after(msi_dev, HWI_TAG_NAME_irq);
        if (loc && irqt) {
            dev->dbi_base = loc->location.base;
            dev->dbi_size = loc->location.len;
            dev->msi_plic_irq = irqt->irq.vector;
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "ECAM: DW MSI DBI %#lx/%#lx PLIC IRQ %u",
                      (unsigned long) dev->dbi_base,
                      (unsigned long) dev->dbi_size, dev->msi_plic_irq);
            break;
        }
    }

    dev->num_windows = 0;
    tag = bus;
    while ((tag = hwinfo_find_tag_after(tag, HWI_TAG_NAME_pci_window)) !=
               NULL &&
           dev->num_windows < MAX_ADDR_WINDOWS) {
        pci_window_t *w = &dev->windows[dev->num_windows++];
        uint32_t hwi_flags = tag->pci_window.flags;

        w->cpu_base = tag->pci_window.cpu_addr;
        w->pci_base = tag->pci_window.pci_addr;
        w->size = tag->pci_window.size;
        w->type = 0;
        if (hwi_flags & HWI_PCI_WINDOW_IO)
            w->type |= PCI_IO_SPACE;
        if (hwi_flags & HWI_PCI_WINDOW_MEM)
            w->type |= PCI_MEM_SPACE;
        if (hwi_flags & HWI_PCI_WINDOW_PREFETCH)
            w->type |= PCI_MEM_PREFETCH;
    }

    tag = hwinfo_find_tag_after(bus, HWI_TAG_NAME_optstr);
    if (tag && platform_out) {
        const char *s = hwinfo_strings() + tag->optstr.string;
        strncpy(platform_out, s, 63);
        platform_out[63] = '\0';
    } else if (platform_out) {
        platform_out[0] = '\0';
    }
    return 0;
}

/* ecam_attach — initialize ECAM backend, mmap config space */

static int ecam_attach(char *options, void **hdl)
{
    ecam_dev_t *dev;
    char platform[64];

    (void) options;

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return ENOMEM;

    if (discover_from_hwinfo(dev, platform) < 0) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "ECAM: no PCIe host bridge in syspage hwinfo");
        free(dev);
        return ENODEV;
    }

    pci_slogf(_SLOGC_PCI, _SLOG_INFO, "ECAM: platform %s",
              platform[0] ? platform : "(unknown)");
    pci_slogf(_SLOGC_PCI, _SLOG_INFO, "ECAM: phys 0x%llx size 0x%x",
              (unsigned long long) dev->ecam_phys, dev->ecam_size);

    /* Cap the mmap length. Full ECAM is 256 bus × 32 dev × 8 fn × 4 KiB = 256
     * MiB; QEMU virt declares exactly that, FU740's U-Boot DTB can report
     * bigger (2 GiB on the HiFive Unmatched seen in testing). Mapping 2 GiB of
     * VA fails with ENOMEM on the server's address space, so clip to 256 MiB --
     * it covers every valid bus/device/function and is only 2x our largest real
     * target.
     */
    size_t mmap_size =
        dev->ecam_size > ECAM_MMAP_MAX ? ECAM_MMAP_MAX : dev->ecam_size;
    if (mmap_size != dev->ecam_size) {
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "ECAM: clipping mmap size from 0x%zx to 0x%zx",
                  (size_t) dev->ecam_size, mmap_size);

        /* mmap_size is bounded above by ECAM_MMAP_MAX (256 MiB), which fits in
         * uint32_t — safe narrowing.
         */
        dev->ecam_size = (uint32_t) mmap_size;
    }

    struct rtmux_resource_handle ecam_handle;
    int rc = delegate_boot_mmio(RTMUX_DEVICE_PCI_ECAM, dev->ecam_phys,
                                mmap_size, &ecam_handle);
    if (rc != EOK) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "ECAM: typed handle %#llx/%#zx failed: %s",
                  (unsigned long long) dev->ecam_phys, mmap_size, strerror(rc));
        free(dev);
        return EIO;
    }
    claim_from_handle(&dev->ecam_claim, &ecam_handle, mmap_size);
    dev->ecam_claimed = 1;

    /* Map the ECAM region into our address space */
    dev->ecam_vaddr =
        mmap(NULL, mmap_size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
             MAP_PHYS | MAP_SHARED, NOFD, (off_t) dev->ecam_phys);
    if (dev->ecam_vaddr == MAP_FAILED) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "ECAM: mmap failed: %s",
                  strerror(errno));
        hwres_release_range(&dev->ecam_claim);
        free(dev);
        return EIO;
    }

    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
              "ECAM: mapped at 0x%lx, %d address windows",
              (unsigned long) dev->ecam_vaddr, dev->num_windows);

    if (dev->dbi_base) {
        size_t dbi_claim_size =
            dev->dbi_size > DW_DBI_MMAP_MAX ? DW_DBI_MMAP_MAX : dev->dbi_size;

        struct rtmux_resource_handle dbi_handle;
        rc = delegate_boot_mmio(RTMUX_DEVICE_PCI_DBI, dev->dbi_base,
                                dbi_claim_size, &dbi_handle);
        if (rc != EOK) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                      "ECAM: DBI typed handle %#lx/%#zx failed: %s",
                      (unsigned long) dev->dbi_base, dbi_claim_size,
                      strerror(rc));
            munmap(dev->ecam_vaddr, dev->ecam_size);
            hwres_release_range(&dev->ecam_claim);
            free(dev);
            return EIO;
        }
        claim_from_handle(&dev->dbi_claim, &dbi_handle, dbi_claim_size);
        dev->dbi_claimed = 1;
    }

    /* On DesignWare PCIe (FU740), the host's "config" window is not a flat ECAM
     * -- the iATU encodes CFG TLPs with a different bit layout, so each
     * non-root config access must re-aim outbound region 0 first. Bring up the
     * iATU now so ecam_{read,write}_cnfg can use it on the very first
     * scan_all() probe.
     */
    if (dev->dbi_base) {
        if (pci_dw_atu_init(dev->dbi_base, dev->dbi_size) != 0) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                      "ECAM: DW iATU init failed -- bus scan will not work");
        }
    }

    /* Bring up the DesignWare PCIe MSI controller if the platform has one
     * (FU740 / Unmatched). This is where posted MSI writes get trapped and
     * turned into a PLIC IRQ for pci-server to demux. It reuses the DBI mapping
     * created by pci_dw_atu_init() above.
     */
    if (dev->dbi_size && dev->msi_plic_irq) {
        if (pci_dwmsi_init(dev->dbi_base, dev->dbi_size, dev->msi_plic_irq) !=
            0) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                      "ECAM: DW MSI init failed -- MSI-X unavailable");
        }
    }

    /* Seed the IRQ resource pool with the 4 INTA..INTD PLIC IRQs assigned to
     * this host bridge. Without this step, pci_alloc_irq (which calls
     * hwres_claim) cannot give out any IRQ because the pool is empty: devices
     * come out of QEMU with Interrupt_Line == 0xFF, so pci_enum's
     * pci_reserve_irq path never fires.
     */
    if (dev->irq_base) {
        hwres_range_t ralloc = {
            .start = dev->irq_base,
            .end = dev->irq_base + 3,
            .flags = HWRES_IRQ,
        };
        if (hwres_create(&ralloc, 1) == -1) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                      "ECAM: IRQ pool seed failed (%u..%u): %s", dev->irq_base,
                      dev->irq_base + 3, strerror(errno));
        } else if (verbose) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "ECAM: IRQ pool seeded %u..%u",
                      dev->irq_base, dev->irq_base + 3);
        }
    }

    *hdl = dev;
    return PCI_SUCCESS;
}

/* ecam_detach — release ECAM mapping */

static int ecam_detach(void *hdl)
{
    ecam_dev_t *dev = hdl;

    if (dev) {
        if (dev->ecam_vaddr && dev->ecam_vaddr != MAP_FAILED)
            munmap(dev->ecam_vaddr, dev->ecam_size);
        if (dev->dbi_claimed)
            hwres_release_range(&dev->dbi_claim);
        if (dev->ecam_claimed)
            hwres_release_range(&dev->ecam_claim);
        free(dev);
    }
    return PCI_SUCCESS;
}

/* ecam_read_cnfg — read 32-bit dword from config space via ECAM
 *
 * Two backends, selected by whether the platform has a DesignWare DBI window
 * (dev->dbi_base != 0):
 * - DesignWare (FU740):
 */
/*      bus 0 dev 0 func 0 = the controller itself, read from DBI[0..0xFFF]. */
/*      Anything else at bus 0 returns 0xFFFFFFFF (no device).            */
/*      bus 1   = immediate child; re-aim iATU region 0 with TYPE_CFG0.   */
/*      bus >=2 = downstream of a bridge; re-aim with TYPE_CFG1.          */
/*  - Generic flat ECAM (QEMU virt, anything where dbi_base == 0):        */
/*      direct address arithmetic on the mmap'd config space.             */

static int ecam_read_cnfg(void *hdl,
                          uint32_t bus,
                          uint32_t devfunc,
                          uint32_t offset,
                          uint32_t width,
                          void *data)
{
    ecam_dev_t *dev = hdl;
    volatile uint32_t *addr;
    uint32_t off_aligned = offset & ~3u;
    (void) width;

    if (dev->dbi_base) {
        if (bus == 0) {
            if (devfunc != 0) {
                *(uint32_t *) data = 0xFFFFFFFFU;
                return PCI_SUCCESS;
            }
            *(uint32_t *) data = pci_dw_dbi_read32(off_aligned);
            return PCI_SUCCESS;
        }
        if (pci_dw_atu_aim_cfg((uint8_t) bus, (uint8_t) ((devfunc >> 3) & 0x1f),
                               (uint8_t) (devfunc & 0x07),
                               (bus == 1) ? DW_ATU_CFG0 : DW_ATU_CFG1,
                               (uint64_t) dev->ecam_phys,
                               (size_t) dev->ecam_size) != 0) {
            *(uint32_t *) data = 0xFFFFFFFFU;
            return PCI_SUCCESS;
        }
        addr = (volatile uint32_t *) (dev->ecam_vaddr + off_aligned);
        *(uint32_t *) data = *addr;
        return PCI_SUCCESS;
    }

    addr =
        (volatile uint32_t *) (dev->ecam_vaddr +
                               ((bus << 20) | (devfunc << 12) | off_aligned));
    *(uint32_t *) data = *addr;
    return PCI_SUCCESS;
}

/* ecam_write_cnfg — write 32-bit dword to config space via ECAM See
 * ecam_read_cnfg for backend selection.
 */

static int ecam_write_cnfg(void *hdl,
                           uint32_t bus,
                           uint32_t devfunc,
                           uint32_t offset,
                           uint32_t width,
                           const uint32_t data)
{
    ecam_dev_t *dev = hdl;
    volatile uint32_t *addr;
    uint32_t off_aligned = offset & ~3u;
    (void) width;

    if (dev->dbi_base) {
        if (bus == 0) {
            if (devfunc != 0)
                return PCI_SUCCESS;
            pci_dw_dbi_write32(off_aligned, data);
            return PCI_SUCCESS;
        }
        if (pci_dw_atu_aim_cfg((uint8_t) bus, (uint8_t) ((devfunc >> 3) & 0x1f),
                               (uint8_t) (devfunc & 0x07),
                               (bus == 1) ? DW_ATU_CFG0 : DW_ATU_CFG1,
                               (uint64_t) dev->ecam_phys,
                               (size_t) dev->ecam_size) != 0) {
            return PCI_SUCCESS;
        }
        addr = (volatile uint32_t *) (dev->ecam_vaddr + off_aligned);
        *addr = data;
        return PCI_SUCCESS;
    }

    addr =
        (volatile uint32_t *) (dev->ecam_vaddr +
                               ((bus << 20) | (devfunc << 12) | off_aligned));
    *addr = data;
    return PCI_SUCCESS;
}

/* ecam_cnfg_bridge — configure a PCI-PCI bridge
 *
 * On RISC-V with ECAM, we pass the ECAM subwindow for the secondary bus so the
 * server can give it to downstream scan operations.
 */

static int ecam_cnfg_bridge(void *hdl,
                            uint32_t bus,
                            uint32_t devfunc,
                            pci_bus_t *pbus)
{
    ecam_dev_t *dev = hdl;

    if (pbus) {
        /* Tell the server where the ECAM window for this bridge's secondary bus
         * lives. The server uses pciex_addr/pciex_size when scanning
         * subordinate buses.
         */
        pbus->pciex_addr = dev->ecam_vaddr + (bus << 20);
        pbus->pciex_size = dev->ecam_size;
    }
    return PCI_SUCCESS;
}

/* ecam_map_irq — map a PCI interrupt pin to a PLIC IRQ
 *
 * Standard swizzle: IRQ = irq_base + ((device + pin - 1) % 4) This matches both
 * QEMU virt and FU740 interrupt-map DT entries.
 */

static int ecam_map_irq(void *hdl,
                        uint32_t bus,
                        uint32_t devfunc,
                        uint32_t line,
                        uint32_t pin,
                        uint32_t flags)
{
    ecam_dev_t *dev = hdl;
    uint32_t device;

    if (pin == 0 || pin > 4)
        return -1;

    if (flags & PCI_UNMAP_IRQ)
        return PCI_SUCCESS;

    device = (devfunc >> 3) & 0x1f;
    return (int) (dev->irq_base + ((device + pin - 1) % 4));
}

/* ecam_avail_irq — return list of available IRQs for a device */

static int ecam_avail_irq(void *hdl,
                          uint32_t bus,
                          uint32_t devfunc,
                          uint32_t *list,
                          uint32_t *nelm,
                          int *msi_info)
{
    ecam_dev_t *dev = hdl;
    uint32_t n = 0;

    /* Read the device's Interrupt_Pin from config space and swizzle through the
     * standard PCIe INTx routing. Returning only the one actual IRQ (not a
     * four-wide list of all INTA..INTD) lets pci_alloc_irq's hwres_claim pick
     * the correct wire instead of the numerically-lowest one.
     */
    uint32_t pin_dw = 0;
    uint32_t device = (devfunc >> 3) & 0x1f;
    ecam_read_cnfg(hdl, bus, devfunc, 0x3c, 4, &pin_dw);
    uint8_t pin = (uint8_t) ((pin_dw >> 8) & 0xff); /* byte 0x3d */
    if (pin >= 1 && pin <= 4 && n < *nelm)
        list[n++] = dev->irq_base + ((device + pin - 1) % 4);

    /* MSI if available */
    if (dev->msi_irq && n < *nelm)
        list[n++] = dev->msi_irq;

    *nelm = n;
    if (msi_info)
        *msi_info = dev->msi_irq ? 1 : 0;

    return PCI_SUCCESS;
}

/* ecam_map_addr — translate between CPU and PCI addresses */

static int ecam_map_addr(void *hdl,
                         uint64_t iaddr,
                         uint64_t *oaddr,
                         uint32_t type)
{
    ecam_dev_t *dev = hdl;
    int to_pci = (type & PCI_MAP_ADDR_PCI) != 0;
    int to_cpu = (type & PCI_MAP_ADDR_CPU) != 0;
    uint32_t space = type & (PCI_IO_SPACE | PCI_MEM_SPACE | PCI_MEM_PREFETCH);

    for (int i = 0; i < dev->num_windows; i++) {
        pci_window_t *w = &dev->windows[i];

        /* Match address space type */
        if ((w->type & space) == 0)
            continue;

        if (to_pci) {
            /* CPU → PCI */
            if (iaddr >= w->cpu_base && iaddr < w->cpu_base + w->size) {
                *oaddr = w->pci_base + (iaddr - w->cpu_base);
                return PCI_SUCCESS;
            }
        } else if (to_cpu) {
            /* PCI → CPU */
            if (iaddr >= w->pci_base && iaddr < w->pci_base + w->size) {
                *oaddr = w->cpu_base + (iaddr - w->pci_base);
                return PCI_SUCCESS;
            }
        }
    }

    /* Identity mapping fallback (common on both platforms) */
    *oaddr = iaddr;
    return PCI_SUCCESS;
}

/* ecam_special_cycle — not supported on PCIe */

static int ecam_special_cycle(void *hdl, uint32_t bus, uint32_t data)
{
    return PCI_UNSUPPORTED_FUNCT;
}
