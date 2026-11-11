/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * pci_device_t scanning, bridge discovery, and bus enumeration.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"

/* scan given device, store config */

static int scan_device(uint8_t bus, uint8_t SecBus, uint8_t devfn)
{
    uint32_t dword;
    uint8_t byte, hdr_type, func;
    uint8_t sec;
    int i, index;
    uint16_t vendor, deviceid, status;
    uint32_t class, subclass;
    pci_device_t *device, *Dev;
    pci_bus_t *pcib;
    bus_map_t *map;

    DEBUGFUNC();
    if (!(Dev = calloc(1, sizeof(*Dev))))
        return -1;

    func = (devfn & 0x07);
    device = DeviceHead;
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "%s: Bus %d - pci_device_t %d - Function %d - SecBus %d",
                  __FUNCTION__, bus, devfn >> 3, devfn & 0x7, SecBus);
    Dev->BusNumber = bus;
    Dev->DevFuncNumber = devfn;
    /* vendor, device */
    PCI_read_config32(bus, devfn, offsetof(struct _pci_config_regs, Vendor_ID),
                      1, (char *) &dword);
    Dev->VendorId = vendor = (uint16_t) (dword & 0xFFFF);
    Dev->DeviceId = deviceid = (uint16_t) (dword >> 16);

    if (device) {
        index = 0;
        while (device) {
            if (device->VendorId == vendor && device->DeviceId == deviceid) {
                index = device->Index + 1;
            }
            device = device->next;
        }
    } else
        index = 0;

    Dev->Index = (uint8_t) index;
    /* class code */
    PCI_read_config32(bus, devfn,
                      offsetof(struct _pci_config_regs, Revision_ID), 1,
                      (char *) &dword);
    Dev->RevisionId = (uint8_t) dword & 0xff;
    dword >>= 8;
    Dev->ClassCode = dword;
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "Vendor 0x%x pci_device_t 0x%x Index %d ClassCode 0x%x",
                  vendor, deviceid, index, dword);

    if (Dev->ClassCode == (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
        if (func < 2) {
            PCI_read_config32(
                bus, devfn,
                offsetof(struct _pci_cardbus_config_regs, Pci_Bus_Num), 1,
                (char *) &dword);
            sec = (uint8_t) ((dword & 0xff00) >> 8);
            if (sec > 0 && sec <= scan_buses) {
                cardbus = SecBus = sec;
            }
            if (bus && !cardbus) {
                SecBus = (uint8_t) (bus + 1 + func);
            }

            /*          if (cardbus) {
             *              ++cardbus;
             *              }
             *          else {
             */
            cardbus = SecBus;
            /*              } */
            endbus = cardbus;
        }
    }
    if (!(map = find_bus(SecBus))) {
        if (!(map = alloc_bus(bus, SecBus))) {
            /* This code is to handle the Intel 6XX chipset that has buses at
             * 128 but no associated host bridge
             */
            if (((bus == 127) || (bus == 63)) && device_without_bridge) {
                if (!(map = alloc_bus(0, bus))) {
                    free(Dev);
                    return -1;
                }
            }
        }
    }
    class = (Dev->ClassCode & 0xff0000);
    subclass = (Dev->ClassCode & 0xff00);

    pcib = map->pci_bus;
    if (!Dev->bmap) /* Link the pci_device_t and bus_map_t structures */
        Dev->bmap = map;
    if (!map->bdev)
        map->bdev = Dev;

    if (!DeviceHead) {
        DeviceHead = Dev;
        DeviceTail = Dev;
    } else {
        DeviceTail->next = Dev;
        DeviceTail = Dev;
    }
    pcib->ndev++;

    /* header type */
    PCI_read_config8(bus, devfn, offsetof(struct _pci_config_regs, Header_Type),
                     1, (char *) &hdr_type);
    Dev->HeaderType = hdr_type;

    if (Dev->ClassCode == (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
        if (func < 2) {
            setup_cardbus_bridge(bus, devfn, Dev, cardbus);
        }
    }

    if ((Dev->ClassCode & 0xffff00) == PCI_CLASS_BRIDGE) {
        /* If the pciex_space was not set by the server, then see if the
         * low-level code will return an address and size
         */
        if ((*pci_drivers->entry->cnfg_bridge)(ll_handle, bus, devfn, pcib) !=
            PCI_SUCCESS) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Unable to configure bridge",
                      __FUNCTION__);
            return -1;
        }
        if (pcib->pciex_addr) {
            pciex_space = (uint8_t *) pcib->pciex_addr;
            pci_ex_size = pcib->pciex_size;
        }
        if (verbose && pciex_space) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "PCIEX base %p - size 0x%x",
                      pciex_space, pci_ex_size);
        }
    }
    /* pci status */
    PCI_read_config16(bus, devfn, offsetof(struct _pci_config_regs, Status), 1,
                      (char *) &status);
    if ((status & PCI_STATUS_BACKTOBACK_OKAY) == 0)
        pcib->fast_b2b = 0;
    if ((status & PCI_STATUS_66MHZ_OKAY) == 0)
        pcib->freq66 = 0;
    i = (status & PCI_STATUS_DEVSEL_MASK) >> PCI_STATUS_DEVSEL_SHIFT;
    if (i > pcib->devsel)
        pcib->devsel = (uint8_t) i;

    PCI_read_config8(bus, devfn, offsetof(struct _pci_config_regs, Min_Gnt), 1,
                     (char *) &byte);
    Dev->Min_Gnt = byte;
    PCI_read_config8(bus, devfn, offsetof(struct _pci_config_regs, Max_Lat), 1,
                     (char *) &byte);
    Dev->Max_Lat = byte;
    if (Dev->Min_Gnt != 0 || Dev->Max_Lat != 0) {
        /* find largest minimum grant time of all devices */
        if (Dev->Min_Gnt != 0 && Dev->Min_Gnt > pcib->Min_Gnt)
            pcib->Min_Gnt = Dev->Min_Gnt;

        /* find smallest maximum latency time of all devices */
        if (Dev->Max_Lat != 0 && Dev->Max_Lat < pcib->Max_Lat)
            pcib->Max_Lat = Dev->Max_Lat;

        /* subtract our minimum on-bus time per second from bus bandwidth */
        pcib->bandwidth -= (uint32_t) (Dev->Min_Gnt * 132000000 /
                                       (Dev->Min_Gnt + Dev->Max_Lat));
        if (verbose) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "Min_Gnt = 0x%x - Max_Lat = 0x%x - Bandwidth = 0x%x",
                      Dev->Min_Gnt, Dev->Max_Lat, pcib->bandwidth);
        }
    }

    /* Check the capabilities */

    PCI_read_config16(bus, devfn, offsetof(struct _pci_config_regs, Status), 1,
                      (char *) &status);
    if (status & PCI_STATUS_CAPABILITIES) {
        if (check_capabilities(bus, devfn, Dev))
            return -1;
    }
    /* Exit if host bridge type */
    if (((hdr_type & 0x7f) == 0) && (class == PCI_CLASS_BRIDGE) &&
        (subclass == PCI_SUBCLASS_BRIDGE_HOST)) {
        return PCI_SUCCESS;
    }

    /* base registers */
    scan_windows(bus, devfn, Dev);

    PCI_read_config32(bus, devfn,
                      offsetof(struct _pci_config_regs, Sub_Vendor_ID), 1,
                      (char *) &dword);
    Dev->SubVendorId = (uint16_t) (dword & 0xFFFF);
    Dev->SubDeviceId = (uint16_t) (dword >> 16);

    /* irq */
    if (map->bdev->ClassCode ==
        (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
        Dev->IrqNumber = map->bdev->IrqNumber;
        byte = (uint8_t) Dev->IrqNumber;
        PCI_write_config8(bus, devfn,
                          offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                          (char *) &byte);
    }
    PCI_read_config8(bus, devfn,
                     offsetof(struct _pci_config_regs, Interrupt_Pin), 1,
                     (char *) &byte);
    if (byte == 0x0) {
        Dev->IrqNumber = 0xffffffff;
        Dev->IntPin = 0;
    } else {
        Dev->IntPin = byte;
        PCI_read_config8(bus, devfn,
                         offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                         (char *) &byte);
        Dev->IrqNumber = (byte == 0xff) ? 0xffffffff : byte;
        if (byte != 0xff) {
            if (pci_reserve_irq((uint32_t) byte)) {
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                          "%s: Unable to reserve IRQ %d", __FUNCTION__, byte);
            }
        }
    }
    PCI_read_config8(bus, devfn,
                     offsetof(struct _pci_config_regs, Latency_Timer), 1,
                     (char *) &byte);
    if (!byte || byte > 0x40) { /* Plug the latency timer if not initialized */
        byte = 0x20;            /* Default value */
        PCI_write_config8(bus, devfn,
                          offsetof(struct _pci_config_regs, Latency_Timer), 1,
                          (char *) &byte);
    }
    return PCI_SUCCESS;
}

