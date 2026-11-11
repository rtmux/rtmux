/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PCI_SERVER_H
#define _PCI_SERVER_H

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include "hwres-client.h"
#include <pcidrvr.h>
#include <inttypes.h>
#include <pci.h>
#include <pci-serv.h>
#include "slogcodes.h"
#include <sys/mman.h>
#include <sys/syspage.h>

#define DEBUGFUNC()                                                  \
    do {                                                             \
        if (verbose > 3)                                             \
            pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s", __FUNCTION__); \
    } while (0);

#define NUM_DEVICES 20
#define SERV_MAX_IRQS 32
#define DEFAULT_SIZE 256
#define MEG1 (1024 * 1024)
#define KB4 (4 * 1024)

#define MSIX_TAB_SIZE 16384

#define CARDBUS_PRI_BUS offsetof(struct _pci_cardbus_config_regs, Pci_Bus_Num)
#define CARDBUS_BUS offsetof(struct _pci_cardbus_config_regs, Cardbus_Bus_Num)
#define CARDBUS_SUB_BUS offsetof(struct _pci_cardbus_config_regs, Sub_Bus_Num)

#define READ 0
#define WRITE 1

#define PCI_IO_SPACE 0x01 /* Note: These must tie up with pcidrvr.h */
#define PCI_MEM_SPACE 0x02
#define PCI_ROM_SPACE 0x04
#define PCI_ISA_SPACE 0x08
#define PCI_BMSTR_SPACE 0x10
#define PCI_MEM_PREFETCH 0x20
#define PCI_DEVICE_ENABLED 0x80

#define PCI_MEM_PER_BUS 0x4000000 /* Try 64Mb per bus */

#define PCI_SUBORDINATE_BUS_MASK 0x00ff0000u
#define PCI_SECONDARY_BUS_MASK 0x0000ff00u
#define PCI_PRIMARY_BUS_MASK 0x000000ffu

#define PCI_PRIMARY_BUS_INSERT(x, y) \
    (((x) & ~PCI_PRIMARY_BUS_MASK) | ((y) << 0))
#define PCI_SECONDARY_BUS_INSERT(x, y) \
    (((x) & ~PCI_SECONDARY_BUS_MASK) | ((y) << 8))
#define PCI_SUBORDINATE_BUS_INSERT(x, y) \
    (((x) & ~PCI_SUBORDINATE_BUS_MASK) | ((y) << 16))

#define PCI_TYPE_PCIX 1
#define PCI_TYPE_PCIEXPRESS 2
#define PCI_TYPE_MSI 4
#define PCI_TYPE_MSIX 8

#define PCI_EX_SIZE_DEFAULT 256

struct BaseRegister {
    uint64_t Address;
    uint64_t Size;
    uint32_t Flags;
    uint32_t Filler;
};

typedef struct _msix_table {
    uint32_t msg_addr_low;
    uint32_t msg_addr_hi;
    uint32_t msg_data;
    uint32_t vector_control;
} msix_table_t;

typedef struct _bus_map bus_map_t;
typedef struct _server_device pci_device_t;
typedef struct _pci_bridge pci_bridge_t;

typedef struct _bus_map {
    bus_map_t *next;
    bus_map_t *prev;
    bus_map_t *peer;
    pci_device_t *bdev;
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    uint8_t filler[5];
    uint64_t io_reqd;
    uint64_t mem_reqd;
    uint64_t pref_reqd;
    uint64_t mem_start;
    uint64_t mem_end;
    uint64_t io_start;
    uint64_t io_end;
    uint64_t prefetch_mem_start;
    uint64_t prefetch_mem_end;
    int8_t name[8];
    pci_bus_t *pci_bus;
    uint32_t filler2;
} bus_map_t;

struct _server_device {
    pci_device_t *next;
    bus_map_t *bmap;
    uint8_t BusNumber;
    uint8_t DevFuncNumber;
    uint8_t HeaderType;
    uint8_t msi_irq;
    uint32_t IrqNumber;
    uint32_t SaveIrq;
    uint16_t VendorId;
    uint16_t DeviceId;
    uint16_t SubVendorId;
    uint16_t SubDeviceId;
    uint32_t ClassCode;
    struct BaseRegister BaseReg[6];
    struct BaseRegister ROMBase;
    struct BaseRegister mem_alloc;
    struct BaseRegister pref_alloc;
    struct BaseRegister io_alloc;
    uint32_t AttachFlags; /* The flags that were used to attach this device */
    pid_t ResourceOwner;
    uint8_t LockFlg; /* Number of time this device has been attached */
    uint8_t Index;
    uint8_t Min_Gnt;
    uint8_t Max_Lat;
    uint8_t RevisionId;
    uint8_t Persistent;
    uint8_t IntPin;
    uint8_t ServiceAttach;
    uint8_t PciType;
    uint8_t PmCap;
    uint8_t msi_off;
    uint8_t msi_req_cnt;
    uint8_t msix_req_cnt;
    uint8_t msix_off;
    uint8_t fill[2];
    uint16_t PmState;
    uint16_t mess_ctrl;
    uint32_t msix_tab_off;
    uint32_t msix_pba_off;
    msix_table_t *msix_tab;
    uint32_t *pba_tab;
};

typedef struct _dev_list {
    unsigned ioflag;
    int num_devices, lockflg;
    pci_device_t *dev_list[NUM_DEVICES];
} pci_dev_list_t;

typedef struct _pdrvr {
    pdrvr_entry_t *entry;
    void *hdl;
    void *next;
} pci_driver_t;

typedef struct {
    uint64_t start_cpu_addr;
    uint64_t end_cpu_addr;
    uint64_t start_pci_addr;
    uint64_t end_pci_addr;
} addr_map_t;

typedef struct _pci_bridge {
    pci_bridge_t *next;
    uint8_t bus;
    uint8_t devfn;
    uint8_t sec_bus;
    uint8_t sub_bus;
    uint32_t class;
} pci_bridge_t;

#endif
