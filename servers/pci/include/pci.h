/*
 * PCI definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* pci.h */
#ifndef __PCI_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _INTTYPES_H_INCLUDED
#include <inttypes.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef _STDDEF_H_INCLUDED
#include <stddef.h>
#endif

#pragma pack(push, 1)

__BEGIN_DECLS struct _pci_config_regs {
    uint16_t Vendor_ID;            /* 0x00 */
    uint16_t Device_ID;            /* 0x02 */
    uint16_t Command;              /* 0x04 */
    uint16_t Status;               /* 0x06 */
    uint8_t Revision_ID;           /* 0x08 */
    uint8_t Class_Code[3];         /* 0x09 */
    uint8_t Cache_Line_Size;       /* 0x0C */
    uint8_t Latency_Timer;         /* 0x0D */
    uint8_t Header_Type;           /* 0x0E */
    uint8_t BIST;                  /* 0x0F */
    uint32_t Base_Address_Regs[6]; /* 0x10 */
    uint32_t CardBus_CIS;          /* 0x28 */
    uint16_t Sub_Vendor_ID;        /* 0x2C */
    uint16_t Sub_System_ID;        /* 0x2E */
    uint32_t ROM_Base_Address;     /* 0x30 */
    uint8_t Capabilities_Pointer;  /* 0x34 */
    uint8_t Reserved2[3];          /* 0x35 */
    uint32_t Reserved3;            /* 0x38 */
    uint8_t Interrupt_Line;        /* 0x3C */
    uint8_t Interrupt_Pin;         /* 0x3D */
    uint8_t Min_Gnt;               /* 0x3E */
    uint8_t Max_Lat;               /* 0x3F */
    uint8_t Device_Dependent_Regs[192];
};

struct _pci_bridge_config_regs {
    uint16_t Vendor_ID;                  /* 0x00 */
    uint16_t Device_ID;                  /* 0x02 */
    uint16_t Command;                    /* 0x04 */
    uint16_t Status;                     /* 0x06 */
    uint8_t Revision_ID;                 /* 0x08 */
    uint8_t Class_Code[3];               /* 0x09 */
    uint8_t Cache_Line_Size;             /* 0x0C */
    uint8_t Latency_Timer;               /* 0x0D */
    uint8_t Header_Type;                 /* 0x0E */
    uint8_t BIST;                        /* 0x0F */
    uint32_t Base_Address_Regs[2];       /* 0x10 */
    uint8_t Primary_Bus_Number;          /* 0x18 */
    uint8_t Secondary_Bus_Number;        /* 0x19 */
    uint8_t Subordinate_Bus_Number;      /* 0x1A */
    uint8_t Secondary_Latency_Timer;     /* 0x1B */
    uint8_t IO_Base;                     /* 0x1C */
    uint8_t IO_Limit;                    /* 0x1D */
    uint16_t Secondary_Status;           /* 0x1E */
    uint16_t Memory_Base;                /* 0x20 */
    uint16_t Memory_Limit;               /* 0x22 */
    uint16_t Prefetchable_Memory_Base;   /* 0x24 */
    uint16_t Prefetchable_Memory_Limit;  /* 0x26 */
    uint32_t Prefetchable_Base_Upper32;  /* 0x28 */
    uint32_t Prefetchable_Limit_Upper32; /* 0x2c */
    uint16_t IO_Base_Upper16;            /* 0x30 */
    uint16_t IO_Limit_Upper16;           /* 0x32 */
    uint8_t Capabilities_Pointer;        /* 0x34 */
    uint8_t Reserved1[3];                /* 0x35 */
    uint32_t ROM_Base_Address;           /* 0x38 */
    uint8_t Interrupt_Line;              /* 0x3C */
    uint8_t Interrupt_Pin;               /* 0x3D */
    uint16_t Bridge_Control;             /* 0x3E */
    uint8_t Device_Dependent_Regs[192];
};

