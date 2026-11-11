/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-boundary helper registration table for U-mode-roottask migration.
 *
 * kext_*.c bodies in kernel/ext/ that originally lived in servers/roottask/mem/
 * sometimes call roottask-side helpers that cannot move without cascading. The
 * kernel link can't see those user-mode symbols at link time. So roottask
 * registers function pointers here during early init, and kernel kext bodies
 * call through them.
 *
 * This is a transitional mechanism; as helpers are pulled into kernel text the
 * corresponding entries in this table are retired.
 */

#include <core/globals.h>
#include "mm-xboundary.h"

uintptr_t *kext_va_rover_p;
kext_pte_map_fn_t kext_pte_map;

struct pa_restrict **kext_restrict_proc_pp;
struct cpu_mm_as **kext_pgtbl_list_pp;

/* Record the roottask-side memory-management helper pointers.
 *
 * Stores the VA rover, pte_map callback, and restriction / page-table list
 * pointers so kernel kext bodies can reach the roottask-resident helpers.
 * Parameters:
 *   va_rover_p       : pointer to roottask's system-VA allocation rover
 *   ptem             : pte_map callback in roottask text
 *   restrict_proc_pp : pointer to roottask's process restriction list
 *   pgtbl_list_pp    : pointer to roottask's page-table as list
 */
void kext_register_mm_helpers(uintptr_t *va_rover_p,
                              kext_pte_map_fn_t ptem,
                              struct pa_restrict **restrict_proc_pp,
                              struct cpu_mm_as **pgtbl_list_pp)
{
    kext_va_rover_p = va_rover_p;
    kext_pte_map = ptem;
    kext_restrict_proc_pp = restrict_proc_pp;
    kext_pgtbl_list_pp = pgtbl_list_pp;
}

/* kernel extension entry — roottask packs the four args into a kcall_args blob;
 * we unpack and call the existing helper. Must mirror the struct
 * kcall_args_register_mm in servers/roottask/sys/misc.c.
 */
struct kcall_args_register_mm {
    uintptr_t *va_rover_p;
    kext_pte_map_fn_t pte_map;
    struct pa_restrict **restrict_proc_pp;
    struct cpu_mm_as **pgtbl_list_pp;
};

/* Register the MM helper pointers from a roottask kcall_args blob (kernel
 * extension body).
 *
 * Unpacks the four helper pointers from the kcall_args struct and forwards them
 * to kext_register_mm_helpers. Parameters:
 *   data : pointer to a struct kcall_args_register_mm
 */
void kext_register_mm_helpers_kex(void *data)
{
    struct kcall_args_register_mm *kap = data;
    kext_register_mm_helpers(kap->va_rover_p, kap->pte_map,
                             kap->restrict_proc_pp, kap->pgtbl_list_pp);
    kext_status(NULL, 0);
}
