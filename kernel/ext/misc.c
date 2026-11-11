/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <cpu-inline.h>

#include "as.h"
#include "mm-xboundary.h"

extern uintptr_t cpu_sysvaddr_find(uintptr_t start, size_t size);


/* Kernel-internal BKL helpers. These are NOT calls — they exist only for
 * kernel-resident callers (kext_pa.c, sysaddr_map, etc.) that already run
 * inside a kernel extension body. U-mode roottask has no business calling them;
 * the kernel-side syscall_priv dispatcher acquires the BKL before invoking any
 * kernel extension body.
 */

/* Assert that the caller is running inside a kcall.
 *
 * Debug-only check that this kernel extension body is executing in a persona
 * context (in_kcall[cpu] > 0); crashes otherwise. Compiled out when NDEBUG is
 * set.
 */
void kext_lock(void)
{
#ifndef NDEBUG
    if (in_kcall[get_cpunum()] == 0) {
        crash();
    }
#endif
}


/* Set the kernel-call return status for a thread.
 *
 * Stores status as the thread's kcall result; defaults to the calling thread
 * when thp is NULL. Parameters:
 *   thp    : target thread, or NULL for the calling thread
 *   status : status value to record
 */
void kext_status(thread_t *thp, int status)
{
    if (!thp) {
        thp = get_active_thread(get_cpunum());
    }
    SETKSTATUS(thp, status);
}


/* Read a single character from the kernel UART (PRIV kernel extension body).
 *
 * Reached from U-mode roottask via __KSYS_PRIV_CON_GETC, and used by roottask's
 * /dev/text I/O service. con_getchar() does the work and the SETKSTATUS write
 * is a single store.
 *
 * Returns -1 if no character is available (cpu_console_getc semantics).
 * Parameters:
 *   unused : ignored
 */
void kext_con_getc(void *unused)
{
    (void) unused;
    SETKSTATUS(get_active_thread(get_cpunum()), con_getchar());
}


/* Return the bitmask of legal (active) hart IDs (PRIV kernel extension body).
 *
 * legal_cpu_bitmask is set once at boot from hart_ids[] and never mutated
 * afterward, so no serialization is needed. The value is returned to the caller
 * via SETREGSTATUS. Parameters:
 *   unused : ignored
 */
void kext_legal_cpu_mask(void *unused)
{
    (void) unused;
    SETREGSTATUS(&get_active_thread(get_cpunum())->reg, legal_cpu_bitmask);
}

/* Return the boot CPU's root page-table PA (PRIV kernel extension body).
 *
 * U-mode roottask's pgtbl_init() needs this to seed dummy_as, since reading
 * satp is illegal from U-mode. All kernel L2 entries are shared across pgdirs
 * (the kernel-L2 prealloc), so the boot pgdir is structurally interchangeable
 * with roottask's own pgdir for dummy_as's purpose (a template for
 * cpu_as_mcreate's kernel-half copy). page_directory is set once at MMU init
 * and immutable thereafter. Parameters:
 *   unused : ignored
 */
void kext_get_kern_pgdir(void *unused)
{
    extern paddr_t page_directory; /* mmu.c — boot CPU pgdir PA */
    (void) unused;
    SETREGSTATUS(&get_active_thread(get_cpunum())->reg,
                 (uintptr_t) page_directory);
}


/* kext_lookup_rcvid REMOVED — see servers/roottask/sys/misc.c for the
 * rationale. The kernel extension returned "connect_t *" and "thread_t *" to
 * U-mode roottask, both kernel-private types under heap PTE_U=0. Its former
 * dispatch slot has since been reused by __KSYS_PRIV_HEARTBEAT.
 */

struct kcall_args_stop_threads {
    process_t *prp;
    unsigned flags;
    unsigned iflags;
};

/* Stop all threads of a process (PRIV kernel extension body).
 *
 * Runs stop_threads over kap->prp->threads, stamping
 * RTMUX_FLG_THR_TO_BE_STOPPED plus the caller's flags onto each thread.
 * prp->vec_slock (PROCVEC) makes the iteration atomic against concurrent
 * thread_create / thread_destroy on the same prp. Parameters:
 *   data : pointer to a struct kcall_args_stop_threads (prp + flags + iflags)
 */
void kext_stop_threads(void *data)
{
    struct kcall_args_stop_threads *kap = data;

    /* The per-thread flag stamp ideally belongs under a per-thread thp->lock
     * (no such field on thread_t today); until that exists the stamp itself
     * relies on the dispatcher's non-preemption, and vec_slock only makes the
     * iteration atomic.
     */
    kspinlock_lock(&kap->prp->vec_slock);
    stop_threads(kap->prp, kap->flags, kap->iflags);
    kspinlock_unlock(&kap->prp->vec_slock);
    SETKSTATUS(get_active_thread(get_cpunum()), 0);
}


/* reboot — reboot or shut down the system.
 *
 * One-shot: neither cpu_reboot() nor cpu_shutdown() returns, so the
 * non-preemption here is permanent: a reboot must be the absolute last thing
 * this CPU does. Parameters:
 *   abnormal : non-zero to take the abnormal (crash) path, which routes
 *              to SBI shutdown so the diagnostic stays on screen; zero
 *              cold-reboots, which is what reboot(8) wants in production.
 */
void reboot(void *abnormal)
{
    if (abnormal)
        cpu_shutdown();
    else
        cpu_reboot();
}


/* system_reboot — kernel-internal entry used by crash() and shutdown(). U-mode
 * roottask has its own equivalent in servers/roottask/proc/kext-wrappers.c that
 * goes through syscall_priv(__KSYS_PRIV_REBOOT, ...).
 */
void system_reboot(int abnormal)
{
    reboot((void *) (uintptr_t) abnormal);
    /* unreachable */
}


/* do_shutdown — shut the system down. Never returns. Parameters:
 *   arg : unused
 */
void do_shutdown(void *arg)
{
    (void) arg;
    cpu_shutdown();
}


/* Find a free system virtual address range and map it (PRIV callback).
 *
 * Asks cpu_sysvaddr_find for a span of the requested size starting at the VA
 * rover, advances the rover past it, and installs PTEs over the span via
 * kext_pte_map. On failure the result is reported back as VA_INVALID.
 * Parameters:
 *   d : pointer to a struct mm_pte_manipulate (in: end/prot/paddr;
 *       out: start = mapped VA or VA_INVALID)
 */
void sysaddr_map(void *d)
{
    struct mm_pte_manipulate *data = d;
    uintptr_t va;

    kext_lock();
    va = cpu_sysvaddr_find(*kext_va_rover_p, data->end);
    if (va != VA_INVALID) {
        *kext_va_rover_p = va + (size_t) data->end;
        if (kext_pte_map(NULL, va, *kext_va_rover_p - 1, (int) data->prot, NULL,
                         data->paddr, 0) != EOK) {
            va = VA_INVALID;
        }
    }
    data->start = va;
}
