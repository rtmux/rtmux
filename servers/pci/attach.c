/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * apertures
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include "globals.h"

/* get config of logical device */

pci_device_t *PCI_attach_device(void *handle,
                                uint32_t flags,
                                uint16_t idx,
                                struct pci_dev_info *ConfigBuffer,
                                pci_dev_list_t *list,
                                pid_t resource_owner_pid,
                                int *local_errno)
{
    pci_device_t *dev_ptr;
    struct BaseRegister *BaseReg;
    uint64_t Address;
    uint32_t ll_Address;
    uint16_t pci_flg;
    int i;
    bool show_pci = false, next_attach = false;
    bool cardbus_device = false;
    uint32_t flg;
    uint8_t BusNumber;
    uint8_t DevFuncNumber;
    bool service_attach;

    DEBUGFUNC();
    pci_flg = 0;
    *local_errno = EOK;
    service_attach = flags & PCI_SERVICE_ATTACH;
    if (!(flags & (PCI_INIT_ALL |
                   PCI_INIT_ROM))) { /* Special case for show_pci. Display all
                                        information but don't enable device
                                        */
        show_pci = true;
    }
    if (verbose > 2) {
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                  "%s: handle %p - flags 0x%x - idx 0x%x", __FUNCTION__, handle,
                  flags, idx);
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s: vendor 0x%x - device 0x%x",
                  __FUNCTION__, ConfigBuffer->VendorId, ConfigBuffer->DeviceId);
    }
    if (handle) {
        dev_ptr = validate_handle((pci_device_t *) handle);
        if (!dev_ptr) {
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Invalid handle",
                          __FUNCTION__);
            *local_errno = EINVAL;
            return NULL;
        }
        fill_config(ConfigBuffer, dev_ptr);
        fill_base(ConfigBuffer, dev_ptr);
        dev_ptr->LockFlg++; /* set device as locked */
        list->lockflg++;
        for (i = 0; i < NUM_DEVICES; i++) {
            if (list->dev_list[i] == dev_ptr) {
                break;
            }
            if (!list->dev_list[i]) {
                list->dev_list[i] = dev_ptr;
                list->num_devices++;
                break;
            }
        }
        return dev_ptr;
    } else {
        (void) idx;
        dev_ptr = search_busdev(ConfigBuffer->BusNumber, ConfigBuffer->DevFunc);

        if (!dev_ptr) {
            *local_errno = ENODEV;
            if (verbose > 2)
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: No device",
                          __FUNCTION__);
            return NULL;
        } else {
            if (dev_ptr->LockFlg) {
                if (!service_attach && !show_pci) {
                    if ((dev_ptr->AttachFlags & PCI_SHARE)) {
                        if (!(flags & PCI_SHARE)) {
                            *local_errno = EBUSY;
                        } else {
                            next_attach = true;
                        }
                    } else {
                        *local_errno = EBUSY;
                    }
                } else {
                    next_attach = true;
                }
            }
        }
    }
    if (*local_errno != EOK) {
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Busy", __FUNCTION__);
        return NULL;
    }

    if (!dev_ptr->ServiceAttach) {
        dev_ptr->ServiceAttach = (uint8_t) service_attach;
    } else {
        next_attach = true;
    }

    if (!show_pci)
        pci_flg = off_pci(dev_ptr->BusNumber, dev_ptr->DevFuncNumber,
                          dev_ptr->ClassCode);
    BusNumber = dev_ptr->BusNumber;
    DevFuncNumber = dev_ptr->DevFuncNumber;
    fill_config(ConfigBuffer, dev_ptr);
    last_device = dev_ptr; /* Cache the last device info */
    clast_bus = BusNumber;
    clast_devfn = (uint8_t) DevFuncNumber;

    if (dev_ptr->ClassCode ==
        (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
        cardbus_device = true;
    }

    if (show_pci) {
        flags = PCI_INIT_ALL | PCI_INIT_ROM | flags;
    } else {
        if (dev_ptr->PmState & 0x03) {
            if (power_up_device(BusNumber, DevFuncNumber, dev_ptr)) {
                *local_errno = EIO;
                on_pci(BusNumber, DevFuncNumber, pci_flg);
                return NULL;
            }
        }
    }

    /* First add up the size of all active registers, so that one chunk of IO
     * and/or Memory space can be allocated.
     */

    if ((*local_errno =
             addup_resources(dev_ptr, flags, show_pci, cardbus_device,
                             next_attach, resource_owner_pid)) != PCI_SUCCESS) {
        if (!show_pci)
            on_pci(BusNumber, DevFuncNumber, pci_flg);
        return NULL;
    }
    if (!show_pci && !next_attach)
        dev_ptr->ResourceOwner = resource_owner_pid;

    for (i = 0, flg = PCI_INIT_BASE0, pci_flg = 0; i < (cardbus_device ? 1 : 6);
         i++, flg <<= 1) {
        if (!(flags & flg)) /* Test user enable flags */
            continue;
        BaseReg = &dev_ptr->BaseReg[i];
        if (!BaseReg->Size)
            continue;
        (*pci_drivers->entry->map_addr)(
            ll_handle, BaseReg->Address, &Address,
            (uint32_t) (BaseReg->Flags | PCI_MAP_ADDR_PCI));
        if (BaseReg->Flags & PCI_IO_SPACE) {
            ConfigBuffer->PciBaseAddress[i] = Address;
            ConfigBuffer->CpuBaseAddress[i] = BaseReg->Address;
            ConfigBuffer->BaseAddressSize[i] = (uint32_t) BaseReg->Size;
            pci_flg |= PCI_ISPACEEN;
        } else if ((BaseReg->Flags & PCI_MEM_SPACE) ||
                   (BaseReg->Flags & PCI_MEM_PREFETCH)) {
            ConfigBuffer->PciBaseAddress[i] = Address;
            ConfigBuffer->CpuBaseAddress[i] = BaseReg->Address;
            ConfigBuffer->BaseAddressSize[i] = (uint32_t) BaseReg->Size;
            pci_flg |= PCI_MSPACEEN;
        }
        if (!show_pci) {
            ll_Address = (uint32_t) Address;
            if (PCI_write_config32(BusNumber, DevFuncNumber,
                                   (uint32_t) offsetof(struct _pci_config_regs,
                                                       Base_Address_Regs[i]),
                                   1, &ll_Address) != PCI_SUCCESS) {
                *local_errno = ENXIO;
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Write failed %d",
                              __FUNCTION__, i);
                on_pci(BusNumber, DevFuncNumber, pci_flg);
                return NULL;
            }
        }
    }
    BaseReg = &dev_ptr->ROMBase;
    if ((BaseReg->Flags & PCI_ROM_SPACE) && (flags & PCI_INIT_ROM) &&
        !cardbus_device) {
        (*pci_drivers->entry->map_addr)(
            ll_handle, BaseReg->Address, &Address,
            (uint32_t) (BaseReg->Flags | PCI_MAP_ADDR_PCI));
        ConfigBuffer->PciRom = Address;
        ConfigBuffer->CpuRom = BaseReg->Address;
        ConfigBuffer->RomSize = (uint32_t) BaseReg->Size;
        if (!show_pci) {
            ll_Address =
                ((uint32_t) Address | 1); /* Set address decode enable */
            if (PCI_write_config32(
                    BusNumber, DevFuncNumber,
                    offsetof(struct _pci_config_regs, ROM_Base_Address), 1,
                    &ll_Address) != PCI_SUCCESS) {
                *local_errno = ENXIO;
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Write failed ROM",
                              __FUNCTION__);
                on_pci(BusNumber, DevFuncNumber, pci_flg);
                return NULL;
            }
        }
        pci_flg |= PCI_MSPACEEN;
    }

    if (show_pci || next_attach) {
        ConfigBuffer->Irq = dev_ptr->IrqNumber;
    } else {
        ConfigBuffer->Irq = 0xff;
        if (get_an_irq(dev_ptr, cardbus_device, flags, next_attach,
                       local_errno)) {
            on_pci(BusNumber, DevFuncNumber, pci_flg);
            return NULL;
        }
        ConfigBuffer->Irq = dev_ptr->IrqNumber;
        ConfigBuffer->msi = dev_ptr->msi_irq;
    }

    PCI_translation(ll_handle, ConfigBuffer);

    if (flags & PCI_MASTER_ENABLE)
        pci_flg |= PCI_MASTEREN;

    if ((dev_ptr->ClassCode & 0xff0000) == PCI_CLASS_DISPLAY) {
        uint16_t cr;
        PCI_read_config16(BusNumber, DevFuncNumber,
                          offsetof(struct _pci_config_regs, Command), 1,
                          (char *) &cr);
        pci_flg |= cr;
        pci_flg |=
            PCI_COMMAND_IO_ENABLE; /* Always enable I/O on video adapters */
    }

    if (!show_pci) {
        on_pci(BusNumber, DevFuncNumber, pci_flg);
        if (!dev_ptr->AttachFlags) {
            dev_ptr->AttachFlags |= flags; /*Save the flag settings */
            if (!service_attach) {
                dev_ptr->LockFlg++; /* set device as locked */
                list->lockflg++;
            }
        } else {
            dev_ptr->AttachFlags |= flags;
            dev_ptr->LockFlg++; /* set device as locked */
            list->lockflg++;
        }
        if (!list->num_devices) {
            list->dev_list[0] = dev_ptr;
        } else {
            for (i = 0; i < NUM_DEVICES; i++) {
                if (list->dev_list[i] == dev_ptr) {
                    break;
                }
                if (!list->dev_list[i]) {
                    list->dev_list[i] = dev_ptr;
                    break;
                }
            }
        }
        list->num_devices++;
    } else {                /* I really don't think this is necessary! */
        dev_ptr->LockFlg++; /* set device as locked */
        list->lockflg++;
        dev_ptr->AttachFlags |= flags;
    }

    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s - Exit", __FUNCTION__);
    return dev_ptr;
}

