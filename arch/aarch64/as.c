/* SPDX-License-Identifier: Apache-2.0 */
#include <core/globals.h>
#include "as.h"
void as_switch(process_t *prp, process_t **current)
{
    if (!prp->memory)
        return;
    unsigned long flags = cpu_irq_save();
    __asm__ volatile(
        "dsb ish; msr ttbr0_el1,%0; isb" ::"r"(prp->memory->cpu.ptroot_paddr)
        : "memory");
    cpu_tlb_flush();
    *current = prp;
    cpu_irq_restore(flags);
}
