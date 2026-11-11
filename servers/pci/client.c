/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * functions
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"

/* Search through the pci_device_t list looking for a particular class of device
 * at a specific index.
 */

/* Match a caller-supplied 24-bit class code (base | subclass | reg_if)
 * against a device's ClassCode, treating 0xFF in each byte as a per-level
 * wildcard. The whole-ClassCode sentinel 0x00FFFFFF thus matches every device
 * (used by callers that want to enumerate all devices without caring about
 * class), while narrower queries like PCI_CCODE(0x01, 0x08, 0xFF) match NVM
 * controllers of any programming interface.
 */
static int ccode_matches(uint32_t want, uint32_t got)
{
    uint8_t w_base = (uint8_t) ((want >> 16) & 0xFFu);
    uint8_t w_sub = (uint8_t) ((want >> 8) & 0xFFu);
    uint8_t w_regif = (uint8_t) ((want >> 0) & 0xFFu);
    uint8_t g_base = (uint8_t) ((got >> 16) & 0xFFu);
    uint8_t g_sub = (uint8_t) ((got >> 8) & 0xFFu);
    uint8_t g_regif = (uint8_t) ((got >> 0) & 0xFFu);
    if (w_base != 0xFFu && w_base != g_base)
        return 0;
    if (w_sub != 0xFFu && w_sub != g_sub)
        return 0;
    if (w_regif != 0xFFu && w_regif != g_regif)
        return 0;
    return 1;
}

static pci_device_t *search_class(uint32_t class, uint32_t idx)
{
    pci_device_t *device = DeviceHead;
    int oidx = 0;

    DEBUGFUNC();
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s - class %x", __FUNCTION__,
                  class);
    while (device) {
        if (ccode_matches(class, device->ClassCode)) {
            if (oidx == idx) {
                return device;
            } else {
                oidx++;
            }
        }
        device = device->next;
    }
    return NULL;
}

/* Search through the pci_device_t list looking for a particular bus number and
 * devfuncnumber.
 */

pci_device_t *search_busdev(uint8_t bus, uint8_t devfunc)
{
    pci_device_t *device = DeviceHead;

    DEBUGFUNC();
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s - bus %d - devfunc 0x%x",
                  __FUNCTION__, bus, devfunc);
    while (device) {
        if (device->BusNumber == bus && device->DevFuncNumber == devfunc) {
            return device;
        }
        device = device->next;
    }
    return NULL;
}

/* Search the pci_device_t list looking for a class of device (ClassCode) at a
 * specific Index. If found, return the busnumber and devfuncnumber.
 */

int PCI_find_class(uint32_t ClassCode,
                   uint32_t Index,
                   uint8_t *bus,
                   uint8_t *devfn)
{
    pci_device_t *device;

    DEBUGFUNC();
    if (!bus || !devfn) {
        return PCI_UNSUPPORTED_FUNCT;
    }
    if (!(device = search_class(ClassCode, Index))) {
        return PCI_DEVICE_NOT_FOUND;
    }
    *bus = device->BusNumber;
    *devfn = device->DevFuncNumber;
    return PCI_SUCCESS;
}

/* This routine checks to see whether an IRQ can be programmed. The code first
 * saves the old irq and then writes another irq and reads it back. If the
 * source and destination irqs agree, then the irq is programmable.
 */

static int irq_ok(uint8_t bus, uint8_t devfn)
{
    uint8_t old_irq, tmp[2];

    DEBUGFUNC();
    if (PCI_read_config8(bus, devfn,
                         offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                         &old_irq) != PCI_SUCCESS) {
        return 0;
    }
    if (old_irq != 0xff && old_irq < 16 && old_irq != 0) {
        return 1; /* IRQ is OK */
    }
    tmp[0] = 0x09;
    if (PCI_write_config8(bus, devfn,
                          offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                          &tmp[0]) != PCI_SUCCESS) {
        return 0;
    }
    if (PCI_read_config8(bus, devfn,
                         offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                         &tmp[1]) != PCI_SUCCESS) {
        return 0;
    }
    if (PCI_write_config8(bus, devfn,
                          offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                          &old_irq) != PCI_SUCCESS) {
        return 0;
    }
    return (tmp[0] == tmp[1]) ? 1 : 0; /* IRQ is OK if tmp [0] == tmp [1] */
}

