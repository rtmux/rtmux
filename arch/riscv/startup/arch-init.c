/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RISC-V architecture-specific startup initialization.
 *
 * Functions called from the generic startup flow (main.c) that are
 * architecture- or platform-specific on RISC-V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <startup.h>
#include <startup-api.h>

int cpu_handle_common_option(int opt)
{
    (void) opt;
    return 0;
}

void *startup_memory_map(unsigned size, paddr_t phys, unsigned prot_flags)
{
    (void) size;
    (void) prot_flags;
    return (void *) ((uintptr_t) phys);
}

void startup_memory_unmap(void *p)
{
    (void) p;
}

void init_clocks(void)
{
    kprintf("%s\n", __func__);
}

/* init_cpuinfo — populate the syspage cpuinfo section from FDT data.
 *
 * CPU name comes from the "riscv,isa" or "compatible" FDT property (captured by
 * board.c during parse_fdt). Speed is the timebase frequency converted to MHz.
 * Hart IDs are used as the cpu field.
 */
void init_cpuinfo(void)
{
    unsigned ncpu = lsp.syspage.p->num_cpu;
    extern unsigned hart_ids[];
    extern char cpu_isa_string[];
    extern char cpu_compatible[];

    set_syspage_section(&lsp.cpuinfo, sizeof(struct cpuinfo_entry) * ncpu);

    const char *name = cpu_isa_string[0]   ? cpu_isa_string
                       : cpu_compatible[0] ? cpu_compatible
                                           : "rv64g";
    unsigned name_off = add_string(name);

    struct cpuinfo_entry *cpu = lsp.cpuinfo.p;
    for (unsigned i = 0; i < ncpu; i++) {
        cpu[i].cpu = hart_ids[i];
        /* cpuinfo.speed is nominally MHz. RTMux has no CPU clock-rate
         * source (the FDT gives only the timebase), so this reports the
         * timebase in MHz. No in-tree consumer reads it; do not derive a
         * cycle rate from it.
         */
        cpu[i].speed = (uint32_t) (kopt.timebase_freq / 1000000);
        cpu[i].flags = CPU_FLAG_MMU | CPU_FLAG_FPU;
        cpu[i].name = (uint16_t) name_off;
        cpu[i].data_cache = CACHE_LIST_END;
        cpu[i].ins_cache = CACHE_LIST_END;
    }

    kprintf("%s: %u CPUs, %s, %lu MHz timebase\n", __func__, ncpu, name,
            kopt.timebase_freq / 1000000);
}

/* init_hwinfo — populate the syspage hwinfo section from FDT.
 *
 * Performs a second FDT pass to discover devices (UART, virtio, etc.) and
 * register them in the hwinfo syspage section. User-space drivers query hwinfo
 * at runtime instead of hardcoding addresses.
 */

/* Per-device state accumulated across FDT property callbacks */
#define FDT_IRQ_MAX 8U

static struct {
    paddr_t reg_base;
    paddr_t reg_size;
    uint32_t irq[FDT_IRQ_MAX];
    unsigned irq_count;
    uint32_t phandle;
    bool has_phandle;
    uint32_t iommu_phandle;
    uint32_t iommu_stream;
    bool has_iommu_stream;
    char compatible[64];
    unsigned bus_off; /* hwinfo offset of the "internal" bus */
} hwi_ctx;

static void hwinfo_handle_node(const char *name)
{
    /* Reset per-node state on each new node */
    hwi_ctx.reg_base = 0;
    hwi_ctx.reg_size = 0;
    hwi_ctx.irq_count = 0;
    hwi_ctx.phandle = 0;
    hwi_ctx.has_phandle = false;
    hwi_ctx.iommu_phandle = 0;
    hwi_ctx.iommu_stream = 0;
    hwi_ctx.has_iommu_stream = false;
    hwi_ctx.compatible[0] = '\0';
}

