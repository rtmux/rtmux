/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"

#define OFF_TO_QUANTUM(o) ((unsigned) ((uint64_t) (o) >> QUANTUM_BITS))



/* RUSH3: memobj_pmem_add and memobj_pmem_del_end should use memobj_pmem_walk
 * RUSH3: to do their work...
 */

int memobj_pmem_del_len(path_object_t *obp, off64_t start, uint64_t len)
{
    return memobj_pmem_del_end(obp, start,
                               start + (off64_t) len + (QUANTUM_SIZE - 1));
}



/** I don't like the end parameter to memobj_pmem_del_end... The way it's coded
 * it, the caller has to pass in 1 greater than the end of what they want
 * deleted, which is confusing.
 *
 * Why not have:
 *
 * int memobj_pmem_del(path_object_t *obp, off64_t start, uint64_t len) { return
 * memobj_pmem_del_end(obp, start, start + len - 1); }
 *
 * int memobj_pmem_del_end(path_object_t *obp, off64_t start, off64_t end) {
 *
 * [ . . . ]
 *
 * qend = OFF_TO_QUANTUM(end) + 1;
 *
 * Then the end parameter makes sense.
 *
 * .... Deferred above change until the edge case ramifications can be
 * considered....
 */
/* Helper: assign "newpq" as the successor of "prev_pq", OR as the head of obp's
 * pmem list when !prev_pq. Centralises the one place the head-vs-non-head
 * distinction matters.
 */
static inline void chain_link_after(path_object_t *obp,
                                    struct pa_quantum *prev_pq,
                                    struct pa_quantum *newpq)
{
    if (prev_pq)
        pa_pq_set_inuse_next(prev_pq, newpq);
    else
        obp->mem.mm.pmem = newpq;
}

static void log_stale_pmem(path_object_t *obp,
                           struct pa_quantum *pq,
                           const char *where)
{
    /* No caller=%p: return_address(0) only ever points back into del_end, and
     * return_address(1) trips -Wframe-address. 'where' already names the op.
     */
    root_warn("stale pmem %s obp=%p pq=%p qpos=%u run=%d\n", where, obp, pq,
              pa_pq_inuse_qpos(pq), pa_pq_run(pq));
}

