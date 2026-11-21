/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"
#include <endian.h>

static const pci_bus_t default_pci_bus = {
    .Min_Gnt = 0,
    .Max_Lat = 255,
    .devsel = 0,
    .fast_b2b = 1,
    .prefetch = 1,
    .ndev = 1,
    .bandwidth = 132000000,
};

/* Allocate a new pci bus structure every time a new bus is discovered. */

bus_map_t *alloc_bus(uint8_t pribus, uint8_t secbus)
{
    pci_bus_t *bus;
    pci_bridge_t *br = pci_bridges;
    bus_map_t *new_map, *map, *prev_map;
    int idx, found = 0;

    DEBUGFUNC();
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: pri %d - sec %d", __FUNCTION__,
                  pribus, secbus);

    /* This code is to handle the Intel 6XX chipset that has buses at 128 but no
     * associated host bridge
     */
    idx = (((pribus > 127) || (pribus > 63)) && device_without_bridge) ? 1 : 0;
    map = bus_map[idx];
    if (!(bus = calloc(1, sizeof(pci_bus_t)))) {
        return NULL;
    }
    if (!(new_map = calloc(1, sizeof(bus_map_t)))) {
        free(bus);
        return NULL;
    }

    new_map->primary_bus = pribus;
    new_map->secondary_bus = secbus;
    if (secbus > endbus) {
        endbus = secbus;
    }
    if (!map) {
        bus_map[idx] = new_map;
    } else {
        if (!(map = find_bus(pribus))) {
            free(bus);
            free(new_map);
            return NULL;
        }
        new_map->prev = map;
        if (!map->next) {
            map->next = new_map;
            /*          map->subordinate_bus = secbus; */
        } else {
            map = map->next;
            if (map->primary_bus == pribus) {
                if (!map->peer) {
                    map->peer = new_map;
                } else {
                    while (map->peer) {
                        prev_map = map;
                        map = map->peer;
                        if (map->secondary_bus > secbus) {
                            new_map->peer = map;
                            map->prev = new_map;
                            prev_map->peer = new_map;
                            break;
                        } else {
                            if (!map->peer) {
                                map->peer = new_map;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    new_map->pci_bus = bus;
    *new_map->pci_bus = default_pci_bus; /* initialize the structure */
    bus->sub_bus = secbus;
    while (br) {
        if (br->bus == pribus && br->sec_bus == secbus) {
            found = 1;
            break;
        }
        br = br->next;
    }
    if (found) {
        if (br->sec_bus >= lastbus) {
            lastbus = br->sec_bus + 1;
            bus_map[idx]->subordinate_bus = br->sec_bus;
        }
        new_map->subordinate_bus = br->sub_bus;
    } else {
        lastbus++;
        if (secbus > lastbus) {
            lastbus = secbus + 1;
            bus_map[idx]->subordinate_bus = secbus;
        } else {
            bus_map[idx]->subordinate_bus = lastbus;
        }
    }
    sprintf((char *) new_map->name, "bus%02d", secbus);
    return new_map;
}

/* Find the bus structure associated with a particular bus number and return a
 * pointer to this structure.
 */

bus_map_t *find_bus(uint8_t bus)
{
    bus_map_t *map;
    int idx;

    DEBUGFUNC();
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: %d", __FUNCTION__, bus);
    idx = (((bus > 127) || (bus > 63)) && device_without_bridge) ? 1 : 0;
    map = bus_map[idx];
    if (map && verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s - idx %d - sec bus %d",
                  __FUNCTION__, idx, map->secondary_bus);
    while (1) {
        if (!map) {
            break;
        }
        if (map->secondary_bus == bus) {
            return map;
        }
        if (map->secondary_bus < bus) {
            if (map->subordinate_bus < bus) {
                if (map->peer) {
                    map = map->peer;
                    continue;
                } else {
                    if (map->next) {
                        map = map->next;
                        continue;
                    } else {
                        break;
                    }
                }
            }
            if (map->next) {
                map = map->next;
                continue;
            } else {
                if (map->peer) {
                    map = map->peer;
                    continue;
                } else {
                    break;
                }
            }
        } else {
            if (map->peer) {
                map = map->peer;
                continue;
            } else {
                break;
            }
        }
    }
    return NULL;
}

/* This routine creates a name by which memory or I/O resources will be
 * allocated. The names are created from the bus number on which the resource
 * needs to be allocated. eg. pcimemory/Bus0 or io/Bus0/Bus1
 */

static int make_name(uint32_t flags, uint8_t bus, uint8_t pref)
{
    char tmp[10];
    int i, j, cnt = 0;
    uint8_t x[MAX_BUSES];
    bus_map_t *map;

    DEBUGFUNC();
    if (flags & HWRES_PCI_MEMORY)
        strcpy(res_name, "pcimemory");
    else if (flags & HWRES_IO_PORT)
        strcpy(res_name, "io");
    else {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Invalid reserve flag %x",
                  __FUNCTION__, flags);
        return -1;
    }
    if (bus) {
        if (!(map = find_bus(bus))) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Map not found - bus %d",
                      __FUNCTION__, bus);
            return -1;
        }
        x[0] = bus;
        cnt++;
        if (map->primary_bus != 0) {
            map = map->prev;
            while (map) {
                if (map->primary_bus == 0)
                    break;
                x[cnt++] = map->primary_bus;
                map = map->prev;
            }
        }
    }
    x[cnt++] = 0;

    for (i = 0, j = cnt - 1; i < cnt; i++, j--) {
        sprintf(tmp, "/Bus%d", x[j]);
        strcat(res_name, tmp);
    }

    if (pref) {
        strcat(res_name, "-pref");
    }

    return 0;
}

/* Reserve a named resource from PCI memory space or I/O space. The name is
 * created depending on which bus the resource needs to be reserved. See the
 * make_name() function.
 */

int reserve_resource(uint64_t start,
                     uint64_t end,
                     uint32_t flags,
                     uint8_t bus,
                     uint8_t pref)
{
    DEBUGFUNC();
    if (make_name(flags, bus, pref))
        return -1;
    hwres_range_t alloc = {
        .start = start,
        .end = end,
        .flags = HWRES_FLAG_NAME,
        .name = res_name,
    };
    if (hwres_create(&alloc, 1) == -1) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "%s: Unable to seed Mem - Start 0x%llx - End 0x%llx",
                  __FUNCTION__, alloc.start, alloc.end);
        return -1;
    }
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "%s: Start 0x%llx - End 0x%llx - Name %s", __FUNCTION__,
                  alloc.start, alloc.end, alloc.name);
    return 0;
}

