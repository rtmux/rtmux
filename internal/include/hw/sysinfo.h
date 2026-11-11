/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __HWSYSINFO_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS

/* This must correspond to AS_NULL_OFF, or some very unexpected results may
 * occur
 */
#define HWI_NULL_OFF ((uint16_t) -1)

/* note that the HWI_ILLEGAL_VECTOR value is selected such that if it is passed
 * to interrupt_attach(), it will fail with 'errno' == EINVAL
 */
#define HWI_ILLEGAL_VECTOR ((unsigned) -1)
#define MAX_SYNONYM_COUNT 10 /* no particular reason for 10, just a choice */
#define HWI_ITEM_ROOT_HW "hw"
#define HWI_ITEM_BUS_PCI "pci"
#define HWI_ITEM_BUS_ISA "isa"
#define HWI_ITEM_BUS_EISA "eisa"
#define HWI_ITEM_BUS_MCA "mca"
#define HWI_ITEM_BUS_PCMCIA "pcmcia"
#define HWI_ITEM_BUS_USB "usb"
#define HWI_ITEM_BUS_I2C "i2c"
#define HWI_ITEM_BUS_SPI "spi"
#define HWI_ITEM_BUS_CAN "can"
#define HWI_ITEM_BUS_SDIO "sdio"
#define HWI_ITEM_BUS_I394 "I394"
#define HWI_ITEM_BUS_MOST "most"
#define HWI_ITEM_BUS_BLUETOOTH "bluetooth"
#define HWI_ITEM_BUS_UNKNOWN "unknown"
#define HWI_ITEM_BUS_INTERNAL "internal"
#define HWI_ITEM_DEVCLASS_SERIAL "serial"
#define HWI_ITEM_DEVCLASS_PARALLEL "parallel"
#define HWI_ITEM_DEVCLASS_DMA "dma"
#define HWI_ITEM_DEVCLASS_RTC "rtc"
#define HWI_ITEM_DEVCLASS_DISK "disk"
#define HWI_ITEM_DEVCLASS_NETWORK "network"
#define HWI_ITEM_DEVCLASS_AUDIO_CTRL "audio_ctrl"
#define HWI_ITEM_DEVCLASS_AUDIO_MIXER "audio_mixer"
#define HWI_ITEM_DEVCLASS_GRAPHICS "graphics"
#define HWI_ITEM_DEVCLASS_INPUT "input"
#define HWI_ITEM_DEVCLASS_NVRAM "nvram"
#define HWI_ITEM_DEVCLASS_MISC "misc"

struct hwi_prefix {
    uint16_t size;
    uint16_t name;
};

struct hwi_item {
    struct hwi_prefix prefix;
    uint16_t itemsize;
    uint16_t itemname;
    uint16_t owner;
    uint16_t kids;
};

#define HWI_TAG_NAME_group "Group"
#define HWI_TAG_ALIGN_group (sizeof(uint32_t))
struct hwi_group {
    struct hwi_item item;
};

#define HWI_TAG_NAME_bus "Bus"
#define HWI_TAG_ALIGN_bus (sizeof(uint32_t))
struct hwi_bus {
    struct hwi_item item;
};


#define HWI_TAG_NAME_device "Device"
#define HWI_TAG_ALIGN_device (sizeof(uint32_t))
struct hwi_device {
    struct hwi_item item;
    uint32_t pnpid;
};

#define HWI_TAG_NAME_synonym "Synonym"
#define HWI_TAG_ALIGN_synonym (sizeof(uint32_t))
struct hwi_synonym {
    struct hwi_item item;
    uint32_t real;
};

#define HWI_TAG_NAME_location "location"
#define HWI_TAG_ALIGN_location (sizeof(uint64_t))
#define HWI_ADDRSPACE_IO 0
#define HWI_ADDRSPACE_MEMORY 1
struct hwi_location {
    struct hwi_prefix prefix;
    uint32_t len;
    uint64_t base;
    uint16_t regshift;
    uint16_t addrspace;
};

#define HWI_TAG_NAME_dma "dma"
#define HWI_TAG_ALIGN_dma (sizeof(uint32_t))
struct hwi_dma {
    struct hwi_prefix prefix;
    uint32_t chnl;
};

