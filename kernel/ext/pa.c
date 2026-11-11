/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Physical memory allocator, kernel-resident: physical memory belongs in the
 * kernel by every microkernel convention (Linux buddy, FreeBSD vm_phys, seL4
 * untyped). kext_pa_* bodies call the helpers in this file directly; roottask
 * keeps thin syscall_priv wrappers in servers/roottask/mem/pa.c.
 */

#define EXTERN

#include <alloca.h>
#include <core/globals.h>
#include "as.h"
#include "pa-priv.h"
#include "mm-xboundary.h"
#include "log.h"

/* Kernel-side spelling of the roottask log helper used in this file. The kernel
 * logs directly; roottask's own root_log() lives in
 * servers/roottask/sys/root-log.c and must not be referenced from kernel code.
 */
#define root_dbg(fmt, ...) LOGF_D(fmt, ##__VA_ARGS__)

#define TAIL_CALC(_runsize) (-((_runsize) - 1))
#define SET_TAIL(_qp, _runsize) \
    if ((_qp)->run > 1)         \
    ((_qp)[(_qp)->run - 1].run = (_runsize))
#define NON_HEAD_QUANTUM (0)
#define IS_BACKQ(run) (run < NON_HEAD_QUANTUM)
#define IS_HEADQ(run) (run > NON_HEAD_QUANTUM)
/* ALIGN_64K, FREE_LIST_END, FREE_LIST_EMPTY moved to pa-priv.h */

#if !defined(NDEBUG)
#define INLINE
#else /* NDEBUG */
#define INLINE __inline__
#endif /* NDEBUG */

/* These are the bits that have to be preserved when the quantum is freed */
#define PAQ_FLAG_PRESERVE_BITS (PAQ_FLAG_INIT_REQUIRED | PAQ_FLAG_COLOUR_MASK)

struct sysram_entry {
    paddr_t start;
    paddr_t end;
};

struct sysram_list {
    unsigned count;
    struct sysram_entry list[1]; /* variable size */
};

/* pa state — kernel-owned. Roottask read-access goes through
 * __KSYS_PRIV_PA_GET_SIZES.
 */
struct block_head **blk_head;
static paddr_t sysram_end;
paddr_t mem_free_size;
memsize_t mem_reserved_size;
static memsize_t mem_reserved_configured;
paddr_t last_paddr;
unsigned mm_flags;
struct pa_restrict *restrict_proc;
struct pa_restrict *restrict_user;
struct vaddr_range system_heap_vaddr;
uintptr_t pgszlist[sizeof(uintptr_t) * 8 + 1];

void _pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size);

static memclass_attr_t sysram_attr = {
    .size = 0, /* filled in in pa_init() */
    .limits = {.alloc = {.size = {.min = __PAGESIZE,
                                  .max = memsize_t_INFINITY}}},
};


static const struct kdump_private kdump_private = {
    &blk_head,
    NULL,
    offsetof(address_t, cpu.ptroot_paddr),
    sizeof(paddr_t),
};

#define dump_all()
#define verify_blk(a1)
#define verify_all()

/* early_alloc — carve a one-shot allocation from system RAM during pa_init,
 * before the allocator's own structures exist.
 *
 * Picks the largest sysram region below CPU_SYSTEM_PADDR_END and returns its
 * kernel-virtual base. crash()es if no region is large enough. Parameters:
 *   sl   : the sorted sysram list
 *   size : number of bytes needed (rounded up to 8)
 * Return value: kernel-virtual pointer to the allocated region.
 */
static void *early_alloc(struct sysram_list *sl, size_t size)
{
    unsigned max = 0;
    paddr_t max_size = 0;
    paddr_t curr;
    paddr_t end;
    unsigned i;

    size = ROUNDUP(size, sizeof(uint64_t));

#if CPU_SYSTEM_PADDR_START != 0
#error code does not support CPU_SYSTEM_PADDR_START != 0
#endif

    for (i = 0; i < sl->count; ++i) {
        if (sl->list[i].start < CPU_SYSTEM_PADDR_END) {
            end = sl->list[i].end;
            if (end > CPU_SYSTEM_PADDR_END)
                end = CPU_SYSTEM_PADDR_END;
            curr = end - sl->list[i].start;
            if (curr > max_size) {
                max_size = curr;
                max = i;
            }
        }
    }
    if (size > max_size) {
        crash();
    }

    /* Kernel-resident pa_init runs after the direct kernel virtual mapping is
     * live, so the early mapping is just the direct-map VA.
     */
    (void) size;
    return (void *) (sl->list[max].start + KERNEL_VIRT_BASE);
}


/* Restrict ranges to allowed values (think 32-bit paddr_t's) */
#define TOP_PADDR (~(paddr_t) 0)

/* find_last_paddr — walk_asinfo callback that records the highest physical
 * address backed by RAM.
 *
 * Sets the global last_paddr from the first AS_NULL_OFF entry (clamped to
 * TOP_PADDR) and stops the walk. Parameters:
 *   as   : the current asinfo entry
 *   name : entry name (unused)
 *   data : unused
 * Return value: 0 to stop the walk once found, 1 to continue.
 */
static int find_last_paddr(struct asinfo_entry *as, char *name, void *data)
{
    paddr64_t end;

    if (as->owner == AS_NULL_OFF) {
        end = as->end;
        if (end > TOP_PADDR)
            end = TOP_PADDR;
        last_paddr = end;
        return 0;
    }
    return 1;
}


/* make_sysram_list — walk_asinfo callback that builds the sorted list of usable
 * system-RAM ranges.
 *
 * Clamps each range to last_paddr, rounds to quantum boundaries, inserts it
 * into the list in descending start order, and splits any range that straddles
 * CPU_SYSTEM_PADDR_END so allocations need not run a long free list before
 * reaching user-restricted memory. Parameters:
 *   as   : the current asinfo entry
 *   name : entry name (unused)
 *   d    : struct sysram_list * being built
 * Return value: 1 to continue the walk.
 */
static int make_sysram_list(struct asinfo_entry *as, char *name, void *d)
{
    struct sysram_list *data = d;
    unsigned i;
    struct sysram_entry se;
    paddr64_t start_addr;
    paddr64_t end_addr;

    start_addr = as->start;
    if (start_addr <= last_paddr) {
        end_addr = as->end;
        if (end_addr > last_paddr)
            end_addr = last_paddr;

        /* Round everything to quantum boundries */
        se.start = ROUNDUP(start_addr, QUANTUM_SIZE);
        se.end = ROUNDDOWN(end_addr + 1, QUANTUM_SIZE) - 1;

        if (se.start < se.end) {
            /* Sort the list of sysram's into descending order. Ignore anything
             * smaller than a page.
             */
            i = data->count;
            for (;;) {
                if (i == 0)
                    break;
                if (data->list[i - 1].start > se.start)
                    break;
                data->list[i] = data->list[i - 1];
                --i;
            }
            data->list[i] = se;
            data->count++;
            if ((se.start < (CPU_SYSTEM_PADDR_END + 1)) &&
                (se.end > CPU_SYSTEM_PADDR_END)) {
                /* One block crosses from the region that roottask can reference
                 * to the region that we're going to prefer for user space
                 * allocations. Split into two so that the allocator doesn't
                 * have to run a long free list in the roottask region before
                 * reaching something that works for the user restriction list.
                 * We know we have enough space in the sysram_list to handle the
                 * splitting since we allocated enough entries to deal with one
                 * per asinfo and some asinfo entries aren't going to be
                 * "sysram"'s.
                 */
                data->list[i].end = CPU_SYSTEM_PADDR_END;
                se.start = CPU_SYSTEM_PADDR_END + 1;
                ++i;
                memmove(&data->list[i + 1], &data->list[i],
                        (data->count - i) * sizeof(se));
                data->list[i] = se;
                data->count++;
            }
            if (se.end > sysram_end)
                sysram_end = se.end;
        }
    }

    return 1;
}


/* dequeue_run — unlink a complete run from whatever free queue it is on.
 * Parameters:
 *   bh  : the run's block head (unused; runs carry their own links)
 *   rtf : the run to dequeue
 * Return value: rtf.
 */
static struct pa_quantum *dequeue_run(struct block_head *bh,
                                      struct pa_quantum *rtf)
{
    rtf->u.flink.prev->u.flink.next = rtf->u.flink.next;
    rtf->u.flink.next->u.flink.prev = rtf->u.flink.prev;