/* This routine finds the upper and lower ranges of PCI memory and I/O space. It
 * does this by allocating 1 byte of the resource from the low range and then 1
 * byte from the upper range. A call is then made to the low-level routines to
 * map these addresses to PCI space.
 */

int find_ranges(void)
{
    hwres_claim_t rreq;
    uint64_t map_addr;
    int i, count;
    hwres_range_t *list;

    DEBUGFUNC();
    mem_page_cnt = 0;
    if ((count = hwres_query(NULL, 0, 0, HWRES_PCI_MEMORY)) == -1) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Query failed", __FUNCTION__);
    } else {
        if ((mem_pages = calloc((size_t) count, sizeof(hwres_range_t))) ==
            NULL) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: calloc failed",
                      __FUNCTION__);
        } else {
            if ((count = hwres_query(mem_pages, count, 0, HWRES_PCI_MEMORY)) ==
                -1) {
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Query1 failed",
                          __FUNCTION__);
            } else {
                mem_page_cnt = count;
            }
        }
    }

    /* Sort the PCI memory in ascending size order */
    if (mem_page_cnt > 1) {
        while (1) {
            int swap = 0;
            uint64_t s, e, s1, s2;
            for (i = 0, list = mem_pages; i < mem_page_cnt - 1; i++, list++) {
                s1 = list->end - list->start;
                s2 = list[1].end - list[1].start;
                if (s1 > s2) {
                    s = list->start;
                    e = list->end;
                    list->start = list[1].start;
                    list->end = list[1].end;
                    list[1].start = s;
                    list[1].end = e;
                    swap = 1;
                }
            }
            if (!swap)
                break;
        }
    }

    if (verbose > 2) {
        for (i = 0, list = mem_pages; i < count; i++, list++) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Start 0x%llx - End 0x%llx",
                      list->start, list->end);
        }
    }

    if ((pci_memory = calloc((size_t) mem_page_cnt, sizeof(addr_map_t))) ==
        NULL) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Addr map calloc failed",
                  __FUNCTION__);
        return -1;
    }

    mem_cpu_high = mem_pci_high = 0;
    mem_cpu_low = mem_pci_low = 0xffffffff;
    io_cpu_low = io_cpu_high = io_pci_low = io_pci_high = 0;

    for (i = 0, list = mem_pages; i < mem_page_cnt; i++, list++) {
        pci_memory[i].start_cpu_addr = list->start;
        pci_memory[i].end_cpu_addr = list->end;
        (*pci_drivers->entry->map_addr)(ll_handle, list->start,
                                        &pci_memory[i].start_pci_addr,
                                        PCI_MEM_SPACE | PCI_MAP_ADDR_PCI);
        (*pci_drivers->entry->map_addr)(ll_handle, list->end,
                                        &pci_memory[i].end_pci_addr,
                                        PCI_MEM_SPACE | PCI_MAP_ADDR_PCI);
        if (verbose > 2) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "Low memory CPU 0x%llx - PCI 0x%llx", list->start,
                      pci_memory[i].start_pci_addr);
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "High memory CPU 0x%llx - PCI 0x%llx", list->end,
                      pci_memory[i].end_pci_addr);
        }
        if (list->start < mem_cpu_low) {
            mem_cpu_low = list->start;
            mem_pci_low = pci_memory[i].start_pci_addr;
        }
        if (list->end > mem_cpu_high) {
            mem_cpu_high = list->end;
            mem_pci_high = pci_memory[i].end_pci_addr;
        }
        if (reserve_resource(mem_cpu_low, mem_cpu_high, HWRES_PCI_MEMORY, 0,
                             0)) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                      "%s: Unable to seed Mem - Start 0x%llx - End 0x%llx",
                      __FUNCTION__, mem_cpu_low, mem_cpu_high);
        }
    }

    rreq = (hwres_claim_t) {
        .length = 1,
        .start = 0,
        .end = 0,
        .flags = HWRES_IO_PORT,
    };
    if (hwres_claim(&rreq, 1) != EOK) {
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s I/O attach failed",
                  __FUNCTION__);
        return -1;
    }
    (*pci_drivers->entry->map_addr)(ll_handle, rreq.start, &map_addr,
                                    PCI_IO_SPACE | PCI_MAP_ADDR_PCI);
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Low port CPU 0x%llx - PCI 0x%llx",
                  rreq.start, map_addr);
    io_cpu_low = rreq.start;
    io_pci_low = map_addr;
    hwres_release(&rreq, 1);

    rreq = (hwres_claim_t) {
        .length = 1,
        .start = 0,
        .end = 0,
        .flags = HWRES_IO_PORT | HWRES_FLAG_TOPDOWN,
    };
    if (hwres_claim(&rreq, 1) != EOK) {
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s I/O attach1 failed",
                  __FUNCTION__);
        return -1;
    }
    (*pci_drivers->entry->map_addr)(ll_handle, rreq.start, &map_addr,
                                    PCI_IO_SPACE | PCI_MAP_ADDR_PCI);
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "High port CPU 0x%llx - PCI 0x%llx",
                  rreq.start - 1, map_addr);
    io_cpu_high = rreq.start;
    io_pci_high = map_addr;
    hwres_release(&rreq, 1);
    if (reserve_resource(io_cpu_low, io_cpu_high, HWRES_IO_PORT, 0, 0)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "%s: Unable to seed I/O - Start 0x%llx - End 0x%llx",
                  __FUNCTION__, io_cpu_low, io_cpu_high);
        return -1;
    }
    return 0;
}

