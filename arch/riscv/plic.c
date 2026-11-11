/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * plic.c — RISC-V Platform-Level Interrupt Controller (PLIC)
 *
 * Provides init, per-hart setup, claim, and complete operations for the PLIC on
 * QEMU virt and platforms with the same PLIC layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <cpu-inline.h>
#include <plic-regs.h>
#include <paging.h>

/* Number of PLIC interrupt sources to configure */
#define PLIC_NUM_SOURCES 128

/* All PLIC registers are accessed via the high-VA identity mapping.
 * startup/mmu.c maps the PLIC PA region at VA = PA + KERNEL_VIRT_BASE.
 */
#define PLIC_REG(off)                                                         \
    ((volatile uint32_t *) ((uintptr_t) (plic_base + (unsigned long) (off)) + \
                            KERNEL_VIRT_BASE))

/* Deferred-complete bookkeeping for IST-dispatched interrupt sources.
 *
 * For IST-dispatched sources the trap handler records which hart claimed the
 * interrupt but does NOT write the complete. The complete is deferred until the
 * IST has drained the device and called __interrupt_unmask — at which point the
 * source has de-asserted, so the PLIC gateway transitions InService→Idle rather
 * than InService→Pending, eliminating the one-spurious-per-real-IRQ we
 * otherwise saw.
 *
 * SCLAIM is addressed by context (per-hart), not by the writing hart, so the
 * IST can safely complete from whichever hart it happens to run on — we just
 * need to know which hart originally claimed.
 */
#define PLIC_NO_CLAIM 0xff
static uint8_t plic_claim_hart[PLIC_NUM_SOURCES];

/* Per-source pinning — recorded by ksys_interrupt_attach_{thread,event} via
 * plic_set_owner_hart() AFTER interrupt_attach returns but BEFORE the explicit
 * __interrupt_unmask the kcall_exit performs. Used by riscv_plic_unmask in
 * init-intrinfo.c to SENABLE only the owning hart (clearing the bit on every
 * other hart). 0xFF = no owner = broadcast delivery for kernel-handler levels
 * and pre-pinning fallback.
 */
#define PLIC_NO_OWNER 0xff

/* Non-static: read by riscv_plic_unmask in init-intrinfo.c. */
uint8_t plic_owner_hart[PLIC_NUM_SOURCES];

/* plic_init — global PLIC initialization (called once on boot CPU).
 *
 * Sets priority > 0 for all interrupt sources so they can be claimed once a
 * handler is attached (priority 0 = "never deliver"). Runtime mask/unmask is
 * then expressed by toggling priority between 0 and 1 — see init-intrinfo.c.
 */
void plic_init(void)
{
    for (int i = 0; i < PLIC_NUM_SOURCES; i++) {
        plic_claim_hart[i] = PLIC_NO_CLAIM;
        plic_owner_hart[i] = PLIC_NO_OWNER;
    }

    for (int i = 1; i < PLIC_NUM_SOURCES; i++)
        *PLIC_REG(PLIC_OFF_PRIORITY + i * 4) = 1;

    kprintf("plic_init: %d sources configured\n", PLIC_NUM_SOURCES - 1);
}

/* plic_inithart — per-hart PLIC initialization.
 *
 * Sets the S-mode priority threshold to 0 (accept all priorities >= 1) and
 * enables the SIE_SEIE bit so external interrupts are delivered.
 *
 * Note: individual interrupt sources are enabled/disabled by the intrinfo
 * mask/unmask callbacks, not here.
 */
void plic_inithart(void)
{
    unsigned hart = get_cpunum();

    /* Accept all interrupt priorities */
    *PLIC_REG(PLIC_OFF_SPRIORITY(hart)) = 0;

    /* Enable supervisor external interrupts */
    w_sie(r_sie() | SIE_SEIE);
}

/* plic_claim — claim the highest-priority pending interrupt.
 *
 * Returns the IRQ number (1-127), or 0 if no interrupt is pending.
 */
int plic_claim(void)
{
    unsigned hart = get_cpunum();
    return (int) *PLIC_REG(PLIC_OFF_SCLAIM(hart));
}

/* plic_complete — signal completion of an interrupt on the current hart.
 *
 * Still used by the trap handler for the "no IST" cases: kernel-handler
 * dispatch (where the handler ran synchronously in trap context) and
 * spurious/unhandled claims. For the IST case, use the record/deferred pair
 * below.
 */
void plic_complete(int irq)
{
    unsigned hart = get_cpunum();
    *PLIC_REG(PLIC_OFF_SCLAIM(hart)) = (uint32_t) irq;
}

/* plic_record_claim — note which hart took this claim, for the deferred
 * complete to target later.
 *
 * Called from the SEXT trap handler immediately after plic_claim(), in place of
 * plic_complete() for IST-dispatched sources.
 */
void plic_record_claim(int irq)
{
    if (irq > 0 && irq < PLIC_NUM_SOURCES)
        plic_claim_hart[irq] = (uint8_t) get_cpunum();
}

/* plic_complete_deferred — complete a previously recorded claim.
 *
 * Called from riscv_plic_unmask (the unmask callback invoked when the IST
 * raises the level back out of masked state). The hart that originally claimed
 * is identified via plic_claim_hart[]; its SCLAIM register is addressed by
 * context, so any hart can perform the write.
 *
 * Idempotent when no claim is outstanding (e.g. the unmask path run at attach
 * time before the first interrupt has ever fired).
 */
void plic_complete_deferred(int irq)
{
    if (irq <= 0 || irq >= PLIC_NUM_SOURCES)
        return;
    uint8_t hart = plic_claim_hart[irq];
    if (hart == PLIC_NO_CLAIM)
        return;
    plic_claim_hart[irq] = PLIC_NO_CLAIM;
    *PLIC_REG(PLIC_OFF_SCLAIM(hart)) = (uint32_t) irq;
}

/* plic_set_owner_hart / plic_clear_owner_hart — pin/unpin which hart handles a
 * given PLIC source. Called by ksys_interrupt_attach_{thread, event} (set) and
 * interrupt_detach_entry (clear, via the weak hook below for
 * arch-independence). Read by riscv_plic_unmask.
 *
 * Out-of-range irq values are silently ignored — callers may compute irq from
 * arbitrary kernel vector numbers.
 */
void plic_set_owner_hart(int irq, unsigned hart)
{
    if (irq <= 0 || irq >= PLIC_NUM_SOURCES)
        return;
    plic_owner_hart[irq] = (uint8_t) hart;
}

void plic_clear_owner_hart(int irq)
{
    if (irq <= 0 || irq >= PLIC_NUM_SOURCES)
        return;
    plic_owner_hart[irq] = PLIC_NO_OWNER;
}