    return rtf;
}


/* enqueue_run — add a free run to the appropriate free queue.
 *
 * Small runs go to their fixed-size queue; larger runs go to the head of the
 * default queue when at least as large as the cached max_free_run (updating
 * it), otherwise to its tail. Parameters:
 *   bh  : the run's block head
 *   rtq : the run to enqueue
 */
static void enqueue_run(struct block_head *bh, struct pa_quantum *rtq)
{
    int32_t run = rtq->run;

    if (run < PA_FREE_QUEUES) {
        /* add to head of individual free queue; they're all the same size */
        rtq->u.flink.next = bh->free[run].next;
        rtq->u.flink.prev = FREE_LIST_END(bh, run);
        bh->free[run].next->u.flink.prev = rtq;
        bh->free[run].next = rtq;
    } else {
        /* If the new entry size is >= than what we think is the largest, add it
         * in front, otherwise add to the end. We are more likely to be able to
         * satisfy large requests with the largest pieces of free memory We use
         * the cached max_free_run to decide this, so no cunning is required.
         */

        if (run >= bh->max_free_run) {
            /* add to head of default queue */
            rtq->u.flink.next = bh->free[PA_DEFAULT_QUEUE].next;
            rtq->u.flink.prev = FREE_LIST_END(bh, PA_DEFAULT_QUEUE);
            bh->free[PA_DEFAULT_QUEUE].next->u.flink.prev = rtq;
            bh->free[PA_DEFAULT_QUEUE].next = rtq;

            bh->max_free_run = (size_t) run;
        } else {
            /* add to tail of default queue */
            rtq->u.flink.next = FREE_LIST_END(bh, PA_DEFAULT_QUEUE);
            rtq->u.flink.prev = bh->free[PA_DEFAULT_QUEUE].prev;
            bh->free[PA_DEFAULT_QUEUE].prev->u.flink.next = rtq;
            bh->free[PA_DEFAULT_QUEUE].prev = rtq;
        }
    }
    return;
}


/* init_block — initialize one block head and its quantum array.
 *
 * Records the block's paddr range, accounts its size into the free / sysram
 * totals, lays out the per-quantum descriptors as one big free run, initializes
 * the free lists, and enqueues the run. Parameters:
 *   bhp     : the block-head pointer array
 *   blk_idx : index of this block within bhp
 *   se      : the sysram range backing this block
 *   quanta  : storage for this block's pa_quantum descriptors
 * Return value: the number of quanta in the block.
 */
static size_t init_block(struct block_head **bhp,
                         unsigned blk_idx,
                         struct sysram_entry *se,
                         struct pa_quantum *quanta)
{
    size_t num;
    struct pa_quantum *pq;
    unsigned i;
    paddr_t block_size;
    struct block_head *bh = bhp[blk_idx];

    bh->start.paddr = se->start;
    bh->end.paddr = se->end;
    block_size = (bh->end.paddr - bh->start.paddr) + 1;
    mem_free_size += block_size;
    sysram_attr.size += block_size;

    bh->generation = 0;
    num = block_size >> QUANTUM_BITS;
    bh->num_to_clean = num;
    bh->num_quanta = num;
    bh->quanta = quanta;
    pq = quanta;
    for (i = 0; i < num; ++i, ++pq) {
        if (((i % 1024) == 0) && (bhp == blk_head)) {
            /* Initial initialization (from pa_init). */
        }
        pq->flags = PAQ_COLOUR_NONE;
        pq->blk = (uint16_t) blk_idx;
        pq->u.flink.next = NULL;
        pq->u.flink.prev = NULL;
        pq->run = NON_HEAD_QUANTUM;
    }

    quanta->run = (int32_t) num;

    /* Store the tail marker as a negative displacement back to the run head. */
    SET_TAIL(quanta, TAIL_CALC(quanta->run));


    /* Initialize the free lists */
    for (i = 0; i < PA_FREE_QUEUES; i++) {
        bh->free[i].next = FREE_LIST_END(bh, i);
        bh->free[i].prev = FREE_LIST_END(bh, i);
    }

    /* Put the full quanta run on the free list. */
    enqueue_run(bh, quanta);
    bh->max_free_run = num;

    return num;
}

/* pa_init — initialize the physical allocator from the system page.
 *
 * Builds the sysram list, one-shot-allocates all allocator structures (quantum
 * arrays, block heads, restriction lists), initializes every block, marks the
 * early-allocation region as in-use, and sets up the page-size list.
 * Parameters:
 *   colors : number of cache colors (must be < PAQ_FLAG_COLOUR_MASK)
 */
void pa_init(unsigned colors)
{
    struct sysram_list *sl;
    unsigned num_asinfos;
    unsigned i;
    size_t num_quanta;
    struct pa_quantum *quanta;
    struct pa_restrict *res;
    void *early;
    void *early_start;
    size_t early_size;
    paddr_t early_paddr;
    struct block_head *blk;

#define ADVANCE_EARLY(size) (early = (uint8_t *) early + (size))

    if (colors >= PAQ_FLAG_COLOUR_MASK) {
        kprintf("%d exceeds supported color limit\n", colors);
        crash();
    }

    walk_asinfo("memory", find_last_paddr, NULL);
    root_dbg("pa_init: last_paddr=%lx\n", (unsigned long) last_paddr);

    num_asinfos = _syspage_ptr->asinfo.entry_size / sizeof(struct asinfo_entry);
    sl = _alloca(sizeof(*sl) + sizeof(sl->list[0]) * num_asinfos);
    sl->count = 0;
    walk_asinfo("sysram", make_sysram_list, sl);

    root_dbg("pa_init: %u sysram blocks\n", sl->count);

    /* Allocate one extra quantum for zero sized allocations */
    num_quanta = 1;
    for (i = 0; i < sl->count; ++i) {
        num_quanta +=
            ((sl->list[i].end - sl->list[i].start) + 1) >> QUANTUM_BITS;
    }

    /* Calculate how much memory we need for all the early allocation structures
     * so we can do just one big alloc - keeps things contiguous and simple
     */

    /* Memory needed for pa_quantum's */
    early_size = num_quanta * sizeof(*quanta);
    /* Memory for block head array. */
    early_size += (sl->count + 1) * sizeof(*blk_head);
    /* Memory for block head entries. */
    early_size += sl->count * sizeof(**blk_head);
    /* Memory for restriction lists (restrict_proc + restrict_user) */
    early_size += sizeof(*res) * ((2 - CPU_SYSTEM_PADDR_MUST) + 1);

    early = early_start = early_alloc(sl, early_size);

    /* Init the default restriction lists. */
    res = early;
    ADVANCE_EARLY(sizeof(*res) * (2 - CPU_SYSTEM_PADDR_MUST));
    restrict_proc = res;
    res->start = CPU_SYSTEM_PADDR_START;
    res->end = CPU_SYSTEM_PADDR_END;
    res->checker = 0;
#if !CPU_SYSTEM_PADDR_MUST
    res->next = &res[1];
    ++res;
    res->start = 0;
    res->end = ~(paddr_t) 0;
    res->checker = 0;
#endif
    res->next = NULL;

#if CPU_SYSTEM_PADDR_START != 0
#error code does not support CPU_SYSTEM_PADDR_START != 0
#endif
    res = early;
    ADVANCE_EARLY(sizeof(*res));
    restrict_user = res;

    res->start = 0;
    res->end = last_paddr;
    res->checker = 0;
    res->next = NULL;

    quanta = early;
    ADVANCE_EARLY(num_quanta * sizeof(*quanta));

    blk = early;
    ADVANCE_EARLY(sl->count * sizeof(**blk_head));
    blk_head = early;
    i = 0;
    do {
        blk_head[i] = blk++;
    } while (++i < sl->count);
    blk_head[i] = NULL;
    CRASHCHECK(((uint8_t *) &blk_head[i] - (uint8_t *) early_start) >
               early_size);

    for (i = 0; i < sl->count; ++i) {
        quanta += init_block(blk_head, i, &sl->list[i], quanta);
        if (i == 0) {
            /* Allocate an extra quanta at the end of the first block that we
             * can use to handle zero size physical allocations
             */
            quanta->flags = PAQ_COLOUR_NONE;
            quanta->blk = 0;
            quanta->u.flink.next = NULL;
            quanta->u.flink.prev = NULL;
            quanta->run = 0;
            ++quanta;
        }
    }
    CRASHCHECK((void *) quanta > (void *) blk_head[0]);

    /* The early allocation area is in one of the blocks - we need to mark it as
     * allocated so that it doesn't get re-used. Keeping the area in the phys
     * allocator structures simpifies things for the kernel dumper and
     * as_mapinfo() code.
     */

    (void) cpu_as_vaddrinfo(NULL, (uintptr_t) early_start, &early_paddr, NULL);
    quanta = pa_paddr_to_quantum(early_paddr);
    CRASHCHECK(!quanta);
    (void) pa_alloc_given(quanta, LEN_TO_NQUANTUM(early_size), NULL);
    quanta->flags |= PAQ_FLAG_SYSTEM;
    system_heap_vaddr.lo = (uintptr_t) early_start;
    system_heap_vaddr.hi = (uintptr_t) early_start + early_size - 1;

    verify_all();

    if (pgszlist[0] == 0) {
        /* Supported page sizes hasn't been set up yet, so just assume a default
         * for right now.
         */
        pgszlist[0] = __PAGESIZE;
    }
}


