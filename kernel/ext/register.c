/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Startup registration kernel extension roottask invokes once at boot.
 *
 * The kernel cannot legally fetch from proc-service callbacks in roottask text
 * (PTE_U=1), so proc_hooks keeps pointing at the kernel-resident table wired in
 * init_vm_ops(). The registration only imports scalar state that the kernel
 * needs.
 */

#include <core/globals.h>

extern void **roottask_stack_cache;
extern size_t roottask_stack_size;

/* kext_register_proc_hooks — absorb roottask's startup proc-hook registration.
 *
 * Wires proc_hooks->proc_stack_code. Parameters:
 *   data : const proc_hooks_t * supplied by roottask (may be NULL)
 */
void kext_register_proc_hooks(void *data)
{
    thread_t *act = get_active_thread(get_cpunum());
    const proc_hooks_t *src = data;

    if (src) {
        proc_hooks->proc_stack_code = src->proc_stack_code;
    }
    SETKSTATUS(act, EOK);
}

struct kcall_args_register_pool_stack {
    void *vaddr;
    size_t size;
};

/* kext_register_pool_stack — push a roottask-owned stack page onto the
 * kernel-resident roottask_stack_cache free list.
 *
 * The running satp is roottask's and SUM is on, so linking through the first
 * word of the user-VA page is legal from S-mode. No new privilege is granted;
 * only memory roottask already owns is touched. Parameters:
 *   data : pointer to a struct kcall_args_register_pool_stack (vaddr + size)
 */
void kext_register_pool_stack(void *data)
{
    struct kcall_args_register_pool_stack *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    void **stk;

    if (!kap->vaddr || kap->size == 0 ||
        (uintptr_t) kap->vaddr >= KERNEL_VIRT_BASE) {
        SETKSTATUS(act, EINVAL);
        return;
    }

    /* roottask_stack_slock protects free-list push against concurrent
     * roottask_stack_alloc/free/purge operations in lifecycle.c.
     */
    kspinlock_lock(&roottask_stack_slock);

    /* First registration sets the cache size; later pushes must match because
     * roottask_stack_alloc keys cache hits on size equality.
     */
    if (roottask_stack_size == 0)
        roottask_stack_size = kap->size;
    else if (roottask_stack_size != kap->size) {
        kspinlock_unlock(&roottask_stack_slock);
        SETKSTATUS(act, EINVAL);
        return;
    }

    stk = (void **) kap->vaddr;
    *stk = roottask_stack_cache;
    roottask_stack_cache = stk;

    kspinlock_unlock(&roottask_stack_slock);
    SETKSTATUS(act, EOK);
}