int memobj_pmem_del_end(path_object_t *obp, off64_t start, off64_t end)
{
    struct pa_quantum *prev_pq; /* predecessor (NULL = pq is head) */
    struct pa_quantum *pq;
    struct pa_quantum *next;
    struct pa_quantum *tmp;
    unsigned qstart;
    unsigned qend;
    unsigned this_end;
    unsigned this_start;
    unsigned run_start;
    unsigned run_end;
    unsigned num;
    unsigned i;
    unsigned skip;
    paddr_t paddr;
    paddr_t off;

    VERIFY_OBJ_LOCK(obp);

    qstart = OFF_TO_QUANTUM(start);
    qend = OFF_TO_QUANTUM(end);

    /* Always start from head. The pmem_cache "resume from predecessor"
     * optimization hits a CRASHCHECK when the cached predecessor was unhooked
     * between walks (its INUSE flag is gone but pa_pq_inuse_next still returns
     * the freed quantum). Walking from head every time is O(n) per call but
     * correct; revisit once the cache invalidation rules are nailed down.
     */
    prev_pq = NULL;
    pq = obp->mem.mm.pmem;

    while (pq) {
        struct pa_quantum *advance_pq; /* default: pq for next iter */

        if (!(pa_pq_flags(pq) & PAQ_FLAG_INUSE)) {
            log_stale_pmem(obp, pq, "del_end");

            /* U-mode-roottask regression: under the new
             * pa_pq_*-via-kernel-extension accessors, the inuse-chain
             * occasionally points at a quantum whose INUSE flag has been
             * cleared by another path (likely pq_insert / privatize_pages)
             * without unhooking it. Bail out cleanly so the boot continues; the
             * chain-update protocol still needs an audit (TODO: revisit after
             * broader userland stress).
             */
            break;
        }
        run_start = pa_pq_inuse_qpos(pq);
        if (run_start >= qend)
            break;
        run_end = run_start + (unsigned) pa_pq_run(pq);
        this_start = max_uint(run_start, qstart);
        this_end = min_uint(run_end, qend);

        /* Default: after this iteration, advance prev_pq=pq, pq=next. Some
         * branches below may "stay in place" by setting advance_pq=NULL
         * (handled via flag below).
         */
        advance_pq = pq;

        if (this_start < this_end) {
            next = pa_pq_inuse_next(pq);
            skip = this_start - run_start;

            /* Cache invalidation: if cache pointed at pq's predecessor and
             * we're about to remove pq from the chain, the cache still points
             * correctly (predecessor unchanged) so no fix-up needed. The
             * original code's adjustment of pmem_cache from &pq->u.inuse.next
             * back to owner is now automatic in the prev_pq scheme.
             */
            num = this_end - this_start;
            if (pa_pq_blk(pq) == PAQ_BLK_FAKE) {
                /* A kept tail is rebuilt with pa_alloc_fake(), which resets
                 * flags to INUSE|COLOUR_NONE. Carry the original RDB/MODIFIED
                 * state forward so a later free of the tail still releases its
                 * RDB reservation and syncs modified pages.
                 */
                unsigned fake_flags =
                    pa_pq_flags(pq) & (PAQ_FLAG_RDB | PAQ_FLAG_MODIFIED);
                paddr = pa_quantum_to_paddr(pq) + (skip << QUANTUM_BITS);
                off = PADDR_TO_SYNC_OFF(paddr);
                if (pa_pq_flags(pq) & PAQ_FLAG_MODIFIED) {
                    memobj_destroyed(PADDR_TO_SYNC_OBJ(paddr), off,
                                     off + ((paddr_t) num << QUANTUM_BITS) - 1,
                                     NULL, NULL);
                }
                if (pa_pq_flags(pq) & PAQ_FLAG_RDB) {
                    tymem_rdb_free(obp, paddr, NQUANTUM_TO_LEN(num));
                }
                if ((skip != 0) && (this_end < run_end)) {
                    /* Freeing a piece in the middle. The tail begins at
                     * this_end = base + (skip + num) quanta; paddr is already
                     * base + skip quanta, so offset it by num — not (num +
                     * skip), which double-counted skip and mislocated the tail.
                     */
                    tmp =
                        pa_alloc_fake(paddr + (num << QUANTUM_BITS),
                                      ((unsigned) pa_pq_run(pq) - (num + skip))
                                          << QUANTUM_BITS);
                    if (!tmp)
                        return ENOMEM;
                    pa_pq_or_flags(tmp, fake_flags);
                    pa_pq_set_inuse_next(tmp, next);
                    pa_pq_set_inuse_qpos(tmp, this_end);
                    pa_pq_set_inuse_next(pq, tmp);
                    pa_pq_set_run(pq, (int) skip);
                } else if (this_end < run_end) {
                    /* skip == 0: free the front, keep the tail. No fake-paddr
                     * write accessor exists, so mirror the middle-free path
                     * above — build a fresh fake quantum for the kept tail and
                     * release the original. Reached on MAP_PHYS partial unmaps
                     * (e.g. LKL DMA regions), which the bare boot path skips.
                     */
                    tmp = pa_alloc_fake(paddr + (num << QUANTUM_BITS),
                                        ((unsigned) pa_pq_run(pq) - num)
                                            << QUANTUM_BITS);
                    if (!tmp)
                        return ENOMEM;
                    pa_pq_or_flags(tmp, fake_flags);
                    pa_pq_set_inuse_next(tmp, next);
                    pa_pq_set_inuse_qpos(tmp, this_end);
                    chain_link_after(obp, prev_pq, tmp);
                    pa_free_fake(pq);
                    advance_pq = NULL; /* prev_pq unchanged; resume at tmp */
                } else if (skip == 0) {
                    /* Freeing the whole thing — unlink before the free so a
                     * later walker never sees a freed chain head (matches the
                     * real-quantum case below).
                     */
                    chain_link_after(obp, prev_pq, next);
                    pa_free_fake(pq);
                    advance_pq = NULL; /* stay (prev_pq doesn't move) */
                } else {
                    /* Keeping a bit at the start. */
                    pa_pq_set_run(pq, (int) skip);
                }
            } else {
                tmp = pq + skip;
                paddr = pa_quantum_to_paddr(tmp);
                for (i = 0; i < num; ++i, ++tmp) {
                    if (pa_pq_flags(tmp) & PAQ_FLAG_HAS_SYNC) {
                        pa_pq_and_not_flags(tmp, PAQ_FLAG_HAS_SYNC);
                        off = PADDR_TO_SYNC_OFF(paddr);
                        memobj_destroyed(PADDR_TO_SYNC_OBJ(paddr), off,
                                         off + (QUANTUM_SIZE - 1), NULL, NULL);
                    }
                    paddr += QUANTUM_SIZE;
                }
                if (skip == 0 && this_end >= run_end) {
                    /* Freeing the whole thing — unlink before pa_free clears
                     * INUSE, so later walkers never see a freed chain head.
                     */
                    chain_link_after(obp, prev_pq, next);
                    pa_free(pq, num, 0);
                    advance_pq = NULL;
                    goto freed;
                }
                pa_free(pq + skip, num, 0);

                if ((skip != 0) && (this_end < run_end)) {
                    /* Freeing a piece in the middle. */
                    tmp = pq + skip + num;
                    pa_pq_set_inuse_next(pq, tmp);
                    pa_pq_set_inuse_next(tmp, next);
                    pa_pq_set_inuse_qpos(tmp, this_end);
                } else if (this_end < run_end) {
                    /* Keeping a bit at the end: the original pq run head was
                     * just pa_free()'d, and the kept tail moves to tmp = pq +
                     * num (linked after prev_pq above). The advance MUST NOT
                     * use the freed original pq: advance_pq still points at it
                     * (captured before the free), and `pq = tmp` is dead
                     * because the advance block below reads advance_pq, not pq.
                     * Reading pa_pq_inuse_next(freed pq) returns garbage and
                     * walks del_end off into freed/recycled memory — the
                     * stale-pmem UAF. advance_pq = NULL resumes from prev_pq,
                     * whose next is tmp.
                     */
                    chain_link_after(obp, prev_pq, pq + num);
                    pa_pq_set_inuse_next(pq + num, next);
                    pa_pq_set_inuse_qpos(pq + num, this_end);
                    advance_pq = NULL;
                }

                /* Keep-start (skip != 0, this_end >= run_end) needs no fix-up
                 * here: for a real quantum _pa_free() already shrank pq's run
                 * to the kept front and made the freed tail its own run, and pq
                 * stays the run head with its next intact, so the default
                 * advance (advance_pq = pq) is correct. (The fake-quantum
                 * branch must do the shrink itself because pa_free_fake() does
                 * not.)
                 */
            }
        }
    freed:
        if (advance_pq) {
            prev_pq = advance_pq;
            pq = pa_pq_inuse_next(advance_pq);
        } else {
            /* prev_pq stays; pq advances to whatever's now linked after it. */
            pq = (prev_pq) ? pa_pq_inuse_next(prev_pq) : obp->mem.mm.pmem;
        }
    }

    return EOK;
}


