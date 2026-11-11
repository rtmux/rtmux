/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"

/* Turn on access to the PCI device */

void on_pci(uint8_t bus, uint8_t devfn, uint16_t flags)
{
    uint16_t cr;

    DEBUGFUNC();
    if (dont_touch)
        return;
    PCI_read_config16(bus, devfn, offsetof(struct _pci_config_regs, Command), 1,
                      (char *) &cr);
    cr |= flags;
    PCI_write_config16(bus, devfn, offsetof(struct _pci_config_regs, Command),
                       1, (char *) &cr);
}

/* Turn off access to the PCI device, except for configuration */

uint16_t off_pci(uint8_t bus, uint8_t devfn, uint32_t class)
{
    uint16_t cr, ret;

    DEBUGFUNC();
    PCI_read_config16(bus, devfn, offsetof(struct _pci_config_regs, Command), 1,
                      (char *) &cr);
    ret = cr;
    if (dont_touch)
        return ret;
    /*  cr &= ~3; */
    cr &= (uint16_t) ~7u;
    PCI_write_config16(bus, devfn, offsetof(struct _pci_config_regs, Command),
                       1, (char *) &cr);
    return ret;
}

/* Check device capabilities */

int check_capabilities(uint8_t bus, uint8_t devfn, pci_device_t *device)
{
    uint8_t cap_ptr, cap;
    uint16_t pwr, msi_ctrl;
    uint32_t msix;
    uint32_t capab;
    char *type, type1[6];

    DEBUGFUNC();
    if ((device->HeaderType & 0x7f) == 2) /* CardBus device */
        capab = offsetof(struct _pci_cardbus_config_regs, Capabilities_Pointer);
    else
        capab = offsetof(struct _pci_config_regs, Capabilities_Pointer);
    if (PCI_read_config8(bus, devfn, capab, 1, &cap_ptr) != PCI_SUCCESS) {
        return -1;
    }

    if (cap_ptr) {
        while (1) {
            type = NULL;
            if (PCI_read_config8(bus, devfn, cap_ptr, 1, &cap) != PCI_SUCCESS) {
                return -1;
            }
            if (cap_ptr == 0 || cap_ptr == 0xff) {
                pci_slogf(_SLOGC_PCI, _SLOG_WARNING,
                          "%s Invalid capability pointer 0x%x", __FUNCTION__,
                          cap_ptr);
                return EOK;
            }
            if (cap == PCI_CAP_PCIX) {
                device->PciType |= PCI_TYPE_PCIX;
                type = "PciX";
            }
            if (cap == PCI_CAP_PCI_EXPRESS) {
                device->PciType |= PCI_TYPE_PCIEXPRESS;
                type = "PciExpress";
            }
            if (cap == PCI_CAP_MSI) {
                device->PciType |= PCI_TYPE_MSI;
                device->msi_off = cap_ptr;
                type = "MSI";
            }
            if (cap == PCI_CAP_MSI_X) {
                device->PciType |= PCI_TYPE_MSIX;
                device->msix_off = cap_ptr;
                type = "MSIX";
            }
            type1[0] = 0;
            if (cap == PCI_CAP_MSI || cap == PCI_CAP_MSI_X) {
                if (PCI_read_config16(bus, devfn, cap_ptr + 2, 1, &msi_ctrl) !=
                    PCI_SUCCESS)
                    return -1;
                sprintf(type1, "%04x", msi_ctrl);
                device->mess_ctrl = msi_ctrl;
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                              "%s: Bus 0x%x devfunc 0x%x message_control 0x%x",
                              __FUNCTION__, bus, devfn, device->mess_ctrl);
            }
            if (cap == PCI_CAP_MSI_X) {
                if (PCI_read_config32(bus, devfn, cap_ptr + 4, 1, &msix) !=
                    PCI_SUCCESS) {
                    return -1;
                }
                device->msix_tab_off = msix;
                if (PCI_read_config32(bus, devfn, cap_ptr + 8, 1, &msix) !=
                    PCI_SUCCESS) {
                    return -1;
                }
                device->msix_pba_off = msix;
            }
            if (cap == PCI_CAP_POWER_MANAGEMENT) {
                if (PCI_read_config16(bus, devfn, cap_ptr + 4, 1, &pwr) !=
                    PCI_SUCCESS) {
                    return -1;
                }
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                              "%s: Bus 0x%x devfunc 0x%x pwr 0x%x",
                              __FUNCTION__, bus, devfn, pwr);
                device->PmState = pwr;
                device->PmCap = cap_ptr;
            }
            if (verbose > 2 && type)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "%s: Bus 0x%x devfunc 0x%x - %s - %s", __FUNCTION__,
                          bus, devfn, type, type1);
            if (PCI_read_config8(bus, devfn, cap_ptr + 1, 1, &cap_ptr) !=
                PCI_SUCCESS) {
                return -1;
            }
            if (!cap_ptr || cap_ptr == 0xff)
                return EOK;
        }
    }

    return EOK;
}

