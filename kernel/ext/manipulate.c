/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PTE-manipulation state machine + the PRIV entry that drives it.
 *
 * Roottask calls __KSYS_PRIV_MANIPULATE; the dispatcher hands the kcall_args
 * blob to do_manipulation, which steps through SPLIT_START / SPLIT_END /
 * MANIPULATE / MERGE invoking the cpu_pte_* helpers in arch/riscv/cpu-pte.c
 * directly. No cross-binary callbacks remain.
 */

#include <core/globals.h>
#include "as.h"

#define MSTATE_SPLIT_START 0
#define MSTATE_SPLIT_END 1
#define MSTATE_MANIPULATE 2
#define MSTATE_MERGE 3

struct pte_manip {
    struct mm_pte_manipulate data;
    unsigned state;
    uintptr_t mapped;
};


/* Drive the PTE-manipulation state machine over one address space.
 *
 * On systems with variable page sizes (and MM_FLAG_VPS set) it walks the
 * SPLIT_START / SPLIT_END / MANIPULATE / MERGE states, splitting large pages at
 * the edges of the range, applying the operation, then re-merging as allowed by
 * the op flags. Otherwise it just calls cpu_pte_manipulate once. The reached
 * extent is recorded in manip->mapped. Parameters:
 *   manip : in/out manipulation state (range, op, paddr, prot, current state)
 * Return value: EOK on success, otherwise an errno from the cpu_pte_* helpers.
 */
static int do_manipulation(struct pte_manip *manip)
{
    int r;

    if (CPU_SYSTEM_HAVE_MULTIPLE_PAGESIZES != VPS_NONE) {
        if (mm_flags & MM_FLAG_VPS) {
            r = EOK;
            switch (manip->state) {
            case MSTATE_SPLIT_START:
                if (!(manip->data.op & PTE_OP_PREALLOC)) {
                    r = cpu_pte_split(manip->data.start, &manip->data);
                    if (r != EOK)
                        return r;
                    manip->data.start = manip->data.first;
                }
                manip->state = MSTATE_SPLIT_END;
                /* fall through */
            case MSTATE_SPLIT_END:
                if (!(manip->data.op & PTE_OP_PREALLOC)) {
                    r = cpu_pte_split(ADDR_PAGE(manip->data.end + 1),
                                      &manip->data);
                    if (r != EOK)
                        return r;
                    manip->data.start = manip->data.first;
                }
                manip->state = MSTATE_MANIPULATE;
                /* fall through */
            case MSTATE_MANIPULATE:
                r = cpu_pte_manipulate(&manip->data);
                manip->mapped = manip->data.start;
                if (r != EOK)
                    return r;
                manip->data.start = manip->data.first;
                manip->state = MSTATE_MERGE;
                /* fall through */
            case MSTATE_MERGE:
                if (manip->data.op &
                    (PTE_OP_MAP | PTE_OP_PROT | PTE_OP_FORCEMERGE)) {
                    r = cpu_pte_merge(&manip->data);
                } else if (manip->data.split_end > manip->data.end) {
                    manip->data.start = manip->data.end + 1;
                    r = cpu_pte_merge(&manip->data);
                    manip->data.start = manip->data.first;
                }
                break;
            default:
                crash();
                break;
            }
            return r;
        }
    }

    r = cpu_pte_manipulate(&manip->data);
    manip->mapped = manip->data.start;
    return r;
}


/* Run the PTE-manipulation state machine for a PRIV call (kernel extension
 * body).
 *
 * Invokes do_manipulation on the supplied kcall_args blob and returns its
 * status. do_manipulation walks the VPS / split / manipulate / merge state
 * machine on a single address_t (data->adp), and multi-step PTE manipulation
 * must be atomic from any other CPU's view of the same as, otherwise the PTE
 * state machine can be corrupted into NULL+offset stores in cpu_pte_*. The
 * global as_slock (LOCK_CLASS_MEMPART) is the minimum sufficient guard; a
 * per-as lock would be finer. The same lock would be needed in
 * cpu_pte_manipulate / cpu_pte_split / cpu_pte_merge if they ever became
 * reachable from paths that don't go through ksys_manipulate. Parameters:
 *   data : pointer to a struct pte_manip (manipulation state blob)
 */
void ksys_manipulate(void *data)
{
    struct pte_manip *manip = data;
    unsigned flush_pending;

    manip->data.tlb_flush_pending = 0;
    kspinlock_lock(&as_slock);
    int r = do_manipulation(manip);
    flush_pending = manip->data.tlb_flush_pending;
    kspinlock_unlock(&as_slock);
    if (flush_pending)
        cpu_tlb_flush_complete();
    SETKSTATUS(get_active_thread(get_cpunum()), r);
}
