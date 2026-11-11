/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

static const unsigned vec_offset[] = {
    offsetof(process_t, chancons),
    offsetof(process_t, fdcons),
    offsetof(process_t, threads),
    offsetof(process_t, timers),
};


void *nano_query(unsigned type,
                 unsigned index1,
                 int subtype,
                 unsigned index2,
                 unsigned *next,
                 void *objbuf,
                 size_t objsize)
{
    vector_t *vecp;
    void *obj;
    process_t *prp;

    if ((vecp = vector_search(&query_vector, type, 0))) {
        switch (type) {
        case _QUERY_PROCESS:
            if (subtype == _QUERY_PROCESS_VECTOR) {
                while (
                    (obj = prp = vector_search(vecp, PINDEX(index1), next))) {
                    if (next) {
                        if (PINDEX(prp->pid) == *next) {
                            break;
                        }
                        index1 = *next + 1;
                    } else {
                        if (prp->pid != index1) {
                            obj = 0;
                        }
                        break;
                    }
                }
            } else if (subtype >= _QUERY_PROCESS_CHANCONS &&
                       subtype <= _QUERY_PROCESS_TIMERS &&
                       (obj = prp = vector_search(vecp, PINDEX(index1), 0)) &&
                       (prp->pid == index1) && (index1 != 0)) {
                /* subtype indexes vec_offset[]; reject any value (e.g. the
                 * slot-less _QUERY_PROCESS_SYNCS, or an out-of-range one) that
                 * would read past the 4-entry table.
                 */
                obj = vector_search(
                    (vector_t
                         *) ((uintptr_t) prp +
                             vec_offset[subtype - _QUERY_PROCESS_CHANCONS]),
                    index2, next);
            } else {
                obj = NULL;
            }
            break;
        case _QUERY_INTERRUPT:
            obj = vector_search(vecp, index1, next);
            if (obj && objbuf) {
                struct interrupt_entry *iep = obj;
                struct interrupt_query_entry *iqep = objbuf;
                struct interrupt_level *ilp;

                /* Fill the query entry. The generic memcpy below copies
                 * sizeof(interrupt_entry) bytes from the kernel object into
                 * iqep->info. We then fill the extra user-visible fields (pid,
                 * tid, vector, event) that procfs reads from the query entry.
                 */
                if (!iep->handler && iep->area) {
                    iqep->event = *(struct sigevent *) iep->area;
                }
                iqep->pid = iep->thread->process->pid;
                iqep->tid = (int) (iep->thread->tid + 1);
                ilp = &interrupt_level[iep->level];
                iqep->vector =
                    ilp->info->vector_base + iep->level - ilp->level_base;

                objsize = sizeof(*iep);
            }
            break;
        default:
            obj = vector_search(vecp, index1, next);
            break;
        }

        if (obj && objbuf) {
            memcpy(objbuf, obj, objsize);
        }

        return obj;
    }

    return NULL;
}