/* Determine the size of memory/io window */

static uint64_t window_size(uint8_t bus,
                            uint8_t devfn,
                            uint32_t Offset,
                            uint32_t type,
                            uint32_t reg_64)
{
    uint32_t cur_addr[2], tmp1, tmp_addr;
    uint64_t size, tmp, tmp2;

    DEBUGFUNC();
    cur_addr[1] = tmp_addr = 0xffffffff;
    PCI_read_config32(bus, devfn, Offset, 1, (char *) &cur_addr[0]);
    if (reg_64) {
        PCI_read_config32(bus, devfn, Offset + 4, 1, (char *) &cur_addr[1]);
    }
    tmp1 = 0xffffffff;
    PCI_write_config32(bus, devfn, Offset, 1, (char *) &tmp1);
    if (reg_64) {
        PCI_write_config32(bus, devfn, Offset + 4, 1, (char *) &tmp1);
    }
    PCI_read_config32(bus, devfn, Offset, 1, (char *) &tmp1);
    tmp = tmp1;
    if (reg_64) {
        PCI_read_config32(bus, devfn, Offset + 4, 1, (char *) &tmp_addr);
        tmp2 = tmp_addr;
        tmp |= (tmp2 << 32);
    }
    switch (type) {
    case PCI_IO_SPACE:
        tmp = PCI_IO_ADDR(tmp);
        break;
    case PCI_MEM_SPACE:
        tmp = PCI_MEM_ADDR(tmp);
        break;
    case PCI_ROM_SPACE:
        tmp = PCI_ROM_ADDR(tmp);
        break;
    }
    if (tmp) {
        if (tmp < 0x10000)
            tmp |= 0xffff0000;
        if (tmp < 0x100000000ULL)
            tmp |= 0xffffffff00000000ULL;
    }
    if (reg_64) {
        if (!(tmp & 0xffffff0000000000ULL)) { /* Check for 40-bit device */
            tmp |= 0xffffff0000000000ULL;
        }
    }

    /* Some chips are pre-programmed with a base address, so default */
    if (tmp1 && cur_addr[0] && tmp_addr == cur_addr[1]) {
        tmp_addr = 0;
        PCI_write_config32(bus, devfn, Offset, 1, (char *) &tmp_addr);
        PCI_read_config32(bus, devfn, Offset, 1, (char *) &tmp_addr);
    }
    PCI_write_config32(bus, devfn, Offset, 1, (char *) &cur_addr[0]);
    if (reg_64) {
        PCI_write_config32(bus, devfn, Offset + 4, 1, (char *) &cur_addr[1]);
    }
    if (tmp_addr == tmp1)
        size = DEFAULT_SIZE;
    else
        size = (~tmp) + 1;
    return size;
}

/* scan base registers */