/* Release all the resources associated with a particular device. */

static int release_resources(pci_device_t *device)
{
    struct BaseRegister *BaseReg;
    uint32_t address = 0;
    int i, enabled = 0;

    DEBUGFUNC();
    for (i = 0; i < 6; i++) {
        BaseReg = &device->BaseReg[i];
        enabled |= (int) (BaseReg->Flags & PCI_DEVICE_ENABLED);
        if (BaseReg->Address && !enabled) {
            PCI_write_config32(device->BusNumber, device->DevFuncNumber,
                               (uint32_t) offsetof(struct _pci_config_regs,
                                                   Base_Address_Regs[i]),
                               1, (char *) &address);
            BaseReg->Address &= PCI_IS_IO(BaseReg->Address) ? 0x3 : 0x0f;
        }
    }
    BaseReg = &device->ROMBase;
    enabled |= (int) (BaseReg->Flags & PCI_DEVICE_ENABLED);
    if (BaseReg->Address && !enabled) {
        PCI_write_config32(device->BusNumber, device->DevFuncNumber,
                           offsetof(struct _pci_config_regs, ROM_Base_Address),
                           1, (char *) &address);
        BaseReg->Address &= 0x7fe;
    }

    if (device->io_alloc.Address) {
        pci_free_resource(&device->io_alloc, device->BusNumber,
                          device->ResourceOwner);
    }
    if (device->mem_alloc.Address) {
        pci_free_resource(&device->mem_alloc, device->BusNumber,
                          device->ResourceOwner);
    }
    if (device->pref_alloc.Address) {
        pci_free_resource(&device->pref_alloc, device->BusNumber,
                          device->ResourceOwner);
    }

    /* Don't release the IRQ if this device is attached to a CardBus controller
     */
    if (device->bmap->bdev->ClassCode ==
        (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
        return 1; /* Leave device enabled */
    }

    if (device->IrqNumber != 0 && device->IrqNumber != 0xffffffff) {
        pci_free_irq(device);
    }
    return enabled;
}

/* release config of device */

int PCI_detach_device(void *handle, pci_dev_list_t *list, int close_flag)
{
    pci_device_t *device = DeviceHead;
    int i, status, enabled = 1;
    uint32_t flags = 0;

    DEBUGFUNC();
    status = PCI_DEVICE_NOT_FOUND;
    while (device) {
        if (device == (pci_device_t *) handle) {
            if (device->LockFlg) {
                if (list && list->lockflg) {
                    if (close_flag)
                        i = (list->lockflg == device->LockFlg) ? list->lockflg
                                                               : 1;
                    else
                        i = 1;
                    device->LockFlg -= (uint8_t) i;
                    list->lockflg -= i;
                } else {
                    device->LockFlg--;
                }
            } else {
                if (device->ServiceAttach) {
                    device->ServiceAttach = 0;
                }
            }
            flags = device->AttachFlags & (PCI_INIT_ALL | PCI_INIT_ROM);
            if (!device->Persistent && flags && !device->LockFlg &&
                !device->ServiceAttach) {
                enabled = release_resources(device);
            }
            if (list && !list->lockflg) {
                for (i = 0; i < NUM_DEVICES; i++) {
                    if (list->dev_list[i] == device) {
                        list->dev_list[i] = NULL;
                        list->num_devices--;
                    }
                }
            }
            status = PCI_SUCCESS;
            break;
        }
        device = device->next;
    }
    if (status == PCI_SUCCESS && !device->LockFlg && !device->ServiceAttach) {
        device->AttachFlags = 0; /* Clear attach flags if last detach */
    }

    if (status == PCI_SUCCESS) {
        uint32_t class = device->ClassCode & 0xff0000;

        if (class != PCI_CLASS_DISPLAY && class != PCI_CLASS_BRIDGE &&
            class != PCI_CLASS_PROCESSOR) {
            if (status == PCI_SUCCESS && !device->Persistent && !enabled) {
                off_pci(device->BusNumber, device->DevFuncNumber,
                        device->ClassCode);
            }
            if ((device->PmState & 0x03) && !device->Persistent && flags)
                power_down_device(device->BusNumber, device->DevFuncNumber,
                                  device);
        }
    }
    return status;
}

/* Setup I/O apertures in PCI-PCI bridges. */

int setup_pci_io(uint64_t io_size)
{
    hwres_claim_t rreq = {
        .length = io_size,
        .align = KB4,
        .flags = HWRES_IO_PORT | HWRES_FLAG_ALIGN | HWRES_FLAG_TOPDOWN,
    };
    int allocd = 0, i, j;
    uint64_t map_addr, size;
    uint8_t bus, devfn;
    bus_map_t *map;
    pci_device_t *dev;
    pci_bridge_t *br = pci_bridges;

    DEBUGFUNC();
    for (i = 0; i < num_pci_bridges; i++, br = br->next) {
        if (!br)
            break;
        j = br->sec_bus;
        if (!(map = find_bus((uint8_t) j))) {
            return -1;
        }
        if (map->io_reqd == 0 && map->io_start == 0)
            continue;
        if (map->io_start || map->io_end) { /* Bridge already setup */
            continue;
        }
        if (!allocd) {
            if (hwres_claim(&rreq, 1) != EOK) {
                pci_slogf(
                    _SLOGC_PCI, _SLOG_ERROR,
                    "%s: Unable to allocate I/O - Start 0x%llx - End 0x%llx",
                    __FUNCTION__, rreq.start, rreq.end);
                return -1;
            }
            (*pci_drivers->entry->map_addr)(ll_handle, rreq.start, &map_addr,
                                            PCI_IO_SPACE | PCI_MAP_ADDR_PCI);
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "CPU I/O 0x%llx - PCI I/O 0x%llx", rreq.start,
                          map_addr);
            allocd = 1;
        }
        size = map->io_reqd;
        while (map) {
            dev = map->bdev;
            if (dev->ClassCode == host_bridge_class)
                break;
            bus = dev->BusNumber;
            devfn = dev->DevFuncNumber;
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                      "Bridge Bus 0x%x - devfunc 0x%x", bus, devfn);
            if (map->io_start == 0) {
                map->io_start = map_addr;
                map->io_end = map_addr + (size - KB4 + (KB4 - 1));
            } else {
                if (map_addr > map->io_start) {
                    map->io_end = map_addr + (size - KB4 + (KB4 - 1));
                } else {
                    map->io_start = map_addr;
                }
            }

            if (reserve_resource(rreq.start, rreq.end, HWRES_IO_PORT,
                                 map->secondary_bus, 0)) {
                break;
            }

            if (configure_bridge_io(bus, devfn, map->io_start, map->io_end)) {
                return -1;
            }
            map = map->prev;
        }
        map_addr += size;
    }
    if (allocd) {
        if (hwres_release(&rreq, 1))
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: I/O detach failed",
                      __FUNCTION__);
    }

    return 0;
}