static void hwinfo_handle_property(const char *node,
                                   const char *prop,
                                   void *data,
                                   const unsigned datalen)
{
    /* Capture "compatible" string */
    if (!strcmp(prop, "compatible") && datalen > 0) {
        unsigned n = datalen < sizeof(hwi_ctx.compatible)
                         ? datalen
                         : sizeof(hwi_ctx.compatible) - 1;
        memcpy(hwi_ctx.compatible, data, n);
        hwi_ctx.compatible[n] = '\0';
    }

    /* Capture "reg" property (#address-cells=2, #size-cells=2 on QEMU virt) */
    if (!strcmp(prop, "reg") && datalen >= 16) {
        uint32_t *cells = data;
        hwi_ctx.reg_base =
            ((uint64_t) ENDIAN_RET32(cells[0]) << 32) | ENDIAN_RET32(cells[1]);
        hwi_ctx.reg_size =
            ((uint64_t) ENDIAN_RET32(cells[2]) << 32) | ENDIAN_RET32(cells[3]);
    }

    /* Capture "interrupts" property. Most QEMU virt devices have one PLIC IRQ;
     * the system IOMMU publishes several, so keep a bounded list.
     */
    if (!strcmp(prop, "interrupts") && datalen >= 4) {
        uint32_t *cells = data;
        unsigned n = datalen / sizeof(uint32_t);

        if (n > FDT_IRQ_MAX)
            n = FDT_IRQ_MAX;
        hwi_ctx.irq_count = n;
        for (unsigned i = 0; i < n; i++)
            hwi_ctx.irq[i] = ENDIAN_RET32(cells[i]);
    }

    if ((!strcmp(prop, "phandle") || !strcmp(prop, "linux,phandle")) &&
        datalen >= 4) {
        uint32_t *cells = data;

        hwi_ctx.phandle = ENDIAN_RET32(cells[0]);
        hwi_ctx.has_phandle = true;
    }

    if (!strcmp(prop, "iommus") && datalen >= 8) {
        uint32_t *cells = data;

        hwi_ctx.iommu_phandle = ENDIAN_RET32(cells[0]);
        hwi_ctx.iommu_stream = ENDIAN_RET32(cells[1]);
        hwi_ctx.has_iommu_stream = true;
    }
}

/* After each FDT_END_NODE, check if the accumulated properties match a known
 * device and register it in hwinfo. We hook this by checking the compatible
 * string at the END of each node (when all properties have been seen).
 *
 * Since the FDT parser doesn't have an end-node callback, we use a different
 * approach: check the compatible string inside the property callback when we
 * see a node-final "compatible" property. In practice, "compatible" is
 * typically one of the first properties, but "reg" and "interrupts" may come
 * before or after.
 *
 * Simpler approach: we do the registration from a post-parse scan.
 */

/* Actually, the simplest approach: parse the FDT, and for each node that has
 * both "compatible" and "reg", register the device. We can do this by storing
 * all discovered devices in a small array and processing them after the parse
 * completes.
 */

#define MAX_FDT_DEVICES 16

static struct fdt_dev {
    paddr_t base;
    paddr_t size;
    uint32_t irq[FDT_IRQ_MAX];
    unsigned irq_count;
    uint32_t phandle;
    bool has_phandle;
    uint32_t iommu_phandle;
    uint32_t iommu_stream;
    bool has_iommu_stream;
    char compatible[48];
} fdt_devices[MAX_FDT_DEVICES];
static unsigned num_fdt_devices;

static void hwinfo_commit_node(void)
{
    if (hwi_ctx.compatible[0] && hwi_ctx.reg_base &&
        num_fdt_devices < MAX_FDT_DEVICES) {
        struct fdt_dev *d = &fdt_devices[num_fdt_devices];
        d->base = hwi_ctx.reg_base;
        d->size = hwi_ctx.reg_size;
        d->irq_count = hwi_ctx.irq_count;
        for (unsigned i = 0; i < hwi_ctx.irq_count; i++)
            d->irq[i] = hwi_ctx.irq[i];
        d->phandle = hwi_ctx.phandle;
        d->has_phandle = hwi_ctx.has_phandle;
        d->iommu_phandle = hwi_ctx.iommu_phandle;
        d->iommu_stream = hwi_ctx.iommu_stream;
        d->has_iommu_stream = hwi_ctx.has_iommu_stream;
        strncpy(d->compatible, hwi_ctx.compatible, sizeof(d->compatible) - 1);
        d->compatible[sizeof(d->compatible) - 1] = '\0';
        num_fdt_devices++;
    }
}

