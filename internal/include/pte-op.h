/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_PTE_OP_H
#define RTMUX_PTE_OP_H

enum pte_op {
    PTE_OP_MAP = 0x0001,
    PTE_OP_PROT = 0x0002,
    PTE_OP_UNMAP = 0x0004,
    PTE_OP_PREALLOC = 0x0008,
    PTE_OP_BAD = 0x0010,
    PTE_OP_PREEMPT = 0x0080,
    PTE_OP_TEMP = 0x0200,
    PTE_OP_FORCEMERGE = 0x0400,
};

#endif