/* This routine checks to see whether the interrupt pin of a non-zero function
 * is the same as function zero.
 */

static int same_int(uint8_t bus, uint8_t devfn)
{
    uint8_t int_pin, int_pin1;

    DEBUGFUNC();
    if (PCI_read_config8(bus, devfn,
                         offsetof(struct _pci_config_regs, Interrupt_Pin), 1,
                         &int_pin) != PCI_SUCCESS) {
        return 0;
    }
    if (PCI_read_config8(bus, (devfn & 0xf8),
                         offsetof(struct _pci_config_regs, Interrupt_Pin), 1,
                         &int_pin1) != PCI_SUCCESS) {
        return 0;
    }
    return (int_pin == int_pin1) ? 1 : 0;
}

/* Return true is this device is not a bridge, else false. */

static int is_not_bridge(uint8_t bus, uint8_t devfn)
{
    uint32_t dword;

    DEBUGFUNC();
    if ((PCI_read_config32(
             bus, (devfn & 0xf8),
             offsetof(struct _pci_bridge_config_regs, Revision_ID), 1,
             (char *) &dword) != PCI_SUCCESS))
        return -1;
    dword >>= 8;
    if ((dword & 0xff0000) == (PCI_CLASS_BRIDGE)) {
        return 0;
    }
    return 1;
}

/* Translate addresses from CPU space to PCI space. */

int PCI_translation(void *ll_handle, struct pci_dev_info *ConfigBuffer)
{
    uint64_t iaddr, oaddr;

    DEBUGFUNC();
    iaddr = 0;
    if ((*pci_drivers->entry->map_addr)(ll_handle, iaddr, &oaddr,
                                        PCI_IO_SPACE | PCI_MAP_ADDR_PCI) != EOK)
        pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "Translate I/O failed");
    else
        ConfigBuffer->CpuIoTranslation = -oaddr;
    if ((*pci_drivers->entry->map_addr)(
            ll_handle, iaddr, &oaddr, PCI_MEM_SPACE | PCI_MAP_ADDR_PCI) != EOK)
        pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "Translate Mem failed");
    else
        ConfigBuffer->CpuMemTranslation = -oaddr;
    if ((*pci_drivers->entry->map_addr)(
            ll_handle, iaddr, &oaddr, PCI_ISA_SPACE | PCI_MAP_ADDR_PCI) != EOK)
        pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "Translate ISA failed");
    else
        ConfigBuffer->CpuIsaTranslation = -oaddr;
    if ((*pci_drivers->entry->map_addr)(ll_handle, iaddr, &oaddr,
                                        PCI_BMSTR_SPACE | PCI_MAP_ADDR_PCI) !=
        EOK)
        pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "Translate Bmstr failed");
    else
        ConfigBuffer->CpuBmstrTranslation = oaddr;

    return 0;
}

/* Validate the handle */

pci_device_t *validate_handle(pci_device_t *handle)
{
    pci_device_t *dev_ptr = DeviceHead;

    DEBUGFUNC();
    while (dev_ptr) {
        if (dev_ptr == handle)
            return handle;
        dev_ptr = dev_ptr->next;
    }
    return NULL;
}

/* Fill base register config information */

void fill_base(struct pci_dev_info *ConfigBuffer, pci_device_t *dev_ptr)
{
    struct BaseRegister *BaseReg;
    int i;
    uint64_t Address;

    DEBUGFUNC();
    for (i = 0; i < 6; i++) {
        BaseReg = &dev_ptr->BaseReg[i];
        if (!BaseReg->Size)
            continue;
        (*pci_drivers->entry->map_addr)(
            ll_handle, BaseReg->Address, &Address,
            (uint32_t) (BaseReg->Flags | PCI_MAP_ADDR_PCI));
        ConfigBuffer->PciBaseAddress[i] = Address;
        ConfigBuffer->CpuBaseAddress[i] = BaseReg->Address;
        ConfigBuffer->BaseAddressSize[i] = (uint32_t) BaseReg->Size;
    }
    BaseReg = &dev_ptr->ROMBase;
    if (BaseReg->Flags & PCI_ROM_SPACE) {
        (*pci_drivers->entry->map_addr)(
            ll_handle, BaseReg->Address, &Address,
            (uint32_t) (BaseReg->Flags | PCI_MAP_ADDR_PCI));
        ConfigBuffer->PciRom = Address;
        ConfigBuffer->CpuRom = BaseReg->Address;
        ConfigBuffer->RomSize = (uint32_t) BaseReg->Size;
    }
    PCI_translation(ll_handle, ConfigBuffer);
}

