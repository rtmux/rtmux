/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * init-intrinfo.c — Populate the syspage intrinfo section.
 *
 * Two interrupt controllers: [0] Timer — S-mode timer interrupt (scause
 * cause=5), 1 vector [1] PLIC — external device interrupts (scause cause=9),
 * 128 vectors PLIC IRQ N maps to interrupt level (32 + N)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <startup.h>
#include <startup-api.h>
#include <cpu-inline.h>
#include <runtime.h>
#include <plic-regs.h>
#include <plic-vector.h>
#include <paging.h>

#define PLIC_REG(off)                                                         \
    ((volatile uint32_t *) ((uintptr_t) (plic_base + (unsigned long) (off)) + \
                            KERNEL_VIRT_BASE))

/* Timer mask/unmask */

/* Mask the S-mode timer interrupt.
 *
 * Parameters:
 *   sp     : syspage entry (unused)
 *   vector : interrupt vector (unused)
 * Return value: 0
 */
static int riscv_timer_mask(struct syspage_entry *sp, int vector)
{
    (void) sp;
    (void) vector;
    w_sie(r_sie() & ~SIE_STIE);
    return 0;
}

/* Unmask the S-mode timer interrupt (unless disabled via kopt).
 *
 * Parameters:
 *   sp     : syspage entry (unused)
 *   vector : interrupt vector (unused)
 * Return value: 0
 */
static int riscv_timer_unmask(struct syspage_entry *sp, int vector)
{
    (void) sp;
    (void) vector;
    if (!kopt.disable_timer)
        w_sie(r_sie() | SIE_STIE);
    return 0;
}

/* PLIC mask/unmask */

/* PLIC mask/unmask callbacks.
 *
 * The 'vector' parameter from the kernel is a 0-based offset within this
 * intrinfo entry (i.e., the PLIC IRQ number directly). The kernel computes it
 * as: level - level_base.
 */

/* PLIC mask/unmask strategy — priority-based, matching FreeBSD's
 * sys/riscv/riscv/plic.c (plic_disable_intr / plic_enable_intr).
 *
 * Masking by toggling the per-hart S-enable bit fails on real SiFive FU740
 * silicon: when senable is cleared across the claim→complete window, the
 * gateway can end up stuck in a state where subsequent level assertions of the
 * source are not re-delivered to the previously-enabled harts. QEMU's PLIC
 * model forgives this; FU740 does not.
 *
 * The priority-based scheme keeps the per-hart enable bits stable for the life
 * of the attachment (set once in unmask, never cleared) and uses the PLIC
 * source-priority register (0 = blocked, 1 = allow) as the mask gate.
 * complete() continues to work correctly because senable is undisturbed.
 */
static int riscv_plic_mask(struct syspage_entry *sp, int irq)
{
    (void) sp;
    *PLIC_REG(PLIC_OFF_PRIORITY + (unsigned) irq * 4) = 0;
    return 0;
}

/* Unmask a PLIC source: enable it on the owner hart(s), acknowledge any
 * deferred completion, then raise its priority above the threshold so the IRQ
 * can fire.
 *
 * Parameters:
 *   sp  : syspage entry (unused)
 *   irq : PLIC IRQ number
 * Return value: 0
 */