/* pa_start_background — start the background page-cleaning thread.
 *
 * Background page scrubbing is not active yet. Allocation paths therefore
 * report PAA_STATUS_NOT_ZEROED conservatively and callers clear pages before
 * exposing them to user mode.
 */
void pa_start_background(void)
{
    (void) kdump_private;
}

/* Large machines can have allocation runs bigger than 2 GiB, so run arithmetic
 * must use paddr_t-sized offsets instead of signed pointer differences.
 */

/* pa_quantum_to_paddr — convert a quantum descriptor to its physical address.
 * Parameters:
 *   pq : the quantum (PAQ_BLK_FAKE quanta carry an embedded paddr)
 * Return value: the physical address of the quantum.
 */
paddr_t pa_quantum_to_paddr(struct pa_quantum *pq)
{
    struct block_head *bh;

    if (pq->blk == PAQ_BLK_FAKE) {
        return ((struct pa_quantum_fake *) pq)->paddr;
    }

    bh = blk_head[pq->blk];
    return PQ_TO_PADDR(bh, pq);
}

/* pa_paddr_to_quantum — convert a physical address to its quantum descriptor.
 *
 * Searches the block heads (resuming from the last-hit block) for the one
 * containing paddr. Parameters:
 *   paddr : the physical address to locate
 * Return value: the containing quantum, or NULL if paddr is not in any
 *               block.
 */
struct pa_quantum *pa_paddr_to_quantum(paddr_t paddr)
{
    struct block_head *bh;
    struct block_head **bhp;
    struct block_head **rover;
    struct block_head **start;
    static unsigned last_blk;

    bhp = blk_head;
    start = rover = bhp + last_blk;
    do {
        bh = *rover;
        if ((paddr >= bh->start.paddr) && (paddr <= bh->end.paddr)) {
            last_blk = (unsigned) (rover - bhp);
            return &bh->quanta[((paddr_t) (paddr - bh->start.paddr)) >>
                               QUANTUM_BITS];
        }
        if (!*++rover)
            rover = bhp;
    } while (rover != start);

    return NULL;
}

/* pa_getrunhead — find the head quantum of the run containing pq. Parameters:
 *   pq : any quantum within a run
 * Return value: the head quantum of the run.
 */
static struct pa_quantum *pa_getrunhead(struct pa_quantum *pq)
{
    struct pa_quantum *curr = pq;

    if (IS_BACKQ(curr->run))
        return curr + curr->run;
    if (IS_HEADQ(curr->run))
        return curr;

    /* Walk left across interior quanta until the run head is reached. */
    for (curr--; curr->run == NON_HEAD_QUANTUM; curr--) {
        /* nothing to do */
    }
    return curr;
}



/* pa_carve — carve a contiguous run of quanta out of a free run.
 *
 * Removes "head" from its free queue and splits it so that the "len" bytes
 * starting at "start" become an in-use run, re-enqueueing the left/right
 * remainders. "start" need not be the head of the run. Parameters:
 *   bhp    : the block head owning the run
 *   head   : the head quantum of the free run being carved
 *   start  : the first quantum of the carved-out region
 *   len    : carve length in bytes (rounded up to QUANTUM_SIZE)
 *   color : requested color (currently unused)
 * Return value: status bits (PAA_STATUS_NOT_ZEROED |
 *               PAA_STATUS_COLOUR_MISMATCH).
 */
static unsigned pa_carve(struct block_head *bhp,
                         struct pa_quantum *head,
                         struct pa_quantum *start,
                         size_t len,
                         unsigned *color)
{
    struct pa_quantum *lhs;
    struct pa_quantum *rhs;
    int32_t total_runlen;
    int32_t nrun;
    unsigned status = 0;

    verify_all();

    CRASHCHECK(!start);

    len = ROUNDUP(len, QUANTUM_SIZE);
    nrun = (int32_t) (len >> QUANTUM_BITS);

    /* can only check head of the run for flags due to run optimizations */
    CRASHCHECK(head->flags & PAQ_FLAG_INUSE);

    total_runlen = head->run;

    /* Linked list structure: each run is a doubly linked list element with next
     * ("N") and prev ("P") pointers; the chain threads through the head and any
     * internal quanta of the run. Run carving handles three shapes: (a) cut off
     * the top of a run, (b) cut from the middle, (c) cut from the end. See
     * verify_blk() for the post-conditions each shape leaves the chain in.
     */

    (void) dequeue_run(bhp, head);

    if (IS_HEADQ(start->run)) {
        start->run = nrun;
        SET_TAIL(start, TAIL_CALC(start->run));

        if (total_runlen == nrun) {
            /* taking whole run */

        } else {
            rhs = start + nrun;

            rhs->run =
                total_runlen - nrun; /* just set the head to some value */
            rhs->flags &=
                (uint16_t) ~PAQ_FLAG_INUSE; /* turn off the inuse flag that
                                               might be set (since rhs is made
                                               from the middle
                                               */
            SET_TAIL(rhs, TAIL_CALC(rhs->run));

            enqueue_run(bhp, rhs);
        }

    } else if (&head[head->run] == &start[nrun]) {
        /* carve from the end */
        head->run -= nrun;
        start->run = nrun;
        SET_TAIL(start, TAIL_CALC(start->run));
        SET_TAIL(head, TAIL_CALC(head->run));

        enqueue_run(bhp, head);
    } else {
        /* carve from middle */
        lhs = head;
        rhs = start + nrun;

        /* Remake the runs in the middle and RHS. For the LHS just stuff the new
         * runlen since the neg. displacements are ok.
         */
        lhs->run = (int32_t) (start - lhs);
        SET_TAIL(lhs, TAIL_CALC(lhs->run));

        start->run = nrun;
        SET_TAIL(start, TAIL_CALC(start->run));

        rhs->run = total_runlen - (lhs->run + nrun);
        SET_TAIL(rhs, TAIL_CALC(rhs->run));

        /* relink LHS and RHS */
        enqueue_run(bhp, rhs);
        enqueue_run(bhp, lhs);
    }

    atomic_add(&bhp->generation, 1);

    start->u.flink.next = NULL;
    start->u.flink.prev = NULL;
    start->flags |= PAQ_FLAG_INUSE;

    verify_blk(blk_head[start->blk]);

    /* Interior quanta do not retain zero-state metadata after run compaction,
     * so callers must treat carved ranges as needing zeroing and color repair.
     */
    status |= PAA_STATUS_NOT_ZEROED | PAA_STATUS_COLOUR_MISMATCH;

    return status;
}

/* pa_largest_pow2 — largest power-of-two no greater than size. Parameters:
 *   size : the value to round down
 * Return value: the highest set bit of size, as a power of two.
 */
static paddr_t pa_largest_pow2(paddr_t size)
{
    paddr_t max;

    for (

        /* Optimization: check if upper 32 bits have any bits; if not, skip that
         * half
         */
        max = ((size >> 32) & 0xffffffffU)
                  ? ((paddr_t) 0x1 << (_PADDR_BITS - 1))
                  : ((paddr_t) 0x1 << 32);
        !(max & size); max >>= 1) {
        /* nothing to do */
    }
    return max;
}