/* Setup prefetchable memory apertures in PCI-PCI bridges. */

int setup_pci_pref(uint64_t pref_size)
{
    hwres_claim_t rreq = {
        .length = pref_size,
        .align = MEG1,
        .flags = HWRES_PCI_MEMORY | HWRES_FLAG_ALIGN | HWRES_FLAG_TOPDOWN,
    };
    int allocd = 0, i, j;
    int rsvd = 0;
    uint64_t map_addr, size;
    uint8_t bus, devfn;
    bus_map_t *map;
    pci_device_t *dev;
    pci_bridge_t *br = pci_bridges;

    DEBUGFUNC();
    for (i = 0; i < num_pci_bridges; i++, br = br->next) {
        if (!br)
            break;
        j = br->sec_bus;
        if (!(map = find_bus((uint8_t) j)))
            return -1;
        if (map->pref_reqd == 0 && map->prefetch_mem_start == 0)
            continue;
        if (map->prefetch_mem_start ||
            map->prefetch_mem_end) { /* Bridge already setup */
            continue;
        }
        if (!allocd) {
            if (hwres_claim(&rreq, 1) != EOK) {
                return -1;
            }
            (*pci_drivers->entry->map_addr)(ll_handle, rreq.start, &map_addr,
                                            PCI_MEM_SPACE | PCI_MAP_ADDR_PCI);
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "CPU memory 0x%llx - PCI memory 0x%llx", rreq.start,
                          map_addr);
            allocd = 1;
        }
        size = map->pref_reqd;
        while (map) {
            dev = map->bdev;
            if (dev->ClassCode == host_bridge_class)
                break;
            bus = dev->BusNumber;
            devfn = dev->DevFuncNumber;
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "Bridge Bus 0x%x - devfunc 0x%x - Pref start 0x%llx "
                          "- end 0x%llx",
                          bus, devfn, map->prefetch_mem_start,
                          map->prefetch_mem_end);
            if (map->prefetch_mem_start == 0) {
                map->prefetch_mem_start = map_addr;
                map->prefetch_mem_end = map_addr + (size - MEG1 + (MEG1 - 1));
            } else {
                if (map_addr > map->prefetch_mem_start) {
                    map->prefetch_mem_end =
                        map_addr + (size - MEG1 + (MEG1 - 1));
                } else {
                    map->prefetch_mem_start = map_addr;
                }
            }

            if (!rsvd) {
                if (reserve_resource(rreq.start, rreq.end, HWRES_PCI_MEMORY,
                                     map->secondary_bus, 1)) {
                    break;
                }
                rsvd = 1;
            }

            if (configure_bridge_pref(bus, devfn, map->prefetch_mem_start,
                                      map->prefetch_mem_end)) {
                return -1;
            }
            map = map->prev;
        }
        map_addr += size;
    }
    if (allocd) {
        if (hwres_release(&rreq, 1))
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Pref detach failed",
                      __FUNCTION__);
    }

    return 0;
}