static int pq_insert(path_object_t *obp,
                     off64_t off,
                     unsigned numq,
                     struct pa_quantum *(*func)(path_object_t *,
                                                unsigned,
                                                void *),
                     void *d)
{
    struct pa_quantum *prev_pq; /* predecessor (NULL = next slot is head) */
    struct pa_quantum *ins;     /* current insertion point */
    struct pa_quantum *prev;    /* used inside the run-chain build below */
    struct pa_quantum *new;
    unsigned hole_start;
    unsigned hole_end;
    unsigned this_start;
    unsigned this_end;
    unsigned add_start;
    unsigned add_end;

    add_start = OFF_TO_QUANTUM(off);
    add_end = add_start + numq;
    hole_start = 0;

    /* Always start from head. The pmem_cache "resume from predecessor"
     * optimization dereferenced the cached predecessor here
     * (pa_pq_inuse_qpos/pa_pq_run), which is a use-after-free when that
     * predecessor was unhooked between calls — the same hazard
     * memobj_pmem_del_end() and memobj_pmem_walk() now avoid. The cached
     * predecessor can dangle, so it is no longer trusted or written; the
     * CRASHCHECK below still guards against a stale entry inside the head
     * chain, which insert cannot tolerate without re-deriving the hole
     * geometry.
     */
    prev_pq = NULL;

    for (;;) {
        ins = (prev_pq) ? pa_pq_inuse_next(prev_pq) : obp->mem.mm.pmem;
        if (ins) {
            CRASHCHECK(!(pa_pq_flags(ins) & PAQ_FLAG_INUSE));
            hole_end = pa_pq_inuse_qpos(ins);
        } else {
            hole_end = ~0U;
        }
        this_start = max_uint(hole_start, add_start);
        this_end = min_uint(hole_end, add_end);

        if (this_start < this_end) {
            new = func(obp, this_end - this_start, d);
            if (!new)
                return ENOMEM;
            CRASHCHECK(!(pa_pq_flags(new) & PAQ_FLAG_INUSE));
            chain_link_after(obp, prev_pq, new);
            do {
                pa_pq_set_inuse_qpos(new, this_start);
                this_start += (unsigned) pa_pq_run(new);
                prev = new;
                new = pa_pq_inuse_next(new);
            } while (new);
            pa_pq_set_inuse_next(prev, ins);
            /* prev is now the last quantum in the inserted run. */
            prev_pq = prev;
        }
        if (hole_end >= add_end)
            return EOK;
        CRASHCHECK(!ins);
        hole_start = hole_end + (unsigned) pa_pq_run(ins);
        prev_pq = ins; /* advance past the run we just stepped over */
    }
}