/* This routine checks a memory or I/O address to see whether it is within the
 * ranges determined by the find_ranges() routine.
 */

int check_range(uint64_t start, uint64_t length, int type, uint8_t bus)
{
    uint64_t low, high, tmp;
    bus_map_t *map = NULL;

    DEBUGFUNC();
    low = high = 0;
    if (bus) {
        if (!(map = find_bus(bus))) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: bus not found %d",
                      __FUNCTION__, bus);
            return EINVAL;
        }
    }

    if (type & PCI_IO_SPACE) {
        if (bus && (map->io_start || map->io_end)) {
            low = map->io_start;
            high = map->io_end;
        } else {
            low = io_pci_low;
            high = io_pci_high;
        }
        tmp = start + length;
        if ((tmp == 0xffff) && (tmp > io_pci_high))
            length = (io_pci_high & 0xfff);
        if (low == 0xf000 && length == 0xfff) { /* Special case for bridges */
            high = 0xffff;
            if (high != io_pci_high)
                length = (io_pci_high & 0xfff);
        }
    } else {
        if (bus) {
            if (type & PCI_MEM_PREFETCH) {
                low = map->prefetch_mem_start;
                high = map->prefetch_mem_end;
            } else {
                low = map->mem_start;
                high = map->mem_end;
            }
        }
        /*      else { */
        if (!bus || (!low && !high)) {
            low = mem_pci_low;
            high = mem_pci_high;
        }
    }

    if (start >= low && ((start + length - 1) <= high))
        return 0;

    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Range failed 0x%llx - length 0x%llx",
                  start, length);

    return 1;
}