/* Property callback that also commits the previous node when we see the start
 * of a new node (via the node name changing).
 */
static const char *hwinfo_prev_node;

static void hwinfo_prop_cb(const char *node,
                           const char *prop,
                           void *data,
                           const unsigned datalen)
{
    /* Detect node transition — commit the previous node's device */
    if (node != hwinfo_prev_node && hwinfo_prev_node) {
        hwinfo_commit_node();
        hwinfo_handle_node(node);
    }
    hwinfo_prev_node = node;

    hwinfo_handle_property(node, prop, data, datalen);
}

static void hwinfo_node_cb(const char *name)
{
    /* Commit any pending device from the previous node */
    if (hwinfo_prev_node) {
        hwinfo_commit_node();
    }
    hwinfo_handle_node(name);
    hwinfo_prev_node = NULL;
}

void init_hwinfo(void)
{
    unsigned root_off, bus_off, dev_off;

    /* Second FDT pass: discover devices */
    num_fdt_devices = 0;
    hwinfo_prev_node = NULL;
    hwinfo_handle_node("");

    fdt_handle_node_cb = hwinfo_node_cb;
    fdt_handle_property_cb = hwinfo_prop_cb;
    parse_fdt(NULL);
    /* Commit the last node */
    hwinfo_commit_node();
    fdt_handle_node_cb = NULL;
    fdt_handle_property_cb = NULL;

    /* Find root hw group (created by hwi_default) */
    root_off = 0; /* root "hw" is always at offset 0 */

    /* Create an "internal" bus for platform MMIO devices */
    bus_off = hwibus_add(HWI_ITEM_BUS_INTERNAL, root_off);

    /* Register discovered devices */
    unsigned num_virtio = 0;
    for (unsigned i = 0; i < num_fdt_devices; i++) {
        struct fdt_dev *d = &fdt_devices[i];

        if (!strncmp(d->compatible, "ns16550", 7)) {
            dev_off = hwidev_add("ns16550", 0, bus_off);
            hwitag_add_location(dev_off, d->base, d->size, 0,
                                HWI_ADDRSPACE_MEMORY);
            if (d->irq_count)
                hwitag_add_irq(dev_off, d->irq[0]);
            kprintf("hwinfo: serial %s @ %#lx irq %u\n", d->compatible,
                    (unsigned long) d->base,
                    d->irq_count ? d->irq[0] : HWI_ILLEGAL_VECTOR);

        } else if (!strncmp(d->compatible, "sifive,fu540-c000-uart", 22) ||
                   !strncmp(d->compatible, "sifive,fu740-c000-uart", 22) ||
                   !strncmp(d->compatible, "sifive,uart0", 12)) {
            /* SiFive UART (FU540/FU740/Unmatched/Unleashed). Register layout is
             * completely different from 8250 — published under a distinct
             * hwinfo name so dev-serial-sifive binds to it and dev-serial-8250
             * ignores it.
             */
            dev_off = hwidev_add("sifive-uart", 0, bus_off);
            hwitag_add_location(dev_off, d->base, d->size, 0,
                                HWI_ADDRSPACE_MEMORY);
            if (d->irq_count)
                hwitag_add_irq(dev_off, d->irq[0]);
            kprintf("hwinfo: serial %s @ %#lx irq %u\n", d->compatible,
                    (unsigned long) d->base,
                    d->irq_count ? d->irq[0] : HWI_ILLEGAL_VECTOR);

        } else if (!strncmp(d->compatible, "virtio,mmio", 11)) {
            dev_off = hwidev_add("virtio", 0, bus_off);
            hwitag_add_location(dev_off, d->base, d->size, 0,
                                HWI_ADDRSPACE_MEMORY);
            if (d->irq_count)
                hwitag_add_irq(dev_off, d->irq[0]);
            if (d->has_iommu_stream) {
                char opt[32];

                ksnprintf(opt, sizeof(opt), "iommu-phandle=%u",
                          d->iommu_phandle);
                hwitag_add_optstr(dev_off, opt);
                ksnprintf(opt, sizeof(opt), "iommu-stream=%u",
                          d->iommu_stream);
                hwitag_add_optstr(dev_off, opt);
            }
            num_virtio++;
            LOGF_T("hwinfo: virtio @ %#lx irq %u\n", (unsigned long) d->base,
                   d->irq_count ? d->irq[0] : HWI_ILLEGAL_VECTOR);
        } else if (!strncmp(d->compatible, "google,goldfish-rtc", 19)) {
            /* An isolated, self-triggerable real IRQ source (its alarm) that is
             * not the firmware console, published so the section 15
             * unified-wait fixture can delegate it and drive a genuine hardware
             * interrupt. The kernel only reads this RTC once at boot for
             * wall-clock time, so a userland owner of its alarm IRQ does not
             * collide.
             */
            dev_off = hwidev_add("goldfish-rtc", 0, bus_off);
            hwitag_add_location(dev_off, d->base, d->size, 0,
                                HWI_ADDRSPACE_MEMORY);
            if (d->irq_count)
                hwitag_add_irq(dev_off, d->irq[0]);
            kprintf("hwinfo: rtc %s @ %#lx irq %u\n", d->compatible,
                    (unsigned long) d->base,
                    d->irq_count ? d->irq[0] : HWI_ILLEGAL_VECTOR);
        } else if (!strncmp(d->compatible, "sifive,gpio0", 12) ||
                   !strncmp(d->compatible, "sifive,fu540-c000-gpio", 22) ||
                   !strncmp(d->compatible, "sifive,fu740-c000-gpio", 22)) {
            /* SiFive GPIO controller (FU540/FU740). Its pins raise real PLIC
             * interrupts, so publishing it lets the gpio-latency fixture
             * delegate a pin IRQ and time the hardware IRQ-to-IST path. QEMU
             * virt has no sifive,gpio0, so this branch never fires there and
             * the fixture reports a clean UNSUPPORTED skip instead.
             */
            dev_off = hwidev_add("sifive-gpio", 0, bus_off);
            hwitag_add_location(dev_off, d->base, d->size, 0,
                                HWI_ADDRSPACE_MEMORY);
            if (d->irq_count)
                hwitag_add_irq(dev_off, d->irq[0]);
            kprintf("hwinfo: gpio %s @ %#lx irq %u\n", d->compatible,
                    (unsigned long) d->base,
                    d->irq_count ? d->irq[0] : HWI_ILLEGAL_VECTOR);
        } else if (!strncmp(d->compatible, "riscv,iommu", 11)) {
            unsigned irq_cells;
            bool two_cell_irqs;

            dev_off = hwidev_add("riscv-iommu", 0, bus_off);
            hwitag_add_location(dev_off, d->base, d->size, 0,
                                HWI_ADDRSPACE_MEMORY);
            two_cell_irqs = d->irq_count > 1 && (d->irq_count % 2) == 0 &&
                            d->irq[1] <= 4;
            irq_cells = two_cell_irqs ? 2 : 1;
            for (unsigned irq = 0; irq < d->irq_count; irq += irq_cells)
                hwitag_add_irq(dev_off, d->irq[irq]);
            if (d->has_phandle) {
                char opt[32];

                ksnprintf(opt, sizeof(opt), "fdt-phandle=%u", d->phandle);
                hwitag_add_optstr(dev_off, opt);
            }
            kprintf("hwinfo: iommu %s @ %#lx irqs %u\n", d->compatible,
                    (unsigned long) d->base,
                    (d->irq_count + irq_cells - 1) / irq_cells);
        }
    }
    if (num_virtio)
        kprintf("hwinfo: %u virtio-mmio slot%s\n", num_virtio,
                num_virtio > 1 ? "s" : "");

    /* Publish the PCIe host bridge captured from FDT (board.c → pci_fdt). Lives
     * under its own "pci" bus so user-space (pci server, dev-nvme) can find it
     * by bus kind without scraping the internal bus.
     */
    if (pci_fdt.ecam_size) {
        unsigned pci_bus_off;

        pci_bus_off = hwibus_add(HWI_ITEM_BUS_PCI, root_off);
        /* ECAM region — treat as MMIO so hwi_find_as() resolves it */
        hwitag_add_location(pci_bus_off, pci_fdt.ecam_base, pci_fdt.ecam_size,
                            0, HWI_ADDRSPACE_MEMORY);
        /* PLIC IRQ for INTA (INTB..INTD follow; driver applies the swizzle) */
        if (pci_fdt.irq_base)
            hwitag_add_irq(pci_bus_off, pci_fdt.irq_base);
        /* One pci_window tag per FDT "ranges" entry */
        for (unsigned i = 0; i < pci_fdt.num_windows; i++) {
            hwitag_add_pci_window(pci_bus_off, pci_fdt.windows[i].pci_addr,
                                  pci_fdt.windows[i].cpu_addr,
                                  pci_fdt.windows[i].size,
                                  pci_fdt.windows[i].flags);
        }
        /* Expose the FDT compatible string so the server can log it */
        if (pci_fdt.compatible[0])
            hwitag_add_optstr(pci_bus_off, pci_fdt.compatible);

        kprintf("hwinfo: pci %s ecam %#lx/%#lx irq %u windows %u\n",
                pci_fdt.compatible, (unsigned long) pci_fdt.ecam_base,
                (unsigned long) pci_fdt.ecam_size, pci_fdt.irq_base,
                pci_fdt.num_windows);

        /* DesignWare MSI controller (FU740). Lives as a child device under the
         * PCI bus, carrying its DBI window as the "location" tag and the
         * aggregate MSI PLIC IRQ as the "irq" tag. The pci-server picks these
         * up to program MSI_ADDR_LO/HI, MSI_INTR_ENABLE and demux
         * MSI_INTR_STATUS. On platforms without a DW controller (QEMU virt),
         * both dbi_size and msi_irq are zero, so nothing is published and
         * pci_device_msix_enable still works with its synthetic vector counter
         * for whoever else eventually provides MSI.
         */
        if (pci_fdt.dbi_size && pci_fdt.msi_irq) {
            unsigned msi_off = hwidev_add("dw_pcie_msi", 0, pci_bus_off);
            hwitag_add_location(msi_off, pci_fdt.dbi_base, pci_fdt.dbi_size, 0,
                                HWI_ADDRSPACE_MEMORY);
            hwitag_add_irq(msi_off, pci_fdt.msi_irq);
            kprintf("hwinfo: pci dw_pcie_msi dbi %#lx/%#lx irq %u\n",
                    (unsigned long) pci_fdt.dbi_base,
                    (unsigned long) pci_fdt.dbi_size, pci_fdt.msi_irq);
        }
    }
}

