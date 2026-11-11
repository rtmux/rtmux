/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * pa_quantum field accessors for U-mode roottask.
 *
 * struct pa_quantum lives in the kernel heap (PTE_U=0); direct field deref from
 * U-mode faults. These inline wrappers route through a single PRIV kernel
 * extension (__KSYS_PRIV_PA_PQ_OP) that does the actual read or write on the
 * kernel side.
 *
 * Use these in place of "pq->flags", "pq->blk", "pq->run", etc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROOTTASK_PA_PQ_H
#define _ROOTTASK_PA_PQ_H

#include <sys/types.h>
#include <private/priv-calls.h>
#include <private/pa-pq-op.h>

extern long syscall_priv(int slot, void *data);

/* Internal — issues the kernel extension, returns kap->result. */
static inline uintptr_t _pa_pq_op(struct pa_quantum *pq,
                                  unsigned op,
                                  uintptr_t set,
                                  uintptr_t clr)
{
    struct kcall_args_pa_pq_op kap;
    kap.pq = pq;
    kap.op = op;
    kap.set = set;
    kap.clr = clr;
    kap.result = 0;
    (void) syscall_priv(__KSYS_PRIV_PA_PQ_OP, &kap);
    return kap.result;
}

/* field-by-field accessors (replace direct pq->X derefs) */

/* pq->flags read */
static inline unsigned pa_pq_flags(struct pa_quantum *pq)
{
    return (unsigned) _pa_pq_op(pq, PA_PQ_GET_FLAGS, 0, 0);
}

/* pq->flags read-modify-write: returns OLD value; new = (old & ~clr) | set. Use
 * clr=0 for pure OR ("|="), use set=0 for pure clear ("&= ~clr").
 */
static inline unsigned pa_pq_modify_flags(struct pa_quantum *pq,
                                          unsigned set,
                                          unsigned clr)
{
    return (unsigned) _pa_pq_op(pq, PA_PQ_MODIFY_FLAGS, set, clr);
}

/* Convenience: bitwise OR (was "pq->flags |= bits"). */
static inline void pa_pq_or_flags(struct pa_quantum *pq, unsigned bits)
{
    (void) pa_pq_modify_flags(pq, bits, 0);
}

/* Convenience: bitwise AND-NOT (was "pq->flags &= ~bits"). */
static inline void pa_pq_and_not_flags(struct pa_quantum *pq, unsigned bits)
{
    (void) pa_pq_modify_flags(pq, 0, bits);
}

/* pq->blk read */
static inline int pa_pq_blk(struct pa_quantum *pq)
{
    return (int) _pa_pq_op(pq, PA_PQ_GET_BLK, 0, 0);
}

/* pq->run read (sign-preserving) */
static inline int pa_pq_run(struct pa_quantum *pq)
{
    return (int) (intptr_t) _pa_pq_op(pq, PA_PQ_GET_RUN, 0, 0);
}

/* pq->run write */
static inline void pa_pq_set_run(struct pa_quantum *pq, int run)
{
    (void) _pa_pq_op(pq, PA_PQ_SET_RUN, (uintptr_t) (intptr_t) run, 0);
}

/* pq->u.inuse.qpos read */
static inline unsigned pa_pq_inuse_qpos(struct pa_quantum *pq)
{
    return (unsigned) _pa_pq_op(pq, PA_PQ_GET_INUSE_QPOS, 0, 0);
}

/* pq->u.inuse.qpos write */
static inline void pa_pq_set_inuse_qpos(struct pa_quantum *pq, unsigned qpos)
{
    (void) _pa_pq_op(pq, PA_PQ_SET_INUSE_QPOS, (uintptr_t) qpos, 0);
}

/* pq->u.inuse.next read */
static inline struct pa_quantum *pa_pq_inuse_next(struct pa_quantum *pq)
{
    return (struct pa_quantum *) _pa_pq_op(pq, PA_PQ_GET_INUSE_NEXT, 0, 0);
}

/* pq->u.inuse.next write */
static inline void pa_pq_set_inuse_next(struct pa_quantum *pq,
                                        struct pa_quantum *next)
{
    (void) _pa_pq_op(pq, PA_PQ_SET_INUSE_NEXT, (uintptr_t) next, 0);
}

#endif /* _ROOTTASK_PA_PQ_H */