/* Fill basic config information */

void fill_config(struct pci_dev_info *ConfigBuffer, pci_device_t *dev_ptr)
{
    uint8_t intline;

    DEBUGFUNC();
    ConfigBuffer->DeviceId = dev_ptr->DeviceId;
    ConfigBuffer->VendorId = dev_ptr->VendorId;
    ConfigBuffer->Class = dev_ptr->ClassCode;
    ConfigBuffer->BusNumber = dev_ptr->BusNumber;
    ConfigBuffer->DevFunc = dev_ptr->DevFuncNumber;
    ConfigBuffer->Revision = dev_ptr->RevisionId;
    ConfigBuffer->SubsystemId = dev_ptr->SubDeviceId;
    ConfigBuffer->SubsystemVendorId = dev_ptr->SubVendorId;
    if (!(dev_ptr->PciType & PCI_TYPE_MSI) &&
        !(dev_ptr->PciType & PCI_TYPE_MSIX)) {
        PCI_read_config8(dev_ptr->BusNumber, dev_ptr->DevFuncNumber,
                         offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                         &intline);
        if (intline != dev_ptr->IrqNumber) {
            dev_ptr->IrqNumber = intline;
        }
    }
    ConfigBuffer->Irq = dev_ptr->IrqNumber;
}

/* Calculate the amount of memory and/or I/O required for a device. If this is
 * not a show_pci command, then allocate the memory and I/O. CardBus devices
 * need to have resources allocated from their controller's primary bus.
 */

