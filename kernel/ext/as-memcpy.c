/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * kext_as_memcpy — kcall slot __KSYS_PRIV_AS_MEMCPY.
 *
 * Cross-as memcpy performed on roottask's behalf. Roottask runs U-mode, and
 * kernel_exit forces as_prp back to roottask's own process on every U-mode
 * return (see arch/riscv/exit.c — the ROOTTASK_PID carve-out), so satp is never
 * redirected for roottask threads and roottask cannot read/write a foreign user
 * VA directly.
 *
 * This kernel extension does it on roottask's behalf:
 *
 *   1. Validate target_pid and the foreign VA.
 *   2. Switch satp to the target process's as.
 *   3. xfer_memcpy() with fault recovery via xfer_handlers.
 *   4. Restore roottask's as.
 *   5. Return EOK or EFAULT (or ESRCH/EPERM).
 *
 * Direction:
 *   AS_MEMCPY_FROM_TARGET — reads target_va in target's as,
 *                                writes roottask_buf (kernel-direct-map
 *                                or roottask U-VA at 0xc0000000+).
 *   AS_MEMCPY_TO_TARGET   — writes target_va in target's as,
 *                                reads roottask_buf.
 *
 * The roottask_buf must remain accessible after the satp swap:
 * kernel-direct-map VAs (kernel-half, PTE_U=1) are present in every pgdir via
 * the shared kernel L2 entries (see commit 3e53ea52); these are always safe.
 * Roottask's own U-VA at 0xc0000000+ is only present in roottask's own pgdir
 * and would NOT survive a satp swap to a different process — callers must use
 * kernel-direct-map buffers.
 */

#include <core/globals.h>

/* Public ABI for the kcall_args and direction enum. */
struct kcall_args_as_memcpy {
    pid_t target_pid;
    void *target_va;    /* user VA in target's as */
    void *roottask_buf; /* kernel-direct-map VA (roottask's
                         * own U-VA would not survive satp swap)
                         */
    size_t len;
    int direction; /* 0 = read from target, 1 = write to target */
};

/* Direction values — exposed to roottask via the wrapper header. */
#define AS_MEMCPY_FROM_TARGET 0
#define AS_MEMCPY_TO_TARGET 1

extern int xfer_memcpy(void *dst, const void *src, size_t len);
extern int pa_sysram_contains(paddr_t start, paddr_t len);

/* kext_as_memcpy — copy between roottask's buffer and a target process's user
 * address space.
 *
 * Switches satp to the target process's as, performs the copy with xfer fault
 * recovery, then restores roottask's as. Parameters:
 *   data : struct kcall_args_as_memcpy * (target_pid, target_va,
 *          roottask_buf, len, direction)
 * Sets the kcall status to EOK, EINVAL, ESRCH, or EFAULT.
 */
