/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PCIDRVR_H_INCLUDED
#define __PCIDRVR_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _INTTYPES_H_INCLUDED
#include <inttypes.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#include <pci.h>

#define PCI_IO_SPACE 0x01
#define PCI_MEM_SPACE 0x02
#define PCI_ROM_SPACE 0x04
#define PCI_ISA_SPACE 0x08
#define PCI_BMSTR_SPACE 0x10
#define PCI_MEM_PREFETCH 0x20
#define PCI_MAP_ADDR_CPU 0x40000000
#define PCI_MAP_ADDR_PCI 0x80000000

#define PCI_UNMAP_IRQ 0x00
#define PCI_MAP_IRQ 0x01

#define PCI_SHARE_IRQ 0x8000

typedef struct {
    int bus;
    void *handle;
} map_handle_t;

typedef struct _pci_bus {
    uint8_t Min_Gnt;    /* largest min grant */
    uint8_t Max_Lat;    /* smallest max latency */
    uint8_t devsel;     /* slowest devsel */
    uint8_t fast_b2b;   /* support fast b2b */
    uint8_t prefetch;   /* support prefetch */
    uint8_t freq66;     /* support 66Mhz */
    uint8_t width64;    /* 64-bit bus */
    uint8_t ndev;       /* # devices on bus */
    uint32_t bandwidth; /* # of .25us ticks/sec @ 33Mhz */
    uint8_t sub_bus;    /* subordinate bus number of PCI/PCI bridge */
    uint8_t filler[3];
    void *pciex_addr;    /* PCI Express address space */
    uint32_t pciex_size; /* Size of PCIEX space */
} pci_bus_t;

typedef struct _pdrvr_entry {
    int (*attach)(char *options, void **hdl);
    int (*detach)(void *hdl);
    int (*cnfg_bridge)(void *hdl,
                       uint32_t bus,
                       uint32_t devfunc,
                       pci_bus_t *pbus);
    int (*read_cnfg)(void *hdl,
                     uint32_t bus,
                     uint32_t devfunc,
                     uint32_t reg,
                     uint32_t width,
                     void *data);
    int (*write_cnfg)(void *hdl,
                      uint32_t bus,
                      uint32_t devfunc,
                      uint32_t reg,
                      uint32_t width,
                      const uint32_t data);
    int (*special_cycle)(void *hdl, uint32_t bus, uint32_t data);
    int (*map_irq)(void *hdl,
                   uint32_t bus,
                   uint32_t devfunc,
                   uint32_t line,
                   uint32_t pin,
                   uint32_t flags);
    int (*avail_irq)(void *hdl,
                     uint32_t bus,
                     uint32_t devfunc,
                     uint32_t *list,
                     uint32_t *nelm,
                     int *);
    int (*map_addr)(void *hdl,
                    uint64_t iaddr,
                    uint64_t *oaddr,
                    uint32_t type); /* address translation */
} pdrvr_entry_t;

#endif
