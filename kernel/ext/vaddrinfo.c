/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ext_vaddrinfo — virtual-address info PRIV callback. cpu_as_vaddrinfo lives in
 * arch/riscv/cpu-pte.c and is called directly.
 */

#include <core/globals.h>
#include "as.h"

extern unsigned cpu_as_vaddrinfo(process_t *prp,
                                 uintptr_t vaddr,
                                 paddr_t *paddrp,
                                 size_t *lenp);

struct args_vaddrinfo {
    off64_t off;
    paddr_t paddr;
    process_t *prp;
    uintptr_t vaddr;
    struct mm_map *mm;
    int init;
    int found;
    unsigned pg_offset;
    size_t len;
};

/* Resolve a virtual address to its physical mapping (PRIV callback).
 *
 * Calls cpu_as_vaddrinfo for the given process and vaddr, filling in the
 * physical address and mapped length, and returns its status. Parameters:
 *   data : pointer to a struct args_vaddrinfo (in: prp, vaddr;
 *          out: paddr, len)
 */
void ext_vaddrinfo(void *data)
{
    struct args_vaddrinfo *args = data;

    kext_lock();
    kext_status(NULL, (int) cpu_as_vaddrinfo(args->prp, args->vaddr,
                                             &args->paddr, &args->len));
}