int addup_resources(pci_device_t *dev_ptr,
                    uint32_t flags,
                    int show_pci,
                    int cardbus_device,
                    int next_attach,
                    pid_t resource_owner_pid)
{
    uint8_t bus, a_bus;
    bus_map_t *map;
    pci_device_t *tmp_dev;
    int i, device_enabled = 0;
    int status, reg_idx = 0;
    uint32_t io_size, mem_size, mem_align, pref_align, rom_align;
    uint32_t pref_size, rom_size, flg;
    uint64_t pref_addr, mem_addr, io_addr, rom_addr;
    struct BaseRegister *BaseReg, alloc_io_reg, alloc_mem_reg;
    struct {
        uint64_t size;
        int reg;
    } reg_sort[7];

    DEBUGFUNC();
    bus = dev_ptr->BusNumber;
    a_bus = bus;
    if (bus) {
        if ((map = find_bus(bus))) {
            tmp_dev = map->bdev;
            a_bus = ((tmp_dev->HeaderType & 2) ? map->primary_bus
                                               : bus); /* CardBus controller? */
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "%s: Header Type 0x%x",
                          __FUNCTION__, tmp_dev->HeaderType);
        }
    }
    io_size = mem_size = pref_size = rom_size = 0;
    mem_align = rom_align = pref_align = 0;
    mem_addr = io_addr = pref_addr = rom_addr = 0;

    for (i = 0, flg = PCI_INIT_BASE0; i < (cardbus_device ? 1 : 6);
         i++, flg <<= 1) {
        if (!(flags & flg)) /* Test user enable flags */
            continue;
        BaseReg = &dev_ptr->BaseReg[i];
        if (!BaseReg->Flags)
            continue;
        reg_sort[reg_idx].size = BaseReg->Size;
        reg_sort[reg_idx++].reg = i;
        device_enabled |= (int) (BaseReg->Flags & PCI_DEVICE_ENABLED);
        if (BaseReg->Address < ((BaseReg->Flags & PCI_IO_SPACE) ? 4u : 16u))
            device_enabled = 0; /* Temporary fix!!!!! */
        if (!device_enabled && !show_pci && !next_attach) {
            if (BaseReg->Flags & PCI_IO_SPACE) {
                io_size +=
                    (uint32_t) ((BaseReg->Size < 256) ? 256 : BaseReg->Size);
            } else { /* Must be memory */
                if (BaseReg->Flags & PCI_MEM_PREFETCH) {
                    pref_size += (uint32_t) BaseReg->Size;
                    if (BaseReg->Size > pref_align)
                        pref_align = (uint32_t) BaseReg->Size;
                } else {
                    if (BaseReg->Size > mem_align) {
                        mem_align = (uint32_t) BaseReg->Size;
                    }
                    mem_size += (uint32_t) BaseReg->Size;
                }
            }
        }
    }
    if (flags & PCI_INIT_ROM) {
        BaseReg = &dev_ptr->ROMBase;
        if (BaseReg->Flags & PCI_ROM_SPACE && !cardbus_device) {
            int d1 = (int) (BaseReg->Flags & PCI_DEVICE_ENABLED);
            if (BaseReg->Address < 2)
                d1 = 0;
            if (!d1 && !show_pci && !next_attach) {
                if (BaseReg->Size > rom_align) {
                    rom_align = (uint32_t) BaseReg->Size;
                }
                rom_size += (uint32_t) BaseReg->Size;
                reg_sort[reg_idx].size = BaseReg->Size;
                reg_sort[reg_idx++].reg = 6;
            }
        }
    }

    if (io_size) {
        memset((char *) &alloc_io_reg, 0, sizeof(struct BaseRegister));
        alloc_io_reg.Flags = PCI_IO_SPACE;
        alloc_io_reg.Size = io_size;
        if ((status = pci_alloc_resource(&alloc_io_reg, 0, a_bus,
                                         resource_owner_pid)) != PCI_SUCCESS) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Alloc I/O failed",
                      __FUNCTION__);
            return status;
        }
        io_addr = alloc_io_reg.Address;
        dev_ptr->io_alloc.Address = io_addr;
        dev_ptr->io_alloc.Size = io_size;
        dev_ptr->io_alloc.Flags = PCI_IO_SPACE;
    }

    if (mem_size) {
        memset((char *) &alloc_mem_reg, 0, sizeof(struct BaseRegister));
        alloc_mem_reg.Flags = PCI_MEM_SPACE;
        alloc_mem_reg.Size = mem_size;
        if ((status = pci_alloc_resource(&alloc_mem_reg, mem_align, a_bus,
                                         resource_owner_pid)) != PCI_SUCCESS) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Alloc mem failed",
                      __FUNCTION__);
            return status;
        }
        mem_addr = alloc_mem_reg.Address;
        dev_ptr->mem_alloc.Address = mem_addr;
        dev_ptr->mem_alloc.Size = mem_size;
        dev_ptr->mem_alloc.Flags = PCI_MEM_SPACE;
    }

    if (pref_size) {
        memset((char *) &alloc_mem_reg, 0, sizeof(struct BaseRegister));
        alloc_mem_reg.Flags = PCI_MEM_PREFETCH;
        alloc_mem_reg.Size = pref_size;
        if ((status = pci_alloc_resource(&alloc_mem_reg, pref_align, a_bus,
                                         resource_owner_pid)) != PCI_SUCCESS) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Alloc pref failed",
                      __FUNCTION__);
            return status;
        }
        pref_addr = alloc_mem_reg.Address;
        dev_ptr->pref_alloc.Address = pref_addr;
        dev_ptr->pref_alloc.Size = pref_size;
        dev_ptr->pref_alloc.Flags = PCI_MEM_PREFETCH;
    }

    if (rom_size) {
        memset((char *) &alloc_mem_reg, 0, sizeof(struct BaseRegister));
        alloc_mem_reg.Flags = PCI_MEM_SPACE;
        alloc_mem_reg.Size = rom_size;
        if ((status = pci_alloc_resource(&alloc_mem_reg, rom_align, a_bus,
                                         resource_owner_pid)) != PCI_SUCCESS) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Alloc ROM failed",
                      __FUNCTION__);
            return status;
        }
        rom_addr = alloc_mem_reg.Address;
        dev_ptr->ROMBase.Address = rom_addr;
        dev_ptr->ROMBase.Size = rom_size;
        dev_ptr->ROMBase.Flags = PCI_ROM_SPACE;
    }
    /* Sort the BaseRegs in descending order */
    while (1) {
        int treg, swap = 0;
        uint64_t tsize;
        for (i = 0; i < reg_idx - 1; i++) {
            if (reg_sort[i].size < reg_sort[i + 1].size) {
                treg = reg_sort[i].reg;
                tsize = reg_sort[i].size;
                reg_sort[i].size = reg_sort[i + 1].size;
                reg_sort[i].reg = reg_sort[i + 1].reg;
                reg_sort[i + 1].size = tsize;
                reg_sort[i + 1].reg = treg;
                swap = 1;
            }
        }
        if (!swap)
            break;
    }


    if ((mem_size || io_size || pref_size) && !show_pci) {
        for (i = 0; i < reg_idx; i++) {
            int reg = reg_sort[i].reg;
            if (reg == 6)
                BaseReg = &dev_ptr->ROMBase;
            else
                BaseReg = &dev_ptr->BaseReg[reg];
            if (verbose)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Reg %d - Size 0x%x", reg,
                          BaseReg->Size);
            if (BaseReg->Address < ((BaseReg->Flags & PCI_IO_SPACE) ? 4 : 16) &&
                BaseReg->Size) {
                if (BaseReg->Flags & PCI_IO_SPACE) {
                    BaseReg->Address |= io_addr; /* Keep flags */
                    if (cardbus_device)
                        BaseReg->Flags |= PCI_DEVICE_ENABLED;
                    io_addr += (BaseReg->Size < 256) ? 256 : BaseReg->Size;
                    io_size -=
                        (uint32_t) ((BaseReg->Size < 256) ? 256
                                                          : BaseReg->Size);
                } else {
                    if (BaseReg->Flags & PCI_MEM_PREFETCH) {
                        BaseReg->Address |= pref_addr; /* Keep flags */
                        if (cardbus_device)
                            BaseReg->Flags |= PCI_DEVICE_ENABLED;
                        pref_addr += BaseReg->Size;
                        pref_size -= (uint32_t) BaseReg->Size;
                    } else {
                        if (reg == 6) {
                            BaseReg->Address |= rom_addr; /* Keep flags */
                            if (cardbus_device)
                                BaseReg->Flags |= PCI_DEVICE_ENABLED;
                            rom_addr += BaseReg->Size;
                            rom_size -= (uint32_t) BaseReg->Size;
                        } else {
                            BaseReg->Address |= mem_addr; /* Keep flags */
                            if (cardbus_device)
                                BaseReg->Flags |= PCI_DEVICE_ENABLED;
                            mem_addr += BaseReg->Size;
                            mem_size -= (uint32_t) BaseReg->Size;
                        }
                    }
                }
            }
        }
    }
    return PCI_SUCCESS;
}