/* Allocate some physical memory. There are several conditions that must be
 * satisfied for usable memory to result:
 *
 *	1. Be within the given restriction list
 * 2. Satisfy contiguous/alignment/size requirements.
 * 3. Conditioned by the 'checker' function of the restriction entry
 *		(not currently implemented).
 *	4. Match the requested color.
 * 5. Contain all zeros.
 *
 * The last two conditions aren't enforced by this code since they're
 * correctable. The pa_alloc() function merely indicates whether they're
 * satisfied or not with the PAA_STATUS_COLOUR_MISMATCH and
 * PAA_STATUS_NOT_ZEROED bits. The upper level code is responsible for dealing
 * with them as needed.
 */

/* PADDR_NEXT64K, PADDR_PREV64K, PAS_*, PA_MAX_RETRIES moved to pa-priv.h */


/* kext_pa_alloc_full — run the full pa_alloc() flow (retry loop, restriction
 * walk, size==0 short-circuit) on behalf of U-mode roottask.
 *
 * Keeps the entire allocation flow kernel-side, since blk_head[] and the
 * restriction chain live in the kernel heap (PTE_U=0) and cannot be
 * dereferenced from U-mode. The result (status / alloc) comes back via the
 * kcall_args_pa_alloc struct. Parameters:
 *   d : struct kcall_args_pa_alloc * (size, align, color, flags,
 *       restriction, resv_size, quantum_flags_or; status/alloc written)
 */
void kext_pa_alloc_full(void *d)
{
    struct kcall_args_pa_alloc *kap = d;
    thread_t *act = get_active_thread(get_cpunum());
    unsigned status = 0;

    kap->alloc = pa_alloc(kap->size, kap->align, kap->color, kap->flags,
                          &status, kap->restriction, kap->resv_size);
    kap->status = status;

    /* Apply quantum_flags_or to the freshly-allocated pq before returning to
     * roottask — roottask cannot modify pq->flags itself since pa_quantum lives
     * in the kernel heap.
     */
    if (kap->alloc && kap->quantum_flags_or != 0) {
        kap->alloc->flags |= (uint16_t) kap->quantum_flags_or;
    }
    kap->state |= PAS_DONE;
    SETKSTATUS(act, EOK);
}