/* This routine scans through all the PCI buses setting up the PCI-PCI bridges.
 */

static int setup_pci2pci_bridges(uint8_t bus)
{
    int DevNumber, devfn, NumFuncs;
    int FuncNumber, i, found = 0;
    uint32_t dword, dword1;
    uint8_t byte, lb = 0;
    pci_bridge_t *br, *br_pre;

    DEBUGFUNC();
    if (verbose > 2) {
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: %d - Lastbus = %d", __FUNCTION__,
                  bus, lastbus);
    }
    for (DevNumber = 0; DevNumber < 32; DevNumber++) {
        NumFuncs = 1;
        for (FuncNumber = 0; FuncNumber < NumFuncs; FuncNumber++) {
            devfn = (DevNumber << 3) | FuncNumber;
            cur_scan_bus = bus;
            if ((PCI_read_config32(
                     bus, (uint8_t) devfn,
                     (uint32_t) offsetof(struct _pci_config_regs, Revision_ID),
                     1, (char *) &dword) != PCI_SUCCESS))
                break;
            dword &= 0xffffff00;
            if (dword == 0 || dword == 0xffffff00)
                continue;
            dword >>= 8;
            if (PCI_read_config8(
                    bus, (uint8_t) devfn,
                    (uint32_t) offsetof(struct _pci_config_regs, Header_Type),
                    1, (char *) &byte) != PCI_SUCCESS)
                break;
            if (FuncNumber == 0) {
                if ((PCI_read_config32(bus, (uint8_t) devfn,
                                       (uint32_t) offsetof(
                                           struct _pci_config_regs, Vendor_ID),
                                       1, (char *) &dword1) != PCI_SUCCESS)) {
                    break;
                }
                if (dword1 == 0xffffffff) {
                    if (verbose > 2) {
                        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                                  "%s: Bus %d - devfn 0x%x - Vendor 0x%x",
                                  __FUNCTION__, bus, devfn, dword1);
                    }
                    continue;
                }
                if (byte & 0x80)
                    NumFuncs = 8;
            }
            byte &= 0x7f;
            if ((dword & 0xff0000) != (PCI_CLASS_BRIDGE))
                continue;
            if (byte == 1 || byte == 2) {
                found = 1;

                if (byte == 1) { /* PCI-PCI header type */
                    for (i = 0, br = br_pre = pci_bridges; i < num_pci_bridges;
                         i++) {
                        if (br->bus == bus && br->devfn == devfn) {
                            if (br->sec_bus)
                                lb = br->sec_bus;
                            else
                                lb = lastbus;
                            break;
                        }
                        br_pre = br;
                        br = br->next;
                        if (!br)
                            break;
                    }
                    if (i >= num_pci_bridges || !br) {
                        if (!br)
                            br = br_pre;
                        if (!(br->next = calloc(1, sizeof(pci_bridge_t)))) {
                            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                                      "%s: Unable to allocate bridge structure",
                                      __FUNCTION__);
                            return 0;
                        }
                        br = br->next;
                        br->bus = bus;
                        br->devfn = (uint8_t) devfn;
                        br->class = dword;
                        num_pci_bridges++;
                        lb = lastbus;
                    }
                    configure_bridge(bus, (uint8_t) devfn, lb);
                } else {
                    if (PCI_read_config8(
                            bus, (uint8_t) devfn,
                            (uint32_t) offsetof(struct _pci_cardbus_config_regs,
                                                Cardbus_Bus_Num),
                            1, (char *) &lb) != PCI_SUCCESS)
                        break;
                    num_pci_bridges++;
                    if (!lb) {
                        if (cur_scan_bus) {
                            lb = (uint8_t) ((FuncNumber < 2) ? cur_scan_bus + 1
                                                             : cardbus);
                        } else {
                            lb = lastbus;
                        }
                    }
                }

                if (scan_device(bus, lb, (uint8_t) devfn) != PCI_SUCCESS) {
                    return -1;
                }
                bridge_count++;

                if (!setup_pci2pci_bridges(lb)) {
                    if (traverse_back(endbus)) {
                        return 0;
                    }
                }
                found = 0;
            }
        }
        if (verbose > 3)
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                      "%s - last_bus %d - scan_buses %d - lb %d", __FUNCTION__,
                      lastbus, scan_buses, lb);
        if (lastbus >= scan_buses && bridge_count == num_pci_bridges &&
            DevNumber >= 32)
            break;
    }
    return found;
}

