/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Local helpers for the NVMe driver. This is not a public API surface; add
 * declarations only when dev-nvme needs them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DEV_NVME_PCI_H_
#define DEV_NVME_PCI_H_

#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/virtio-mmio-msg.h>

/* ------------------------------------------------------------------ *
 * Types and macros *
 */

/* pci_bdf_t — uint32_t encoding Bus/Device/Function:
 *   xxxx xxxx xxxx xxxa bbbb bbbb dddd dfff
 * The ARI bit at position 16 is recognized but ARI itself isn't used by any
 * RTMux hardware yet.
 */
typedef uint32_t pci_bdf_t;

#define PCI_BDF(_b_, _d_, _f_)                                      \
    ((pci_bdf_t) ((((_b_) & 0xFFu) << 8) | (((_d_) & 0x1Fu) << 3) | \
                  ((_f_) & 0x7u)))

#define _PCI_ARI_BIT 16
#define PCI_IS_ARI(_bdf_) (((_bdf_) & ((pci_bdf_t) 1 << _PCI_ARI_BIT)) != 0)
#define _PCI_BDF_FUNC(_bdf_) ((((pci_bdf_t) (_bdf_)) >> 0) & 0x7u)
#define _PCI_BDF_DEV(_bdf_) ((((pci_bdf_t) (_bdf_)) >> 3) & 0x1Fu)
#define _PCI_BDF_BUS(_bdf_) ((((pci_bdf_t) (_bdf_)) >> 8) & 0xFFu)
#define _PCI_ARI_FUNC(_bdf_) ((((pci_bdf_t) (_bdf_)) >> 0) & 0xFFu)
#define _PCI_ARI_DEV(_bdf_) ((pci_bdf_t) 0)
#define _PCI_ARI_BUS(_bdf_) ((((pci_bdf_t) (_bdf_)) >> 8) & 0xFFu)

#define PCI_BUS(_bdf_) \
    (PCI_IS_ARI(_bdf_) ? _PCI_ARI_BUS(_bdf_) : _PCI_BDF_BUS(_bdf_))
#define PCI_DEV(_bdf_) \
    (PCI_IS_ARI(_bdf_) ? _PCI_ARI_DEV(_bdf_) : _PCI_BDF_DEV(_bdf_))
#define PCI_FUNC(_bdf_) \
    (PCI_IS_ARI(_bdf_) ? _PCI_ARI_FUNC(_bdf_) : _PCI_BDF_FUNC(_bdf_))

#define PCI_BDF_NONE ((pci_bdf_t) - 1)

/* Configuration-space register types. */
typedef uint32_t pci_ccode_t; /* 24-bit: class | subclass | prog-if */

#define PCI_CCODE(_C_, _c_, _r_)                                       \
    ((pci_ccode_t) ((((_C_) & 0xFFu) << 16) | (((_c_) & 0xFFu) << 8) | \
                    ((_r_) & 0xFFu)))

#define PCI_CCODE_SUBCLASS_ANY 0xFFu
#define PCI_CCODE_REG_IF_ANY 0xFFu
#define PCI_CCODE_ANY \
    PCI_CCODE(0xFFu, PCI_CCODE_SUBCLASS_ANY, PCI_CCODE_REG_IF_ANY)

/* Handle returned from pci_device_attach(). */
typedef void *pci_devhdl_t;

/* Attach flags used by the in-tree NVMe driver. */
typedef enum {
    PCI_ATTACH_EXCLUSIVE = 1,
    PCI_ATTACH_OWNER = (1u << 1),
    PCI_ATTACH_EXCLUSIVE_OWNER = (PCI_ATTACH_EXCLUSIVE | PCI_ATTACH_OWNER),
} pci_attach_flags_t;

/* Error codes — the low range mirrors errno values exactly, the high range
 * holds PCI-specific conditions.
 */
typedef enum {
    PCI_ERR_OK = 0,
    PCI_ERR_EINVAL = EINVAL,
    PCI_ERR_ENODEV = ENODEV,
    PCI_ERR_EIO = EIO,
    PCI_ERR_ENOTSUP = ENOTSUP,
    PCI_ERR_ENOMEM = ENOMEM,
    PCI_ERR_ENOSPC = ENOSPC,
    PCI_ERR_SERVER_NOT_AVAILABLE = 400,
} pci_err_t;

/* Request type — for calls that can be advisory vs mandatory. For
 * pci_device_read_ba() we typically pass MANDATORY.
 */
typedef enum {
    PCI_REQ_MANDATORY = 100,
    PCI_REQ_ADVISORY,
    PCI_REQ_UNSPECIFIED,
} pci_req_type_t;

/* Base address space type + attributes, and the pci_ba_t returned from
 * pci_device_read_ba().
 */
typedef uint64_t pci_ba_val_t;

typedef enum {
    PCI_BAR_NONE = 0,
    PCI_BAR_MEM,
    PCI_BAR_IO,
} pci_bar_type_t;

typedef enum {
    PCI_BAR_32BIT = (1u << 14),
    PCI_BAR_64BIT = (2u << 14),
    PCI_BAR_PREFETCH = (1u << 16),
} pci_bar_attr_t;

typedef struct {
    pci_ba_val_t addr;
    uint64_t size;
    pci_bar_type_t type;
    pci_bar_attr_t attr;
    int bar_num;
    struct rtmux_resource_handle handle;
} pci_ba_t;

/* IRQ returned from pci_device_read_irq(): negative values mean invalid. */
typedef int pci_irq_t;

/* ------------------------------------------------------------------ *
 * API *
 */

/* Miscellaneous */
const char *pci_strerror(pci_err_t err);

/* Device attach / detach / find */
pci_devhdl_t pci_device_attach(pci_bdf_t bdf,
                               pci_attach_flags_t flags,
                               pci_err_t *err);
pci_err_t pci_device_detach(pci_devhdl_t hdl);
pci_bdf_t pci_device_find_class(uint32_t idx, pci_ccode_t classcode);

/* Per-attached-device info */
pci_err_t pci_device_read_irq(pci_devhdl_t hdl, int *nirq, pci_irq_t *irq);
pci_err_t pci_device_read_ba(pci_devhdl_t hdl,
                             int *nba,
                             pci_ba_t *ba,
                             pci_req_type_t reqType);

#endif /* DEV_NVME_PCI_H_ */