/* pa_alloc_body — the heart of the physical allocator. */
static void pa_alloc_body(void *d)
{
    struct kcall_args_pa_alloc *kap = d;
    paddr_t size = kap->size;
    const paddr_t resv_size = kap->resv_size;
    paddr_t curr_resv_size = resv_size;
    paddr_t align = kap->align;
    unsigned color = kap->color;
    unsigned flags = kap->flags;
    struct pa_restrict *curr_res;
    struct pa_quantum *qp, *rq_start, *rq_end;
    struct pa_quantum *prev_qp;
    struct pa_quantum *mod_rq_start, *mod_rq_end;
    struct pa_quantum *qblk_start, *qblk_end;
    struct pa_quantum *found_run, **tail;
    struct block_head *bhp;
    struct block_head **rover;
    paddr_t curr_size, rp_start, rp_end;
    paddr_t size_left;
    int valid_restrict;
    size_t current_q;
    struct pa_quantum *free_list_end;
    size_t max_blk_free_run;
    uintptr_t *pgsz;

    verify_all();

    /* Always lock during allocation — the search path has a TOCTOU race: two
     * concurrent allocations can find and carve the same free run, causing
     * physical page double-allocation. TODO: re-validate found_run after
     * re-acquiring the lock instead of holding it for the entire search.
     *
     * pa_slock is held across the whole body — the free-run search + carve +
     * mem_free_size update must be atomic against a concurrent _pa_free on
     * another hart (which would corrupt the bh free chains -> dequeue_run NULL
     * deref). Single exit via the "done:" tail, so one unlock suffices.
     */
    kext_lock();
    kspinlock_lock(&pa_slock);

    tail = &kap->alloc;
    /* Get 'tail' to end of allocated list, (we've preempted and restarted) */
    while (*tail) {
        tail = &(*tail)->u.flink.next;
    }

    /* Check aggregate availability before walking the free lists. pa_slock
     * keeps the accounting and block lists stable across concurrent allocator
     * calls.
     */
    if (mem_reserved_size < resv_size)
        goto fail; /* must be first */
    if (mem_free_size < (size - resv_size))
        goto fail;

    CRASHCHECK(flags & MAP_RESERVMASK);

    size_left = size;

    for (curr_res = kap->restriction; curr_res; curr_res = curr_res->next) {
        rq_start = pa_paddr_to_quantum(curr_res->start);
        rq_end = pa_paddr_to_quantum(curr_res->end);

        /* Non-contiguous requests are still searched in power-of-two chunks so
         * suitable ranges can preserve large-page alignment when possible.
         */

        pgsz = pgszlist;
        for (curr_size = (flags & PAA_FLAG_CONTIG) ? size_left
                                                   : pa_largest_pow2(size_left);
             curr_size >= QUANTUM_SIZE; curr_size >>= 1) {
            size_t nquants = LEN_TO_NQUANTUM(curr_size);

            if (curr_size > size_left)
                continue; /* skip over sizes that aren't applicable */

            if (kap->state & PAS_VPS_ALIGN) {
                /* If the memory might be used for a big page, our alignment
                 * should match the power of two size we're allocating to
                 * increase the probability of being able to use one. We can
                 * start looking in the pgszlist array from where we last left
                 * off, since curr_size will always be decreasing.
                 */
                for (;;) {
                    align = *pgsz;
                    if (curr_size >= align)
                        break;
                    ++pgsz;
                }
                CRASHCHECK(align == 0);
            }

            valid_restrict = 0;
            rover = blk_head;
            for (;;) {
                bhp = *rover++;
                if (!bhp)
                    break;

                /* Use physical addresses for cross-block restriction checks.
                 * Quantum descriptor addresses are only ordered within their
                 * own block.
                 */
                if ((curr_res->start > bhp->end.paddr) ||
                    (curr_res->end < bhp->start.paddr)) {
                    continue;
                }

                valid_restrict = 1;

                /* No free blocks big enough to handle the allocation */
                if (bhp->max_free_run < nquants) {
                    continue;
                }

                found_run = NULL;
                qblk_start = bhp->quanta;
                qblk_end = &bhp->quanta[bhp->num_quanta - 1];
                mod_rq_start = rq_start;
                mod_rq_end = rq_end;

                /* Clamp restrictions that end outside this block to its tail.
                 */
                if ((!mod_rq_end) || (mod_rq_end > qblk_end) ||
                    (mod_rq_end->blk != qblk_end->blk)) {
                    mod_rq_end = qblk_end;
                }

                /* Clamp restrictions that start outside this block to its head.
                 */
                if ((!mod_rq_start) || (mod_rq_start < qblk_start) ||
                    (mod_rq_start->blk != qblk_start->blk)) {
                    mod_rq_start = qblk_start;
                }

                /* After clamping, the effective restriction must be fully
                 * inside this block.
                 */

                if (!((mod_rq_start >= qblk_start) &&
                      (mod_rq_end <= qblk_end))) {
                    /* Contiguous allocations cannot span block_head instances.
                     * Try the next block if the clamped restriction still does
                     * not fit.
                     */
                    continue; /* try next block */
                }

                /* Carving a run rewrites its free-list links, so save the next
                 * candidate before carving. Free-list order is independent of
                 * physical-address order.
                 */

                /* look at all the single-size queues larger than 'nquants'. If
                 * we don't find anything there, use the default queue
                 */
                if (nquants < PA_FREE_QUEUES) {
                    current_q = nquants;
                    while (FREE_LIST_EMPTY(bhp, current_q)) {
                        current_q++;
                        if (current_q >= PA_FREE_QUEUES) {
                            /* All the individual queues have been checked use
                             * the default one
                             */
                            current_q = PA_DEFAULT_QUEUE;
                            break;
                        }
                    }
                } else {
                    current_q = PA_DEFAULT_QUEUE;
                }
                qp = bhp->free[current_q].next;
                free_list_end = FREE_LIST_END(bhp, current_q);

                /* Since we may not examine all of the single size free lists to
                 * see if they contain any entries, we can not reliably set
                 * max_blk_free_run to a value smaller than the largest of them.
                 * This doesn't really slow us down any though, since the code
                 * below will very quickly bop out if there aren't any entires
                 * on an individual free list.
                 */
                max_blk_free_run = PA_FREE_QUEUES - 1;

                /* these are subset restrictions that are applicable only to
                 * this blk
                 */
                rp_start = pa_quantum_to_paddr(mod_rq_start);
                rp_end = pa_quantum_to_paddr(mod_rq_end);

                for (;;) {           /* manually control the loop */
                    paddr_t s, e, c; /* start, end, cut at */
                    paddr_t coloured_c, nox64k_c;
                    struct pa_quantum *qp_end;

                    if (qp == free_list_end) {
                        /* End of the road for this free queue */
                        if (current_q != PA_DEFAULT_QUEUE) {
                            /* Try the next queue */
                            current_q++;
                            if (current_q >= PA_FREE_QUEUES) {
                                /* All the individual queues have been checked
                                 * use the default one
                                 */
                                current_q = PA_DEFAULT_QUEUE;
                            }

                            qp = bhp->free[current_q].next;
                            free_list_end = FREE_LIST_END(bhp, current_q);
                            continue;
                        }

                        /* We've looked at all the runs, can update the
                         * max_free_run field to the true value. The stored
                         * value may not be correct as it's not reduced during
                         * carving.
                         */
                        bhp->max_free_run = max_blk_free_run;
                        break;
                    }

                    CRASHCHECK(qp->run > (int32_t) bhp->max_free_run);
                    if ((size_t) qp->run > max_blk_free_run) {
                        max_blk_free_run = (size_t) qp->run;
                    }

                    prev_qp = qp->u.flink.prev;
                    CRASHCHECK(qp->flags & PAQ_FLAG_INUSE);

                    /* check if we're interested in this run (ie. a piece of it
                     * could be within the restriction
                     */
                    qp_end = qp + qp->run - 1;

                    if ((qp->run < LEN_TO_NQUANTUM(curr_size)) ||
                        ((qp_end < mod_rq_start) || (qp > mod_rq_end))) {
                        qp = qp->u.flink.next;
                        continue; /* nope we're not */
                    }

                    s = pa_quantum_to_paddr(qp);

                    /* we cut from the end of the run, or the end of the
                     * restriction within the run
                     */
                    e = (qp_end >= mod_rq_end) ? rp_end
                                               : pa_quantum_to_paddr(qp_end);

                    /* AM Note: (c)
                       +----------------------------+
                       |                            |
                       v                            v
                       +-----+-----+-----+-----+-----+-----+-----+-----+
                       |  8  |  -  |  -  |  -  |  -  |  -  |  -  |  -7 |
                       +-----+-----+-----+-----+-----+-----+-----+-----+
                       ^                                         ^
                       |                                         |
                     * (s) (e)
                     *
                     * Since we can't use quantum pointers that are off the edge
                     * of a run (as it might not be addressable (or even a valid
                     * pointer), we use qp[qp->run-1] as our end quantum
                     * pointer. *But* we add 0xfff to the paddr we get when
                     * translating that end quantum pointer, so that we have a
                     * paddr as close to the end as we can safely get (and still
                     * translate back to a quantum).
                     */

                    coloured_c = nox64k_c = 0;

                    /* Adjust down for alignment */
                    c = ROUNDDOWN(((e + (QUANTUM_SIZE - 1)) - (curr_size - 1)),
                                  align);

                    /* Adjust down for color (only if we don't already match) */
                    coloured_c =
                        COLOUR_ADJUST_PA(c, (color << ADDR_OFFSET_BITS));
                    if (coloured_c >
                        c) { /* we'll catch other conditions farther down */
                        qp = qp->u.flink.next;
                        continue; /* doesn't fit with alignment + color adj */
                    }
                    c = coloured_c;

                    /* check if we're going to need to back up for the X64K case
                     * (grrr)
                     */
                    if ((flags & PAA_FLAG_NOX64K) &&
                        ((c + curr_size) > PADDR_NEXT64K(c))) {
                        /* we crossed a 64k boundary, backup the amount we cross
                         */
                        nox64k_c =
                            c -
                            ROUNDUP((c + curr_size - PADDR_NEXT64K(c)), align);

                        /* if >64K requested, must ensure the paddr is aligned
                         * on 64K
                         */
                        if ((curr_size > ALIGN_64K) &&
                            (nox64k_c & (ALIGN_64K - 1))) {
                            nox64k_c = PADDR_PREV64K(nox64k_c);
                        }

                        if ((nox64k_c > c) || (nox64k_c < s) ||
                            (nox64k_c < rp_start)) {
                            qp = qp->u.flink.next;
                            continue; /* doesn't fit with the NOX64K adj */
                        }
                        c = nox64k_c;
                    }

                    /* check to see if we fell outside of the run. The
                     * adjustments can cause us to have a c (cut point) less
                     * than the start, or greater (in the case of a wrap from
                     * 0x0000000). FUTURE: If the alignment constraints made
                     * this run unusable, but the alignment was just to increase
                     * our chances of using a big page, we should think about
                     * just ignoring the alignment and using the entry anyway,
                     * if memory is really fragmented and we're spending a long
                     * time searching the free list (e.g. count the number of
                     * free list entries seached and throw the alignment away if
                     * we cross a threshold value).
                     */
                    if ((c < s) || (c < rp_start) ||
                        ((c + curr_size - QUANTUM_SIZE) > rp_end)) {
                        qp = qp->u.flink.next;
                        continue; /* doesn't fit with alignment adjusted */
                    }

                    found_run =
                        qp +
                        (LEN_TO_NQUANTUM(c - s)); /* avoid a paddr2quant
                                                     conversion by doing this
                                                     */

                    /* Already locked at function entry */

                    CRASHCHECK((found_run < qp) ||
                               ((found_run + LEN_TO_NQUANTUM(curr_size)) >
                                qp + qp->run));

                    kap->status |=
                        pa_carve(bhp, qp, found_run, curr_size, &color);
                    (void) color;
                    if (curr_size <= curr_resv_size) {
                        mem_reserved_size -= curr_size;
                        curr_resv_size -= curr_size;
                    } else {
                        mem_reserved_size -= curr_resv_size;
                        mem_free_size -= (curr_size - curr_resv_size);
                        curr_resv_size =
                            0; /* cur_resv_size -= cur_resv_size : ) */
                    }
                    size_left -= curr_size;

                    /* Update sizes to allocate in case we preempt and restart
                     */
                    kap->size = size_left;
                    kap->resv_size = curr_resv_size;
                    CRASHCHECK(!(found_run->flags & PAQ_FLAG_INUSE));

                    /* reload new qp to try from the same (or what was the same)
                     */
                    qp = prev_qp->u.flink.next;

                    /* link onto our found list */
                    found_run->u.flink.next = NULL;
                    found_run->u.flink.prev = NULL;
                    *tail = found_run;
                    tail = &found_run->u.flink.next;

                    if (size_left == 0)
                        goto done;

                    /* We've managed to allocate another chunk of memory, so
                     * reset the retry count since we're now making some forward
                     * progress on satifying the allocation request
                     */
                    kap->retries = 0;

                    /* Lock held for entire allocation — no preemption window */

                    if (size_left < curr_size)
                        break; /* try the next size */

                    /* Looks like we can still try to grab more likesized runs,
                     * so we use the same current run, and try again
                     */

                } /* for each free run */

                if ((found_run) && (size_left < curr_size)) {
                    break; /* cause us to re-evaluate our next size */
                }

            } /* for each blkhead */

            if (!valid_restrict || (flags & PAA_FLAG_CONTIG)) {
                /* it seems we've iterated through all the blks and the
                 * restriction doesn't fit any of them - short circuit out so we
                 * don't spin through all the sizes in the non-contig case
                 */
                break;
            }


        } /* for each size */

    } /* for each restriction */

    /* If we get to this point, we didn't manage to allocate everything we
     * needed (size_left != 0).
     */
    CRASHCHECK(size_left == 0);

fail:
    kext_lock();             /* make sure we're locked */
    kap->state |= PAS_EFREE; /* trigger a pa_free outside */

done:
    /* still locked from last carve */
    found_run = kap->alloc;
    if (found_run) {
        if (!found_run->u.flink.next) {
            kap->status |= PAA_STATUS_ISCONTIG;
        }
    }

    /* Conservatively require zeroing and color repair for allocated ranges. */
    kap->status |= (PAA_STATUS_NOT_ZEROED | PAA_STATUS_COLOUR_MISMATCH);

    verify_all();
    kap->state |= PAS_DONE;
    kspinlock_unlock(&pa_slock);
}