int scan_windows(uint8_t bus, uint8_t devfn, pci_device_t *device)
{
    int i, reg_64, device_enabled;
    int num_regs = 6;
    uint32_t Offset;
    uint32_t dword, class;
    uint64_t addr64, size;
    uint16_t oldcr;
    struct BaseRegister *BaseReg;
    bus_map_t *map;

    DEBUGFUNC();
    if (verbose > 2) {
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: bus %d - devfn 0x%x",
                  __FUNCTION__, bus, devfn);
    }
    if (!(map = find_bus(bus))) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Unable to find bus",
                  __FUNCTION__);
        return -1;
    }

    class = (device->ClassCode & 0xff0000);
    /* disable */
    oldcr = off_pci(bus, devfn, device->ClassCode);
    device_enabled = (oldcr & (PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MEM_ENABLE));
    if (!program_display_device) {
        oldcr |= (class == PCI_CLASS_DISPLAY || class == PCI_CLASS_BRIDGE ||
                  class == PCI_CLASS_PROCESSOR)
                     ? PCI_COMMAND_MASTER_ENABLE
                     : 0;
    }

    if ((device->HeaderType & 0x7f) == 2) { /* CardBus device */
        num_regs = 1;
    } else {
        if ((device->HeaderType & 0x7f) == 1) { /* PCI-PCI bridge */
            num_regs = 2;
        }
    }

    /* probe */
    for (i = 0; i < num_regs; i++) {
        BaseReg = &device->BaseReg[i];
        Offset =
            (uint32_t) offsetof(struct _pci_config_regs, Base_Address_Regs[i]);
        PCI_read_config32(bus, devfn, Offset, 1, (char *) &dword);
        if (PCI_IS_IO(dword)) {
            size = window_size(bus, devfn, Offset, PCI_IO_SPACE, 0);
            size &= 0xffffU; /* I/O cannot be more than 64K */
            if (!size)
                continue;
            if (size == 0xffffU) {
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "Rejecting I/O size %llx - window %d", size, i);
                continue;
            }
            BaseReg->Size = size;
            map->io_reqd += size;
            BaseReg->Flags |= PCI_IO_SPACE;
            if (check_range((uint64_t) (dword & ~3u), (uint64_t) size,
                            (int) BaseReg->Flags, bus)) {
                pci_slogf(_SLOGC_PCI, _SLOG_WARNING,
                          "Range check failed (IO) - Dev 0x%x - Vend 0x%x - "
                          "Class 0x%x - Addr 0x%x - Size 0x%llx",
                          device->DeviceId, device->VendorId, device->ClassCode,
                          dword & ~3u, size);
                if (class != PCI_CLASS_STORAGE && class != PCI_CLASS_DISPLAY) {
                    device_enabled = 0; /* Not in valid range */
                }
            }
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "IO 0x%x 0x%llx",
                          PCI_IO_ADDR(dword), size);
            BaseReg->Address =
                device_enabled ? dword : (dword &= 3); /* Leave reserved bits */
            if (!device_enabled || !(dword & ~3u)) {
                PCI_write_config32(bus, devfn, Offset, 1, (char *) &dword);
            } else {
                uint64_t Address;

                (*pci_drivers->entry->map_addr)(
                    ll_handle, BaseReg->Address, &Address,
                    (uint32_t) BaseReg->Flags | PCI_MAP_ADDR_CPU);

                if (alloc_resource((uint32_t) PCI_IO_ADDR(Address),
                                   (uint32_t) size, HWRES_IO_PORT, bus, 0)) {
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                              "%s: Alloc failed 0x%llx - Size 0x%llx",
                              __FUNCTION__, Address, size);
                }
                BaseReg->Address = Address;
                BaseReg->Flags |= PCI_DEVICE_ENABLED;
            }
        } else {
            reg_64 = 0;
            if ((dword & 0x6) == 0x4) { /* 64-bit address */
                uint32_t tmp = 0;
                PCI_read_config32(bus, devfn, Offset + 4, 1, (char *) &tmp);
                size = (uint64_t) tmp;
                addr64 = (size << 32) | dword;
                reg_64 = 1;
                i++;
            } else {
                addr64 = (uint64_t) dword;
            }
            if ((size = window_size(bus, devfn, Offset, PCI_MEM_SPACE,
                                    (uint32_t) reg_64)) < 8) {
                continue;
            }
            BaseReg->Size = size;
            BaseReg->Flags |= PCI_MEM_SPACE;
            if (addr64 & 0x08) { /* Test for prefetchable */
                BaseReg->Flags |= PCI_MEM_PREFETCH;
                map->pref_reqd += size;
            } else
                map->mem_reqd += size;
            if (check_range((addr64 & ~0x0fULL), (uint64_t) size,
                            (int) BaseReg->Flags, bus)) {
                pci_slogf(_SLOGC_PCI, _SLOG_WARNING,
                          "Range check failed (MEM) - Dev 0x%x - Vend 0x%x - "
                          "Class 0x%x - Addr 0x%llx - Size 0x%llx",
                          device->DeviceId, device->VendorId, device->ClassCode,
                          addr64 & ~0x0fULL, size);
                if (class != PCI_CLASS_STORAGE && class != PCI_CLASS_DISPLAY &&
                    class != PCI_CLASS_PROCESSOR) {
                    device_enabled = 0; /* Not in valid range */
                }
            }
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "MEM 0x%llx 0x%llx %s",
                          PCI_MEM_ADDR(addr64), size,
                          addr64 & 0x08 ? "pref" : "");
            BaseReg->Address = device_enabled ? addr64 : (addr64 &= 0x0f);
            if (!device_enabled) {
                PCI_write_config32(bus, devfn, Offset, 1, (char *) &addr64);
                if (reg_64) {
                    addr64 >>= 32;
                    PCI_write_config32(bus, devfn, Offset + 4, 1,
                                       (char *) &addr64);
                }
            } else {
                uint64_t Address;

                (*pci_drivers->entry->map_addr)(
                    ll_handle, addr64, &Address,
                    (uint32_t) BaseReg->Flags | PCI_MAP_ADDR_CPU);

                if (alloc_resource(
                        PCI_MEM_ADDR(Address), (uint32_t) size,
                        HWRES_PCI_MEMORY, bus,
                        (BaseReg->Flags & PCI_MEM_PREFETCH) ? 1 : 0)) {
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                              "%s: Alloc failed 0x%llx - Size 0x%llx",
                              __FUNCTION__, Address, size);

                    /*                  BaseReg->Address = 0;
                     *                  BaseReg->Flags &= ~PCI_DEVICE_ENABLED;
                     *                  dword = 0;
                     *                  PCI_write_config32 (bus, devfn, Offset,
                     * 1, (char *)&dword);
                     */
                }
                /*              else { */
                BaseReg->Address = Address;
                BaseReg->Flags |= PCI_DEVICE_ENABLED;
            }
        }
    }
    /*      } */
    /* enable */
    if (num_regs < 6) {
        on_pci(bus, devfn, oldcr);
        return PCI_SUCCESS;
    }

    Offset = offsetof(struct _pci_config_regs, ROM_Base_Address);
    PCI_read_config32(bus, devfn, Offset, 1, (char *) &dword);
    BaseReg = &device->ROMBase;
    if ((size = window_size(bus, devfn, Offset, PCI_MEM_SPACE, 0)) >= 8) {
        BaseReg->Address = device_enabled ? dword : (dword &= 0x7fe);
        BaseReg->Size = size;
        map->mem_reqd += size;
        BaseReg->Flags |= PCI_ROM_SPACE;
        if (check_range((uint64_t) (dword & ~1u), (uint64_t) size,
                        (int) BaseReg->Flags, bus)) {
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_WARNING,
                          "Range check failed (ROM) - Dev 0x%x - Vend 0x%x - "
                          "Class 0x%x - Addr 0x%x - Size 0x%llx",
                          device->DeviceId, device->VendorId, device->ClassCode,
                          dword & ~1u, size);
        }
        if (verbose)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "ROM 0x%x - 0x%llx",
                      PCI_ROM_ADDR(dword), size);
    }
    if (!device_enabled) {
        dword &= 0x7fe; /* Leave reserved bits */
        PCI_write_config32(bus, devfn, Offset, 1, (char *) &dword);
    } else {
        uint64_t Address;

        (*pci_drivers->entry->map_addr)(
            ll_handle, BaseReg->Address, &Address,
            (uint32_t) BaseReg->Flags | PCI_MAP_ADDR_CPU);

        alloc_resource(Address, (uint32_t) size,
                       (HWRES_MEMORY | HWRES_PCI_MEMORY), bus,
                       (BaseReg->Flags & PCI_MEM_PREFETCH) ? 1 : 0);

        BaseReg->Flags |= PCI_DEVICE_ENABLED;
        on_pci(bus, devfn, oldcr);
        if (verbose) {
            for (i = 0; i < 6; i++) {
                BaseReg = &device->BaseReg[i];
                if (BaseReg->Flags) {
                    if (BaseReg->Flags & PCI_IO_SPACE)
                        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                                  "IO 0x%llx 0x%llx - Flags 0x%x",
                                  PCI_IO_ADDR(BaseReg->Address), BaseReg->Size,
                                  BaseReg->Flags);
                    else if (BaseReg->Flags & PCI_MEM_SPACE)
                        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                                  "MEM 0x%llx 0x%llx - Flags 0x%x",
                                  PCI_MEM_ADDR(BaseReg->Address), BaseReg->Size,
                                  BaseReg->Flags);
                    else
                        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                                  "ROM 0x%llx 0x%llx - Flags 0x%x",
                                  PCI_ROM_ADDR(BaseReg->Address), BaseReg->Size,
                                  BaseReg->Flags);
                }
            }
        }
    }
    return PCI_SUCCESS;
}