/* Find a host bridge. This routine assumes that the first device found is the
 * host bridge chip and sets up the host_bridge_class variable with the class of
 * this device.
 */

static int find_host_bridge(uint32_t class)
{
    uint8_t bus, dev, func, lb;
    uint8_t devfn, numfunc, byte;
    int status, found;
    int num_host_bridges = 0;
    uint32_t dword;

    DEBUGFUNC();
    for (dev = bus = 0, found = 0; dev < 32; dev++) {
        numfunc = 1;
        for (func = 0; func < numfunc; func++) {
            devfn = (dev << 3) | func;
            if (func == 0) {
                if (PCI_read_config8(
                        bus, devfn,
                        offsetof(struct _pci_config_regs, Header_Type), 1,
                        (char *) &byte) != PCI_SUCCESS)
                    break;
                if (byte & 0x80)
                    numfunc = 8;
            }
            if ((status = PCI_read_config32(
                     bus, devfn, offsetof(struct _pci_config_regs, Revision_ID),
                     1, (char *) &dword)) != PCI_SUCCESS) {
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                          "%s: Read config status 0x%x", __FUNCTION__, status);
                break;
            }
            dword &= 0xffffff00;
            if (dword == 0 || dword == 0xffffff00)
                continue;
            dword >>= 8;
            if ((dword & 0xffff00) == class || class == 0xffffffff) {
                if ((byte & 0x7f) != 0 &&
                    (class == PCI_CLASS_BRIDGE)) { /* Some bridges have an
                                                      incorrect class code.
                                                      */
                    continue;
                }
                if (verbose > 2) {
                    pci_slogf(
                        _SLOGC_PCI, _SLOG_INFO,
                        "Found host bridge bus 0x%x - devfn 0x%x - class 0x%x",
                        bus, devfn, dword);
                }
                if (found &&
                    (class == 0xffffffff && dword != host_bridge_class)) {
                    continue;
                }
                if (configure_bridge(bus, devfn, lastbus) !=
                    PCI_SUCCESS) { /* Configure the host/pci bridge first */
                    return -1;
                }
                lb = lastbus ? lastbus - 1 : lastbus;
                if (scan_device(bus, lb, devfn) != PCI_SUCCESS) {
                    return -1;
                }
                endbus = (uint8_t) setup_pci2pci_bridges(lb);
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "%s - bridge_count %d - num_pci_bridges %d",
                          __FUNCTION__, bridge_count, num_pci_bridges);

                /* This code is to handle the Intel 6XX chipset that has buses
                 * at 128 but no associated host bridge
                 */
                if (bridge_count != num_pci_bridges) {
                    pci_bridge_t *br = pci_bridges;
                    int i = 0;
                    while (br) {
                        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                                  "%s - bridge bus %x", __FUNCTION__, br->bus);
                        br = br->next;
                        if (++i == bridge_count)
                            break;
                    }
                    lb = br->bus;
                    endbus = (uint8_t) setup_pci2pci_bridges(lb);
                }
                if (host_bridge_class == 0xffffffff) {
                    host_bridge_class = dword;
                }
                found = 1;
                num_host_bridges++;
            }
        }
    }
    if (verbose && found) {
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "No. Host bridges %d - Class 0x%x",
                  num_host_bridges, host_bridge_class);
    }
    return found ? PCI_SUCCESS : -1;
}

