/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PCI_GLOBALS_H
#define _PCI_GLOBALS_H

struct _dev_list;
#define IOSERV_OCB_T struct _dev_list

#include <server.h>

#define MAX_BUSES 10

union _pci_data {
    struct _pci_class class;
    struct _pci_attach lock;
    struct _pci_detach unlock;
};

/* Shared globals */

extern pci_driver_t *pci_drivers;
extern void *ll_handle;
extern int verbose;

extern uint8_t lastbus, endbus, cardbus;
extern uint8_t clast_bus, clast_devfn;
extern uint8_t *pciex_space;
extern int scan_buses;
extern int map_irqs;
extern int cur_scan_bus;
extern int dont_touch;
extern int program_display_device;
extern int mem_page_cnt;
extern int num_pci_bridges;
extern int bridge_count;
extern int class_override;
extern int use_msi;
extern int enable_crt_msi;
extern int device_without_bridge;
extern int max_msi;
extern uint32_t host_bridge_class;
extern uint64_t mem_cpu_low, mem_cpu_high, mem_pci_low, mem_pci_high;
extern uint64_t io_cpu_low, io_cpu_high, io_pci_low, io_pci_high;
extern uint64_t pci_ex_bar;
extern uint32_t pci_ex_size;
extern pci_device_t *DeviceHead, *DeviceTail;
extern pci_device_t *last_device;
extern bus_map_t *bus_map[2];
extern addr_map_t *pci_memory;
extern hwres_range_t *mem_pages;
extern pci_bridge_t *pci_bridges;
extern char res_name[100];

/* config.c */

ssize_t pci_slogf(int opcode, int severity, const char *fmt, ...);
int PCI_read_config8(uint8_t bus,
                     uint8_t devfn,
                     uint32_t off,
                     uint32_t cnt,
                     void *buf);
int PCI_read_config16(uint8_t bus,
                      uint8_t devfn,
                      uint32_t off,
                      uint32_t cnt,
                      void *buf);
int PCI_read_config32(uint8_t bus,
                      uint8_t devfn,
                      uint32_t off,
                      uint32_t cnt,
                      void *buf);
int PCI_write_config8(uint8_t bus,
                      uint8_t devfn,
                      uint32_t off,
                      uint32_t cnt,
                      void *buf);
int PCI_write_config16(uint8_t bus,
                       uint8_t devfn,
                       uint32_t off,
                       uint32_t cnt,
                       void *buf);
int PCI_write_config32(uint8_t bus,
                       uint8_t devfn,
                       uint32_t off,
                       uint32_t cnt,
                       void *buf);

/* resource.c */

bus_map_t *alloc_bus(uint8_t pribus, uint8_t secbus);
bus_map_t *find_bus(uint8_t bus);
int reserve_resource(uint64_t start,
                     uint64_t end,
                     uint32_t flags,
                     uint8_t bus,
                     uint8_t pref);
int find_ranges(void);
int check_range(uint64_t start, uint64_t length, int type, uint8_t bus);
int pci_alloc_resource(struct BaseRegister *BaseReg,
                       uint32_t align,
                       uint8_t bus,
                       pid_t owner_pid);
int pci_free_resource(struct BaseRegister *BaseReg,
                      uint8_t bus,
                      pid_t owner_pid);
uint32_t pci_alloc_irq(uint32_t *list, int cnt, uint8_t bus, int msi);
uint32_t pci_free_irq(pci_device_t *device);
int pci_reserve_irq(uint32_t irq);
int configure_bridge_io(uint8_t bus,
                        uint8_t devfn,
                        uint64_t start,
                        uint64_t end);
int configure_bridge_mem(uint8_t bus,
                         uint8_t devfn,
                         uint64_t start,
                         uint64_t end);
int configure_bridge_pref(uint8_t bus,
                          uint8_t devfn,
                          uint64_t start,
                          uint64_t end);
int alloc_resource(uint64_t address,
                   uint32_t size,
                   uint32_t flags,
                   uint8_t bus,
                   uint8_t pref);

/* bridge.c */

int configure_bridge(uint8_t bus, uint8_t devfunc, uint8_t sec_bus);
int setup_cardbus_bridge(uint8_t Bus,
                         uint8_t DevFn,
                         pci_device_t *Dev,
                         uint8_t secbus);
int traverse_back(uint8_t sub_bus);
int power_down_device(uint8_t bus, uint8_t devfunc, pci_device_t *dev);
int power_up_device(uint8_t bus, uint8_t devfunc, pci_device_t *dev);

/* scan.c */

void on_pci(uint8_t bus, uint8_t devfn, uint16_t flags);
uint16_t off_pci(uint8_t bus, uint8_t devfn, uint32_t class);
int check_capabilities(uint8_t bus, uint8_t devfn, pci_device_t *device);
int scan_windows(uint8_t bus, uint8_t devfn, pci_device_t *device);
int scan_all(void);

/* client.c */

pci_device_t *search_busdev(uint8_t bus, uint8_t devfunc);
int PCI_find_class(uint32_t ClassCode,
                   uint32_t Index,
                   uint8_t *bus,
                   uint8_t *devfn);
int PCI_translation(void *ll_handle, struct pci_dev_info *ConfigBuffer);
pci_device_t *validate_handle(pci_device_t *handle);
void fill_base(struct pci_dev_info *ConfigBuffer, pci_device_t *dev_ptr);
void fill_config(struct pci_dev_info *ConfigBuffer, pci_device_t *dev_ptr);
int addup_resources(pci_device_t *dev_ptr,
                    uint32_t flags,
                    int show_pci,
                    int cardbus_device,
                    int next_attach,
                    pid_t resource_owner_pid);
int get_an_irq(pci_device_t *dev_ptr,
               int cardbus_device,
               uint32_t flags,
               int next_attach,
               int *local_errno);

/* dwmsi.c */

int pci_dwmsi_init(paddr_t dbi_base, size_t dbi_size, uint32_t plic_irq);
uint32_t pci_dwmsi_target_addr(void);

#define MSI_REG_ADDR pci_dwmsi_target_addr()

/* dw-atu.c */

/* DesignWare iATU CFG TLP types -- pass to pci_dw_atu_aim_cfg(). */
#define DW_ATU_CFG0 0x4 /* immediate child bus (e.g. bus 1)   */
#define DW_ATU_CFG1 0x5 /* downstream of any bridge (bus >=2) */

int pci_dw_atu_init(uint64_t dbi_pa, size_t dbi_size);
int pci_dw_atu_aim_cfg(uint8_t bus,
                       uint8_t slot,
                       uint8_t func,
                       int type,
                       uint64_t cfg_pa,
                       size_t cfg_size);
uint32_t pci_dw_dbi_read32(uint32_t off);
void pci_dw_dbi_write32(uint32_t off, uint32_t val);
int pci_dw_dbi_ready(void);

/* attach.c */

pci_device_t *PCI_attach_device(void *handle,
                                uint32_t flags,
                                uint16_t idx,
                                struct pci_dev_info *ConfigBuffer,
                                pci_dev_list_t *list,
                                pid_t resource_owner_pid,
                                int *local_errno);
int PCI_detach_device(void *handle, pci_dev_list_t *list, int close_flag);
int setup_pci_io(uint64_t io_size);
int setup_pci_pref(uint64_t pref_size);
int setup_pci_mem(uint64_t mem_size);

/* ECAM driver entry (from ecam.c) */
extern pdrvr_entry_t ecam_entry;

#endif