/* init_qtime — populate the syspage qtime section.
 *
 * Timer rate is derived from the timebase frequency (from FDT). Boot time is
 * read from the goldfish-rtc before MMU is enabled (see read_rtc_boot_time() in
 * board.c, called from main.c).
 */
void init_qtime(void)
{
    extern uint32_t rtc_boot_time;

    set_syspage_section(&lsp.qtime, sizeof(struct qtime_entry));
    systime_t *qtp = lsp.qtime.p;
    memset(qtp, 0, sizeof(*qtp));

    qtp->cycles_per_sec = kopt.timebase_freq;
    /* timer_rate x 10^timer_scale = seconds per tick. With timer_scale = -9
     * (nanoseconds): timer_rate = ns per tick. For 10 MHz: 1e9 / 10e6 = 100 ns
     * per tick.
     */
    qtp->timer_rate = 1000000000UL / kopt.timebase_freq;
    qtp->timer_scale = -9;
    qtp->intr = 5; /* S-mode timer interrupt vector */
    qtp->epoch = 1970;
    qtp->flags = 0;

    if (rtc_boot_time) {
        qtp->boot_time = rtc_boot_time;
        kprintf("%s: boot_time=%u (Unix epoch)\n", __func__, qtp->boot_time);
    }

    LOGF_I("%s: cycles_per_sec=%lu timer_rate=%lu intr=%ld\n", __func__,
           qtp->cycles_per_sec, qtp->timer_rate, qtp->intr);
}

/* Debug device setup */

extern void rvq_putc_ser_dbg(int c);

static const struct debug_device debug_devices[] = {
    {.name = "sbi", .init = NULL, .put = rvq_putc_ser_dbg},
};

void arch_init_debug(void)
{
    select_debug(debug_devices, sizeof(debug_devices));
}