/* Allocate a memory or I/O resource from the I/O service and place the result
 * in the BaseReg structure.
 */

int pci_alloc_resource(struct BaseRegister *BaseReg,
                       uint32_t align,
                       uint8_t bus,
                       pid_t owner_pid)
{
    hwres_claim_t rreq;
    bus_map_t *busmap = NULL;
    int i;

    DEBUGFUNC();
    if (BaseReg->Flags &
        PCI_DEVICE_ENABLED) { /* Don't alloc a resource if device enabled */
        return PCI_SUCCESS;
    }

    if (bus) { /* We only need the bus structure for buses > 0 */
        if (!(busmap = find_bus(bus))) {
            return EINVAL;
        }
    }

    memset((char *) &rreq, 0, sizeof(rreq));
    rreq.length = BaseReg->Size < 256 ? 256 : BaseReg->Size;
    if (BaseReg->Flags & PCI_MEM_SPACE || BaseReg->Flags & PCI_ROM_SPACE ||
        BaseReg->Flags & PCI_MEM_PREFETCH) {
        BaseReg->Address &= 0x0f; /* Keep flags */
        for (i = 0; i < mem_page_cnt; i++) {
            rreq.flags = HWRES_FLAG_NAME | HWRES_FLAG_ALIGN;
            if (make_name(HWRES_PCI_MEMORY, bus,
                          (BaseReg->Flags & PCI_MEM_PREFETCH) ? 1 : 0))
                return EINVAL;
            if (bus) {
                if (BaseReg->Flags & PCI_MEM_PREFETCH) {
                    (*pci_drivers->entry->map_addr)(
                        ll_handle, busmap->prefetch_mem_start, &rreq.start,
                        PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
                    (*pci_drivers->entry->map_addr)(
                        ll_handle, busmap->prefetch_mem_end, &rreq.end,
                        PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
                } else {
                    (*pci_drivers->entry->map_addr)(
                        ll_handle, busmap->mem_start, &rreq.start,
                        PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
                    (*pci_drivers->entry->map_addr)(
                        ll_handle, busmap->mem_end, &rreq.end,
                        PCI_MEM_SPACE | PCI_MAP_ADDR_CPU);
                }
            } else {
                rreq.start = pci_memory[i].start_cpu_addr;
                rreq.end = pci_memory[i].end_cpu_addr;
                rreq.flags |= (HWRES_FLAG_RANGE | HWRES_FLAG_TOPDOWN);
                /*              rreq.flags |= HWRES_FLAG_RANGE; */
            }
            rreq.align = align ? align : rreq.length;
            if (rreq.start == rreq.end) {
                rreq.start = rreq.end =
                    0; /* ppc mapped addresses are non-zero! */
                rreq.flags |= HWRES_FLAG_TOPDOWN;
            } else {
                rreq.flags |= HWRES_FLAG_RANGE;
            }
            rreq.name = res_name;
            if (verbose > 2)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "Memory Request Start 0x%llx - End 0x%llx - Size = "
                          "0x%llx - Name %s",
                          rreq.start, rreq.end, rreq.length, res_name);
            if (hwres_claim_for_pid(&rreq, 1, owner_pid) == EOK) {
                break;
            }
            if (verbose > 2)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Failed %s", strerror(errno));
            if (bus)
                return EINVAL;
        }
    } else {
        if (BaseReg->Flags & PCI_IO_SPACE) {
            BaseReg->Address &= 0x3; /* Keep flags */
            if (bus) {
                (*pci_drivers->entry->map_addr)(
                    ll_handle, busmap->io_start, &rreq.start,
                    PCI_IO_SPACE | PCI_MAP_ADDR_CPU);
                (*pci_drivers->entry->map_addr)(
                    ll_handle, busmap->io_end, &rreq.end,
                    PCI_IO_SPACE | PCI_MAP_ADDR_CPU);
            } else {
                rreq.start = rreq.end = 0;
            }
            if (make_name(HWRES_IO_PORT, bus, 0))
                return EINVAL;
            rreq.name = res_name;
            rreq.align = align ? align : rreq.length;
            if (rreq.start == rreq.end) {
                rreq.start = io_cpu_low;
                rreq.end = io_cpu_high;
                rreq.flags =
                    HWRES_FLAG_ALIGN | HWRES_FLAG_TOPDOWN | HWRES_FLAG_RANGE;
            } else {
                rreq.flags = HWRES_FLAG_ALIGN | HWRES_FLAG_RANGE;
            }
            rreq.flags |= HWRES_FLAG_NAME;
            if (verbose > 2)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "Port Request Start 0x%llx - End 0x%llx - Size = "
                          "0x%llx - Name %s",
                          rreq.start, rreq.end, rreq.length, res_name);
            if (hwres_claim_for_pid(&rreq, 1, owner_pid)) {
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Failed");
                return EINVAL;
            }
        } else {
            return EINVAL;
        }
    }

    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Address = 0x%llx - End 0x%llx",
                  rreq.start, rreq.end);
    BaseReg->Address |= rreq.start; /* Or address with flags */
    return PCI_SUCCESS;
}

