/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * On platforms using the Synopsys DesignWare PCIe IP (e.g. SiFive FU740 /
 * Unmatched), the host's "config" memory window is not a true flat ECAM. Each
 * config-space access requires re-aiming iATU outbound region 0 to the desired
 * (bus, slot, func) before the read or write reaches the right device, because
 * the DW core encodes CFG TLPs in the iATU TARGET as
 * bus << 24 | slot << 19 | func << 16 | reg
 * which is *not* the same bit layout as standard ECAM
 * bus << 20 | slot << 15 | func << 12 | reg
 * so a single iATU mapping can't cover the whole bus space the way ECAM does.
 *
 * The iATU lives in the DBI register window, in either of two layouts:
 * - "viewport" — indexed at DBI + 0x900..0x91F (one viewport visible at a time,
 * selected by writing DW_IATU_VIEWPORT)
 * - "unrolled" — flat per-region block at DBI + 0x300000 + region * 0x200, one
 * block per outbound region
 *
 * Detection: read DBI+0x900 (DW_IATU_VIEWPORT). In unrolled mode the register
 * is unimplemented and reads 0xFFFFFFFF.
 *
 * We use only outbound region 0, re-aimed per config access.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

/* DBI iATU register offsets (viewport-indexed layout). */
#define DW_IATU_VIEWPORT 0x900
#define IATU_VIEWPORT_INDEX(x) ((x) & 0x7)
#define DW_IATU_CTRL1 0x904
#define DW_IATU_CTRL2 0x908
#define IATU_CTRL2_REGION_EN (1U << 31)
#define DW_IATU_LWR_BASE_ADDR 0x90C
#define DW_IATU_UPPER_BASE_ADDR 0x910
#define DW_IATU_LIMIT_ADDR 0x914
#define DW_IATU_LWR_TARGET_ADDR 0x918
#define DW_IATU_UPPER_TARGET_ADDR 0x91C

/* DBI iATU register offsets (unrolled per-region blocks). */
#define DW_IATU_UR_BASE 0x300000
#define DW_IATU_UR_STEP 0x200
#define IATU_UR_CTRL1 0x00
#define IATU_UR_CTRL2 0x04
#define IATU_UR_LWR_BASE_ADDR 0x08
#define IATU_UR_UPPER_BASE_ADDR 0x0C
#define IATU_UR_LIMIT_ADDR 0x10
#define IATU_UR_LWR_TARGET_ADDR 0x14
#define IATU_UR_UPPER_TARGET_ADDR 0x18

/* Map enough DBI to access either viewport iATU regs (within first 4 KiB) or
 * unrolled iATU regs (DBI + 0x300000 + per-region blocks). 4 MiB easily covers
 * both.
 */
#define DW_DBI_MMAP_SIZE (4UL << 20)
#define DW_IATU_VIEWPORT_END (DW_IATU_VIEWPORT + sizeof(uint32_t))
#define DW_IATU_UR_REGION0_END \
    (DW_IATU_UR_BASE + IATU_UR_UPPER_TARGET_ADDR + sizeof(uint32_t))

static volatile uint8_t *s_dbi_vaddr;
static size_t s_dbi_size;
static int s_unrolled;
static int s_inited;

static inline uint32_t atu_rd4(uint32_t off)
{
    return *(volatile uint32_t *) (s_dbi_vaddr + off);
}
static inline void atu_wr4(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *) (s_dbi_vaddr + off) = val;
}

