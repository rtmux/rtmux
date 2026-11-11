/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * management
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"
#include <time.h>

static void sleep_msec(unsigned msec)
{
    struct timespec ts;

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (long) (msec % 1000) * 1000000L;
    (void) nanosleep(&ts, NULL);
}

/* This routine is called whenever a bridge type chip is encountered. On RISC-V
 * with ECAM, the config space base is already known. No vendor-specific bridge
 * probing is required.
 */

static int pci_bridge_setup(uint8_t bus, uint8_t devfn)
{
    DEBUGFUNC();
    /* On RISC-V with ECAM, the config space base is already known. */
    if (pci_ex_bar != 0) {
        if (verbose)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "PCI Express BAR 0x%llx - size 0x%x", pci_ex_bar,
                      pci_ex_size);
        return 0;
    }
    return -1;
}

/* If a bus structure has not been allocated for this bus, then allocate one and
 * fill it with the information regarding this bus. ie. Memory and prefetchable
 * memory ranges as well as I/O ranges. If this is a PCI-PCI bridge, then setup
 * the bus number registers.
 */

int configure_bridge(uint8_t bus, uint8_t devfunc, uint8_t sec_bus)
{
    uint64_t qword;
    uint64_t cpu_start, cpu_end;
    uint32_t dword, dword1;
    uint16_t word, word1;
    uint8_t byte;
    bus_map_t *map;


    DEBUGFUNC();
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "%s: bus %d - devfunc 0x%x - sec_bus %d", __FUNCTION__, bus,
                  devfunc, sec_bus);
    if (!(map = find_bus(sec_bus))) {
        /* This code is to handle the Intel 6XX chipset that has buses at 128
         * but no associated host bridge
         */
        if (((bus == 128) || (bus == 64)) && device_without_bridge) {
            if (!(map = alloc_bus(bus, bus))) {
                return -1;
            }
        }
        if (!(map = alloc_bus(bus, sec_bus)))
            return -1;
    }

    if (pci_bridge_setup(bus, devfunc)) {
        if (verbose)
            pci_slogf(_SLOGC_PCI, _SLOG_WARNING,
                      "No bridge definition for this device");
        /*      bridge_dll_load (bus, devfunc); */
    }

    PCI_read_config16(bus, devfunc,
                      offsetof(struct _pci_bridge_config_regs, Command), 1,
                      &word);
    word |= PCI_COMMAND_MASTER_ENABLE;
    PCI_write_config16(bus, devfunc,
                       offsetof(struct _pci_bridge_config_regs, Command), 1,
                       &word);
    if ((PCI_read_config8(bus, devfunc,
                          offsetof(struct _pci_bridge_config_regs, Header_Type),
                          1, (char *) &byte) != PCI_SUCCESS))
        return -1;
    byte &= 0x7f;
    if (byte == 0) { /* Header type 0 */
        map->io_start = io_pci_low;
        map->io_end = io_pci_high;
        map->mem_start = mem_pci_low;
        map->mem_end = mem_pci_high;
        return PCI_SUCCESS;
    }

    PCI_read_config32(
        bus, devfunc,
        offsetof(struct _pci_bridge_config_regs, Primary_Bus_Number), 1,
        &dword);
    dword = PCI_PRIMARY_BUS_INSERT(dword, bus);
    dword = PCI_SECONDARY_BUS_INSERT(dword, sec_bus);
    dword = PCI_SUBORDINATE_BUS_INSERT(
        dword, 0xff); /* Allow config. cycles to propagate */
    PCI_write_config32(
        bus, devfunc,
        offsetof(struct _pci_bridge_config_regs, Primary_Bus_Number), 1,
        &dword);
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "%s: Bus 0x%x - Devfn 0x%x - dword 0x%x", __FUNCTION__, bus,
                  devfunc, dword);

    if (byte == 1) { /* Header type 1 = PCI-PCI bridge */
        if ((PCI_read_config16(
                 bus, devfunc,
                 offsetof(struct _pci_bridge_config_regs, IO_Base), 1,
                 (char *) &word) != PCI_SUCCESS))
            return -1;
        word1 = 0;
        if (word & 0x0f) { /* 32-bit I/O addressing */
            if ((PCI_read_config16(
                     bus, devfunc,
                     offsetof(struct _pci_bridge_config_regs, IO_Base_Upper16),
                     1, (char *) &word1) != PCI_SUCCESS))
                return -1;
        }
        map->io_start =
            ((uint64_t) (word1 << 16) | ((uint64_t) (word & 0xf0) << 8));
        word1 = 0;
        if (word & 0x0f00) { /* 32-bit I/O addressing */
            if ((PCI_read_config16(
                     bus, devfunc,
                     offsetof(struct _pci_bridge_config_regs, IO_Limit_Upper16),
                     1, (char *) &word1) != PCI_SUCCESS))
                return -1;
        }
        map->io_end =
            (((uint64_t) (word1 << 16) | (uint64_t) (word & 0xf000)) + 0xfff);
        if (map->io_end > io_pci_high) {
            map->io_end = io_pci_high;
        }
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "%s: bus %d iostart 0x%llx - ioend 0x%llx", __FUNCTION__,
                      bus, map->io_start, map->io_end);
        if (map->io_end < map->io_start) { /* I/O not used */
            map->io_start = map->io_end = 0;
        } else {
            if (check_range(map->io_start, map->io_end - map->io_start,
                            PCI_IO_SPACE, bus)) {
                map->io_start = map->io_end = 0;
                word = 0xff;
                PCI_write_config16(
                    bus, devfunc,
                    offsetof(struct _pci_bridge_config_regs, IO_Base), 1,
                    (char *) &word);
            }
        }

        if ((PCI_read_config32(
                 bus, devfunc,
                 offsetof(struct _pci_bridge_config_regs, Memory_Base), 1,
                 (char *) &dword) != PCI_SUCCESS))
            return -1;
        map->mem_start = (uint64_t) (dword & 0xffff) << 16;
        map->mem_end = (uint64_t) ((dword & 0xfff00000) + 0xfffff);
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "%s: bus %d memstart 0x%llx - memend 0x%llx",
                      __FUNCTION__, bus, map->mem_start, map->mem_end);
        if (map->mem_end < map->mem_start) { /* Memory not used */
            map->mem_start = map->mem_end = 0;
        } else {
            if (check_range(map->mem_start, map->mem_end - map->mem_start,
                            PCI_MEM_SPACE, bus)) {
                map->mem_start = map->mem_end = 0;
                dword = 0xffff;
                PCI_write_config32(
                    bus, devfunc,
                    offsetof(struct _pci_bridge_config_regs, Memory_Base), 1,
                    (char *) &dword);
            }
        }

        if ((PCI_read_config32(bus, devfunc,
                               offsetof(struct _pci_bridge_config_regs,
                                        Prefetchable_Memory_Base),
                               1, (char *) &dword) != PCI_SUCCESS))
            return -1;
        qword = 0;
        if (dword & 0x0f) { /* 64-bit addressing */
            if ((PCI_read_config32(bus, devfunc,
                                   offsetof(struct _pci_bridge_config_regs,
                                            Prefetchable_Base_Upper32),
                                   1, (char *) &dword1) != PCI_SUCCESS))
                return -1;
            qword = (uint64_t) dword1;
        }
        map->prefetch_mem_start =
            (uint64_t) ((qword << 32) | ((dword & 0xfff0) << 16));
        qword = 0;
        if (dword & 0xf0000) { /* 64-bit addressing */
            if ((PCI_read_config32(bus, devfunc,
                                   offsetof(struct _pci_bridge_config_regs,
                                            Prefetchable_Limit_Upper32),
                                   1, (char *) &dword1) != PCI_SUCCESS))
                return -1;
            qword = (uint64_t) dword1;
        }
        map->prefetch_mem_end =
            (uint64_t) (((qword << 32) | (dword & 0xfff00000)) + 0xfffff);
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "%s: bus %d prefstart 0x%llx - prefend 0x%llx",
                      __FUNCTION__, bus, map->prefetch_mem_start,
                      map->prefetch_mem_end);
        if (map->prefetch_mem_end <
            map->prefetch_mem_start) { /* Memory not used */
            map->prefetch_mem_start = map->prefetch_mem_end = 0;
        } else {
            if (check_range(map->prefetch_mem_start,
                            map->prefetch_mem_end - map->prefetch_mem_start,
                            PCI_MEM_PREFETCH, bus)) {
                map->prefetch_mem_start = map->prefetch_mem_end = 0;
                dword = 0xffff;
                PCI_write_config32(bus, devfunc,
                                   offsetof(struct _pci_bridge_config_regs,
                                            Prefetchable_Memory_Base),
                                   1, (char *) &dword);
            }
        }

        if (map->io_start || map->io_end) {
            (*pci_drivers->entry->map_addr)(ll_handle, map->io_start,
                                            &cpu_start,
                                            PCI_IO_SPACE | PCI_MAP_ADDR_CPU);
            (*pci_drivers->entry->map_addr)(ll_handle, map->io_end, &cpu_end,
                                            PCI_IO_SPACE | PCI_MAP_ADDR_CPU);
            reserve_resource(cpu_start, cpu_end, HWRES_IO_PORT, sec_bus, 0);
        }

        if (map->mem_start || map->mem_end) {
            (*pci_drivers->entry->map_addr)(ll_handle, map->mem_start,
                                            &cpu_start,
                                            PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
            (*pci_drivers->entry->map_addr)(ll_handle, map->mem_end, &cpu_end,
                                            PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
            reserve_resource(cpu_start, cpu_end, HWRES_PCI_MEMORY, sec_bus, 0);
        }

        if (map->prefetch_mem_start || map->prefetch_mem_end) {
            (*pci_drivers->entry->map_addr)(ll_handle, map->prefetch_mem_start,
                                            &cpu_start,
                                            PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
            (*pci_drivers->entry->map_addr)(ll_handle, map->prefetch_mem_end,
                                            &cpu_end,
                                            PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
            reserve_resource(cpu_start, cpu_end, HWRES_PCI_MEMORY, sec_bus, 1);
        }
    }

    return PCI_SUCCESS;
}

/* Setup the primary, secondary and subordinate bus number registers of CardBus
 * controllers.
 */

int setup_cardbus_bridge(uint8_t Bus,
                         uint8_t DevFn,
                         pci_device_t *Dev,
                         uint8_t secbus)
{
    uint8_t tmp;
    bus_map_t *map, *map1;
    uint32_t dword;


    DEBUGFUNC();
    PCI_read_config32(Bus, DevFn, CARDBUS_PRI_BUS, 1, &dword);
    dword = PCI_PRIMARY_BUS_INSERT(dword, Bus);
    dword = PCI_SECONDARY_BUS_INSERT(dword, secbus);
    dword = PCI_SUBORDINATE_BUS_INSERT(dword, secbus);
    PCI_write_config32(Bus, DevFn, CARDBUS_PRI_BUS, 1, &dword);
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                  "%s: Bus %d - lastbus %d - dword 0x%x", __FUNCTION__, Bus,
                  lastbus, dword);

    if (scan_windows(Bus, DevFn, Dev) == -1) {
        return -1;
    }

    if (PCI_read_config8(Bus, DevFn,
                         offsetof(struct _pci_config_regs, Interrupt_Pin), 1,
                         &tmp) != PCI_SUCCESS) {
        return -1;
    }
    Dev->IntPin = tmp;

    PCI_read_config32(
        Bus, DevFn,
        offsetof(struct _pci_cardbus_config_regs, Subsystem_Vendor_Id), 1,
        (char *) &dword);
    Dev->SubVendorId = (uint16_t) (dword & 0xFFFF);
    Dev->SubDeviceId = (uint16_t) (dword >> 16);

    if (!(map = find_bus(secbus))) {
        if (!(map = alloc_bus(Bus, secbus)))
            return -1;
    }

    if (!Dev->bmap) /* Link the pci_device_t and bus_map_t structures */
        Dev->bmap = map;
    if (!map->bdev)
        map->bdev = Dev;
    Dev->Persistent = 1;

    if (Bus) {
        if (!(map1 = find_bus(Bus))) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Unable to find Bus %d",
                      __FUNCTION__, Bus);
        } else {
            map->mem_start = map1->mem_start;
            map->mem_end = map1->mem_end;
            map->io_start = map1->io_start;
            map->io_end = map1->io_end;
            map->prefetch_mem_start = map1->prefetch_mem_start;
            map->prefetch_mem_end = map1->prefetch_mem_end;
        }

        /* If the CardBus controller is on a bus other than zero, make sure that
         * the PCI-PCI bridge above it has memory and I/O apertures programmed.
         */

        if (map->mem_start == 0) {
            map1->mem_reqd += MEG1;
        }
        if (map->io_start == 0) {
            map1->io_reqd += KB4;
        }
    }
    return 0;
}