/* Free a previously allocated memory or I/O resource CardBus device resources
 * must be returned to their controller's primary bus.
 */

int pci_free_resource(struct BaseRegister *BaseReg,
                      uint8_t bus,
                      pid_t owner_pid)
{
    hwres_claim_t rreq;
    bus_map_t *busmap = NULL;
    pci_device_t *tmp_dev;
    char tmp[20];
    uint8_t f_bus = bus;

    DEBUGFUNC();
    if (bus) { /* We only need the bus name for buses > 0 */
        if (!(busmap = find_bus(bus))) {
            return EINVAL;
        }
        tmp_dev = busmap->bdev;
        f_bus = ((tmp_dev->HeaderType & 2) ? busmap->primary_bus : bus);
        if (verbose)
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s: Header Type 0x%x",
                      __FUNCTION__, tmp_dev->HeaderType);
    }

    memset((char *) &rreq, 0, sizeof(rreq));
    rreq.length = BaseReg->Size < 256 ? 256 : BaseReg->Size;
    if (BaseReg->Flags & PCI_MEM_SPACE || BaseReg->Flags & PCI_ROM_SPACE ||
        BaseReg->Flags & PCI_MEM_PREFETCH) {
        if (make_name(HWRES_PCI_MEMORY, f_bus,
                      (BaseReg->Flags & PCI_MEM_PREFETCH) ? 1 : 0))
            return EINVAL;
        rreq.start = PCI_MEM_ADDR(BaseReg->Address);
        rreq.flags = HWRES_FLAG_NAME | HWRES_FLAG_RANGE;
        BaseReg->Address &= 0x0f; /* Keep flags */
        if (verbose > 2)
            sprintf(tmp, "Free Memory");
    } else {
        if (BaseReg->Flags & PCI_IO_SPACE) {
            if (make_name(HWRES_IO_PORT, f_bus, 0))
                return EINVAL;
            rreq.start = PCI_IO_ADDR(BaseReg->Address);
            rreq.flags = HWRES_FLAG_NAME | HWRES_FLAG_RANGE;
            BaseReg->Address &= 0x3; /* Keep flags */
            if (verbose > 2)
                sprintf(tmp, "Free Port");
        } else {
            return EINVAL;
        }
    }
    rreq.end = rreq.start + rreq.length - 1;
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                  "%s Start 0x%llx - End 0x%llx - Length 0x%llx - %s", tmp,
                  rreq.start, rreq.end, rreq.length, res_name);
    rreq.name = res_name;
    if (hwres_release_for_pid(&rreq, 1, owner_pid)) {
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Failed");
        return EINVAL;
    }

    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Address = 0x%llx", rreq.start);
    return PCI_SUCCESS;
}