/* pa_alloc — kernel-internal physical allocator.
 *
 * Shapes the request (rounding, alignment, VPS big-page hints), then runs the
 * allocation body inline, retrying through the purger on PAS_EFREE. Kernel
 * callers (cpu-pte.c's page-table walker, pa_init's early-alloc marking) use
 * this directly. Parameters:
 *   size        : bytes to allocate
 *   align       : required alignment (0 -> __PAGESIZE)
 *   color      : requested cache color
 *   flags       : PAA_FLAG_* allocation flags
 *   status_p    : if non-NULL, receives the PAA_STATUS_* result bits
 *   restriction : restriction list the allocation must satisfy
 *   resv_size   : reserved-memory portion of the request
 * Return value: the allocated run, or NULL on failure.
 */
struct pa_quantum *pa_alloc(paddr_t size,
                            paddr_t align,
                            unsigned color,
                            unsigned flags,
                            unsigned *status_p,
                            struct pa_restrict *restriction,
                            paddr_t resv_size)
{
    struct kcall_args_pa_alloc data;
    unsigned init_state;

    if (size == 0) {
        if (status_p)
            *status_p = PAA_STATUS_ISCONTIG;
        return &blk_head[0]->quanta[blk_head[0]->num_quanta];
    }

    for (;;) {
        if (restriction->start < sysram_end)
            break;
        restriction = restriction->next;
        if (!restriction)
            return NULL;
    }

    /* A size near the top of the address space would wrap ROUNDUP down to a
     * tiny (or zero) run — silent under-allocation. Fail safe to NULL (ENOMEM
     * upstream); the size==0 sentinel above already handled the exact-0 case.
     */
    if (size > (~(paddr_t) 0) - (QUANTUM_SIZE - 1))
        return NULL;
    size = ROUNDUP(size, QUANTUM_SIZE);
    if (size == QUANTUM_SIZE)
        flags |= PAA_FLAG_CONTIG;

    init_state = 0;
    if ((flags & PAA_FLAG_CONTIG) && (align != 0)) {
        align = ROUNDUP(align, QUANTUM_SIZE);
    } else if (mm_flags & MM_FLAG_VPS) {
        switch (CPU_SYSTEM_HAVE_MULTIPLE_PAGESIZES) {
        case VPS_HIGHUSAGE:
            if (!(flags & PAA_FLAG_HIGHUSAGE))
                break;
            /* fallthrough */
        case VPS_AUTO:
            init_state = PAS_VPS_ALIGN;
            break;
        default:
            break;
        }
    }
    if (align == 0)
        align = __PAGESIZE;

    for (;;) {
        data.size = size;
        data.resv_size = resv_size;
        data.align = align;
        data.color = color;
        data.flags = flags;
        data.restriction = restriction;
        data.status = 0;
        data.alloc = NULL;
        data.retries = 0;
        data.state = init_state;

        do {
            pa_alloc_body(&data);
        } while (!(data.state & PAS_DONE));

        if (status_p)
            *status_p = data.status;

        if (!(data.state & PAS_EFREE))
            break;

        pa_free_list(data.alloc, resv_size);
        if (!purger_invoke(size)) {
            if (!((flags & PAA_FLAG_CONTIG) && (data.state & PAS_VPS_ALIGN) &&
                  (size > QUANTUM_SIZE))) {
                return NULL;
            }
            init_state &= ~(unsigned) PAS_VPS_ALIGN;
        }
    }

    return data.alloc;
}


/* pa_alloc_given — allocate a specific run of quanta. Internal helper called
 * from pa_init() (to mark its own early_alloc region as allocated) and from
 * kext_pa_alloc_given via roottask.
 */
int pa_alloc_given(struct pa_quantum *pq, size_t num, unsigned *status_p)
{
    unsigned tmp_status;
    struct pa_quantum *start;
    struct pa_quantum *next;
    struct pa_quantum *pq_end;

    start = pq;
    while (start->run == 0) {
        --start;
    }
    if (start->run < 0)
        start += start->run;
    if (start->flags & PAQ_FLAG_INUSE)
        return ENXIO;
    next = start + start->run;
    pq_end = pq + num;
    if (pq_end > next)
        return ENXIO;

    /* pa_slock serializes the carve + counter update: pa_carve mutates
     * blk_head[start->blk]'s free chains and run-header bookkeeping,
     * mem_free_size is a global counter, and the pq->u.inuse.next assignment
     * finishes the carve — all must be atomic against a concurrent
     * allocate/free. Per-block_head granularity (each blk_head[i] carrying its
     * own slock, keeping NUMA / color partitions independent) is a later
     * optimization.
     */
    kspinlock_lock(&pa_slock);
    tmp_status =
        pa_carve(blk_head[start->blk], start, pq, num << QUANTUM_BITS, NULL);
    mem_free_size -= NQUANTUM_TO_LEN(num);
    pq->u.inuse.next = NULL;
    kspinlock_unlock(&pa_slock);
    if (status_p)
        *status_p = tmp_status;
    return EOK;
}


/* _pa_free — internal physical-quantum free. Called from kext_pa_free, pa_free,
 * pa_free_paddr, pa_free_list (all kernel side). No U-mode caller — roottask
 * issues syscall_priv.
 */
void _pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size)
{
    struct pa_quantum *prev = NULL; /* GCC silence */
    struct pa_quantum *curr;
    struct pa_quantum *next;
    struct pa_quantum *run_start;
    struct pa_quantum *paq_end;
    struct block_head *bh;
    int coalesed = 0;
    unsigned i;

    if (num == 0) {
        return;
    }

#ifndef NDEBUG
    if (!paq)
        crash();
    if (paq->blk == PAQ_BLK_FAKE)
        crash();
    run_start = pa_getrunhead(paq);
    if (!(run_start->flags & PAQ_FLAG_INUSE)) {
        kprintf(
            "paq %p paq_head %p, paq->run %d, runhead r/f/bl = %d/%08x/%d\n",
            paq, run_start, paq->run, run_start->run, run_start->flags,
            run_start[run_start->run - 1].run);
        crash();
    }
#endif

    bh = blk_head[paq->blk];

    /* pa_slock is held across the whole free body — the flags-clear loop, the
     * bh->generation bump, and the run-merge dequeue_run/enqueue_run linkage +
     * mem_free_size update. Without it a concurrent pa_alloc_body/_pa_free on
     * another hart corrupts the bh free chains, causing dequeue_run() to store
     * through a NULL flink.prev (observed: scause 15, stval 0xe8). Per-bh
     * granularity (frees on different blocks in parallel) is a later
     * optimization.
     */
    kspinlock_lock(&pa_slock);

#ifdef CONFIG_TRACKPMEM
    kprintf("PMEM FREE  0x%x for %d [free=0x%x]\n", (uintptr_t) paq, num,
            (unsigned) mem_free_size);
#endif

    /* Turn off (almost) all the flags */
    for (i = 0; i < num; ++i) {
        paq[i].flags &= PAQ_FLAG_PRESERVE_BITS;
    }

    atomic_add(&bh->generation, 1);

    run_start = paq;
    paq_end = paq + num;

    /* run_start is the first quantum being freed. paq_end is one past the last
     * quantum being freed; the range may cover only part of an allocation run.
     */

    if (IS_HEADQ(paq->run)) {
        /* freeing from start of run */
        curr = paq;
        next = &curr[curr->run];

        /* Don't try to coalesce left if we're the first quanta in the block */
        if (curr > bh->quanta) {
            prev = pa_getrunhead(curr - 1);

            /* coalesce with previous run if it's free */
            if (!(prev->flags & PAQ_FLAG_INUSE)) {
                dequeue_run(bh, prev);
                SET_TAIL(prev, 0);
                curr->run = 0;
                prev->run += (int32_t) num;
                SET_TAIL(prev, TAIL_CALC(prev->run));
                run_start = prev;
                coalesed = 1;

                /* Now have a new, larger run starting at run_start */
            }
        }
    } else {
        /* Freeing from the middle of an allocation run is uncommon but valid.
         */
        curr = pa_getrunhead(paq);
        next = &curr[curr->run];

        /* turn quanta in front into their own run */
        curr->run = (int32_t) (paq - curr);
        SET_TAIL(curr, TAIL_CALC(curr->run));

        /* turn quanta on the end into their own run is done farther down */
    }

    if (!coalesed) {
        /* If we haven't already coalesced it, make it into a separate run */
        paq->run = (int32_t) num;
        SET_TAIL(paq, TAIL_CALC(paq->run));
    }

    /* At this point, next points at start of next run */

    if (paq_end < next) {
        /* The freed range ends before the original allocation. Keep the
         * trailing quanta as a separate in-use run.
         */
        paq_end->run = (int32_t) (next - paq_end);
        paq_end->flags |= PAQ_FLAG_INUSE;
        SET_TAIL(paq_end, TAIL_CALC(paq_end->run));
#ifndef NDEBUG
    } else if (paq_end > next) {
        crash();
#endif

    } else if ((next < &bh->quanta[bh->num_quanta]) &&
               !(next->flags & PAQ_FLAG_INUSE)) {
        /* The next run is in bounds and free; coalesce with it first remove
         * 'next' from free list
         */
        dequeue_run(bh, next);

        /* Add the right hand side to the run */
        SET_TAIL(run_start, 0); /* remove old tail */
        run_start->run += next->run;
        next->run = NON_HEAD_QUANTUM;
        SET_TAIL(run_start, TAIL_CALC(run_start->run));
        coalesed = 1;
    }

    /* We've coalesced what we can; put whatever we have on the free list */
    enqueue_run(bh, run_start);

    verify_all();

    {
        paddr_t amount_freed = (paddr_t) num << QUANTUM_BITS;
#ifndef NDEBUG
        if (amount_freed < resv_size) {
            kprintf("amount_freed = %P, resv_size = %P\n",
                    (paddr_t) amount_freed, (paddr_t) resv_size);
        }
#endif /* NDEBUG */
        mem_reserved_size += resv_size;
        mem_free_size += (amount_freed - resv_size);
    }
    kspinlock_unlock(&pa_slock);
}