/* Traverse backwards through the bus_map_t structures. */

int traverse_back(uint8_t sub_bus)
{
    pci_device_t *dev;
    bus_map_t *map;
    uint32_t dword;
    uint8_t devfn, byte;
    uint8_t bus, sbus;
    int i;
    pci_bridge_t *br;

    DEBUGFUNC();
    sbus = (uint8_t) cur_scan_bus;
    if (!(map = find_bus(sbus))) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Bus %d not found!",
                  __FUNCTION__, sbus);
        return -1;
    }
    dev = map->bdev;
    bus = dev->BusNumber;
    devfn = dev->DevFuncNumber;
    if ((PCI_read_config8(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs, Header_Type),
                          1, (char *) &byte) != PCI_SUCCESS)) {
        return -1;
    }
    byte &= 0x7f;
    if (byte == 1) { /* This is a PCI-PCI bridge */
        PCI_read_config32(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, Primary_Bus_Number), 1,
            &dword);
        dword = PCI_PRIMARY_BUS_INSERT(dword, bus);
        dword = PCI_SECONDARY_BUS_INSERT(dword, sbus);
        if (!map->next) {
            dword = PCI_SUBORDINATE_BUS_INSERT(dword, sbus);
            map->subordinate_bus = sbus;
        } else {
            dword = PCI_SUBORDINATE_BUS_INSERT(dword, sub_bus);
            map->subordinate_bus = sub_bus;
        }
        PCI_write_config32(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, Primary_Bus_Number), 1,
            &dword);
        if (verbose > 2) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "%s: Bus 0x%x - Devfn 0x%x - dword 0x%x", __FUNCTION__,
                      bus, devfn, dword);
        }
        for (i = 0, br = pci_bridges; i < num_pci_bridges; i++) {
            if (br->bus == bus && br->devfn == devfn) {
                if (br->sec_bus != sbus)
                    br->sec_bus = sbus;
                if (br->sub_bus < sub_bus)
                    br->sub_bus = sub_bus;
                if (br->sub_bus == 0xff)
                    br->sub_bus = sub_bus;
                break;
            }
            br = br->next;
            if (!br)
                break;
        }
    }
    cur_scan_bus = map->primary_bus;
    if (cur_scan_bus == 0) {
        endbus = 0;
    }
    while (map->prev) {
        map = map->prev;
        dev = map->bdev;
        if (!dev) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: dev is NULL!",
                      __FUNCTION__);
            break;
        }
        if (dev->BusNumber == bus)
            continue;
        if (map->subordinate_bus < sub_bus) {
            map->subordinate_bus = sub_bus;
        }
    }
    return PCI_SUCCESS;
}

