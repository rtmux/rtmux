/*
 * RISC-V IOMMU register constants used by RTMux probes and LKL handoff checks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_RISCV_IOMMU_H
#define _SYS_RISCV_IOMMU_H

#include <stdint.h>

#define RISCV_IOMMU_CAPABILITIES 0x00U
#define RISCV_IOMMU_DDTP         0x10U
#define RISCV_IOMMU_CQB          0x18U
#define RISCV_IOMMU_CQH          0x20U
#define RISCV_IOMMU_CQT          0x24U
#define RISCV_IOMMU_CQCSR        0x48U
#define RISCV_IOMMU_TR_REQ_IOVA  0x258U
#define RISCV_IOMMU_TR_REQ_CTL   0x260U
#define RISCV_IOMMU_TR_RESPONSE  0x268U

#define RISCV_IOMMU_VERSION_1_0  0x10U
#define RISCV_IOMMU_CAP_DBG      (1ULL << 31)
#define RISCV_IOMMU_CAP_SV39X4   (1ULL << 17)
#define RISCV_IOMMU_MODE_MASK    0xfU

#define RISCV_IOMMU_CQCSR_CQEN (1U << 0)
#define RISCV_IOMMU_CQCSR_CQON (1U << 16)
#define RISCV_IOMMU_CQCSR_BUSY (1U << 17)
#define RISCV_IOMMU_CQCSR_ERR  ((1U << 8) | (1U << 9) | (1U << 10))

#define RISCV_IOMMU_CMD_IOTINVAL 1U
#define RISCV_IOMMU_CMD_IOFENCE  2U
#define RISCV_IOMMU_CMD_IODIR    3U
#define RISCV_IOMMU_CMD_FUNC_SHIFT 7U
#define RISCV_IOMMU_CMD_IOTINVAL_FUNC_GVMA 1U
#define RISCV_IOMMU_CMD_IOTINVAL_AV (1ULL << 10)
#define RISCV_IOMMU_CMD_IOTINVAL_GV (1ULL << 33)
#define RISCV_IOMMU_CMD_IODIR_DV (1ULL << 33)
#define RISCV_IOMMU_DID_SHIFT    40U

#define RISCV_IOMMU_DDTP_BUSY    (1ULL << 4)
#define RISCV_IOMMU_DDTP_PPN_MASK (((1ULL << 54) - 1ULL) & ~0x3ffULL)
#define RISCV_IOMMU_DDTP_MODE_OFF 0U
#define RISCV_IOMMU_DDTP_MODE_BARE 1U
#define RISCV_IOMMU_DDTP_MODE_1LVL 2U
#define RISCV_IOMMU_DDTP_MODE_2LVL 3U
#define RISCV_IOMMU_DDTP_MODE_3LVL 4U

#define RISCV_IOMMU_DC_TC_V      1ULL
#define RISCV_IOMMU_IOHGATP_MODE_SV39X4 8ULL

#define RISCV_IOMMU_PTE_V        1ULL
#define RISCV_IOMMU_PTE_R        (1ULL << 1)
#define RISCV_IOMMU_PTE_W        (1ULL << 2)
#define RISCV_IOMMU_PTE_U        (1ULL << 4)
#define RISCV_IOMMU_PTE_A        (1ULL << 6)
#define RISCV_IOMMU_PTE_D        (1ULL << 7)

#define RISCV_IOMMU_TR_REQ_GO    1ULL
#define RISCV_IOMMU_TR_REQ_NW    (1ULL << 3)
#define RISCV_IOMMU_TR_RESPONSE_FAULT 1ULL
#define RISCV_IOMMU_TR_RESPONSE_PPN_MASK \
    RISCV_IOMMU_DDTP_PPN_MASK

#define RISCV_IOMMU_CQ_BYTES   4096U
#define RISCV_IOMMU_DDT_BYTES  4096U
#define RISCV_IOMMU_ROOT_BYTES (4U * 4096U)

struct riscv_iommu_cmd {
    uint64_t dword0;
    uint64_t dword1;
};

struct riscv_iommu_dc {
    uint64_t tc;
    uint64_t iohgatp;
    uint64_t ta;
    uint64_t fsc;
    uint64_t msiptp;
    uint64_t msi_addr_mask;
    uint64_t msi_addr_pattern;
    uint64_t reserved;
};

static inline uint64_t riscv_iommu_cmd_word(unsigned opcode, unsigned func)
{
    return opcode | ((uint64_t) func << RISCV_IOMMU_CMD_FUNC_SHIFT);
}

static inline struct riscv_iommu_cmd riscv_iommu_iodir_inval_ddt_cmd(
    uint32_t did)
{
    return (struct riscv_iommu_cmd) {
        .dword0 = riscv_iommu_cmd_word(RISCV_IOMMU_CMD_IODIR, 0) |
                  RISCV_IOMMU_CMD_IODIR_DV |
                  ((uint64_t) did << RISCV_IOMMU_DID_SHIFT),
        .dword1 = 0,
    };
}

static inline struct riscv_iommu_cmd riscv_iommu_iofence_cmd(void)
{
    return (struct riscv_iommu_cmd) {
        .dword0 = riscv_iommu_cmd_word(RISCV_IOMMU_CMD_IOFENCE, 0),
        .dword1 = 0,
    };
}

static inline struct riscv_iommu_cmd riscv_iommu_iotinval_gvma_cmd(
    uint64_t iova)
{
    return (struct riscv_iommu_cmd) {
        .dword0 = riscv_iommu_cmd_word(RISCV_IOMMU_CMD_IOTINVAL,
                                       RISCV_IOMMU_CMD_IOTINVAL_FUNC_GVMA) |
                  RISCV_IOMMU_CMD_IOTINVAL_AV |
                  RISCV_IOMMU_CMD_IOTINVAL_GV,
        .dword1 = iova >> 2,
    };
}

static inline uint64_t riscv_iommu_pte(uint64_t pa, uint64_t flags)
{
    return ((pa >> 12) << 10) | flags;
}

#endif