/* Setup MSI-X structures */

static int setup_msix(uint8_t bus,
                      uint8_t devfn,
                      pci_device_t *dev_ptr,
                      uint32_t *irqlist,
                      int num_irq)
{
    int base_reg, tab_size;
    int i;
    size_t bar_count = sizeof(dev_ptr->BaseReg) / sizeof(dev_ptr->BaseReg[0]);
    uint32_t tab_offset;
    uint64_t tab_bytes;
    uint16_t cr, cr1;
    off_t offset;
    msix_table_t *m_tab, *m_tab1;
    uint8_t *tmp;
    struct BaseRegister *BaseReg;

    DEBUGFUNC();
    base_reg = (int) (dev_ptr->msix_tab_off & PCI_MSIX_BIR_MASK);
    tab_offset = (dev_ptr->msix_tab_off & ~(uint32_t) PCI_MSIX_BIR_MASK);
    tab_size = (dev_ptr->mess_ctrl & PCI_MSIX_TAB_SIZE) + 1;
    tab_bytes = (uint64_t) tab_size * sizeof(msix_table_t);
    if ((size_t) base_reg >= bar_count ||
        dev_ptr->BaseReg[base_reg].Size == 0 ||
        tab_offset > dev_ptr->BaseReg[base_reg].Size ||
        tab_bytes > dev_ptr->BaseReg[base_reg].Size - tab_offset) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "Invalid MSIX table: bir %d off 0x%x entries %d "
                  "bar size 0x%llx",
                  base_reg, tab_offset, tab_size,
                  (unsigned long long) ((size_t) base_reg < bar_count
                                            ? dev_ptr->BaseReg[base_reg].Size
                                            : 0));
        return -1;
    }
    BaseReg = &dev_ptr->BaseReg[base_reg];
    offset = (off_t) PCI_MEM_ADDR(BaseReg->Address);
    if ((m_tab =
             mmap(NULL, BaseReg->Size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                  MAP_PHYS | MAP_SHARED, NOFD, offset)) ==
        (msix_table_t *) MAP_FAILED) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "Unable to mmap MSIX memory");
        return -1;
    }
    /* You cannot write to the MSIX memory space if the device is disabled */
    PCI_read_config16(bus, devfn, offsetof(struct _pci_config_regs, Command), 1,
                      (char *) &cr);
    if (!(cr & PCI_MSPACEEN)) {
        cr1 = cr | (PCI_MSPACEEN | PCI_ISPACEEN | PCI_MASTEREN);
        PCI_write_config16(bus, devfn,
                           offsetof(struct _pci_config_regs, Command), 1,
                           (char *) &cr1);
    } else {
        cr = cr1 = 0;
    }
    m_tab1 = m_tab;
    /*  memset ((char *) m_tab, 0, BaseReg->Size); */
    tmp = (uint8_t *) m_tab;
    tmp += tab_offset;
    m_tab = (msix_table_t *) tmp;
    dev_ptr->msix_tab = (msix_table_t *) tmp;
    memset((char *) m_tab, 0, ((size_t) tab_size * sizeof(msix_table_t)));
    if (verbose > 2) {
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                  "%s MSIX table size %d - offset 0x%x", __FUNCTION__, tab_size,
                  tab_offset);
    }
    if (verbose > 2) {
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s msix_addr %p - basereg 0x%llx",
                  __FUNCTION__, m_tab, PCI_MEM_ADDR(BaseReg->Address));
    }
    for (i = 0; i < num_irq * 2; i += 2, m_tab++) {
        m_tab->msg_addr_low = MSI_REG_ADDR;
        m_tab->msg_addr_hi = 0;
        /*      m_tab->msg_data = irqlist [i + 1] | 0xc000; */
        m_tab->msg_data = irqlist[i + 1];
        m_tab->vector_control = 0;
        if (verbose > 2) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "%s msg_addr 0x%x - msg_data 0x%x", __FUNCTION__,
                      m_tab->msg_addr_low, m_tab->msg_data);
        }
    }
    dev_ptr->msix_tab = NULL;
    dev_ptr->pba_tab = NULL;
    (void) munmap(m_tab1, BaseReg->Size);
    if (cr1) {
        PCI_write_config16(bus, devfn,
                           offsetof(struct _pci_config_regs, Command), 1,
                           (char *) &cr);
    }
    return 0;
}

