/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * \file init-asinfo.c
 *
 * Add machine-specific asinfo entries under "memory".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <startup.h>
#include <startup-api.h>

/**
 * \brief Add I/O, boot ROM, and PCI entries under "memory".
 *
 * \param mem Offset of the "memory" asinfo entry.
 */
void init_asinfo(unsigned mem)
{
    unsigned io;

    LOGF_T("%s: mem=%0#x\n", __func__, mem);

    if (clint_base) {
        as_add(0, clint_base - 1, AS_ATTR_NONE, "bootrom", mem);
        io = as_add(clint_base, ram_phys_start - 1, AS_ATTR_DEV, "io", mem);
    } else {
        io = as_add(0, ram_phys_start - 1, AS_ATTR_DEV, "io", mem);
    }

    /* Add PCIe address windows from FDT so roottask hardware-resource seeding
     * can classify them as PCI memory or I/O-port ranges.
     *
     * Window flags from FDT ranges property:
     *   0x01 = IO space
     *   0x02 = MEM32
     *   0x03 = MEM64 prefetchable
     */
    for (unsigned i = 0; i < pci_fdt.num_windows; i++) {
        uint64_t base = pci_fdt.windows[i].cpu_addr;
        uint64_t end = base + pci_fdt.windows[i].size - 1;
        uint32_t flags = pci_fdt.windows[i].flags;

        if (flags == 0x01) {
            /* PCI IO window — add under "io" */
            as_add(base, end, AS_ATTR_DEV, "io", io);
            kprintf("asinfo: PCI IO  %#llx - %#llx\n",
                    (unsigned long long) base, (unsigned long long) end);
        } else {
            /* PCI MEM / PREF window — add as "pcimemory" under "memory" */
            as_add(base, end, AS_ATTR_DEV, "pcimemory", mem);
            kprintf("asinfo: PCI MEM %#llx - %#llx%s\n",
                    (unsigned long long) base, (unsigned long long) end,
                    (flags == 0x03) ? " (prefetchable)" : "");
        }
    }
}