struct _pci_cardbus_config_regs {
    uint16_t Vendor_ID;             /* 0x00 */
    uint16_t Device_ID;             /* 0x02 */
    uint16_t Command;               /* 0x04 */
    uint16_t Status;                /* 0x06 */
    uint8_t Revision_ID;            /* 0x08 */
    uint8_t Class_Code[3];          /* 0x09 */
    uint8_t Cache_Line_Size;        /* 0x0C */
    uint8_t Latency_Timer;          /* 0x0D */
    uint8_t Header_Type;            /* 0x0E */
    uint8_t BIST;                   /* 0x0F */
    uint32_t Socket_Exca_Base_Reg;  /* 0x10 */
    uint8_t Capabilities_Pointer;   /* 0x14 */
    uint8_t reserved1;              /* 0x15 */
    uint16_t Secondary_Status;      /* 0x16 */
    uint8_t Pci_Bus_Num;            /* 0x18 */
    uint8_t Cardbus_Bus_Num;        /* 0x19 */
    uint8_t Sub_Bus_Num;            /* 0x1A */
    uint8_t Cardbus_Latency_Timer;  /* 0x1B */
    uint32_t Mem_Base_Reg_0;        /* 0x1C */
    uint32_t Mem_Limit_Reg_0;       /* 0x20 */
    uint32_t Mem_Base_Reg_1;        /* 0x24 */
    uint32_t Mem_Limit_Reg_1;       /* 0x28 */
    uint32_t Io_Base_Reg_0;         /* 0x2C */
    uint32_t Io_Limit_Reg_0;        /* 0x30 */
    uint32_t Io_Base_Reg_1;         /* 0x34 */
    uint32_t Io_Limit_Reg_1;        /* 0x38 */
    uint8_t Interrupt_Line;         /* 0x3C */
    uint8_t Interrupt_Pin;          /* 0x3D */
    uint16_t Bridge_Control;        /* 0x3E */
    uint16_t Subsystem_Vendor_Id;   /* 0x40 */
    uint16_t Subsystem_Id;          /* 0x42 */
    uint32_t If_Legacy_Base_Reg;    /* 0x44 */
    uint8_t reserved2[56];          /* 0x48 */
    uint32_t System_Control;        /* 0x80 */
    uint8_t reserved3[8];           /* 0x84 */
    uint32_t Multifunction_Routing; /* 0x8C */
    uint8_t Retry_Status;           /* 0x90 */
    uint8_t Card_Control;           /* 0x91 */
    uint8_t Device_Control;         /* 0x92 */
    uint8_t Buffer_Control;         /* 0x93 */
    uint32_t Dma_Reg_0;             /* 0x94 */
    uint32_t Dma_Reg_1;             /* 0x98 */
    uint8_t Device_Dependent_Regs[100];
};


/* Power management capability - 0x01 */

struct _pci_capability_power {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t Power_PMC;
    uint16_t Power_PMCSR;
    uint8_t Power_PMCSR_BSE;
    uint8_t Power_Data;
};

/* AGP capability - 0x02 */

struct _pci_capability_agp {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint8_t Major_Minor_Revision;
    uint8_t Reserved;
    uint32_t AGP_Status;
    uint32_t AGP_Command;
};

/* Vital Product Data Capability - 0x03 */

struct _pci_capability_vpd {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t VPD_Address;
    uint32_t VPD_Data;
};

/* Slot Numbering capability - 0x04 */

struct _pci_capability_slot_numbering {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint8_t Card_Slot;
    uint8_t Chassis_Number;
};

/* MSI capability for 32-bit message address - 0x05 */

struct _pci_capability_msi32 {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t Message_Control;
    uint32_t Message_Address;
    uint16_t Message_Data;
    uint16_t Filler;
};

/* MSI capability for 64-bit message address - 0x05 */

struct _pci_capability_msi64 {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t Message_Control;
    uint32_t Message_Address;
    uint32_t Message_Address_Upper;
    uint16_t Message_Data;
    uint16_t Filler;
};

/* MSI capability for 32-bit message address and per-vector masking - 0x05 */

struct _pci_capability_msi32_PVM {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t Message_Control;
    uint32_t Message_Address;
    uint16_t Message_Data;
    uint16_t Filler;
    uint32_t Mask_Bits;
    uint32_t Pending_Bits;
};

/* MSI capability for 64-bit message address and per-vector masking - 0x05 */

struct _pci_capability_msi64_PVM {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t Message_Control;
    uint32_t Message_Address;
    uint32_t Message_Address_Upper;
    uint16_t Message_Data;
    uint16_t Filler;
    uint32_t Mask_Bits;
    uint32_t Pending_Bits;
};

/* Message Control for MSI definitions */

#define PCI_MSI_64BIT 0x0080     /* 64-bit address capable */
#define PCI_MSI_ENABLE 0x0001    /* MSI Enable */
#define PCI_MSIX_ENABLE 0x8000   /* MSI-X Enable */
#define PCI_MSIX_TAB_SIZE 0x07ff /* MSI-X Table size mask */
#define PCI_MSIX_BIR_MASK 0x0007 /* MSI-X Base Index Register mask */

/* PCI-X capability - 0x07 */

struct _pci_capability_pcix {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t PCI_X_Command;
    uint32_t PCI_X_Status;
};

/* PCI-X bridge capability - 0x07 */

struct _pci_capability_pcix_bridge {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t PCI_X_Secondary_Status;
    uint32_t PCI_X_Bridge_Status;
    uint32_t Upstream_Split_Transaction_Control;
    uint32_t Downstream_Split_Transaction_Control;
};

/* PCI Hot Plug capability - 0x0C */

struct _pci_capability_hot_plug {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint8_t Dword_Select;
    uint8_t CSP_CIP;
    uint32_t Dword_Data;
};

/* PCI-Express capability - 0x10 */

struct _pci_capability_pci_express {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t PCI_Express_Cap_Reg;
    uint32_t Device_Capabilities;
    uint16_t Device_Control;
    uint16_t Device_Status;
    uint32_t Link_Capabilities;
    uint16_t Link_Control;
    uint16_t Link_Status;
    uint32_t Slot_Capabilities;
    uint16_t Slot_Control;
    uint16_t Slot_Status;
    uint16_t Root_Control;
    uint16_t Root_Capabilities;
    uint32_t Root_Status;
};