/* Power down a PCI device */

int power_down_device(uint8_t bus, uint8_t devfunc, pci_device_t *dev)
{
    uint8_t cap_ptr = dev->PmCap;
    uint16_t cmd, sav_cmd, pwr = dev->PmState;

    DEBUGFUNC();
    if (PCI_read_config16(bus, devfunc,
                          offsetof(struct _pci_config_regs, Command), 1,
                          &sav_cmd) != PCI_SUCCESS)
        return -1;
    cmd = sav_cmd & (uint16_t) ~0x7u;
    if (PCI_write_config16(bus, devfunc,
                           offsetof(struct _pci_config_regs, Command), 1,
                           &cmd) != PCI_SUCCESS)
        return -1;
    if (PCI_write_config16(bus, devfunc, cap_ptr + 4, 1, &pwr) != PCI_SUCCESS)
        return -1;
    if (PCI_write_config16(bus, devfunc,
                           offsetof(struct _pci_config_regs, Command), 1,
                           &sav_cmd) != PCI_SUCCESS)
        return -1;
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "Powered down bus 0x%x - devfunc 0x%x", bus, devfunc);
    return EOK;
}

/* Power up a PCI device */

int power_up_device(uint8_t bus, uint8_t devfunc, pci_device_t *dev)
{
    uint8_t cap_ptr = dev->PmCap;
    uint16_t cmd, sav_cmd, pwr = dev->PmState;
    uint32_t pci_regs[20];

    DEBUGFUNC();
    if (pwr & 0x3) {
        if (PCI_read_config16(bus, devfunc,
                              offsetof(struct _pci_config_regs, Command), 1,
                              &sav_cmd) != PCI_SUCCESS)
            return -1;
        cmd = sav_cmd & (uint16_t) ~0x7u;
        if (PCI_write_config16(bus, devfunc,
                               offsetof(struct _pci_config_regs, Command), 1,
                               &cmd) != PCI_SUCCESS)
            return -1;
        if (PCI_read_config32(bus, devfunc, 0, 20, pci_regs) != PCI_SUCCESS)
            return -1;
        pwr &= (uint16_t) ~3u;
        if (PCI_write_config16(bus, devfunc, cap_ptr + 4, 1, &pwr) !=
            PCI_SUCCESS)
            return -1;
        sleep_msec(20);
        if (PCI_write_config32(bus, devfunc, 0, 20, pci_regs) != PCI_SUCCESS)
            return -1;
        if (PCI_write_config16(bus, devfunc,
                               offsetof(struct _pci_config_regs, Command), 1,
                               &sav_cmd) != PCI_SUCCESS)
            return -1;
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "Powered up bus 0x%x - devfunc 0x%x", bus, devfunc);
    }
    return EOK;
}
