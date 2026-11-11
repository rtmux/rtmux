/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _pci_serv_h_
#define _pci_serv_h_

#ifndef __PCI_H_INCLUDED
#include <pci.h>
#endif

#include <sys/iomsg.h>
#include <sys/virtio-mmio-msg.h>

typedef enum {
    IOM_PCI_FIND_CLASS,
    IOM_PCI_ATTACH_DEVICE,
    IOM_PCI_DETACH_DEVICE
} PCI_FUNCS;

struct _pci_class {
    uint32_t class;
    uint32_t index;
    uint32_t busnum;
    uint32_t devfuncnum;
};

struct _pci_attach {
    uint32_t flags;
    void *handle;
    uint16_t idx;
    uint16_t filler;
    struct pci_dev_info configbuffer;
    struct rtmux_resource_handle bar_handles[6];
};

struct _pci_detach {
    void *handle;
};

struct _pci_reply_hdr {
    uint32_t reply_status;
    uint32_t reply_length;
};

struct _pci_message {
    io_msg_t msg_hdr;
    struct _pci_reply_hdr rep_hdr;
    union {
        struct _pci_class class;
        struct _pci_attach lock;
        struct _pci_detach unlock;
    } msg;
};

#endif