static int riscv_plic_unmask(struct syspage_entry *sp, int irq)
{
    extern void plic_complete_deferred(int irq);
    extern uint8_t plic_owner_hart[]; /* PLIC_NUM_SOURCES entries */
    extern unsigned hart_ids[];
    unsigned ncpu = sp->num_cpu;

    /* Per-hart pinning: if a kcall_exit recorded an owner via
     * plic_set_owner_hart(), SENABLE only that hart and clear the bit on every
     * other hart (to drop any stale broadcast SENABLE a previous attach may
     * have left behind). Otherwise, fall back to the broadcast model for
     * kernel-handler levels and sources that haven't been pinned yet.
     *
     * Pre-existing in-flight claim on a non-owner hart (rare; would require a
     * broadcast pin earlier in this boot) completes harmlessly via
     * plic_complete_deferred -> SCLAIM write below; SENABLE clearing only
     * affects future deliveries.
     */
    uint8_t owner = (irq > 0 && irq < 128) ? plic_owner_hart[irq] : 0xff;
    if (owner != 0xff) {
        for (unsigned i = 0; i < ncpu; i++) {
            unsigned hart = hart_ids[i];
            volatile uint32_t *en =
                PLIC_REG(PLIC_OFF_SENABLE(hart) + ((unsigned) irq / 32) * 4);
            if (hart == owner)
                *en |= 1U << ((unsigned) irq % 32);
            else
                *en &= ~(1U << ((unsigned) irq % 32));
        }
    } else {
        /* Broadcast: kernel-handler sources, or attach paths that ran before
         * pinning was recorded.
         */
        for (unsigned i = 0; i < ncpu; i++) {
            unsigned hart = hart_ids[i];
            volatile uint32_t *en =
                PLIC_REG(PLIC_OFF_SENABLE(hart) + ((unsigned) irq / 32) * 4);
            *en |= 1U << ((unsigned) irq % 32);
        }
    }

    /* If the trap handler deferred completion for this source (IST model),
     * acknowledge it now, BEFORE raising priority. The device has been drained
     * by the IST, so the gateway transitions InService→Idle; priority=1 then
     * reopens delivery for the next genuine source assertion — no spurious IRQ.
     *
     * No-op when nothing is outstanding (e.g. first unmask at attach time, or
     * for a kernel-handler source that completes in trap).
     */
    plic_complete_deferred(irq);

    /* Raise priority above the per-hart threshold (0) so the IRQ can fire. */
    *PLIC_REG(PLIC_OFF_PRIORITY + (unsigned) irq * 4) = 1;
    return 0;
}

/* Init */

/* KVA() — force a kernel-half VA for a function pointer, regardless of whether
 * init_intrinfo runs pre- or post-MMU-switch.
 *
 * "init_intrinfo" is called from hardware_init() which executes BEFORE
 * switch_to_high_va() in entry.c (line 157 vs 176). At that point the MMU is in
 * physical-addressing mode and PC is the kernel's load PA (~0x80200000+). GCC
 * under -mcmodel=medany emits PC-relative "auipc + addi" for symbol addresses,
 * so &riscv_plic_unmask resolves to the PA at runtime — e.g. 0x0000000080207b88
 * instead of the linked VA 0xFFFFFFC080207B88. The 8-byte sd writes that
 * truncated value into iip[*].mask / .unmask, and every subsequent
 * ilp->info->unmask() jumps to a low-half VA that isn't mapped in any process
 * pgdir, with scause=12 instruction-page-fault as the user-visible symptom.
 *
 * Fix: OR the PC-relative-derived address with KERNEL_VIRT_BASE. The upper bits
 * of KERNEL_VIRT_BASE (0xFFFFFFC000000000) and the kernel's load-PA range
 * (0x80000000+) do not overlap, so OR == ADD here, and the result is the linked
 * kernel VA in either MMU mode (idempotent if the symbol address already has
 * the upper bits set).
 */
#define KVA(fn) ((typeof(fn) *) ((uintptr_t) (fn) | KERNEL_VIRT_BASE))

/* Populate the syspage intrinfo section with the timer and PLIC
 * interrupt-controller entries and their mask/unmask callbacks.
 */
void init_intrinfo()
{
    /* Allocate space for 2 intrinfo entries: timer + PLIC */
    set_syspage_section(&lsp.intrinfo, 2 * sizeof(struct intrinfo_entry));
    struct intrinfo_entry *iip = lsp.intrinfo.p;
    memset(iip, 0, 2 * sizeof(*iip));

    /* [0] Timer interrupt */
    iip[0].vector_base = 5;
    iip[0].num_vectors = 1;
    iip[0].cascade_vector = ~0U;
    iip[0].mask = KVA(riscv_timer_mask);
    iip[0].unmask = KVA(riscv_timer_unmask);

    /* [1] PLIC — external device interrupts */
    iip[1].vector_base = PLIC_VECTOR_BASE;
    iip[1].num_vectors = 128;
    iip[1].cascade_vector = ~0U;
    iip[1].mask = KVA(riscv_plic_mask);
    iip[1].unmask = KVA(riscv_plic_unmask);

    LOGF_I("%s: timer(base=%u,n=%u) plic(base=%u,n=%u)\n", __func__,
           iip[0].vector_base, iip[0].num_vectors, iip[1].vector_base,
           iip[1].num_vectors);
}