/* Request an IRQ from the I/O service. Step through list for cnt items until an
 * IRQ can be allocated.
 */

uint32_t pci_alloc_irq(uint32_t *list, int cnt, uint8_t bus, int msi)
{
    hwres_claim_t *rreq, *req1, *req2;
    int i, j;
    uint32_t irq;

    DEBUGFUNC();
    if (!cnt)
        return 0;

    if (!(rreq = calloc((size_t) cnt, sizeof(hwres_claim_t)))) {
        return 0;
    }
    j = (cnt > 16) ? 16 : cnt;
    req1 = req2 = rreq;
    while (1) {
        for (i = 0; i < j; i++, req1++) {
            req1->length = 1;
            req1->start = *list;
            req1->end = *list;
            req1->flags = HWRES_IRQ | HWRES_FLAG_RANGE |
                          (cnt == 1 ? HWRES_FLAG_SHARE : 0);
            if (j && i < j - 1)
                req1->flags |= HWRES_FLAG_LIST;
            list += (msi > 0) ? 2 : 1;
        }

        if (hwres_claim(req2, (unsigned) j) != -1) {
            if (verbose > 2)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "Alloc IRQ 0x%llx - Flags 0x%x", req2->start,
                          req2->flags);
            if (cnt > 16) {
                cnt -= 16;
                req2 += j;
                j = cnt;
                continue;
            }
            irq = (uint32_t) rreq->start;
            free(rreq);
            return irq;
        } else
            break;
    }

    if (verbose > 2) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "%s: IRQ Failed Start 0x%llx - End 0x%llx - Flags 0x%x",
                  __FUNCTION__, req2->start, req2->end, req2->flags);
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: %s", __FUNCTION__,
                  strerror(errno));
    }
    free(rreq);
    return 0;
}

/* Return an IRQ to the I/O service. */

uint32_t pci_free_irq(pci_device_t *device)
{
    hwres_claim_t rreq;
    uint32_t dword, irq, msi_off;
    uint8_t bus, devfn;
    uint16_t word, mess_ctrl;
    uint8_t save_irq;
    int cnt = 0;

    DEBUGFUNC();
    memset((char *) &rreq, 0, sizeof(rreq));
    irq = device->IrqNumber;
    if (((device->PciType & PCI_TYPE_MSI) ||
         (device->PciType & PCI_TYPE_MSIX)) &&
        device->msi_irq) {
        bus = device->BusNumber;
        devfn = device->DevFuncNumber;
        if (device->AttachFlags & PCI_USE_MSIX) {
            msi_off = device->msix_off;
            PCI_read_config16(bus, devfn, msi_off + 2, 1, &word);
            word &= (uint16_t) ~(uint16_t) PCI_MSIX_ENABLE;
            PCI_write_config16(bus, devfn, msi_off + 2, 1, &word);
            cnt = device->msix_req_cnt;
        } else {
            msi_off = (device->PciType & PCI_TYPE_MSI) ? device->msi_off
                                                       : device->msix_off;
            PCI_read_config16(bus, devfn, msi_off + 2, 1, &mess_ctrl);
            if (device->PciType & PCI_TYPE_MSI) {
                mess_ctrl &= (uint16_t) ~(uint16_t) PCI_MSI_ENABLE;
            } else {
                mess_ctrl &= (uint16_t) ~(uint16_t) PCI_MSIX_ENABLE;
            }
            PCI_write_config16(bus, devfn, msi_off + 2, 1, &mess_ctrl);
            dword = 0;
            PCI_write_config32(bus, devfn, msi_off + 4, 1, &dword);
            if (mess_ctrl & PCI_MSI_64BIT) {
                PCI_write_config32(bus, devfn, msi_off + 8, 1, &dword);
            }
            word = 0;
            PCI_write_config16(
                bus, devfn,
                (mess_ctrl & PCI_MSI_64BIT) ? msi_off + 12 : msi_off + 8, 1,
                &word);
            cnt = device->msi_req_cnt;
        }
        device->IrqNumber = device->SaveIrq;
        save_irq = (uint8_t) device->IrqNumber;
        PCI_write_config8(
            bus, devfn,
            (uint32_t) offsetof(struct _pci_config_regs, Interrupt_Line), 1,
            &save_irq);
        device->msi_irq = 0;
    } else {
        cnt = 1;
    }

    rreq.length = 1;
    rreq.start = irq;
    rreq.end = irq + (uint32_t) cnt - 1;
    rreq.flags = HWRES_IRQ | HWRES_FLAG_RANGE;
    if (!hwres_release(&rreq, 1)) {
        if (verbose > 2)
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Free IRQ 0x%x - end 0x%llx", irq,
                      rreq.end);
        return (uint32_t) rreq.start;
    }
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "%s: Free IRQ failed - start 0x%llx - end 0x%llx",
                  __FUNCTION__, rreq.start, rreq.end);
    return 0;
}

