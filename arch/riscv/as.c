/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * as_switch — switch to a process's Sv39 address space.
 *
 * This function lives in kernel text because it is called from "kernel_exit" on
 * every context switch and from message-passing SWITCH_AS paths, which must be
 * S-mode-fetchable even when roottask text is marked PTE_U=1.
 */

#include <core/globals.h>
#include "as.h"
#include <cpu-inline.h>

/* as_switch — load the page table of a process and record it as the current
 * address space.
 *
 * Builds the Sv39 satp value from the process's address descriptor, installs
 * it, and flushes the TLB. Processes without an address descriptor are left
 * untouched.
 *
 * Parameters:
 *   actprp  : process whose address space to activate
 *   pactprp : out — updated to actprp once the switch has taken effect
 */
void as_switch(process_t *actprp, process_t **pactprp)
{
    address_t *adp;

    if ((adp = actprp->memory)) {
        /* Build satp value:
         *   [63:60] = mode (8 = Sv39)
         *   [59:44] = ASID
         *   [43:0]  = PPN (ptroot_paddr >> 12)
         */
        uint64_t satp = (uint64_t) SATP_SV39 |
                        ((uint64_t) adp->cpu.asid << 44) |
                        (adp->cpu.ptroot_paddr >> PGSHIFT);

        interrupt_disable();
        w_satp(satp);
        sfence_vma();
        *pactprp = actprp;
        interrupt_enable();
    }
}