/* A debug function to scan the pci bus for all devices. */

static void find_all(void)
{
    int bus, dev, func, numfunc, devfunc;
    int vend, device;
    uint32_t dword;
    uint8_t byte;

    DEBUGFUNC();
    for (bus = 0; bus <= scan_buses; bus++) {
        for (dev = 0; dev < 32; dev++) {
            numfunc = 1;
            for (func = 0; func < numfunc; func++) {
                devfunc = (dev << 3) | func;
                if ((PCI_read_config32(
                         (uint8_t) bus, (uint8_t) devfunc,
                         (uint32_t) offsetof(struct _pci_config_regs,
                                             Revision_ID),
                         1, (char *) &dword) != PCI_SUCCESS))
                    continue;
                /*                  break; */
                dword &= 0xffffff00;
                if (dword == 0 || dword == 0xffffff00)
                    continue;
                dword >>= 8;
                if (func == 0) {
                    if (PCI_read_config8(
                            (uint8_t) bus, (uint8_t) devfunc,
                            (uint32_t) offsetof(struct _pci_config_regs,
                                                Header_Type),
                            1, (char *) &byte) != PCI_SUCCESS)
                        break;
                    if (byte & 0x80)
                        numfunc = 8;
                }
                PCI_read_config32(
                    (uint8_t) bus, (uint8_t) devfunc,
                    (uint32_t) offsetof(struct _pci_config_regs, Vendor_ID), 1,
                    (char *) &dword);
                vend = (int) (dword & 0xFFFF);
                device = (int) (dword >> 16);
                PCI_read_config32(
                    (uint8_t) bus, (uint8_t) devfunc,
                    (uint32_t) offsetof(struct _pci_config_regs, Revision_ID),
                    1, (char *) &dword);
                dword >>= 8;
                pci_slogf(
                    _SLOGC_PCI, _SLOG_INFO,
                    "Bus %d - Dev %d - Func %d - Vend 0x%x - pci_device_t "
                    "0x%x - Class 0x%x",
                    bus, dev, func, vend, device, dword);
            }
        }
    }
}