/* Reserve an IRQ from the hardware resource database. */
int pci_reserve_irq(uint32_t irq)
{
    DEBUGFUNC();
    hwres_range_t ralloc = {
        .start = irq,
        .end = irq,
        .flags = HWRES_IRQ | HWRES_FLAG_RSVP,
    };
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "reserve IRQ %lld", ralloc.start);
    return hwres_create(&ralloc, 1);
}

/* Configure a PCI-PCI bridge's I/O aperture with a start and end address */

int configure_bridge_io(uint8_t bus,
                        uint8_t devfn,
                        uint64_t start_addr,
                        uint64_t end_addr)
{
    int bit_32 = 0;
    union {
        uint8_t io_base[2];
        uint16_t word;
    } un;
    uint16_t base, limit;

    DEBUGFUNC();
    if (PCI_read_config16(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs, IO_Base), 1,
                          &un.word)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Read config error",
                  __FUNCTION__);
        return -1;
    }
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "io_base0 0x%x - io_base1 0x%x",
                  un.io_base[0], un.io_base[1]);
    if (un.io_base[0] & 1) /* 32-bit addressing */
        bit_32 = 1;
    un.io_base[0] = (uint8_t) ((start_addr & 0xf000) >> 8);
    un.io_base[1] = (uint8_t) ((end_addr & 0xf000) >> 8);
    un.word = ENDIAN_LE16(un.word);
    PCI_write_config16(bus, devfn,
                       offsetof(struct _pci_bridge_config_regs, IO_Base), 1,
                       &un.word);
    if (bit_32) {
        PCI_read_config16(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, IO_Base_Upper16), 1,
            &base);
        PCI_read_config16(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, IO_Limit_Upper16), 1,
            &limit);
        if (verbose)
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "base 0x%x - limit 0x%x", base,
                      limit);
        base = (uint16_t) (start_addr >> 16);
        limit = (uint16_t) (end_addr >> 16);
        PCI_write_config16(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, IO_Base_Upper16), 1,
            &base);
        PCI_write_config16(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, IO_Limit_Upper16), 1,
            &limit);
    }
    if (PCI_read_config16(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs, Command), 1,
                          &base)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Read config error",
                  __FUNCTION__);
        return -1;
    }
    base |= (PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MASTER_ENABLE);
    PCI_write_config16(bus, devfn,
                       offsetof(struct _pci_bridge_config_regs, Command), 1,
                       &base);
    return 0;
}

/* Configure a PCI-PCI bridge's memory aperture with a start and end address. */

