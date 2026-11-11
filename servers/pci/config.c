/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"
#include <endian.h>
#include <fcntl.h>
#include <sys/slog.h>

/* Output messages to the system logger. Also to stderr if verbose is high. */

ssize_t pci_slogf(int opcode, int severity, const char *fmt, ...)
{
    int packet[_SLOG_MAXSIZE / sizeof(int)];
    char *text = (char *) &packet[_SLOG_HDRINTS];
    size_t avail = _SLOG_MAXSIZE - _SLOG_HDRINTS * sizeof(int);
    size_t nbytes;
    ssize_t ret;
    va_list arglist;
    int fd;
    int n;

    if (verbose > 4) {
        va_start(arglist, fmt);
        vfprintf(stderr, fmt, arglist);
        fprintf(stderr, "\n");
        va_end(arglist);
    }

    va_start(arglist, fmt);
    n = vsnprintf(text, avail, fmt, arglist);
    va_end(arglist);
    if (n < 0)
        return -1;

    if ((size_t) (n + 1) < avail)
        nbytes = (size_t) n + 1;
    else
        nbytes = avail;

    packet[0] = severity | _SLOG_TEXTBIT;
    packet[1] = opcode;
    packet[2] = 0;

    /* ipc-send: pci -> roottask-core,logger-ring */
    fd = open("/dev/slog", O_WRONLY);
    if (fd == -1)
        return -1;
    ret = write(fd, packet, _SLOG_HDRINTS * sizeof(int) + nbytes);
    close(fd);

    return ret;
}

/* Search the pci_device_t structures for a particular bus and devfn. When
 * found, cache this structure for future use.
 */

static int find_pciex_device(uint8_t bus, uint8_t devfn)
{
    DEBUGFUNC();
    if (!(last_device = search_busdev(bus, devfn)))
        return PCI_DEVICE_NOT_FOUND;
    clast_bus = bus;
    clast_devfn = devfn;
    return PCI_SUCCESS;
}

/* Read/write the extended PCI Express configuration space, if this is a PCI
 * Express device.
 */

static int pciex_check(uint8_t bus,
                       uint8_t devfn,
                       uint32_t offset,
                       uint32_t cnt)
{
    int status;

    DEBUGFUNC();
    if (!pciex_space)
        return PCI_UNSUPPORTED_FUNCT;
    if ((offset + (cnt * 4)) > 4096)
        return PCI_BAD_REGISTER_NUMBER;
    if (bus != clast_bus && devfn != clast_devfn) {
        if ((status = find_pciex_device(bus, devfn)) != PCI_SUCCESS)
            return status;
    }
    if ((last_device->PciType & PCI_TYPE_PCIEXPRESS) != PCI_TYPE_PCIEXPRESS)
        return PCI_BAD_REGISTER_NUMBER;
    return PCI_SUCCESS;
}

/* All calls to the low level PCI routines will read/write 32-bit quantities.
 * The following 6 routines will extract the relevant information from these
 * 32-bit fields.
 */