int pci_dw_atu_init(uint64_t dbi_pa, size_t dbi_size)
{
    void *va;
    size_t map_size;

    if (s_inited)
        return 0;
    if (dbi_size < DW_IATU_VIEWPORT_END) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "DW iATU: DBI window too small: %#zx", dbi_size);
        return -1;
    }

    map_size = dbi_size > DW_DBI_MMAP_SIZE ? DW_DBI_MMAP_SIZE : dbi_size;
    va = mmap(NULL, map_size, PROT_READ | PROT_WRITE | PROT_NOCACHE,
              MAP_PHYS | MAP_SHARED, NOFD, (off_t) dbi_pa);
    if (va == MAP_FAILED) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "DW iATU: DBI mmap failed: %s",
                  strerror(errno));
        return -1;
    }
    s_dbi_vaddr = va;
    s_dbi_size = map_size;

    s_unrolled = (atu_rd4(DW_IATU_VIEWPORT) == 0xFFFFFFFFU);
    if (s_unrolled && s_dbi_size < DW_IATU_UR_REGION0_END) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "DW iATU: unrolled DBI window too small: %#zx", s_dbi_size);
        munmap(va, map_size);
        s_dbi_vaddr = NULL;
        s_dbi_size = 0;
        return -1;
    }
    pci_slogf(_SLOGC_PCI, _SLOG_INFO, "DW iATU: %s mode @ DBI %#lx",
              s_unrolled ? "unrolled" : "viewport", (unsigned long) dbi_pa);

    s_inited = 1;
    return 0;
}

int pci_dw_atu_aim_cfg(uint8_t bus,
                       uint8_t slot,
                       uint8_t func,
                       int type,
                       uint64_t cfg_pa,
                       size_t cfg_size)
{
    uint64_t target;
    int i;

    if (!s_inited)
        return -1;

    target = ((uint64_t) bus << 24) | ((uint64_t) (slot & 0x1f) << 19) |
             ((uint64_t) (func & 0x07) << 16);

    if (s_unrolled) {
        uint32_t base = DW_IATU_UR_BASE; /* region 0 */
        atu_wr4(base + IATU_UR_LWR_BASE_ADDR, (uint32_t) cfg_pa);
        atu_wr4(base + IATU_UR_UPPER_BASE_ADDR, (uint32_t) (cfg_pa >> 32));
        atu_wr4(base + IATU_UR_LIMIT_ADDR, (uint32_t) (cfg_pa + cfg_size - 1));
        atu_wr4(base + IATU_UR_LWR_TARGET_ADDR, (uint32_t) target);
        atu_wr4(base + IATU_UR_UPPER_TARGET_ADDR, (uint32_t) (target >> 32));
        atu_wr4(base + IATU_UR_CTRL1, (uint32_t) type);
        atu_wr4(base + IATU_UR_CTRL2, IATU_CTRL2_REGION_EN);
        for (i = 0; i < 100; i++) {
            if (atu_rd4(base + IATU_UR_CTRL2) & IATU_CTRL2_REGION_EN)
                return 0;
        }
    } else {
        atu_wr4(DW_IATU_VIEWPORT, IATU_VIEWPORT_INDEX(0));
        atu_wr4(DW_IATU_LWR_BASE_ADDR, (uint32_t) cfg_pa);
        atu_wr4(DW_IATU_UPPER_BASE_ADDR, (uint32_t) (cfg_pa >> 32));
        atu_wr4(DW_IATU_LIMIT_ADDR, (uint32_t) (cfg_pa + cfg_size - 1));
        atu_wr4(DW_IATU_LWR_TARGET_ADDR, (uint32_t) target);
        atu_wr4(DW_IATU_UPPER_TARGET_ADDR, (uint32_t) (target >> 32));
        atu_wr4(DW_IATU_CTRL1, (uint32_t) type);
        atu_wr4(DW_IATU_CTRL2, IATU_CTRL2_REGION_EN);
        for (i = 0; i < 100; i++) {
            if (atu_rd4(DW_IATU_CTRL2) & IATU_CTRL2_REGION_EN)
                return 0;
        }
    }
    return -1;
}

uint32_t pci_dw_dbi_read32(uint32_t off)
{
    if (!s_inited)
        return 0xFFFFFFFFU;
    return *(volatile uint32_t *) (s_dbi_vaddr + off);
}

void pci_dw_dbi_write32(uint32_t off, uint32_t val)
{
    if (!s_inited)
        return;
    *(volatile uint32_t *) (s_dbi_vaddr + off) = val;
}

int pci_dw_dbi_ready(void)
{
    return s_inited;
}