/* Setup memory apertures in PCI-PCI bridges. */

int setup_pci_mem(uint64_t mem_size)
{
    hwres_claim_t rreq = {
        .length = mem_size,
        .align = MEG1,
        .flags = HWRES_PCI_MEMORY | HWRES_FLAG_ALIGN | HWRES_FLAG_TOPDOWN,
    };
    int allocd = 0, i, j;
    uint64_t map_addr, size;
    uint8_t bus, devfn;
    bus_map_t *map;
    pci_device_t *dev;
    pci_bridge_t *br = pci_bridges;

    DEBUGFUNC();
    for (i = 0; i < num_pci_bridges; i++, br = br->next) {
        if (!br)
            break;
        j = br->sec_bus;
        if (!(map = find_bus((uint8_t) j)))
            return -1;
        if (map->mem_reqd == 0 && map->mem_start == 0)
            continue;
        if (map->mem_start || map->mem_end) { /* Bridge already setup */
            continue;
        }
        if (!allocd) {
            if (hwres_claim(&rreq, 1) != EOK) {
                return -1;
            }
            (*pci_drivers->entry->map_addr)(ll_handle, rreq.start, &map_addr,
                                            PCI_MEM_SPACE | PCI_MAP_ADDR_PCI);
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "CPU memory 0x%llx - PCI memory 0x%llx", rreq.start,
                          map_addr);
            allocd = 1;
        }
        size = map->mem_reqd;
        while (map) {
            dev = map->bdev;
            if (dev->ClassCode == host_bridge_class)
                break;
            bus = dev->BusNumber;
            devfn = dev->DevFuncNumber;
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                      "Bridge Bus 0x%x - devfunc 0x%x", bus, devfn);
            if (map->mem_start == 0) {
                map->mem_start = map_addr;
                map->mem_end = map_addr + (size - MEG1 + (MEG1 - 1));
            } else {
                if (map_addr > map->mem_start) {
                    map->mem_end = map_addr + (size - MEG1 + (MEG1 - 1));
                } else {
                    map->mem_start = map_addr;
                }
            }
            if (reserve_resource(rreq.start, rreq.end, HWRES_PCI_MEMORY,
                                 map->secondary_bus, 0)) {
                break;
            }

            if (configure_bridge_mem(bus, devfn, map->mem_start,
                                     map->mem_end)) {
                return -1;
            }
            map = map->prev;
        }
        map_addr += size;
    }
    if (allocd) {
        if (hwres_release(&rreq, 1))
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Mem detach failed",
                      __FUNCTION__);
    }

    return 0;
}