int PCI_read_config8(uint8_t bus,
                     uint8_t devfn,
                     uint32_t Offset,
                     uint32_t Cnt,
                     void *ConfigBuffer)
{
    int i, status = 0;
    uint32_t offset, cnt;
    uint8_t *p;
    uint8_t temp[256];

    DEBUGFUNC();
    if (Cnt > sizeof(temp))
        return PCI_BUFFER_TOO_SMALL;
    offset = Offset & ~3u;
    cnt = (Cnt + 3) >> 2;
    if (cnt > 64)
        cnt = 64;
    if (offset >= 0x100) {
        if ((status = pciex_check(bus, (uint8_t) devfn, offset, cnt)) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    for (i = 0, p = temp; i < (int) cnt;
         i++, p += sizeof(uint32_t), offset += sizeof(uint32_t)) {
        if ((status = (*pci_drivers->entry->read_cnfg)(
                 ll_handle, bus, devfn, offset, 4, p)) != PCI_SUCCESS) {
            if (verbose > 3)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: status 0x%x",
                          __FUNCTION__, status);
            return status;
        }
    }
    memcpy(ConfigBuffer, &temp[Offset % 4], Cnt);
    return status;
}

/* See PCI_read_config8 */

int PCI_read_config16(uint8_t bus,
                      uint8_t devfn,
                      uint32_t Offset,
                      uint32_t Cnt,
                      void *ConfigBuffer)
{
    int i, status = 0;
    uint32_t offset, cnt;
    uint16_t *p;
    uint8_t temp[256];
    uint32_t dword;

    DEBUGFUNC();
    if ((Cnt * 2) > sizeof(temp))
        return PCI_BUFFER_TOO_SMALL;
    if (Offset & 1) /* Can't have odd offsets */
        return PCI_BAD_REGISTER_NUMBER;
    offset = Offset & ~3u;
    cnt = (Cnt + 1) >> 1;
    if (cnt > 64)
        cnt = 64;
    if (offset >= 0x100) {
        if ((status = pciex_check(bus, (uint8_t) devfn, offset, cnt)) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    for (i = 0, p = (uint16_t *) temp; i < (int) cnt;
         i++, offset += sizeof(uint32_t)) {
        if ((status = (*pci_drivers->entry->read_cnfg)(
                 ll_handle, bus, devfn, offset, 4, &dword)) != PCI_SUCCESS) {
            if (verbose > 3)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: status 0x%x",
                          __FUNCTION__, status);
            return status;
        }
        dword = ENDIAN_LE32(dword);
        *p++ = (uint16_t) (dword & 0xffff);
        *p++ = (uint16_t) ((dword & 0xffff0000) >> 16);
    }
    memcpy(ConfigBuffer, &temp[Offset % 4], (size_t) Cnt * 2);
    return status;
}

/* See PCI_read_config8 */

int PCI_read_config32(uint8_t bus,
                      uint8_t devfn,
                      uint32_t Offset,
                      uint32_t Cnt,
                      void *ConfigBuffer)
{
    uint32_t i;
    int status = 0;
    uint32_t *p;

    DEBUGFUNC();
    if (Offset & 3) /* Can't have odd offsets */
        return PCI_BAD_REGISTER_NUMBER;
    if (Cnt > 64)
        Cnt = 64;
    if (Offset >= 0x100) {
        if ((status = pciex_check(bus, (uint8_t) devfn, Offset, Cnt)) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    for (i = 0, p = ConfigBuffer; i < Cnt;
         i++, p++, Offset += sizeof(uint32_t)) {
        if ((status = (*pci_drivers->entry->read_cnfg)(
                 ll_handle, bus, devfn, Offset, 4, (uint8_t *) p)) !=
            PCI_SUCCESS) {
            if (verbose > 3)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: status 0x%x",
                          __FUNCTION__, status);
            return status;
        }
        *p = ENDIAN_LE32(*p);
    }
    return status;
}

/* The PCI write routines are similar to the read functions, except that where a
 * value less than 32-bits is written, a read will first have to be performed
 * and the new value ored into this 32-bit value.
 */

int PCI_write_config8(uint8_t bus,
                      uint8_t devfn,
                      uint32_t Offset,
                      uint32_t Cnt,
                      void *ConfigBuffer)
{
    int i, status = 0;
    uint32_t offset, cnt;
    uint8_t *p;
    uint8_t temp[256];

    DEBUGFUNC();
    if (Cnt > sizeof(temp))
        return PCI_BUFFER_TOO_SMALL;
    offset = Offset & ~3u;
    cnt = (Cnt + 3) >> 2;
    if (cnt > 64)
        cnt = 64;
    if (offset >= 0x100) {
        if ((status = pciex_check(bus, (uint8_t) devfn, offset, cnt)) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    for (i = 0, p = temp; i < (int) cnt;
         i++, p += sizeof(uint32_t), offset += sizeof(uint32_t)) {
        if ((status = (*pci_drivers->entry->read_cnfg)(
                 ll_handle, bus, devfn, offset, 4, p)) != PCI_SUCCESS) {
            if (verbose > 3)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: status 0x%x",
                          __FUNCTION__, status);
            return status;
        }
    }
    memcpy(&temp[Offset % 4], ConfigBuffer, Cnt);
    offset = Offset & ~3u;
    for (i = 0, p = temp; i < (int) cnt;
         i++, p += sizeof(uint32_t), offset += sizeof(uint32_t)) {
        if ((status = (*pci_drivers->entry->write_cnfg)(
                 ll_handle, bus, devfn, offset, 4, (*(uint32_t *) p))) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    return status;
}

/* See PCI_write_config8 */

int PCI_write_config16(uint8_t bus,
                       uint8_t devfn,
                       uint32_t Offset,
                       uint32_t Cnt,
                       void *ConfigBuffer)
{
    int i, status = 0;
    uint32_t offset, cnt;
    uint16_t *p, *q;
    uint8_t temp[256];
    uint32_t dword;

    DEBUGFUNC();
    if ((Cnt * 2) > sizeof(temp))
        return PCI_BUFFER_TOO_SMALL;
    if (Offset & 1) /* Can't have odd offsets */
        return PCI_BAD_REGISTER_NUMBER;
    offset = Offset & ~3u;
    cnt = (Cnt + 1) >> 1;
    if (cnt > 64)
        cnt = 64;
    if (offset >= 0x100) {
        if ((status = pciex_check(bus, (uint8_t) devfn, offset, cnt)) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    for (i = 0, p = (uint16_t *) temp; i < (int) cnt;
         i++, offset += sizeof(uint32_t)) {
        if ((status = (*pci_drivers->entry->read_cnfg)(
                 ll_handle, bus, devfn, offset, 4, &dword)) != PCI_SUCCESS) {
            if (verbose > 3)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: status 0x%x",
                          __FUNCTION__, status);
            return status;
        }
        dword = ENDIAN_LE32(dword);
        *p++ = (uint16_t) (dword & 0xffff);
        *p++ = (uint16_t) ((dword & 0xffff0000) >> 16);
    }
    for (i = 0, p = (uint16_t *) &temp[Offset % 4],
        q = (uint16_t *) ConfigBuffer;
         i < (int) Cnt; i++)
        *p++ = *q++;
    offset = Offset & ~3u;
    for (i = 0, p = (uint16_t *) temp; i < (int) cnt;
         i++, offset += sizeof(uint32_t)) {
        dword = *p++;
        dword |= (uint32_t) *p++ << 16;
        dword = ENDIAN_LE32(dword);
        if ((status = (*pci_drivers->entry->write_cnfg)(
                 ll_handle, bus, devfn, offset, 4, dword)) != PCI_SUCCESS) {
            return status;
        }
    }
    return status;
}

/* See PCI_write_config8 */

int PCI_write_config32(uint8_t bus,
                       uint8_t devfn,
                       uint32_t Offset,
                       uint32_t Cnt,
                       void *ConfigBuffer)
{
    uint32_t i;
    int status = 0;
    const uint32_t *p;
    uint32_t dword;

    DEBUGFUNC();
    if (Offset & 3) /* Can't have odd offsets */
        return PCI_BAD_REGISTER_NUMBER;
    if (Offset >= 0x100) {
        if ((status = pciex_check(bus, (uint8_t) devfn, Offset, Cnt)) !=
            PCI_SUCCESS) {
            return status;
        }
    }
    for (i = 0, p = ConfigBuffer; i < Cnt;
         i++, p++, Offset += sizeof(uint32_t)) {
        dword = ENDIAN_LE32(*p);
        if ((status = (*pci_drivers->entry->write_cnfg)(
                 ll_handle, bus, devfn, Offset, 4, dword)) != PCI_SUCCESS) {
            if (verbose > 3)
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "%s: status 0x%x",
                          __FUNCTION__, status);
            return status;
        }
    }
    return status;
}
