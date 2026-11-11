/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * entry
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"

#define PCI_MODE (S_IFCHR | 0600)

static time_t pci_created;
static ioserv_ops_t io_funcs;
static const char *progname = "pci_server:";

/* Initialize all variables and scan the PCI bus(ses) for devices. */

static int pci_startup(void)
{
    int i, status;
    pci_device_t *device;
    pci_bus_t *bus;
    bus_map_t *map, *peer;
    uint64_t io_size, mem_size, pref_size;
    uint64_t tot_mem_size, tot_mem_reqd;
    DEBUGFUNC();
    lastbus = endbus = cardbus = 0;
    clast_bus = clast_devfn = 0xff;
    DeviceHead = DeviceTail = NULL;
    io_size = mem_size = pref_size = 0;

    if (find_ranges()) {
        return -1;
    }

    if (scan_all() != PCI_SUCCESS || lastbus == 0) { /* No PCI found! */
        return -1;
    }

    /* Scan through all the devices and configure the bridges */

    for (i = 1; i < lastbus; i++) {
        if (!(map = find_bus((uint8_t) i)))
            continue;
        if (map->io_reqd) {
            if (map->io_reqd < KB4) {
                io_size += KB4;
                map->io_reqd = KB4;
            } else {
                if (map->io_reqd % KB4) {
                    map->io_reqd += KB4;
                    map->io_reqd &= ~((uint64_t) KB4 - 1);
                }
            }
            io_size += map->io_reqd;
        }
        if (map->mem_reqd) {
            if (map->mem_reqd < MEG1) {
                mem_size += MEG1;
                map->mem_reqd = MEG1;
            } else {
                if (map->mem_reqd % MEG1) {
                    map->mem_reqd += MEG1;
                    map->mem_reqd &= ~((uint64_t) MEG1 - 1);
                }
            }
            mem_size += map->mem_reqd;
        }
        if (map->pref_reqd) {
            if (map->pref_reqd < MEG1) {
                pref_size += MEG1;
                map->pref_reqd = MEG1;
            } else {
                if (map->pref_reqd % MEG1) {
                    map->pref_reqd += MEG1;
                    map->pref_reqd &= ~((uint64_t) MEG1 - 1);
                }
            }
            pref_size += map->pref_reqd;
        }
    }
    if (verbose > 2)
        pci_slogf(_SLOGC_PCI, _SLOG_DEBUG1,
                  "I/O reqd 0x%llx - Mem reqd 0x%llx - Pref reqd 0x%llx",
                  io_size, mem_size, pref_size);

    tot_mem_size = mem_cpu_high - mem_cpu_low;
    tot_mem_reqd = mem_size + pref_size;
    if (tot_mem_reqd > tot_mem_size) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "Memory required (0x%llx) is > memory size (0x%llx)",
                  tot_mem_reqd, tot_mem_size);
    }

    if (io_size) {
        if (setup_pci_io(io_size))
            return -1;
    }

    if (mem_size) {
        if (setup_pci_mem(mem_size))
            return -1;
    }

    if (pref_size) {
        if (setup_pci_pref(pref_size))
            return -1;
    }

    device = DeviceHead;
    while (device) {
        if (((device->ClassCode & 0xff0000) == PCI_CLASS_BRIDGE) &&
            (device->DevFuncNumber & 7) == 0) {
            if (!(map = find_bus(device->bmap->secondary_bus)))
                return -1;
            bus = map->pci_bus;
            if (verbose > 2) {
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "Busmap Pri 0x%x - Sec 0x%x - Sub 0x%x",
                          map->primary_bus, map->secondary_bus,
                          map->subordinate_bus);
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "I/O Start 0x%llx - I/O End 0x%llx", map->io_start,
                          map->io_end);
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "Mem Start 0x%llx - Mem End 0x%llx", map->mem_start,
                          map->mem_end);
                pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                          "I/O 0x%llx - Mem 0x%llx - Pref 0x%llx", map->io_reqd,
                          map->mem_reqd, map->pref_reqd);
            }
            if ((status = (*pci_drivers->entry->cnfg_bridge)(
                     ll_handle, device->BusNumber, device->DevFuncNumber,
                     bus)) != PCI_SUCCESS) {
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                          "%s: Unable to configure bridge", __FUNCTION__);
                return status;
            }
        }
        device = device->next;
    }

    if (verbose) {
        map = bus_map[0];
        while (map) {
            pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                      "Map %p - Next %p - Prev %p - Peer %p", map, map->next,
                      map->prev, map->peer);
            pci_slogf(_SLOGC_PCI, _SLOG_INFO, "Prim 0x%x - Sec 0x%x - Sub 0x%x",
                      map->primary_bus, map->secondary_bus,
                      map->subordinate_bus);
            if (map->peer) {
                peer = map->peer;
                while (peer) {
                    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                              "Peer %p - Next %p - Prev %p - Peer %p", peer,
                              peer->next, peer->prev, peer->peer);
                    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
                              "Prim 0x%x - Sec 0x%x - Sub 0x%x",
                              peer->primary_bus, peer->secondary_bus,
                              peer->subordinate_bus);
                    peer = peer->peer;
                }
            }
            map = map->next;
        }
    }

    return PCI_SUCCESS;
}