int configure_bridge_mem(uint8_t bus,
                         uint8_t devfn,
                         uint64_t start_addr,
                         uint64_t end_addr)
{
    uint16_t base;
    union {
        uint16_t word[2];
        uint32_t dword;
    } un;

    DEBUGFUNC();
    if (PCI_read_config32(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs, Memory_Base),
                          1, &un.dword)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Read config error",
                  __FUNCTION__);
        return -1;
    }
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "word0 0x%x - word1 0x%x",
                  un.word[0], un.word[1]);
    un.word[0] = (uint16_t) ((start_addr & 0xfff00000) >> 16);
    un.word[1] = (uint16_t) ((end_addr & 0xfff00000) >> 16);
    PCI_write_config32(bus, devfn,
                       offsetof(struct _pci_bridge_config_regs, Memory_Base), 1,
                       &un.dword);
    if (PCI_read_config16(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs, Command), 1,
                          &base)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Read config error",
                  __FUNCTION__);
        return -1;
    }
    base |= (PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_MASTER_ENABLE);
    PCI_write_config16(bus, devfn,
                       offsetof(struct _pci_bridge_config_regs, Command), 1,
                       &base);
    return 0;
}

/* Configure a PCI-PCI bridge's prefetchable memory aperture with a start and
 * end address.
 */

int configure_bridge_pref(uint8_t bus,
                          uint8_t devfn,
                          uint64_t start_addr,
                          uint64_t end_addr)
{
    int bit_64 = 0;
    uint32_t base, limit;
    union {
        uint16_t word[2];
        uint32_t dword;
    } un;

    DEBUGFUNC();
    if (PCI_read_config32(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, Prefetchable_Memory_Base),
            1, &un.dword)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Read config error",
                  __FUNCTION__);
        return -1;
    }
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "word0 0x%x - word1 0x%x",
                  un.word[0], un.word[1]);
    if (un.word[0] & 1) /* 64-bit addressing */
        bit_64 = 1;
    un.word[0] = (uint16_t) ((start_addr & 0xfff00000) >> 16);
    un.word[1] = (uint16_t) ((end_addr & 0xfff00000) >> 16);
    PCI_write_config32(
        bus, devfn,
        offsetof(struct _pci_bridge_config_regs, Prefetchable_Memory_Base), 1,
        &un.dword);
    if (bit_64) {
        PCI_read_config32(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, Prefetchable_Base_Upper32),
            1, &base);
        PCI_read_config32(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs,
                                   Prefetchable_Limit_Upper32),
                          1, &limit);
        if (verbose)
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "base 0x%x - limit 0x%x", base,
                      limit);
        base = (uint32_t) (start_addr >> 32);
        limit = (uint32_t) (end_addr >> 32);
        PCI_write_config32(
            bus, devfn,
            offsetof(struct _pci_bridge_config_regs, Prefetchable_Base_Upper32),
            1, &base);
        PCI_write_config32(bus, devfn,
                           offsetof(struct _pci_bridge_config_regs,
                                    Prefetchable_Limit_Upper32),
                           1, &limit);
    }
    if (PCI_read_config16(bus, devfn,
                          offsetof(struct _pci_bridge_config_regs, Command), 1,
                          &base)) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Read config error",
                  __FUNCTION__);
        return -1;
    }
    base |= (PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_MASTER_ENABLE);
    PCI_write_config16(bus, devfn,
                       offsetof(struct _pci_bridge_config_regs, Command), 1,
                       &base);
    return 0;
}

/* Allocate a memory or I/O resource, depending on the size and flags passed. */

int alloc_resource(uint64_t address,
                   uint32_t size,
                   uint32_t flags,
                   uint8_t bus,
                   uint8_t pref)
{
    DEBUGFUNC();
    if (!address || !size)
        return EOK;
    if (make_name(flags, bus, pref))
        return EINVAL;
    hwres_claim_t rreq = {
        .length = size,
        .start = address,
        .end = address + size,
        .flags = flags | HWRES_FLAG_RANGE | HWRES_FLAG_NAME,
        .name = res_name,
    };

    if (hwres_claim(&rreq, 1) == -1) {
        if (verbose > 2) {
            pci_slogf(
                _SLOGC_PCI, _SLOG_ERROR,
                "%s: Unable to seed resource Addr 0x%llx - Size 0x%x - Name %s",
                __FUNCTION__, address, size, res_name);
        }
        return ENOMEM;
    }

    /* pci_slogf (_SLOGC_PCI, _SLOG_INFO, "Alloc resource 0x%llx - Size 0x%x -
     * Flags 0x%x", address, size, flags);
     */
    return EOK;
}