/* pa_free — free a run of quanta back to the allocator. Parameters:
 *   paq       : the run to free
 *   num       : number of quanta in the run
 *   resv_size : reserved-memory portion being returned
 */
void pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size)
{
    _pa_free(paq, num, resv_size);
}

/* pa_free_paddr() used to be the following define in mm-internal.h
 *
 * #define pa_free_paddr(p, s, r) \
 *						pa_free(pa_paddr_to_quantum(p), LEN_TO_NQUANTUM(s), r)
 *
 * Virtual contiguity does not imply one physical allocation run. A system heap
 * unmap can cover adjacent physical pages that came from separate allocation
 * runs; passing the full virtual length to pa_free() with only the first
 * quantum would overrun that run. Walk each physical run and free only the
 * quanta it actually owns.
 */
void pa_free_paddr(paddr_t p, paddr_t s, paddr_t resv_size)
{
    int num = (int) LEN_TO_NQUANTUM(s);

    while (num > 0) {
        struct pa_quantum *paq = pa_paddr_to_quantum(p);
        int num_to_free;
        paddr_t free_size;

        CRASHCHECK(!paq);
        num_to_free = min_int(paq->run, num);
        free_size = NQUANTUM_TO_LEN(num_to_free);
        pa_free(paq, (size_t) num_to_free, min(resv_size, free_size));
        num -= num_to_free;
        p += free_size;
        if (resv_size >= free_size)
            resv_size -= free_size;

        CRASHCHECK(num < 0);
        CRASHCHECK((resv_size & (NQUANTUM_TO_LEN(1) - 1)) != 0);
    }
}

/* pa_free_list — free a flink-linked chain of runs. Parameters:
 *   qp        : head of the run chain (linked via u.flink.next)
 *   resv_size : reserved-memory portion applied to each freed run
 */
void pa_free_list(struct pa_quantum *qp, paddr_t resv_size)
{
    struct pa_quantum *np;

    while (qp) {
        np = qp->u.flink.next;
        pa_free(qp, (size_t) qp->run, resv_size);
        qp = np;
    }
}


/* pa_alloc_fake / pa_free_fake — roottask-only. These create "fake" pa_quantum
 * structs (PAQ_BLK_FAKE) for memory regions that aren't in the sysram pool
 * (MMIO, foreign physical mappings). The struct lives in roottask heap
 * (_scalloc/_sfree); kernel-side dereferencing via pa_quantum_to_paddr handles
 * PAQ_BLK_FAKE specially by reading the embedded paddr.
 */


/* pa_sysram_overlap — does the restriction overlap any sysram block? Read-only
 * walk over blk_head.
 */
int pa_sysram_overlap(struct pa_restrict *res)
{
    struct block_head *bh;
    struct block_head **bhp;

    bhp = blk_head;
    for (;;) {
        bh = *bhp++;
        if (!bh)
            break;
        if (!((res->end < bh->start.paddr) || (res->start > bh->end.paddr))) {
            return 1;
        }
    }
    return 0;
}


/* pa_sysram_contains — is the WHOLE [start, start+len) range inside a single
 * sysram block? (bh->end.paddr is inclusive, matching pa_sysram_overlap.) Used
 * by kext_kpage_zero to refuse zeroing anything that isn't real RAM, e.g. MMIO.
 *
 * Returns 1 if fully contained, 0 otherwise.
 */
int pa_sysram_contains(paddr_t start, paddr_t len)
{
    struct block_head *bh;
    struct block_head **bhp;
    paddr_t end;

    if (len == 0 || start + len < start) /* empty or overflow */
        return 0;
    end = start + len - 1;
    for (bhp = blk_head; (bh = *bhp); bhp++) {
        if (start >= bh->start.paddr && end <= bh->end.paddr)
            return 1;
    }
    return 0;
}


/* pa_get_sizes — report used and free-unreserved memory totals. Parameters:
 *   used            : if non-NULL, receives total allocated (incl.
 * reserved-used) free_unreserved : if non-NULL, receives free unreserved memory
 */
static void pa_get_sizes(memsize_t *used, memsize_t *free_unreserved)
{
    memsize_t resv_free = mem_reserved_size;
    memsize_t resv_used = mem_reserved_configured - resv_free;
    memsize_t unresv_free = mem_free_size;
    memsize_t unresv_used =
        sysram_attr.size - (resv_used + unresv_free + resv_free);
    if (used)
        *used = resv_used + unresv_used;
    if (free_unreserved)
        *free_unreserved = unresv_free;
}


/* pa_free_size
 *
 * the amount of unallocated unreserved memory
 */
memsize_t pa_free_size(void)
{
    return mem_free_size;
}

/* pa_used_size
 *
 * the amount of allocated memory (including reserved)
 */
memsize_t pa_used_size(void)
{
    memsize_t used;
    pa_get_sizes(&used, NULL);
    return used;
}

/* pa_reserved_size
 *
 * the amount of unallocated reserved memory
 */
memsize_t pa_reserved_size(void)
{
    return mem_reserved_size;
}


/* kernel extension entry points — reached via syscall_priv from roottask. Each
 * does its work under BKL (acquired by syscall_priv), then returns the result
 * via SETKSTATUS.
 */

/* kext_pa_alloc_given — allocate a specific run on behalf of roottask.
 * Parameters:
 *   data : struct kcall_args_pa_alloc_given * (pq, num, status)
 */
void kext_pa_alloc_given(void *data)
{
    struct kcall_args_pa_alloc_given *kap = data;
    int r = pa_alloc_given(kap->pq, kap->num, kap->status);
    kext_status(NULL, r);
}


/* kext_pa_free — free a run of quanta on behalf of roottask. Parameters:
 *   data : struct kcall_args_pa_free * (paq, num, resv_size)
 */
void kext_pa_free(void *data)
{
    struct kcall_args_pa_free *kap = data;
    _pa_free(kap->paq, kap->num, kap->resv_size);
    kext_status(NULL, 0);
}


/* kext_pa_free_info — iterate a block head's free list for roottask.
 *
 * Returns one free run per call, advancing kap->pq / kap->freeq. Uses
 * bh->generation as a "did the free-list change underneath us?" guard so an
 * iteration that crosses a concurrent _pa_free restarts at the head; the
 * generation guard catches iteration loss but does not by itself replace
 * pa_slock. Parameters:
 *   data : struct kcall_args_pa_free_info * (bh, generation, freeq, pq,
 *          run_start, run_end, total, contig)
 */
