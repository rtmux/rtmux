/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Cross-boundary helper pointers for kext_* bodies that originally lived in
 * servers/roottask/mem/. Roottask registers these at startup; kernel kext
 * bodies call through them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_KERNEL_KEXT_MM_XBOUNDARY_H
#define _RTMUX_KERNEL_KEXT_MM_XBOUNDARY_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "objects.h"

/* Forward decls — defined in as.h. Headers stay loose so kext_* bodies can pull
 * in as.h or not, without forcing it on includers.
 */
struct address_t;

typedef int (*kext_pte_map_fn_t)(struct address_t *adp,
                                 uintptr_t start,
                                 uintptr_t end,
                                 int flags,
                                 path_object_t *obp,
                                 paddr_t paddr,
                                 unsigned extra);

/* cpu_sysvaddr_find / cpu_as_vaddrinfo / cpu_pte_* / pa.c and do_manipulation
 * live in kernel text; their kext_* indirections are gone.
 */

/* Physical address of a user virtual address in the current process, via a
 * page-table walk of the running hart's satp. 0 on success (*pa set), -1 if out
 * of range or unmapped. Used to key process-shared sync objects by their
 * physical (cross-process-stable) address; see docs/pshared-sync-design.md.
 */
int cpu_uvaddr_to_paddr(uintptr_t vaddr, paddr_t *pa);

/* pa allocator now lives natively in kernel/ext/pa.c — no kext_* indirection
 * needed. restrict_proc and pgtbl_list still come from roottask and are
 * registered below.
 */
struct pa_quantum;
struct block_head;
struct pa_restrict;
struct cpu_mm_as;

extern uintptr_t *kext_va_rover_p;
extern kext_pte_map_fn_t kext_pte_map;
extern struct pa_restrict **kext_restrict_proc_pp;
extern struct cpu_mm_as **kext_pgtbl_list_pp;

void kext_register_mm_helpers(uintptr_t *va_rover_p,
                              kext_pte_map_fn_t ptem,
                              struct pa_restrict **restrict_proc_pp,
                              struct cpu_mm_as **pgtbl_list_pp);

#endif