static void dump_bridge(int bus, int devfn)
{
    struct _pci_bridge_config_regs regs;
    uint32_t *p;
    int i;

    DEBUGFUNC();
    PCI_read_config32((uint8_t) bus, (uint8_t) devfn, 0, 0x40, (char *) &regs);
    for (i = 0, p = (uint32_t *) &regs; i < 0x40; i += 4) {
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "0x%x - 0x%08x", i, *p++);
    }
}

/* This routine finds all PCI/PCI bridges and sets the secondary and subordinate
 * buses to zero, except if the bus contains a video controller.
 */

static int find_bridges(void)
{
    int bus, dev, func, numfunc, devfunc;
    int bus_idx = 0;
    uint32_t class;
    uint32_t dword;
    uint8_t byte;
    uint8_t pri, sec, sub;
    pci_bridge_t *br, *br1;

    DEBUGFUNC();
    for (bus = 0; bus < scan_buses; bus++) {
        for (dev = 0; dev < 32; dev++) {
            numfunc = 1;
            for (func = 0; func < numfunc; func++) {
                devfunc = (dev << 3) | func;
                PCI_read_config32(
                    (uint8_t) bus, (uint8_t) devfunc,
                    (uint32_t) offsetof(struct _pci_bridge_config_regs,
                                        Vendor_ID),
                    1, (char *) &dword);
                if (dword == 0xffffffff) {
                    continue;
                }
                if ((PCI_read_config32(
                         (uint8_t) bus, (uint8_t) devfunc,
                         (uint32_t) offsetof(struct _pci_config_regs,
                                             Revision_ID),
                         1, (char *) &dword) != PCI_SUCCESS))
                    break;
                dword &= 0xffffff00;
                if (dword == 0 || dword == 0xffffff00)
                    continue;
                dword >>= 8;
                if (PCI_read_config8((uint8_t) bus, (uint8_t) devfunc,
                                     (uint32_t) offsetof(
                                         struct _pci_config_regs, Header_Type),
                                     1, (char *) &byte) != PCI_SUCCESS)
                    break;
                if (func == 0) {
                    if (byte & 0x80)
                        numfunc = 8;
                }
                class = (dword & 0xffff00);
                if ((byte & 0x7f) != 1) { /* Header Type */
                    if ((byte & 0x7f) == 2) {
                        PCI_read_config32(
                            (uint8_t) bus, (uint8_t) devfunc,
                            (uint32_t) offsetof(struct _pci_cardbus_config_regs,
                                                Pci_Bus_Num),
                            1, (char *) &dword);
                        pri = (uint8_t) (dword & 0xff);
                        sec = (uint8_t) ((dword & 0xff00) >> 8);
                        sub = (uint8_t) ((dword & 0xff0000) >> 16);
                        if (verbose > 2) {
                            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                                      "CardBus Bridge Bus %d - Dev %d - Func "
                                      "%d - Pri %d - Sec %d - Sub %d",
                                      bus, dev, func, pri, sec, sub);
                        }
                    }
                    continue;
                }
                PCI_read_config32(
                    (uint8_t) bus, (uint8_t) devfunc,
                    (uint32_t) offsetof(struct _pci_bridge_config_regs,
                                        Primary_Bus_Number),
                    1, (char *) &dword);
                pri = (uint8_t) (dword & 0xff);
                sec = (uint8_t) ((dword & 0xff00) >> 8);
                sub = (uint8_t) ((dword & 0xff0000) >> 16);
                if (verbose > 2) {
                    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                              "PCI Bridge Bus %d - Dev %d - Func %d - Pri %d - "
                              "Sec %d - Sub %d",
                              bus, dev, func, pri, sec, sub);
                }
                if (!(br = calloc(1, sizeof(pci_bridge_t)))) {
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                              "%s: Unable to allocate bridge structure",
                              __FUNCTION__);
                    return -1;
                }
                br->bus = (uint8_t) bus;
                br->devfn = (uint8_t) devfunc;
                br->sec_bus = sec;
                br->sub_bus = sub;
                br->class = class;
                if (sub >= scan_buses) {
                    scan_buses = (sub >= 0xfd) ? sub : sub + 2;
                    if (verbose > 2)
                        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                                  "%s: scan_buses set to %d", __FUNCTION__,
                                  scan_buses);
                }
                if (!pci_bridges)
                    pci_bridges = br;
                else {
                    br1 = pci_bridges;
                    while (br1->next)
                        br1 = br1->next;
                    br1->next = br;
                }
                bus_idx++;
                if (verbose > 3)
                    dump_bridge(bus, devfunc);
            }
        }
    }
    num_pci_bridges = bus_idx;

    return 0;
}