/* Allocate an ocb for this open, attach a device list buffer to it. */

static int PCI_Open(ioserv_context_t *ctp,
                    io_open_t *msg,
                    void *handle,
                    void *extra)
{
    pci_dev_list_t *ocb;
    unsigned want;
    struct _client_info info;

    DEBUGFUNC();
    if (verbose)
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "io_open for id = 0x%x", ctp->rcvid);

    (void) handle;
    (void) extra;
    if (msg->connect.subtype != _IO_CONNECT_OPEN &&
        msg->connect.subtype != _IO_CONNECT_COMBINE &&
        msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)
        return ENOSYS;
    if (msg->connect.file_type != _FTYPE_ANY)
        return ENOSYS;
    want = msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR);
    if (want != 0) {
        if (connect_client_info_r(ctp->info.scoid, &info, 0) != EOK)
            return errno;
        if (info.cred.euid != 0)
            return EACCES;
    }
    if (!(ocb = calloc(1, sizeof(*ocb)))) {
        if (verbose) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: couldn't allocate %d bytes",
                      __FUNCTION__, sizeof(*ocb));
        }
        return ENFILE;
    }

    ocb->ioflag =
        msg->connect.ioflag & (~(unsigned) _IO_FLAG_MASK | msg->connect.access);
    if (ioserv_open_bind(ctp, ocb, &io_funcs) == -1) {
        int rc = errno;

        free(ocb);
        return rc;
    }
    return EOK;
}

/* Free the ocb allocated above as well as the device list buffer. */

static int PCI_CloseOcb(ioserv_context_t *ctp,
                        void *reserved,
                        pci_dev_list_t *list)
{
    int i;

    DEBUGFUNC();
    (void) ctp;
    (void) reserved;
    if (verbose) {
        pci_slogf(_SLOGC_PCI, _SLOG_INFO, "io_close_ocb");
    }

    for (i = 0; i < NUM_DEVICES; i++) {
        if (list->dev_list[i]) {
            PCI_detach_device(list->dev_list[i], list, 1);
        }
    }

    free(list);
    return EOK;
}