void kext_pa_free_info(void *data)
{
    struct kcall_args_pa_free_info *kap = data;
    struct block_head *bh = kap->bh;
    struct pa_quantum *pq;

    if (kap->generation != bh->generation) {
        kap->generation = bh->generation;
        kap->freeq = 0;
        kap->pq = bh->free[kap->freeq].next;
        kap->total = 0;
        kap->contig = 0;
    }
    pq = kap->pq;
    if (pq == FREE_LIST_END(bh, kap->freeq)) {
        kap->freeq++;
        if (kap->freeq >= PA_FREE_QUEUES) {
            kext_status(NULL, 0);
        } else {
            kap->pq = bh->free[kap->freeq].next;
        }
    } else {
        kap->run_start = PQ_TO_PADDR(bh, pq);
        kap->run_end = kap->run_start + NQUANTUM_TO_LEN(pq->run) - 1;
        kap->pq = pq->u.flink.next;
        kext_status(NULL, 1);
    }
}


/* kext_pa_init — runs pa_init() under BKL. Roottask calls this once during
 * early init. The OUT fields hand the post-init values back to roottask so its
 * cached references (last_paddr, restrict_proc, restrict_user,
 * system_heap_vaddr, blk_head) match the kernel-side state.
 */
struct kcall_args_pa_init {
    unsigned colors;
    paddr_t last_paddr_out;
    struct pa_restrict *restrict_proc_out;
    struct pa_restrict *restrict_user_out;
    struct vaddr_range system_heap_vaddr_out;
    struct block_head **blk_head_out;
};

/* kext_pa_init — run pa_init() on behalf of roottask and hand the post-init
 * allocator state back. See the struct comment above. Parameters:
 *   data : struct kcall_args_pa_init * (colors in; last_paddr,
 *          restrict_proc/_user, system_heap_vaddr, blk_head out)
 */
void kext_pa_init(void *data)
{
    struct kcall_args_pa_init *kap = data;
    pa_init(kap->colors);
    kap->last_paddr_out = last_paddr;
    kap->restrict_proc_out = restrict_proc;
    kap->restrict_user_out = restrict_user;
    kap->system_heap_vaddr_out = system_heap_vaddr;
    kap->blk_head_out = blk_head;
    kext_status(NULL, 0);
}


/* kext_pa_pq_op — single-dispatch pa_quantum field accessor. See
 * kernel/include/private/pa-pq-op.h for the op codes. All roottask field access
 * on a kernel-allocated pa_quantum routes through here, since the heap is
 * PTE_U=0.
 */
#include <private/pa-pq-op.h>

/* kext_pa_pq_op — get or set a single pa_quantum field on behalf of roottask
 * (the quantum lives in the PTE_U=0 kernel heap). See the comment above and
 * private/pa-pq-op.h for the op codes. Parameters:
 *   data : struct kcall_args_pa_pq_op * (pq, op, set, clr; result written)
 */
void kext_pa_pq_op(void *data)
{
    struct kcall_args_pa_pq_op *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    struct pa_quantum *pq = kap->pq;
    uintptr_t result = 0;

    if (!pq) {
        kap->result = 0;
        SETKSTATUS(act, EINVAL);
        return;
    }

    switch (kap->op) {
    case PA_PQ_GET_FLAGS:
        result = pq->flags;
        break;
    case PA_PQ_MODIFY_FLAGS: {
        unsigned old = pq->flags;
        pq->flags = (uint16_t) ((old & ~kap->clr) | kap->set);
        result = old;
        break;
    }
    case PA_PQ_GET_BLK:
        result = pq->blk;
        break;
    case PA_PQ_GET_RUN:
        /* sign-extend int32 into uintptr_t */
        result = (uintptr_t) (intptr_t) (int32_t) pq->run;
        break;
    case PA_PQ_SET_RUN:
        pq->run = (int32_t) kap->set;
        break;
    case PA_PQ_GET_INUSE_QPOS:
        result = pq->u.inuse.qpos;
        break;
    case PA_PQ_SET_INUSE_QPOS:
        pq->u.inuse.qpos = (uint32_t) kap->set;
        break;
    case PA_PQ_GET_INUSE_NEXT:
        result = (uintptr_t) pq->u.inuse.next;
        break;
    case PA_PQ_SET_INUSE_NEXT:
        pq->u.inuse.next = (struct pa_quantum *) kap->set;
        break;
    case PA_PQ_GET_FAKE_PADDR:
        if (pq->blk == PAQ_BLK_FAKE) {
            result = (uintptr_t) ((struct pa_quantum_fake *) pq)->paddr;
        }
        break;
    default:
        kap->result = 0;
        SETKSTATUS(act, EINVAL);
        return;
    }
    kap->result = result;
    SETKSTATUS(act, EOK);
}


/* Pointer/paddr return slots — these set the a0 register directly via
 * SETREGSTATUS so the full uintptr_t makes it back to the caller (kext_status /
 * SETKSTATUS narrow to int).
 */

/* kext_pa_quantum_to_paddr — return a quantum's physical address.
 *
 * A deterministic arithmetic helper (offset of pq inside its block array →
 * paddr) that mutates no PA state; the result is returned in a0. Parameters:
 *   data : struct pa_quantum *
 */
void kext_pa_quantum_to_paddr(void *data)
{
    struct pa_quantum *pq = data;
    paddr_t r = pa_quantum_to_paddr(pq);
    SETREGSTATUS(&get_active_thread(get_cpunum())->reg, r);
}


/* kext_pa_paddr_to_quantum — return the quantum for a physical address.
 *
 * The inverse arithmetic helper of kext_pa_quantum_to_paddr; the resulting
 * pointer is returned in a0. Parameters:
 *   data : the physical address (cast from void *)
 */
void kext_pa_paddr_to_quantum(void *data)
{
    paddr_t paddr = (paddr_t) (uintptr_t) data;
    struct pa_quantum *pq = pa_paddr_to_quantum(paddr);
    SETREGSTATUS(&get_active_thread(get_cpunum())->reg, pq);
}


/* kext_pa_start_background — start the background page cleaner for roottask.
 * Parameters:
 *   data : unused
 */
void kext_pa_start_background(void *data)
{
    (void) data;
    pa_start_background();
    kext_status(NULL, 0);
}


struct kcall_args_pa_free_paddr {
    paddr_t p;
    paddr_t s;
    paddr_t resv_size;
};


/* kext_pa_free_paddr — free a physical range by address on behalf of roottask.
 * Parameters:
 *   data : struct kcall_args_pa_free_paddr * (p, s, resv_size)
 */
void kext_pa_free_paddr(void *data)
{
    struct kcall_args_pa_free_paddr *kap = data;
    pa_free_paddr(kap->p, kap->s, kap->resv_size);
    kext_status(NULL, 0);
}


struct kcall_args_pa_free_list {
    struct pa_quantum *qp;
    paddr_t resv_size;
};


/* kext_pa_free_list — free a chain of runs on behalf of roottask. Parameters:
 *   data : struct kcall_args_pa_free_list * (qp, resv_size)
 */
void kext_pa_free_list(void *data)
{
    struct kcall_args_pa_free_list *kap = data;
    pa_free_list(kap->qp, kap->resv_size);
    kext_status(NULL, 0);
}


/* kext_pa_sysram_overlap — test a restriction against sysram for roottask.
 * Parameters:
 *   data : struct pa_restrict *
 * Returns 1 (via kext_status) if the restriction overlaps any sysram block, 0
 * otherwise.
 */
void kext_pa_sysram_overlap(void *data)
{
    struct pa_restrict *res = data;
    int r = pa_sysram_overlap(res);
    kext_status(NULL, r);
}


struct kcall_args_pa_sizes {
    memsize_t used;
    memsize_t free_unreserved;
    memsize_t reserved;
    memsize_t total;
};


/* kext_pa_get_sizes — report memory totals to roottask. Parameters:
 *   data : struct kcall_args_pa_sizes * (used, free_unreserved, reserved,
 *          total written)
 */
void kext_pa_get_sizes(void *data)
{
    struct kcall_args_pa_sizes *kap = data;
    pa_get_sizes(&kap->used, &kap->free_unreserved);
    kap->reserved = mem_reserved_size;
    kap->total = sysram_attr.size;
    kext_status(NULL, 0);
}