#define HWI_TAG_NAME_irq "irq"
#define HWI_TAG_ALIGN_irq (sizeof(uint32_t))
struct hwi_irq {
    struct hwi_prefix prefix;
    uint32_t vector;
};

#define HWI_TAG_NAME_irqmapping "irqmapping"
#define HWI_TAG_ALIGN_irqmapping (sizeof(uint32_t))
struct __hwi_specific_s {
    uint8_t devfunc;
    int8_t adjust;
};
struct hwi_irqmapping {
    struct hwi_prefix prefix;
    uint32_t irq;
    uint16_t num;
    __FLEXARY(struct __hwi_specific_s,
              specific); /* struct __hwi_specific_s specific[] */
};

#define HWI_TAG_NAME_irqrange "irqrange"
#define HWI_TAG_ALIGN_irqrange (sizeof(uint32_t))
struct hwi_irqrange {
    struct hwi_prefix prefix;
    uint32_t irq;
    uint16_t num;
};

#define HWI_TAG_NAME_diskgeometry "diskgeometry"
#define HWI_TAG_ALIGN_diskgeometry (sizeof(uint32_t))
struct hwi_diskgeometry {
    struct hwi_prefix prefix;
    uint8_t disknumber;
    uint8_t sectorsize; /* as a power of two */
    uint16_t heads;
    uint16_t cyls;
    uint16_t sectors;
    uint32_t nblocks;
};

#define HWI_TAG_NAME_diskpartition "diskpartition"
#define HWI_TAG_ALIGN_diskpartition (sizeof(uint64_t))
struct hwi_diskpartition {
    struct hwi_prefix prefix;
    uint8_t part_flags;
    uint8_t part_num;
    uint16_t part_name;
    uint64_t part_slba;
    uint64_t part_nlba;
};

#define HWI_TAG_NAME_regname "regname"
#define HWI_TAG_ALIGN_regname (sizeof(uint32_t))
struct hwi_regname {
    struct hwi_prefix prefix;
    uint16_t regname;
    uint16_t offset;
};

#define HWI_TAG_NAME_inputclk "inputclk"
#define HWI_TAG_ALIGN_inputclk (sizeof(uint32_t))
struct hwi_inputclk {
    struct hwi_prefix prefix;
    uint32_t clk;
    uint32_t div;
};

#define HWI_TAG_NAME_nicaddr "nicaddr"
#define HWI_TAG_ALIGN_nicaddr (sizeof(uint32_t))
struct hwi_nicaddr {
    struct hwi_prefix prefix;
    uint32_t len;
    __FLEXARY(uint8_t, addr); /* uint8_t addr[] */
};

#define HWI_TAG_NAME_nicphyaddr "nicphyaddr"
#define HWI_TAG_ALIGN_nicphyaddr (sizeof(uint32_t))
struct hwi_nicphyaddr {
    struct hwi_prefix prefix;
    uint32_t addr;
};

#define HWI_TAG_NAME_busattr "busattr"
#define HWI_TAG_ALIGN_busattr (sizeof(uint64_t))
struct hwi_busattr {
    struct hwi_prefix prefix;
    uint32_t flags; /* see hwi_busattr_flags_e */
    uint64_t addr;  /* bus address */
    uint32_t speed;
    uint16_t width; /* width of the bus */
    uint16_t spare; /* pad out for size alignment */
    uint64_t size;  /* bus address size (intentionally at end of structure) */
};
/* some or all of the following may be applicable depending on the particular
 * bus
 */
__extension__ typedef enum {
    hwi_busattr_MASTER = (1U << 0), /* otherwise slave */
    hwi_busattr_USE_ADDR =
        (1U << 1), /* addr field is valid (because 0 is a valid address) */

    /* the following apply to PCI only */
    hwi_busattr_PCI_memtype_mask =
        (7U << 28), /* 3 bits for 8 possible types (see below) */
    hwi_busattr_isPCIe = (1U << 31)
    /* add as required */
} hwi_busattr_flags_e;

