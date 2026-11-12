/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* AArch64 kcall helpers: EL0 uses SVC; kernel callers dispatch PRIV directly. */

#define _RTMUX_NO_SYSCALL_PROTOS
#include <core/globals.h>
extern void cpu_console_write(const char *, unsigned long);

/* ksys_dbgprint — kcall handler for __KSYS_DBGPRINT (slot 3).
 *
 * Prints len bytes from an any-mode caller to the kernel console. Invoked via
 * ksys_call_table[__KSYS_DBGPRINT]. User bytes are copied through a kernel
 * buffer before writing the PL011 console.
 *
 * The staging buffer is smaller than an arbitrary caller write, so the copy
 * streams in buffer-sized chunks and loops until len is drained rather than
 * truncating at the buffer size (which dropped the tail of any longer write and
 * split whatever escape sequence straddled the boundary). Each chunk is copied
 * through the fault-guarded xfer_memcpy, so a bad or short user pointer stops
 * the write cleanly instead of faulting the kernel, and is flushed with one console
 * write that bypasses any kprintf lock entanglement.
 *
 * Parameters:
 *   act : calling thread (unused)
 *   kap : kcall arguments, str (bytes) and len (byte count)
 * Return value: always 0.
 */
int kdecl ksys_dbgprint(thread_t *act, struct kcall_args_dbgprint *kap)
{
    const char *str = kap->str;
    size_t len = kap->len;
    char buf[256];

    (void) act;

    if (!str || len == 0)
        return 0;

    /* str/len come straight from the caller's registers. xfer_memcpy's fault
     * guard only recovers user-space faults (the trap handler longjmps only for
     * fault_addr < KERNEL_VIRT_BASE), so a kernel VA here would either stream
     * the RAM direct-map to the console — kernel-memory disclosure — or fault
     * unrecoverably and panic. Reject the wrap and any address at or above
     * KERNEL_VIRT_BASE. A per-process boundary_addr check is NOT enough here:
     * roottask's boundary_addr is VM_KERN_SPACE_BOUNDARY, so it would accept
     * kernel VAs — KERNEL_VIRT_BASE is the real user/kernel split for every
     * process. len is nonzero here, so str + len - 1 is the inclusive last
     * byte.
     */
    uintptr_t last = (uintptr_t) str + len - 1;
    if (last < (uintptr_t) str || last >= KERNEL_VIRT_BASE)
        return 0;

    while (len > 0) {
        size_t chunk = min_size(len, sizeof(buf));

        /* Fault-guarded copy: a bad user address returns nonzero here instead
         * of trapping the kernel, so stop the write at that point.
         */
        if (xfer_memcpy(buf, str, chunk) != 0)
            break;
        cpu_console_write(buf, chunk);
        str += chunk;
        len -= chunk;
    }
    return 0;
}

/* Heartbeat sequence counter. Bumped on every __KSYS_PRIV_HEARTBEAT. The
 * heartbeat thread is dispatched by the scheduler, so a steadily advancing
 * heartbeat_seq proves the scheduler is still dispatching roottask threads. The
 * stall detector (driven by the timer IRQ, not the scheduler) watches this
 * counter and dumps state if it goes stale — catching a scheduler/timer wedge
 * that freezes the spinner.
 */
volatile unsigned heartbeat_seq;

/* kext_heartbeat — PRIV liveness probe (__KSYS_PRIV_HEARTBEAT).
 *
 * Invoked by roottask's heartbeat thread on a fixed interval; bumps
 * heartbeat_seq so the privileged-kcall path can be observed to be alive. The
 * stall detector watches this counter (kernel/debug/stall-detector.c) — it is
 * the actual liveness mechanism.
 *
 * It used to also paint a spinner glyph in the console corner via SBI DECSC/
 * DECRC, but that scribbled on the shared console: it interleaved with, and
 * split the escape sequences of, any full-screen app (vi, less), and the
 * DECSC/DECRC (ESC 7 / ESC 8) leaked as literal '7'/'8' on terminals that don't
 * support them. A counter needs no screen real estate, so the paint is gone.
 * Parameters:
 *   arg : unused
 */
void kext_heartbeat(void *arg)
{
    (void) arg;
    heartbeat_seq++;
}

/* syscall_priv — in-kernel EL1 shortcut for the slot-2 roottask-privileged
 * kcall.
 *
 * For kernel-internal callers (idle.c, clock-core.c, kernel extension public
 * wrappers), performs the same operation as a EL0 caller's SVC would:
 * range-check id, gate through priv_check(), run the handler, and return
 * whatever the handler set via SETKSTATUS.
 *
 * Parameters:
 *   id  : roottask-private subcall id
 *   arg : argument passed through to the handler
 * Return value: the handler's status, -ENOSYS for an invalid id, or
 *               -EPERM if the caller lacks the required PRIV cap.
 */
long syscall_priv(int id, void *arg)
{
    thread_t *act;
    long ret;
    unsigned cpu;

    /* in_kcall[cpu]++ marks "doing kernel work on this CPU" so the timer-trap
     * routing takes its minimal kernel-self path while the kernel extension
     * body runs. Always bump on entry, decrement on exit — the counter is a
     * per-CPU nesting depth, so re-entry from an already- in-kernel caller
     * (e.g. a kernel extension invoking another via this same trampoline) just
     * bumps the count further and unwinds symmetrically.
     */
    cpu = get_cpunum();
    in_kcall_inc();
    in_kcall_phase(priv_call_phase((unsigned) id));

    act = get_active_thread_chk(cpu);
    if (!act || !act->process)
        crash_halt("bad actives[] in syscall_priv");

    SETREGSTATUS(&act->reg, 0);
    ret = priv_dispatch(act->process, (unsigned) id, arg);
    if (ret != EOK) {
        ret = -ret;
        goto out;
    }

    ret = (long) REGSTATUS(&act->reg);

out:
    in_kcall_dec();
    return ret;
}

/* Every kcall runs without a big kernel lock; each handler body is solely
 * responsible for protecting shared state with fine-grained kspinlocks. See
 * doc/tex/KerFuncRef/ for the per-handler audit.
 */