/* MSI-X capability - 0x11 */

struct _pci_capability_msix {
    uint8_t Capability_ID;
    uint8_t Next_Pointer;
    uint16_t Message_Control;
    uint32_t Table_Offset;
    uint32_t PBA_Offset;
};

struct _pci_msix_table {
    uint32_t Message_Address;
    uint32_t Message_Address_Upper;
    uint32_t Message_Data;
    uint32_t Vector_Control;
};

struct pci_dev_info {
    uint16_t DeviceId;
    uint16_t VendorId;
    uint16_t SubsystemId;
    uint16_t SubsystemVendorId;
    uint8_t BusNumber;
    uint8_t DevFunc;
    uint8_t Revision;
    uint8_t Rsvd[5];
    uint32_t Class;
    uint32_t Irq;

    uint64_t CpuIoTranslation;    /* pci_addr = cpu_addr - translation */
    uint64_t CpuMemTranslation;   /* pci_addr = cpu_addr - translation */
    uint64_t CpuIsaTranslation;   /* pci_addr = cpu_addr - translation */
    uint64_t CpuBmstrTranslation; /* pci_addr = cpu_addr + translation */

    uint64_t PciBaseAddress[6];
    uint64_t CpuBaseAddress[6];
    uint32_t BaseAddressSize[6];
    uint64_t PciRom;
    uint64_t CpuRom;
    uint32_t RomSize;
    uint32_t Rsvd1;
    uint64_t BusIoStart;
    uint64_t BusIoEnd;
    uint64_t BusMemStart;
    uint64_t BusMemEnd;
    uint8_t msi;
    uint8_t Rsvd2[3];
    uint32_t Rsvd3;
};

/* PCI class defines */
#define PCI_CLASS_STORAGE 0x00010000
#define PCI_CLASS_DISPLAY 0x00030000
#define PCI_CLASS_BRIDGE 0x00060000
#define PCI_SUBCLASS_BRIDGE_HOST 0x00000000
#define PCI_SUBCLASS_BRIDGE_PCI 0x00000400
#define PCI_SUBCLASS_BRIDGE_CARDBUS 0x00000700
#define PCI_CLASS_PROCESSOR 0x000b0000


#define PCI_IS_IO(address) (((address) & 1) && 1)
#define PCI_IO_ADDR(address) ((address) & ~0x3ULL)
#define PCI_MEM_ADDR(address) ((address) & ~0xfULL)
#define PCI_ROM_ADDR(address) ((address) & ~1ULL)
#define PCI_IS_PREFETCH(address) ((address) & 0x8)


#define PCI_COMMAND_IO_ENABLE 0x0001
#define PCI_COMMAND_MEM_ENABLE 0x0002
#define PCI_COMMAND_MASTER_ENABLE 0x0004
#define PCI_COMMAND_INTX_DISABLE 0x0400

#define PCI_STATUS_CAPABILITIES 0x0010
#define PCI_STATUS_66MHZ_OKAY 0x0020
#define PCI_STATUS_BACKTOBACK_OKAY 0x0080
#define PCI_STATUS_DEVSEL_MASK 0x0600
#define PCI_STATUS_DEVSEL_SHIFT 9

#define PCI_ISPACEEN 0x00000001 /* IO space enable */
#define PCI_MSPACEEN 0x00000002 /* Memory space enable */
#define PCI_MASTEREN 0x00000004 /* Master enable */

/* pci capability defines */

#define PCI_CAP_POWER_MANAGEMENT 0x01
#define PCI_CAP_MSI 0x05
#define PCI_CAP_PCIX 0x07
#define PCI_CAP_PCI_EXPRESS 0x10
#define PCI_CAP_MSI_X 0x11

/* pci_attach_device flags */

#define PCI_SHARE 0x00000001
#define PCI_INIT_IRQ 0x00010000
#define PCI_INIT_ROM 0x00020000
#define PCI_INIT_BASE0 0x00040000
#define PCI_INIT_BASE1 0x00080000
#define PCI_INIT_BASE2 0x00100000
#define PCI_INIT_BASE3 0x00200000
#define PCI_INIT_BASE4 0x00400000
#define PCI_INIT_BASE5 0x00800000
#define PCI_MASTER_ENABLE 0x01000000
#define PCI_SERVICE_ATTACH 0x04000000
#define PCI_USE_MSIX 0x40000000
#define PCI_INIT_ALL 0x00fd0000

#define PCI_MSI 1
#define PCI_MSIX 2

/* pci return codes */

#define PCI_SUCCESS 0x00
#define PCI_UNSUPPORTED_FUNCT 0x81
#define PCI_BAD_VENDOR_ID 0x83
#define PCI_DEVICE_NOT_FOUND 0x86
#define PCI_BAD_REGISTER_NUMBER 0x87
#define PCI_SET_FAILED 0x88
#define PCI_BUFFER_TOO_SMALL 0x89

__END_DECLS
#pragma pack(pop)
#define __PCI_H_INCLUDED
#endif
