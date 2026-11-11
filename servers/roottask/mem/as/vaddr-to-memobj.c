/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"


/* Note: this is a generic version of the function; however, for speed, it is
 * overridden by a CPU-specific one.
 */
path_object_t *as_vaddr_to_memobj(process_t *prp,
                                  void *vaddr,
                                  paddr_t *offset,
                                  int create)
{
    paddr_t paddr;
    struct pa_quantum *pq;
    unsigned prot;

    /* RUSH3: This won't work once we start swapping out. Problem RUSH3: With
     * using object & offset though - offset is 64 bits and we RUSH3: only have
     * room for 32. Also, this is be called from the RUSH3: kernel. Will we have
     * race conditions walking the structures?
     */
    prot = cpu_as_vaddrinfo(prp, (uintptr_t) vaddr, &paddr, NULL);
#ifndef NDEBUG
    if (prot == PROT_NONE)
        crash();
#endif

    if (create) {
        /* RUSH3: Turn on a bit in the object saying that it has sync objects
         * RUSH3: in it? Would allow us to handle demand paging better.
         */

        /* whack in our sync flag, so on free we can avoid MemobjDestroy stuff
         */
        pq = pa_paddr_to_quantum(paddr);
        if (pq) {
#ifndef NDEBUG
            if (!(pa_pq_flags(pq) & PAQ_FLAG_INITIALIZED) &&
                WITHIN_BOUNDARY((uintptr_t) vaddr, (uintptr_t) vaddr,
                                user_boundary_addr)) {
                crash();
            }
#endif
            pa_pq_or_flags(pq, PAQ_FLAG_HAS_SYNC);
        }
    }

    /* Key the kernel's sync_hash on the calling process, not on the singleton
     * _syspage_ptr. On RISC-V today, cpu_as_vaddrinfo does NOT walk the user
     * page tables — it just biases the vaddr — so two independent processes
     * calling pthread_mutex_init() on the same heap address end up with
     * identical (obj, offset) keys and the second one fails with EBUSY. Keying
     * by prp makes every process's private mutexes live in its own hash bucket.
     *
     * PROCESS_SHARED mutexes aren't used anywhere in RTMux yet; when they
     * arrive, the shared-memory memobj should be returned here instead.
     */
    *offset = PADDR_TO_SYNC_OFF(paddr);
    return (path_object_t *) prp;
}
