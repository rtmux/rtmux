/*
 * ELF range arithmetic helpers for loader trust boundaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ELF_RANGE_H
#define _ELF_RANGE_H

#include <stddef.h>
#include <stdint.h>

static inline int elf_file_range_overflows(uint64_t off,
                                           uint64_t len,
                                           size_t file_size)
{
    return off > (uint64_t) file_size || len > (uint64_t) file_size - off;
}

static inline int elf_addr_range_overflows(uint64_t start,
                                           uint64_t len,
                                           uintptr_t *endp)
{
    if (start > UINTPTR_MAX || len > UINTPTR_MAX - (uintptr_t) start)
        return 1;
    *endp = (uintptr_t) start + (uintptr_t) len;
    return 0;
}

static inline int elf_roundup_overflows(uintptr_t value,
                                        uintptr_t align,
                                        uintptr_t *roundedp)
{
    uintptr_t mask = align - 1;

    if ((align & mask) != 0 || value > UINTPTR_MAX - mask)
        return 1;
    *roundedp = (value + mask) & ~mask;
    return 0;
}

static inline int elf_addr_reloc_overflows(uintptr_t start,
                                           intptr_t reloc,
                                           uintptr_t *resultp)
{
    uintptr_t delta;

    if (reloc >= 0)
        return elf_addr_range_overflows(start, (uintptr_t) reloc, resultp);

    delta = (uintptr_t) (-(reloc + 1)) + 1;
    if (start < delta)
        return 1;
    *resultp = start - delta;
    return 0;
}

static inline int elf_phdr_table_overflows(uint64_t phoff,
                                           uint64_t phnum,
                                           uint64_t phentsize,
                                           size_t file_size)
{
    uint64_t bytes;

    if (phnum == 0 || phentsize == 0)
        return 1;
    if (phnum > UINT64_MAX / phentsize)
        return 1;
    bytes = phnum * phentsize;
    return elf_file_range_overflows(phoff, bytes, file_size);
}

#endif /* _ELF_RANGE_H */