/* get an irq */

int get_an_irq(pci_device_t *dev_ptr,
               int cardbus_device,
               uint32_t flags,
               int next_attach,
               int *local_errno)
{
    int msi_irq = 0, status, class, num_irq = 0;
    uint8_t bus, devfn;
    uint16_t word, mess_ctrl, mme;
    uint32_t msi_off, dword, irqlist[SERV_MAX_IRQS * 2];

    DEBUGFUNC();
    class = (dev_ptr->ClassCode & 0xff0000);
    if (use_msi) {
        msi_irq = ((dev_ptr->PciType & PCI_TYPE_MSI) ||
                   (dev_ptr->PciType & PCI_TYPE_MSIX));
        if (dev_ptr->PciType & PCI_TYPE_MSI) {
            dev_ptr->msi_req_cnt = 1 << ((dev_ptr->mess_ctrl >> 1) & 0x07);
            if (dev_ptr->msi_req_cnt > max_msi)
                dev_ptr->msi_req_cnt = (uint8_t) max_msi;
        }
        if ((dev_ptr->PciType & PCI_TYPE_MSIX) && (flags & PCI_USE_MSIX)) {
            dev_ptr->msix_req_cnt =
                (uint8_t) ((dev_ptr->mess_ctrl & PCI_MSIX_TAB_SIZE) + 1);
        }
        if (flags & PCI_USE_MSIX) {
            if (dev_ptr->msix_req_cnt) {
                msi_irq = dev_ptr->msix_req_cnt;
            }
        } else {
            if (dev_ptr->msi_req_cnt) {
                msi_irq = dev_ptr->msi_req_cnt;
            }
        }
        if (msi_irq > SERV_MAX_IRQS) {
            if (verbose > 2)
                pci_slogf(
                    _SLOGC_PCI, _SLOG_DEBUG1,
                    "%s Requested MSI[X] vectors %d, but only allowing %d",
                    __FUNCTION__, msi_irq, SERV_MAX_IRQS);
            msi_irq = SERV_MAX_IRQS;
        } else {
            if (verbose > 2)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                          "%s Requested MSI[X] vectors %d", __FUNCTION__,
                          msi_irq);
        }
    }
    if (class == PCI_CLASS_DISPLAY && !enable_crt_msi) {
        msi_irq = 0;
    }
    bus = dev_ptr->BusNumber;
    devfn = dev_ptr->DevFuncNumber;
    if (dev_ptr->IntPin && (flags & PCI_INIT_IRQ) && irq_ok(bus, devfn)) {
        if (devfn & 0x07 && dev_ptr->IntPin == 1 && is_not_bridge(bus, devfn) &&
            same_int(bus, devfn)) { /* Functions on same IntPin */
            /* Changed this from a read_config to avail_irq, as interrupts on
             * some boards are larger than 8-bits.
             */
            num_irq = SERV_MAX_IRQS;
            if ((*pci_drivers->entry->avail_irq)(
                    ll_handle, bus, msi_irq ? devfn : (devfn & 0xf8), irqlist,
                    (uint32_t *) &num_irq, &msi_irq) != PCI_SUCCESS) {
                *local_errno = ENXIO;
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: No irq",
                              __FUNCTION__);
                return -1;
            }
            if (msi_irq) {
                dev_ptr->SaveIrq = dev_ptr->IrqNumber;
            }
            dev_ptr->IrqNumber = irqlist[0];
        } else {
            if (dev_ptr->bmap && !cardbus_device) {
                /* For CardBus devices, the IRQ has to be programmed into the
                 * CardBus controller, not the card.
                 */
                if (dev_ptr->bmap->bdev->ClassCode ==
                    (PCI_CLASS_BRIDGE | PCI_SUBCLASS_BRIDGE_CARDBUS)) {
                    bus = dev_ptr->bmap->bdev->BusNumber;
                    devfn = dev_ptr->bmap->bdev->DevFuncNumber;
                    cardbus_device = 2;
                }
            }
            num_irq = SERV_MAX_IRQS;
            if ((*pci_drivers->entry->avail_irq)(ll_handle, bus, devfn, irqlist,
                                                 (uint32_t *) &num_irq,
                                                 &msi_irq) != PCI_SUCCESS) {
                *local_errno = ENXIO;
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: No irq2",
                              __FUNCTION__);
                return -1;
            }
            if (cardbus_device == 2 && dev_ptr->bmap->bdev->IrqNumber) {
                dev_ptr->IrqNumber = dev_ptr->bmap->bdev->IrqNumber;
            } else {
                if (!next_attach && num_irq) {
                    if (msi_irq) {
                        dev_ptr->SaveIrq = dev_ptr->IrqNumber;
                    }
                    if (!(dev_ptr->IrqNumber =
                              pci_alloc_irq(irqlist, num_irq, bus, msi_irq))) {
                        if (num_irq != 1) { /* If the allocate failed on a list,
                                               big problem!
                                               */
                            *local_errno = ENXIO;
                            if (verbose > 2)
                                pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                                          "%s: Alloc IRQ failed", __FUNCTION__);
                            return -1;
                        } else {
                            if (msi_irq) {
                                dev_ptr->SaveIrq = dev_ptr->IrqNumber;
                            }
                            dev_ptr->IrqNumber = irqlist[0];
                        }
                    } else {
                        if (cardbus_device == 2) {
                            dev_ptr->bmap->bdev->IrqNumber = dev_ptr->IrqNumber;
                        }
                    }
                }
            }
        }
        if (!msi_irq && map_irqs && dev_ptr->IrqNumber != 0xffffffff) {
            if ((status = (*pci_drivers->entry->map_irq)(
                     ll_handle, bus, devfn, dev_ptr->IrqNumber,
                     dev_ptr->IntPin + 9, PCI_MAP_IRQ)) != PCI_SUCCESS) {
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                              "%s: Map IRQ 0x%x failed 0x%x!", __FUNCTION__,
                              dev_ptr->IrqNumber, status);
            }
        }

        {
            uint8_t irq = (uint8_t) dev_ptr->IrqNumber;
            if (PCI_write_config8(
                    bus, devfn,
                    offsetof(struct _pci_config_regs, Interrupt_Line), 1,
                    &irq) != PCI_SUCCESS) {
                *local_errno = ENXIO;
                if (verbose > 2)
                    pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: Write IRQ failed",
                              __FUNCTION__);
                return -1;
            }
        }
    }

    if (msi_irq && dev_ptr->IrqNumber > 15) {
        if ((flags & PCI_USE_MSIX) && dev_ptr->msix_req_cnt) {
            dev_ptr->msi_irq = PCI_MSIX;
            msi_off = (uint32_t) dev_ptr->msix_off;
            PCI_read_config16(bus, devfn, msi_off + 2, 1, &mess_ctrl);
            if (setup_msix(bus, devfn, dev_ptr, irqlist, num_irq)) {
                dev_ptr->msix_tab = NULL;
                return -1;
            }
            mess_ctrl |= PCI_MSIX_ENABLE;
            PCI_write_config16(bus, devfn, msi_off + 2, 1, &mess_ctrl);
        } else {
            dev_ptr->msi_irq = PCI_MSI;
            msi_off = (uint32_t) dev_ptr->msi_off;
            PCI_read_config16(bus, devfn, msi_off + 2, 1, &mess_ctrl);
            dword = MSI_REG_ADDR;
            PCI_write_config32(bus, devfn, msi_off + 4, 1, &dword);
            if (mess_ctrl & PCI_MSI_64BIT) {
                dword = 0;
                PCI_write_config32(bus, devfn, msi_off + 8, 1, &dword);
            }
            word = (uint16_t) irqlist[1];
            PCI_write_config16(
                bus, devfn,
                (mess_ctrl & PCI_MSI_64BIT) ? msi_off + 12 : msi_off + 8, 1,
                &word);
            mess_ctrl |= PCI_MSI_ENABLE;
            if (msi_irq >
                1) { /* msi_irq contains the number of vectors returned */
                switch (msi_irq) { /* from the call to avail_irq */
                case 2:
                    mme = 1;
                    break;
                case 4:
                    mme = 2;
                    break;
                case 8:
                    mme = 3;
                    break;
                case 16:
                    mme = 4;
                    break;
                case 32:
                    mme = 5;
                    break;
                default:
                    pci_slogf(_SLOGC_NETWORK, _SLOG_ERROR,
                              "Unknown MSI IRQ count %d", msi_irq);
                    mme = 0;
                    break;
                }
                mess_ctrl |= (mme << 4);
            }
            PCI_write_config16(bus, devfn, msi_off + 2, 1, &mess_ctrl);
        }
        if (verbose > 2) {
            int i;
            for (i = 0; i < msi_irq * 2; i++)
                pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1, "irqlist %d - 0x%x", i,
                          irqlist[i]);
        }
        PCI_read_config16(bus, devfn,
                          offsetof(struct _pci_config_regs, Command), 1, &word);
        word |= PCI_COMMAND_INTX_DISABLE;
        PCI_write_config16(
            bus, devfn, offsetof(struct _pci_config_regs, Command), 1, &word);
    }
    return 0;
}
