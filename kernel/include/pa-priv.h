/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * pa-priv.h — file-local macros and structs from servers/roottask/mem/pa.c that
 * are needed by the relocated kext_pa_* bodies in kernel/ext/pa.c.
 *
 * Originally these lived as private to pa.c. Lifted to a shared header during
 * the U-mode-roottask migration so the kernel extension bodies can be moved to
 * kernel text while still operating on the pa allocator state that remains in
 * roottask.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_KERNEL_PA_KEXT_H
#define _RTMUX_KERNEL_PA_KEXT_H

#include "as.h"

/* file-local helpers from pa.c, de-staticified for cross-boundary use */
#define ALIGN_64K 0x10000

#define FREE_LIST_END(bh, list) ((struct pa_quantum *) &(bh)->free[list])
#define FREE_LIST_EMPTY(bh, list) \
    ((bh)->free[list].next == (struct pa_quantum *) &(bh)->free[list])

#define PADDR_NEXT64K(p) ROUNDUP((p) + 1, ALIGN_64K)
#define PADDR_PREV64K(p) ROUNDDOWN((p), ALIGN_64K)

#define PAS_PREEMPT 0x01
#define PAS_EFREE 0x02
#define PAS_DONE 0x04
#define PAS_VPS_ALIGN 0x08

#define PA_MAX_RETRIES 50

#define PQ_TO_PADDR(bh, pq) \
    ((bh)->start.paddr + (((paddr_t) ((pq) - (bh)->quanta)) << QUANTUM_BITS))

/* verify_all() is a retained hook point; no live allocator debug verifier is
 * wired in.
 */
#define verify_all()

/* kcall_args structs mirror those in pa.c — keep the private call layout stable
 */
struct kcall_args_pa_alloc {
    paddr_t size;
    paddr_t resv_size;
    paddr_t align;
    unsigned color;
    unsigned flags;
    unsigned status;
    struct pa_restrict *restriction;
    struct pa_quantum *alloc;
    unsigned state;
    unsigned retries;

    /* quantum_flags_or — bits ORed into alloc->flags by the kernel extension
     * before it returns. Used by U-mode roottask to set quantum-state flags
     * (e.g. PAQ_FLAG_SYSTEM) on the allocated pq, since the pq itself lives in
     * kernel heap (PTE_U=0) and can't be modified from U-mode. Only honored by
     * the __KSYS_PRIV_PA_ALLOC_FULL path.
     */
    unsigned quantum_flags_or;
};

struct kcall_args_pa_alloc_given {
    struct pa_quantum *pq;
    size_t num;
    unsigned *status;
};

struct kcall_args_pa_free {
    struct pa_quantum *paq;
    size_t num;
    paddr_t resv_size;
};

struct kcall_args_pa_free_info {
    paddr_t run_start;
    paddr_t run_end;
    paddr_t total;
    paddr_t contig;
    struct pa_quantum *pq;
    struct block_head *bh;
    unsigned generation;
    unsigned freeq;
};

#endif
