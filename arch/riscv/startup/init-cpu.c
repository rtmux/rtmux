/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* RISC-V CPU initialization.
 *
 * init_traps — set stvec to point to __strap_entry init_cpu — per-CPU
 * initialization (SIE, SUM) cpu_interrupt_init — placeholder (no dynamic
 * codegen on RISC-V)
 *
 * License: Apache 2.0
 */

#include <startup.h>
#include <cpu-inline.h>
#include <sbi.h>

extern void __strap_entry(void);

/* init_traps - set up the RISC-V trap vector.
 *
 * Points stvec to our trap handler so that any S-mode exception or interrupt is
 * dispatched properly.
 */
void init_traps(void)
{
    w_stvec((uint64_t) __strap_entry);
}

/* init_cpu - CPU-specific initialization.
 *
 * Called once per CPU from kernel_init() after all other init is done. RISC-V
 * only needs interrupt and status-register setup here.
 */
void init_cpu(void)
{
    /* Enable supervisor software interrupts (IPI reception) on boot CPU */
    w_sie(r_sie() | SIE_SSIE);

    /* Set SUM (Supervisor User Memory access) so S-mode code can access pages
     * marked with PTE_U. Needed because the process loader thread runs in
     * S-mode but must read/write user pages (e.g. ELF segments, interpreter
     * path) mapped with PTE_U.
     */
    w_sstatus(r_sstatus() | SSTATUS_SUM);

    /* Let U-mode read the time counter directly (rdtime) instead of trapping.
     * This is what lets a userland probe self-time a sub-tick operation like an
     * IPC round trip, since the software clock (CLOCK_MONOTONIC) only advances
     * per tick. Only bit 1 (TM) is set: the cycle (CY) and instret (IR)
     * counters stay trapped, keeping the exposed timing side channel as narrow
     * as the one measured consumer needs (widen deliberately if a
     * cycle-accurate real-board probe ever needs CY). Exposing rdtime is still
     * a deliberate, documented side channel; see docs/rt-budgets.md. Requires
     * M-mode to have delegated it via mcounteren, which OpenSBI does.
     */
    w_scounteren(0x2);
}

/* cpu_interrupt_init - generate/initialize interrupt dispatch code.
 *
 * RISC-V uses a single trap vector with cause-based dispatch, so there is no
 * dynamic interrupt code generation.
 */
void cpu_interrupt_init(unsigned num_levels)
{
    /* Nothing to do for RISC-V — no dynamic interrupt codegen */
    (void) num_levels;
}
