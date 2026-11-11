/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Translates dev-nvme's PCI helpers into messages understood by RTMux's
 * /dev/pci server.
 *
 * Threading: a single fd to /dev/pci is shared by all API calls and guarded by
 * a mutex so msg_send/reply pairs do not interleave.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include <pci-serv.h>
#include <pci.h>
#include "pci.h"

/* ------------------------------------------------------------------ *
 * Connection state (singleton per process) *
 */

static int pci_fd = -1;
static pthread_mutex_t pci_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Lazily open /dev/pci on first use. Caller must hold pci_mtx. */
static pci_err_t ensure_connection(void)
{
    if (pci_fd >= 0)
        return PCI_ERR_OK;
    /* ipc-send: dev-nvme -> roottask-core,pci */
    pci_fd = open("/dev/pci", O_RDONLY);
    if (pci_fd < 0)
        return PCI_ERR_SERVER_NOT_AVAILABLE;
    return PCI_ERR_OK;
}

/* Send one message / receive one reply. Caller must hold pci_mtx. */
static int pci_send(int subtype, struct _pci_message *m)
{
    m->msg_hdr.i.type = _IO_MSG;
    m->msg_hdr.i.combine_len =
        sizeof(m->msg_hdr.i) + sizeof(m->rep_hdr) + sizeof(m->msg);
    m->msg_hdr.i.protocol = _IOMSG_PCI;
    m->msg_hdr.i.subtype = (uint16_t) subtype;
    /* ipc-send: dev-nvme -> roottask-core,pci */
    if (msg_send(pci_fd, m, sizeof(*m), m, sizeof(*m)) == -1)
        return -1;
    return (int) m->rep_hdr.reply_status;
}

/* Translate a PCI_SUCCESS / PCI_DEVICE_NOT_FOUND / ... return from the
 * pci-server into a pci_err_t.
 */
static pci_err_t srv_to_err(int rc)
{
    switch (rc) {
    case PCI_SUCCESS:
        return PCI_ERR_OK;
    case PCI_DEVICE_NOT_FOUND:
        return PCI_ERR_ENODEV;
    case PCI_UNSUPPORTED_FUNCT:
        return PCI_ERR_ENOTSUP;
    case PCI_BAD_REGISTER_NUMBER:
        return PCI_ERR_EINVAL;
    case PCI_SET_FAILED:
        return PCI_ERR_EIO;
    case PCI_BUFFER_TOO_SMALL:
        return PCI_ERR_ENOSPC;
    case PCI_BAD_VENDOR_ID:
        return PCI_ERR_ENODEV;
    default:
        return PCI_ERR_EIO;
    }
}

/* ------------------------------------------------------------------ *
 * Handle state — allocated for each successful attach *
 */

struct nvme_pci_dev {
    void *srv_handle;         /* returned by IOM_PCI_ATTACH_DEVICE */
    struct pci_dev_info info; /* BARs, IRQ, etc. */
    struct rtmux_resource_handle bar_handles[6];
};

/* ------------------------------------------------------------------ *
 * pci_device_find_class *
 */

pci_bdf_t pci_device_find_class(uint32_t idx, pci_ccode_t classcode)
{
    /* Delegate to the server's pre-enumerated device list via
     * IOM_PCI_FIND_CLASS. The server's search_class() handles the per-byte
     * wildcards in classcode (including PCI_CCODE_ANY, which is 0x00FFFFFF —
     * "every device regardless of class").
     *
     * Previously this routine walked the full 256 × 32 × 8 BDF space by
     * repeated cfg_rd32 calls; that made an idx-past-the-end call cost roughly
     * five seconds on QEMU virt (one msg_send per empty slot). It also tempted
     * "optimizations" that skip f>0 after a vendor=FFFF read at f==0 — a
     * real-world hazard on systems where a function is present despite an
     * unresponsive f==0 (seen in a prior hypervisor bring-up).
     */
    struct _pci_message m;
    int rc;

    pthread_mutex_lock(&pci_mtx);
    if (ensure_connection() != PCI_ERR_OK) {
        pthread_mutex_unlock(&pci_mtx);
        return PCI_BDF_NONE;
    }
    memset(&m, 0, sizeof(m));
    m.msg.class.class = (uint32_t) classcode;
    m.msg.class.index = idx;
    rc = pci_send(IOM_PCI_FIND_CLASS, &m);
    pthread_mutex_unlock(&pci_mtx);

    if (rc != PCI_SUCCESS)
        return PCI_BDF_NONE;

    uint32_t bus = m.msg.class.busnum;
    uint32_t df = m.msg.class.devfuncnum;
    return PCI_BDF(bus, df >> 3, df & 7);
}

/* ------------------------------------------------------------------ *
 * pci_device_attach / pci_device_detach *
 */

