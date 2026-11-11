/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"

pci_driver_t *pci_drivers;

uint8_t lastbus, endbus, cardbus;
uint8_t clast_bus, clast_devfn;
uint8_t *pciex_space = NULL;
int scan_buses;
int map_irqs = 1;
int cur_scan_bus;
int dont_touch = 0;
int program_display_device = 0;
int mem_page_cnt;
int num_pci_bridges = 0;
int bridge_count = 0;
int class_override = 0;
int use_msi = 1;        /* default is to use MSI interrupts */
int enable_crt_msi = 0; /* Some video chips only use MSI for PME */
int device_without_bridge = 0;
int max_msi = 1;
uint32_t host_bridge_class = 0xffffffff;
uint64_t mem_cpu_low, mem_cpu_high, mem_pci_low, mem_pci_high;
uint64_t io_cpu_low, io_cpu_high, io_pci_low, io_pci_high;
uint64_t pci_ex_bar = 0;
uint32_t pci_ex_size = 0;
pci_device_t *DeviceHead, *DeviceTail;
pci_device_t *last_device = NULL;
bus_map_t *bus_map[2];

addr_map_t *pci_memory;
hwres_range_t *mem_pages;
pci_bridge_t *pci_bridges = NULL;
void *ll_handle;
char res_name[100];
int verbose = 0;