static int PCI_Stat(ioserv_context_t *ctp, io_stat_t *msg, pci_dev_list_t *ocb)
{
    (void) ocb;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_ino = 1;
    msg->o.st_mode = PCI_MODE;
    msg->o.st_nlink = 1;
    msg->o.st_uid = 0;
    msg->o.st_gid = 0;
    msg->o.st_atime = msg->o.st_mtime = msg->o.st_ctime = pci_created;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static void fill_bar_handles(struct rtmux_resource_handle handles[6],
                             pci_device_t *dev,
                             pid_t owner_pid)
{
    memset(handles, 0, sizeof(struct rtmux_resource_handle) * 6);
    if (!dev)
        return;

    for (unsigned i = 0; i < 6; i++) {
        struct BaseRegister *bar = &dev->BaseReg[i];

        if (!bar->Size)
            continue;
        if ((bar->Flags & (PCI_MEM_SPACE | PCI_MEM_PREFETCH)) == 0)
            continue;

        handles[i] = (struct rtmux_resource_handle) {
            .kind = RTMUX_RESOURCE_PCI_BAR,
            .base = PCI_MEM_ADDR(bar->Address),
            .length = bar->Size,
            .owner_pid = owner_pid,
            .device_id = dev->ClassCode,
            .generation = (uint64_t) ((uintptr_t) dev) ^ (uint64_t) (i + 1U),
            .irq_vector = (int32_t) dev->IrqNumber,
            .reserved = i,
        };
    }
}

/* Handle user I/O messages. */

static int pci_io_msg(ioserv_context_t *ctp,
                      io_msg_t *msg,
                      pci_dev_list_t *ocb,
                      void *attr)
{
    const size_t data_off = sizeof(msg->i) + sizeof(struct _pci_reply_hdr);
    union _pci_data *data = (union _pci_data *) ((char *) (&msg->i) + data_off);
    uint8_t bus = 0, devfn = 0;
    int ret, reply_length = 0;
    int local_errno;
    size_t copy_len;
    struct _pci_message msg_reply;
    pid_t client_pid;

    if (msg->i.protocol != _IOMSG_PCI)
        return ENOSYS;

    (void) msg_info(ctp->rcvid, &ctp->info);
    client_pid = ctp->info.pid;

    local_errno = EOK;

    /* combine_len is a client-supplied uint16 claimed length. The incoming
     * message is snapshotted into msg_reply because the protocol echoes the
     * request body fields back in the reply, but copying a forged combine_len
     * larger than msg_reply would overflow it on the stack and over-read the
     * receive buffer. Zero the reply first so a short message cannot echo stale
     * stack bytes, then cap the copy to the destination size. A well-formed
     * message never exceeds msg_max_size (sizeof msg_reply), so this only trims
     * forged input.
     */
    memset(&msg_reply, 0, sizeof(msg_reply));
    copy_len = msg->i.combine_len;
    if (copy_len > sizeof(msg_reply))
        copy_len = sizeof(msg_reply);
    memcpy(&msg_reply, &msg->i, copy_len);

    /* if (verbose > 2)
     *      pci_slogf (_SLOGC_PCI, _SLOG_INFO, "Message type %d - id = 0x%x",
     * msg->i.subtype, ctp->rcvid);
     */
    switch (msg->i.subtype) {
    case IOM_PCI_FIND_CLASS:
        /* Require the request fields this subtype reads (class, index) to have
         * been received, so a truncated message cannot feed the lookup stale
         * bytes left in the shared receive buffer by an earlier message. The
         * bound is the read extent, not sizeof the struct, whose tail holds
         * reply-only fields a client never sends.
         */
        if (ctp->info.msglen < data_off + offsetof(struct _pci_class, busnum))
            return EMSGSIZE;
        ret =
            PCI_find_class(data->class.class, data->class.index, &bus, &devfn);
        msg_reply.msg.class.busnum = bus;
        msg_reply.msg.class.devfuncnum = devfn;
        reply_length += (int) sizeof(struct _pci_class);
        break;
    case IOM_PCI_ATTACH_DEVICE:
        if (ctp->info.msglen <
            data_off + offsetof(struct _pci_attach, idx) + sizeof(uint16_t))
            return EMSGSIZE;
        msg_reply.msg.lock.handle = PCI_attach_device(
            data->lock.handle, data->lock.flags, data->lock.idx,
            &msg_reply.msg.lock.configbuffer, ocb, client_pid, &local_errno);
        fill_bar_handles(msg_reply.msg.lock.bar_handles,
                         (pci_device_t *) msg_reply.msg.lock.handle,
                         client_pid);
        reply_length +=
            msg_reply.msg.lock.handle ? (int) sizeof(struct _pci_attach) : 0;
        ret = reply_length ? PCI_SUCCESS : PCI_DEVICE_NOT_FOUND;
        break;
    case IOM_PCI_DETACH_DEVICE:
        if (ctp->info.msglen < data_off + sizeof(struct _pci_detach))
            return EMSGSIZE;
        ret = PCI_detach_device(data->unlock.handle, ocb, 0);
        break;
    default:
        local_errno = ENOSYS;
        ret = PCI_SUCCESS;
        break;
    }
    if (local_errno != EOK)
        return local_errno;
    IOSERV_STATUS(ctp, local_errno);
    msg_reply.rep_hdr.reply_status = (uint32_t) ret;
    reply_length += (int) sizeof(struct _pci_reply_hdr);
    msg_reply.rep_hdr.reply_length = (uint32_t) reply_length;
    msg_reply.msg_hdr.i.combine_len =
        (uint16_t) (reply_length + (int) sizeof(io_msg_t));
    SETIOV(ctp->iov, &msg_reply, (size_t) reply_length + sizeof(io_msg_t));
    msg_replyv(ctp->rcvid, ret, ctp->iov, 1);
    return IOSERV_NOREPLY;
}

static void pci_signal_handler(int signo)
{
    pci_driver_t *pdrvr;

    if (signo == SIGTERM) {
        /* for each driver call detach */
        pdrvr = pci_drivers;
        for (; pdrvr; pdrvr = pdrvr->next) {
            if (pdrvr->entry->detach) {
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s shutting down pci %p",
                          progname, pdrvr->hdl);
                pdrvr->entry->detach(pdrvr->hdl);
            }
        }
        exit(0);
    }
}