pci_devhdl_t pci_device_attach(pci_bdf_t bdf,
                               pci_attach_flags_t flags,
                               pci_err_t *err)
{
    struct _pci_message m;
    struct nvme_pci_dev *dev = NULL;
    pci_err_t local_err = PCI_ERR_OK;
    int rc;

    pthread_mutex_lock(&pci_mtx);
    if ((local_err = ensure_connection()) != PCI_ERR_OK)
        goto out;

    memset(&m, 0, sizeof(m));

    /* Map the public attach flags onto the server's exact-BDF attach request.
     * BASE0..BASE5 ask the server to allocate address space for every BAR.
     * PCI_MASTER_ENABLE sets the Bus Master bit in CMD.
     */
    uint32_t srv_flags = PCI_INIT_BASE0 | PCI_INIT_BASE1 | PCI_INIT_BASE2 |
                         PCI_INIT_BASE3 | PCI_INIT_BASE4 | PCI_INIT_BASE5 |
                         PCI_INIT_IRQ | PCI_MASTER_ENABLE;
    if (flags & PCI_ATTACH_OWNER)
        srv_flags |= PCI_SERVICE_ATTACH;
    if ((flags & PCI_ATTACH_EXCLUSIVE) == 0)
        srv_flags |= PCI_SHARE;

    m.msg.lock.flags = srv_flags;
    m.msg.lock.idx = 0;
    m.msg.lock.configbuffer.BusNumber = (uint8_t) PCI_BUS(bdf);
    m.msg.lock.configbuffer.DevFunc =
        (uint8_t) ((PCI_DEV(bdf) << 3) | PCI_FUNC(bdf));

    rc = pci_send(IOM_PCI_ATTACH_DEVICE, &m);
    if (rc != PCI_SUCCESS) {
        local_err = srv_to_err(rc);
        goto out;
    }

    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        local_err = PCI_ERR_ENOMEM;
        /* Try to release the server-side reservation. */
        struct _pci_message dm;
        memset(&dm, 0, sizeof(dm));
        dm.msg.unlock.handle = m.msg.lock.handle;
        pci_send(IOM_PCI_DETACH_DEVICE, &dm);
        goto out;
    }
    dev->srv_handle = m.msg.lock.handle;
    dev->info = m.msg.lock.configbuffer;
    memcpy(dev->bar_handles, m.msg.lock.bar_handles, sizeof(dev->bar_handles));

out:
    pthread_mutex_unlock(&pci_mtx);
    if (err)
        *err = local_err;
    return dev;
}

pci_err_t pci_device_detach(pci_devhdl_t hdl)
{
    struct nvme_pci_dev *dev = hdl;
    struct _pci_message m;
    int rc;

    if (!dev)
        return PCI_ERR_EINVAL;

    pthread_mutex_lock(&pci_mtx);
    if (pci_fd < 0) {
        pthread_mutex_unlock(&pci_mtx);
        free(dev);
        return PCI_ERR_SERVER_NOT_AVAILABLE;
    }
    memset(&m, 0, sizeof(m));
    m.msg.unlock.handle = dev->srv_handle;
    rc = pci_send(IOM_PCI_DETACH_DEVICE, &m);
    pthread_mutex_unlock(&pci_mtx);

    free(dev);
    return rc == PCI_SUCCESS ? PCI_ERR_OK : srv_to_err(rc);
}

/* ------------------------------------------------------------------ *
 * Per-attached-device info: IRQ and base addresses *
 */

pci_err_t pci_device_read_irq(pci_devhdl_t hdl, int *nirq, pci_irq_t *irq)
{
    struct nvme_pci_dev *dev = hdl;

    if (!dev || !nirq)
        return PCI_ERR_EINVAL;

    if (!irq) {
        *nirq = 1;
        return PCI_ERR_OK;
    }
    if (*nirq < 1)
        return PCI_ERR_ENOSPC;

    irq[0] = (pci_irq_t) dev->info.Irq;
    *nirq = 1;
    return PCI_ERR_OK;
}

pci_err_t pci_device_read_ba(pci_devhdl_t hdl,
                             int *nba,
                             pci_ba_t *ba,
                             pci_req_type_t reqType)
{
    struct nvme_pci_dev *dev = hdl;
    int out = 0, max;

    (void) reqType;

    if (!dev || !nba)
        return PCI_ERR_EINVAL;

    /* Count populated BARs first. */
    int populated = 0;
    for (int i = 0; i < 6; i++)
        if (dev->info.BaseAddressSize[i])
            populated++;

    if (!ba) {
        *nba = populated;
        return PCI_ERR_OK;
    }

    max = *nba;
    for (int i = 0; i < 6 && out < max; i++) {
        uint32_t sz = dev->info.BaseAddressSize[i];
        if (!sz)
            continue;

        ba[out].addr = dev->info.CpuBaseAddress[i] & ~(uint64_t) 0xF;
        ba[out].size = sz;
        ba[out].bar_num = i;
        ba[out].handle = dev->bar_handles[i];

        /* BaseAddressSize in pci_dev_info doesn't carry type — we infer from
         * the returned CpuBaseAddress' low bits (bit 0 set means I/O) and from
         * the PciBaseAddress alignment class. That matches the server's
         * classification.
         */
        if (dev->info.CpuBaseAddress[i] & 0x1) {
            ba[out].type = PCI_BAR_IO;
            ba[out].attr = PCI_BAR_32BIT;
        } else {
            ba[out].type = PCI_BAR_MEM;
            ba[out].attr = PCI_BAR_64BIT;
            if (dev->info.CpuBaseAddress[i] & 0x8)
                ba[out].attr |= PCI_BAR_PREFETCH;
        }
        out++;
    }
    *nba = out;
    return PCI_ERR_OK;
}