/* Scan all the PCI buses, setting up the PCI-PCI bridges and devices below
 * these bridges.
 */

int scan_all(void)
{
    int DevNumber, FuncNumber, devfn;
    int NumFuncs, lb;
    uint32_t dword;
    uint8_t byte, bus = 0;
    pci_bridge_t *br;

    DEBUGFUNC();
    if (verbose > 3) {
        find_all();
    }

    if (find_bridges())
        return -1;

    if (find_host_bridge(PCI_CLASS_BRIDGE) != PCI_SUCCESS) {
        if (find_host_bridge(0xffffffff) != PCI_SUCCESS) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: No host bridges found",
                      __FUNCTION__);
            return -1;
        }
    }

    for (bus = 0; bus < scan_buses; bus++) {
        for (DevNumber = 0; DevNumber < 32; DevNumber++) {
            NumFuncs = 1;
            for (FuncNumber = 0; FuncNumber < NumFuncs; FuncNumber++) {
                devfn = (DevNumber << 3) | FuncNumber;
                if ((PCI_read_config32(bus, (uint8_t) devfn,
                                       (uint32_t) offsetof(
                                           struct _pci_config_regs, Vendor_ID),
                                       1, (char *) &dword) != PCI_SUCCESS))
                    continue;
                /*                  break; */
                if (dword == 0 || dword == 0xffffffff)
                    continue;
                if (FuncNumber == 0) {
                    if (PCI_read_config8(
                            bus, (uint8_t) devfn,
                            (uint32_t) offsetof(struct _pci_config_regs,
                                                Header_Type),
                            1, (char *) &byte) != PCI_SUCCESS)
                        break;
                    if (byte & 0x80)
                        NumFuncs = 8;
                }
                if ((PCI_read_config32(
                         bus, (uint8_t) devfn,
                         (uint32_t) offsetof(struct _pci_config_regs,
                                             Revision_ID),
                         1, (char *) &dword) != PCI_SUCCESS))
                    /*                  break; */
                    continue;
                dword &= 0xffffff00;
                switch (dword) {
                case 0:
                    if (class_override & 1)
                        continue;
                    break;
                case 0xffffff00:
                    if (class_override & 2)
                        continue;
                    break;
                default:
                    break;
                }

                /*              if (!class_override) {
                 *                  if (dword == 0 || dword == 0xffffff00)
                 *                      continue;
                 *                  }
                 */
                dword >>= 8;
                dword &= 0xffff00;
                if (dword == (PCI_CLASS_BRIDGE) ||
                    dword == (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_PCI) ||
                    dword == (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
                    continue; /* Bridges have already been setup */
                }

                if (scan_device(bus, bus, (uint8_t) devfn) != PCI_SUCCESS) {
                    return -1;
                }
            }
        }
    }
    if (num_pci_bridges) {
        for (lb = 0, br = pci_bridges; lb < num_pci_bridges; lb++) {
            if (br->sub_bus > lastbus)
                lastbus = br->sub_bus;
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "PCI Bridge Bus %d - DevFn 0x%x - Sec %d - Sub %d",
                          br->bus, br->devfn, br->sec_bus, br->sub_bus);
            br = br->next;
            if (!br)
                break;
        }
    }

    return PCI_SUCCESS;
}