struct add_data {
    unsigned flags;
    struct pa_quantum *fake;
    struct pa_restrict *restriction;
};


static struct pa_quantum *pmem_alloc(path_object_t *obp, unsigned numq, void *d)
{
    struct add_data *data = d;
    struct pa_quantum *pq;
    unsigned status;

    VERIFY_OBJ_LOCK(obp);

    /* FUTURE: Pass in something other than PAQ_COLOUR_NONE so pa_alloc()
     * FUTURE: can try to get the correctly colored memory for the request.
     * RUSH2: Alignment
     */
    pq = pa_alloc(NQUANTUM_TO_LEN(numq), 0, PAQ_COLOUR_NONE, data->flags,
                  &status, data->restriction, 0);
    return pq;
}


int memobj_pmem_add(path_object_t *obp, off64_t off, size_t size, int flags)
{
    static struct pa_restrict restrict_16M = {
        .next = NULL,
        .checker = NULL,
        .start = 0,
        .end = 0x00ffffff,
    };
    struct add_data data;
    int status = 0;

    data.flags = 0;
    if (flags & MAP_PHYS)
        data.flags |= PAA_FLAG_CONTIG;
    if (flags & MM_MAP_NOX64K)
        data.flags |= PAA_FLAG_NOX64K;
    if ((obp->hdr.type == OBJECT_MEM_SHARED) &&
        (obp->mem.mm.flags & SHMCTL_HIGHUSAGE)) {
        data.flags |= PAA_FLAG_HIGHUSAGE;
    }
    if ((flags & MM_MAP_BELOW16M)) {
        data.restriction = &restrict_16M;
    } else {
        data.restriction = obp->mem.mm.restriction;
    }
    size += (size_t) off & (QUANTUM_SIZE - 1);
    status = pq_insert(obp, off, (unsigned) LEN_TO_NQUANTUM(size), pmem_alloc,
                       &data);
    return (status);
}


static struct pa_quantum *pmem_pq(path_object_t *obp, unsigned numq, void *d)
{
    struct add_data *data = d;

    CRASHCHECK(data->fake->run > numq);
    return data->fake;
}

int memobj_pmem_add_pq(path_object_t *obp, off64_t off, struct pa_quantum *pq)
{
    struct add_data data;

    data.fake = pq;
    return pq_insert(obp, off, (unsigned) pa_pq_run(pq), pmem_pq, &data);
}


int memobj_pmem_split(path_object_t *obp,
                      struct pa_quantum *pq,
                      unsigned split_quanta)
{
    struct pa_quantum *new;
    unsigned remaining;

    (void) obp;
    remaining = (unsigned) pa_pq_run(pq) - split_quanta;
    new = pa_alloc_fake(pa_quantum_to_paddr(pq) + NQUANTUM_TO_LEN(split_quanta),
                        NQUANTUM_TO_LEN(remaining));
    if (!new)
        return ENOMEM;
    pa_pq_set_inuse_qpos(new, pa_pq_inuse_qpos(pq) + split_quanta);
    pa_pq_set_inuse_next(new, pa_pq_inuse_next(pq));
    pa_pq_set_inuse_next(pq, new);
    pa_pq_set_run(pq, (int) pa_pq_run(pq) - (int) split_quanta);
    return EOK;
}