/* 3 currently defined PCI memory types (only applies to PCI buses */
#define hwi_busattr_PCI_memtype_NONE (0U << 28)
#define hwi_busattr_PCI_memtype_IO (1U << 28)
#define hwi_busattr_PCI_memtype_MEM (2U << 28)
#define hwi_busattr_PCI_memtype_MEM_PREFETCH (3U << 28)


#define HWI_TAG_NAME_optstr "optstr"
#define HWI_TAG_ALIGN_optstr (sizeof(uint32_t))
struct hwi_optstr {
    struct hwi_prefix prefix;
    uint32_t string;
};

/* PCI address window (one tag per entry of the PCIe host bridge "ranges").
 *
 * pci_addr — bus-side base of the window (seen by PCI devices) cpu_addr —
 * CPU-side physical base (seen by CPU / used for mmap) size — window length
 * flags — bit 0: I/O space, bit 1: memory space, bit 5: prefetchable
 *              (matches PCI_IO_SPACE / PCI_MEM_SPACE / PCI_MEM_PREFETCH)
 */
#define HWI_TAG_NAME_pci_window "pci_window"
#define HWI_TAG_ALIGN_pci_window (sizeof(uint64_t))
#define HWI_PCI_WINDOW_IO 0x01
#define HWI_PCI_WINDOW_MEM 0x02
#define HWI_PCI_WINDOW_PREFETCH 0x20
struct hwi_pci_window {
    struct hwi_prefix prefix;
    uint32_t flags;
    uint64_t pci_addr;
    uint64_t cpu_addr;
    uint64_t size;
};

#define HWI_TAG_NAME_dll "drvr_dll"
#define HWI_TAG_ALIGN_dll (sizeof(uint32_t))
struct hwi_dll {
    struct hwi_prefix prefix;
    uint32_t name;
};

#define HWI_TAG_NAME_errata "errata"
#define HWI_TAG_ALIGN_errata (sizeof(uint32_t))
struct hwi_errata {
    struct hwi_prefix prefix;
    uint32_t num; /* errata number (as defined for the device in errata docs) */
};
#define HWI_TAG_NAME_nanospin "nanospin"
#define HWI_TAG_ALIGN_nanospin (sizeof(uint32_t))

struct hwi_nanospin {
    struct hwi_prefix prefix;
    uint32_t hundred_loop_time;
    uint32_t overhead;
};

#define HWI_TAG_NAME_pad "pad"
#define HWI_TAG_ALIGN_pad (sizeof(uint32_t))
struct hwi_pad {
    struct hwi_prefix prefix;
};

#define HWI_TAG_NAME_hwversion "hwversion"
#define HWI_TAG_ALIGN_hwversion (sizeof(uint32_t))
struct hwi_hwversion {
    struct hwi_prefix prefix;
    uint8_t hclass;
    uint8_t version;
    uint16_t name;
};

typedef union {
    struct hwi_prefix prefix;
    struct hwi_item item;
    struct hwi_group group;
    struct hwi_bus bus;
    struct hwi_device device;
    struct hwi_location location;
    struct hwi_dma dma;
    struct hwi_irq irq;
    struct hwi_irqmapping irqmapping;
    struct hwi_irqrange irqrange;
    struct hwi_diskgeometry diskgeometry;
    struct hwi_diskpartition diskpartition;
    struct hwi_regname regname;
    struct hwi_inputclk inputclk;
    struct hwi_nicaddr nicaddr;
    struct hwi_nicphyaddr nicphyaddr;
    struct hwi_pad pad;
    struct hwi_synonym synonym;
    struct hwi_busattr busattr;
    struct hwi_dll dll;
    struct hwi_optstr optstr;
    struct hwi_pci_window pci_window;
    struct hwi_errata errata;
    struct hwi_nanospin nanospin;
    struct hwi_hwversion hwversion;
} hwi_tag;

void *__hwi_base(void);
char *__hwi_find_string(unsigned __off);

static inline ptrdiff_t hwi_tag2off(void *tag)
{
    return tag - __hwi_base();
}

static inline void *hwi_off2tag(unsigned off)
{
    return (uint8_t *) __hwi_base() + off;
}

__END_DECLS

#define __HWSYSINFO_H_INCLUDED
#endif
