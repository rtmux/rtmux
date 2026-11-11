/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"

int as_memobj_phys(path_object_t *obp, paddr_t pa)
{
    struct pa_quantum *pq;
    int r;

    pq = pa_alloc_fake(pa, (paddr_t) obp->mem.mm.size);
    if (!pq)
        return ENOMEM;
    r = memobj_pmem_add_pq(obp, 0, pq);
    if (r != EOK) {
        pa_free_fake(pq);
    }
    return r;
}