void kext_as_memcpy(void *data)
{
    struct kcall_args_as_memcpy *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *target_prp;
    process_t *saved_as_prp;
    unsigned cpu = get_cpunum();
    int status;

    /* Snapshot every kcall_args field BEFORE the satp swap. kap itself lives in
     * roottask's U-VA (roottask_mmap_anon_user region at L2[3]) and vanishes
     * the moment we switch to the target process's pgdir on line
     * "vm_ops->as(target_prp, ...)" below. Touching kap after the swap would
     * page-fault in S-mode (scause=13).
     */
    pid_t target_pid_l = kap->target_pid;
    void *target_va_l = kap->target_va;
    void *roottask_buf_l = kap->roottask_buf;
    size_t len_l = kap->len;
    int direction_l = kap->direction;

    /* Reject obviously invalid VAs. The caller is trusted only after priv_caps
     * validation, but a defensive check guards against the kernel extension
     * being co-opted to read kernel memory if a bug ever leaks the slot id.
     * target_va must be a user VA (< KERNEL_VIRT_BASE). roottask_buf must be a
     * kernel-direct-map VA (>= KERNEL_VIRT_BASE); a U-mode roottask buffer
     * would not survive the satp swap.
     */
    if (direction_l != AS_MEMCPY_FROM_TARGET &&
        direction_l != AS_MEMCPY_TO_TARGET) {
        SETKSTATUS(act, EINVAL);
        return;
    }
    if ((uintptr_t) target_va_l >= KERNEL_VIRT_BASE ||
        (uintptr_t) roottask_buf_l < KERNEL_VIRT_BASE) {
        SETKSTATUS(act, EINVAL);
        return;
    }
    if (len_l == 0) {
        SETKSTATUS(act, EOK);
        return;
    }

    /* The END of each range must be checked too, not just the start. target_va
     * + len must stay in the user half (< KERNEL_VIRT_BASE); roottask_buf + len
     * must not wrap. The first test (len bounded by the gap to
     * KERNEL_VIRT_BASE) also rules out target_va wrap.
     */
    if (len_l > KERNEL_VIRT_BASE - (uintptr_t) target_va_l ||
        (uintptr_t) roottask_buf_l + len_l < (uintptr_t) roottask_buf_l) {
        SETKSTATUS(act, EINVAL);
        return;
    }

    target_prp = lookup_pid_rcu(target_pid_l);
    if (!target_prp) {
        rcu_read_unlock(&process_rcu);
        SETKSTATUS(act, ESRCH);
        return;
    }
    status = process_pin(target_prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        SETKSTATUS(act, status);
        return;
    }

    /* Save current as, switch to target's as. */
    saved_as_prp = pcpu_array[cpu].pc_as_prp;
    vm_ops->as(target_prp, &pcpu_array[cpu].pc_as_prp);

    /* Cross-as copy with xfer fault recovery. xfer_memcpy installs
     * xfer_src_handlers and a setjmp; a fault on the user pointer longjmps back
     * with a non-zero status.
     */
    if (direction_l == AS_MEMCPY_FROM_TARGET) {
        status = xfer_memcpy(roottask_buf_l, target_va_l, len_l);
    } else {
        status = xfer_memcpy(target_va_l, roottask_buf_l, len_l);
    }

    /* Restore roottask's as before returning. */
    vm_ops->as(saved_as_prp, &pcpu_array[cpu].pc_as_prp);
    process_unpin(target_prp);

    SETKSTATUS(act, status ? EFAULT : EOK);
}


/* kext_kpage_zero — zero "len" bytes at the kernel-direct-map VA corresponding
 * to physical address "paddr". Companion to kext_as_memcpy: U-mode roottask
 * cannot write the kernel- direct-map (PTE_U=0), but the kernel-side body runs
 * in S-mode where it's directly addressable.
 *
 * Used by pq_init_run (servers/roottask/mem/mm-reference.c): after pa_alloc
 * hands roottask a fresh pa_quantum, roottask needs to zero the page before
 * exposing it to a user mapping.
 *
 * paddr is validated to lie within real RAM bounds (avoid using the slot to
 * scribble on MMIO). len is capped at MEG(16) as a defensive safety net — a
 * single zero op should be at most one page (QUANTUM_SIZE = 4 KiB) in practice.
 */
struct kcall_args_kpage_zero {
    paddr_t paddr;
    size_t len;
};

void kext_kpage_zero(void *data)
{
    struct kcall_args_kpage_zero *kap = data;
    thread_t *act = get_active_thread(get_cpunum());

    if (kap->len == 0) {
        SETKSTATUS(act, EOK);
        return;
    }
    if (kap->len > MEG(16) ||
        kap->paddr + kap->len < kap->paddr) { /* overflow */
        SETKSTATUS(act, EINVAL);
        return;
    }

    /* Refuse anything not entirely backed by real RAM — the comment above
     * promised this; without it the slot can zero MMIO.
     */
    if (!pa_sysram_contains(kap->paddr, kap->len)) {
        SETKSTATUS(act, EINVAL);
        return;
    }

    memset((void *) ((uintptr_t) kap->paddr + KERNEL_VIRT_BASE), 0, kap->len);
    SETKSTATUS(act, EOK);
}

/* kext_icache_sync_all — broadcast an I-cache fence (fence.i) to every hart on
 * roottask's behalf. Slot __KSYS_PRIV_ICACHE_SYNC.
 *
 * RISC-V does not keep the I-cache coherent with data stores, and it has no
 * user-mode fence.i for cross-hart scope. After roottask's loader writes a
 * program's text into freshly-allocated (recycled) pages, the hart that ends up
 * executing it must have fence.i'd those physical pages — otherwise it fetches
 * the previous tenant's stale instructions. Roottask runs U-mode and cannot
 * issue fence.i / SBI itself, so it routes the sync through this kernel
 * extension. Takes no arguments.
 */
void kext_icache_sync_all(void *data)
{
    thread_t *act = get_active_thread(get_cpunum());

    (void) data;
    cpu_icache_sync_all();
    SETKSTATUS(act, EOK);
}