static void pci_ioserv_loop(ioserv_context_t *ctp)
{
    while (1) {
        if (!ioserv_block(ctp)) {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s: block error", __FUNCTION__);
            exit(EXIT_FAILURE);
        }
        ioserv_handler(ctp);
    }
}

int main(int argc, char **argv)
{
    int c;
    ioserv_connect_ops_t connect_funcs;
    dispatch_t *dpp;
    ioserv_context_t *ctp;

    pci_drivers = NULL;
    scan_buses = MAX_BUSES;

    /* Enable IO capability. */
    if (thread_ctl(RTMUX_TCTL_IO, NULL) == -1) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s thread_ctl", progname);
        return EPERM;
    }

    dpp = dispatch_create();
    if (!dpp) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "%s: Unable to allocate dispatch context.", argv[0]);
        exit(EXIT_FAILURE);
    }

    ioserv_attr_t ioserv_attr = {
        .nparts_max = 20,
        .msg_max_size = sizeof(struct _pci_message),
    };

    /* process args */

    while (optind < argc) {
        if ((c = getopt(argc, argv, "b:c:DmMn:pP:vx")) == -1) {
            optind++;
            continue;
        }

        switch (c) {
        case 'b':
            scan_buses = atoi(optarg);
            break;
        case 'c':
            class_override = atoi(optarg);
            if ((class_override & 3) == 0) {
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                          "-c value must be 1, 2 or 3");
                exit(EXIT_FAILURE);
            }
            break;

        case 'D':
            enable_crt_msi = 1;
            pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "%s enabling MSI for video",
                      progname);
            break;

        case 'm':
            map_irqs ^= 1; /* Flip the flag */
            break;

        case 'M':
            use_msi = 0;
            pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "%s MSI is turned off",
                      progname);
            break;

        case 'n':
            max_msi = atoi(optarg);
            switch (max_msi) {
            case 1:
            case 2:
            case 4:
            case 8:
            case 16:
            case 32:
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "max msi set to %d", max_msi);
                break;
            default:
                pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                          "Incorrect value for max_msi %d", max_msi);
                return -1;
            }

        case 'p':
            program_display_device = 1;
            break;

        case 'P':
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                      "%s -P raw PCIEX mapping is unsupported", progname);
            return -1;

        case 'v':
            verbose++;
            break;

        case 'x':
            dont_touch = 1;
            break;

        default:
            pci_slogf(_SLOGC_PCI, _SLOG_WARNING, "%s unknown option", progname);
            break;
        }
    }

    /* Attach the ECAM driver directly (no DLL loading) */
    {
        pci_driver_t *pdrvr = calloc(1, sizeof(pci_driver_t));
        if (pdrvr && !ecam_entry.attach(NULL, &pdrvr->hdl)) {
            ll_handle = pdrvr->hdl;
            pci_drivers = pdrvr;
            pdrvr->entry = &ecam_entry;
        } else {
            pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s ECAM attach failed!",
                      progname);
            exit(EXIT_FAILURE);
        }
    }

    pci_created = time(NULL);
    memset(&connect_funcs, 0, sizeof(connect_funcs));
    memset(&io_funcs, 0, sizeof(io_funcs));
    connect_funcs.nfuncs = IOSERV_CONNECT_NFUNCS;
    io_funcs.nfuncs = IOSERV_IO_NFUNCS;
    connect_funcs.open = PCI_Open;
    io_funcs.close_ocb = PCI_CloseOcb;
    io_funcs.stat = PCI_Stat;
    io_funcs.msg = (void *) &pci_io_msg;

    /* Initialize PCI configuration access. */
    if (pci_startup() != PCI_SUCCESS) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "%s pci_startup failed!", progname);
        exit(EXIT_FAILURE);
    }

    if (ioserv_attach(dpp, &ioserv_attr, "/dev/pci", _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, NULL) == -1) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "ioserv_attach failed");
        exit(EXIT_FAILURE);
    }

    if (!(ctp = ioserv_context_alloc(dpp))) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "Could not allocate I/O service context");
        exit(EXIT_FAILURE);
    }
    rtmux_detach(EXIT_SUCCESS);

    signal(SIGTERM, pci_signal_handler);
    pci_ioserv_loop(ctp);

    exit(EXIT_SUCCESS);
}
