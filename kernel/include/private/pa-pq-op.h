/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * pa_quantum accessor operations (kernel <-> roottask shared header).
 *
 * struct pa_quantum lives in the kernel heap; with PTE_U=0 enforced on the
 * heap, U-mode roottask cannot read/write its fields directly. All field access
 * from roottask now goes through __KSYS_PRIV_PA_PQ_OP — this header defines the
 * op selector and the kcall_args struct used for the dispatch. The inline
 * wrappers in servers/roottask/include/pa-pq.h call into it with the right op
 * for each field.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_PA_PQ_OP_H
#define _SYS_PA_PQ_OP_H

#include <stdint.h>

struct pa_quantum; /* opaque on the roottask side */

enum pa_pq_op_code {
    PA_PQ_GET_FLAGS,    /* result = pq->flags */
    PA_PQ_MODIFY_FLAGS, /* result = old flags; pq->flags = (old & ~clr) | set */
    PA_PQ_GET_BLK,      /* result = pq->blk */
    PA_PQ_GET_RUN,      /* result = (int32_t) pq->run  (sign-extended) */
    PA_PQ_SET_RUN,      /* pq->run = (int32_t) set */
    PA_PQ_GET_INUSE_QPOS, /* result = pq->u.inuse.qpos */
    PA_PQ_SET_INUSE_QPOS, /* pq->u.inuse.qpos = (uint32_t) set */
    PA_PQ_GET_INUSE_NEXT, /* result = (uintptr_t) pq->u.inuse.next */
    PA_PQ_SET_INUSE_NEXT, /* pq->u.inuse.next = (struct pa_quantum *) set */
    PA_PQ_GET_FAKE_PADDR, /* result = ((pa_quantum_fake *) pq)->paddr (only when
                             blk == PAQ_BLK_FAKE)
                             */
};

struct kcall_args_pa_pq_op {
    struct pa_quantum *pq;
    unsigned op;      /* enum pa_pq_op_code */
    uintptr_t set;    /* in: bits to set / value to write */
    uintptr_t clr;    /* in: bits to clear (MODIFY ops only) */
    uintptr_t result; /* out */
};

#endif /* _SYS_PA_PQ_OP_H */