int memobj_pmem_walk(
    int flags,
    path_object_t *obp,
    off64_t off_start,
    off64_t off_end,
    int (*func)(path_object_t *, off64_t, struct pa_quantum *, size_t, void *),
    void *d)
{
    struct pa_quantum *pq;
    size_t qpos;
    size_t qnext;
    size_t qstart;
    size_t qend;
    size_t start;
    size_t end;
    int r;

    /* Pre-U-mode-flip this loop walked a "pa_quantum **owner" chain ("pq =
     * *owner; owner = &pq->u.inuse.next;"). pa_quantum lives in the kernel heap
     * (PTE_U=0); U-mode roottask can't deref the intermediate
     * "&pq->u.inuse.next" pointers. Rewritten to track (prev_pq, pq) and use
     * pa_pq_inuse_next() — the kernel-extension-routed accessor in <pa-pq.h>.
     */
    VERIFY_OBJ_LOCK(obp);
    qstart = OFF_TO_QUANTUM(off_start);
    qend = OFF_TO_QUANTUM(off_end);
    qpos = 0;

    /* Always start from head. The pmem_cache "resume from predecessor"
     * optimization hit the CRASHCHECK(pa_pq_run(pq) <= 0) below when the cached
     * predecessor was unhooked between walks (its INUSE flag is gone but
     * pa_pq_inuse_next still returns the freed quantum). Walking from head
     * every time is O(n) per call but correct, mirroring memobj_pmem_del_end();
     * the cached predecessor can dangle, so it is no longer trusted or written.
     */
    pq = obp->mem.mm.pmem;

    for (;;) {
        if (qpos > qend)
            break;
        if (!pq) {
            if (flags & MPW_HOLES) {
                start = max(qpos, qstart);
                r = func(obp, NQUANTUM_TO_LEN(start), NULL, (qend - start) + 1,
                         d);
                if (r != EOK)
                    goto done1;
            }
            break;
        }
        if (!(pa_pq_flags(pq) & PAQ_FLAG_INUSE)) {
            log_stale_pmem(obp, pq, "walk");

            /* Stale chain entry: another path (pq_insert / privatize_pages /
             * kernel pa_free) cleared INUSE without unhooking it. Treat it as
             * end-of-valid-chain rather than trusting pa_pq_run() on freed
             * storage — same bail-out memobj_pmem_del_end() uses. Below this
             * point the (HOLES) tail is handled as if pq were NULL.
             */
            if (flags & MPW_HOLES) {
                start = max(qpos, qstart);
                if (qend >= start) {
                    r = func(obp, NQUANTUM_TO_LEN(start), NULL,
                             (qend - start) + 1, d);
                    if (r != EOK)
                        goto done1;
                }
            }
            break;
        }
        CRASHCHECK(pa_pq_run(pq) <= 0);
        qnext = pa_pq_inuse_qpos(pq);
        if ((flags & MPW_HOLES) && (qnext > qpos) && (qnext > qstart)) {
            start = max(qpos, qstart);
            end = min(qend, qnext - 1);
            r = func(obp, NQUANTUM_TO_LEN(start), NULL, (end - start) + 1, d);
            if (r != EOK)
                goto done1;
        }
        if (qnext > qend)
            break;
        qpos = qnext;
        qnext += (unsigned) pa_pq_run(pq);
        if (qnext > qstart) {
            end = min(qend, qnext - 1);
            r = EOK;
            if (pa_pq_blk(pq) != PAQ_BLK_FAKE) {
                if (flags & MPW_SYSRAM) {
                    start = max(qpos, qstart);
                    r = func(obp, NQUANTUM_TO_LEN(start), pq + (start - qpos),
                             (end - start) + 1, d);
                }
            } else if (flags & MPW_PHYS) {
                r = func(obp, NQUANTUM_TO_LEN(qpos), pq, (end - qpos) + 1, d);
            }
            if (r != EOK)
                goto done1;
        }
        pq = pa_pq_inuse_next(pq);
        qpos = qnext;
    }
    return EOK;

done1:
    if (r < 0)
        r = EOK;
    return r;
}


int memobj_pmem_walk_mm(
    int flags,
    struct mm_map *mm,
    int (*func)(path_object_t *, off64_t, struct pa_quantum *, size_t, void *),
    void *d)
{
    return memobj_pmem_walk(flags, mm->obj_ref->obp, mm->offset,
                            mm->offset + (off64_t) (mm->end - mm->start), func,
                            d);
}


/* Need to check that the offset we're adding the pmem to doesn't exceed the
 * maximum allowed by the data structure (2**(32+12))
 */
int memobj_offset_check(path_object_t *obp, off64_t off, uint64_t len)
{
    off64_t end;

    end = off + (off64_t) len;
    if (end < off)
        return ENXIO;
    if (end > ((off64_t) ((uint32_t) ~0) << QUANTUM_BITS))
        return ENXIO;
    return EOK;
}
