/*
 * Host-side tests for sv39_manip_pages, the loop behind cpu_pte_manipulate.
 *
 * Checks the error each operation reports (EEXIST over a superpage, ENOENT
 * swallowed for UNMAP and PROT, ENOMEM from the table allocator) and, for a
 * call that fails half way, that the progress it leaves is exact: the first
 * page not done and the frame it would have got. That progress is what lets
 * roottask's pte_map undo the pages a failed MAP already mapped. Table
 * "physical addresses" are host pointers, so PA_TO_VA is the identity.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PA_TO_VA(pa) ((void *) (uintptr_t) (pa))
#include <sv39-lookup.h>

#define MIB2 sv39_leaf_size(1)
#define LEAF_RW (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D)
#define USER_RW (LEAF_RW | PTE_U)
#define PBMT_NC (1UL << 61) /* Svpbmt attribute bits, above the PPN */

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "sv39-manip: %s:%d: %s\n", __func__, __LINE__, \
                    #cond);                                               \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static int allocs;
static int alloc_budget; /* tables test_alloc still hands out */

/* Allocator for the engine: a zeroed host table linked as the kernel links its
 * own, or 0 once the budget is spent (ENOMEM).
 */
static pte_t test_alloc(void)
{
    pagetable_t t;

    if (alloc_budget-- <= 0)
        return 0;
    t = aligned_alloc(PGSIZE, PGSIZE);
    CHECK(t);
    memset(t, 0, PGSIZE);
    allocs++;
    return PA2PTE((uintptr_t) t) | PTE_V;
}

static pagetable_t new_root(void)
{
    pagetable_t root = aligned_alloc(PGSIZE, PGSIZE);

    CHECK(root);
    memset(root, 0, PGSIZE);
    return root;
}

/* One engine call over pages [start, end), with the progress it leaves. */
struct run {
    uintptr_t start;
    uint64_t paddr;
    int changed;
    int err;
};

static struct run manip(pagetable_t root,
                        enum sv39_manip_op op,
                        uintptr_t start,
                        uintptr_t end,
                        uint64_t paddr,
                        uint64_t bits)
{
    struct run r = {.start = start, .paddr = paddr};

    r.err = sv39_manip_pages(root, op, &r.start, end, &r.paddr, bits,
                             test_alloc, &r.changed);
    return r;
}

static pte_t *slot(pagetable_t root, uintptr_t va)
{
    int err;

    return sv39_walk_slot(root, va, NULL, &err);
}

/* Make the L1 slot covering va a 2 MiB leaf; the tables above it must exist. */
static void plant_megapage(pagetable_t root, uintptr_t va)
{
    pagetable_t l1 = PA_TO_VA(PTE2PA(root[PX(2, va)]));

    l1[PX(1, va)] = PA2PTE(0x200000000UL) | LEAF_RW;
}

/* MAP with room for every table: the range is mapped and the progress is the
 * whole range.
 */
static void check_map_ok(void)
{
    pagetable_t root = new_root();
    uintptr_t va = 0x10000000UL;
    struct run r;

    alloc_budget = 8;
    r = manip(root, SV39_MANIP_MAP, va, va + 3 * PGSIZE, 0x80000000UL,
              USER_RW);
    CHECK(r.err == 0 && r.changed);
    CHECK(r.start == va + 3 * PGSIZE && r.paddr == 0x80003000UL);
    CHECK(*slot(root, va) == (PA2PTE(0x80000000UL) | USER_RW));
    CHECK(*slot(root, va + 2 * PGSIZE) == (PA2PTE(0x80002000UL) | USER_RW));
}

/* A MAP that runs into a 2 MiB leaf after two pages reports EEXIST and leaves
 * the progress at the leaf's first page, with those two pages mapped.
 */
static void check_map_over_superpage(void)
{
    pagetable_t root = new_root();
    uintptr_t leaf = 0x40600000UL;
    struct run r;
    int before;
    int err;

    alloc_budget = 8;
    /* The tables for the region below the leaf, then the leaf in the next L1
     * slot of the same L1 table.
     */
    r = manip(root, SV39_MANIP_PREALLOC, leaf - MIB2, leaf - MIB2 + PGSIZE, 0,
              0);
    CHECK(r.err == 0 && PX(2, leaf) == PX(2, leaf - MIB2));
    plant_megapage(root, leaf);
    CHECK(!sv39_walk_slot(root, leaf, NULL, &err) && err == EEXIST);

    before = allocs;
    r = manip(root, SV39_MANIP_MAP, leaf - 2 * PGSIZE, leaf + 2 * PGSIZE,
              0x80000000UL, USER_RW);
    CHECK(r.err == EEXIST);
    CHECK(r.start == leaf && r.paddr == 0x80002000UL && r.changed);
    CHECK(*slot(root, leaf - 2 * PGSIZE) == (PA2PTE(0x80000000UL) | USER_RW));
    CHECK(*slot(root, leaf - PGSIZE) == (PA2PTE(0x80001000UL) | USER_RW));
    CHECK(allocs == before); /* the leaf was never read as a table */

    /* Starting on the leaf itself: nothing done, nothing changed. */
    r = manip(root, SV39_MANIP_MAP, leaf, leaf + PGSIZE, 0x90000000UL,
              USER_RW);
    CHECK(r.err == EEXIST && r.start == leaf && r.paddr == 0x90000000UL);
    CHECK(!r.changed);
}

/* A 1 GiB leaf is reported the same way as a 2 MiB one. */
static void check_map_over_gigapage(void)
{
    pagetable_t root = new_root();
    struct run r;

    root[2] = PA2PTE(0x140000000UL) | LEAF_RW;
    alloc_budget = 8;
    r = manip(root, SV39_MANIP_MAP, 0x80001000UL, 0x80003000UL, 0x1000UL,
              USER_RW);
    CHECK(r.err == EEXIST && r.start == 0x80001000UL && !r.changed);
}

/* The allocator runs dry on the second 2 MiB region: ENOMEM, with every page of
 * the first region mapped and the progress at the first page of the second.
 */
static void check_map_enomem_midway(void)
{
    pagetable_t root = new_root();
    uintptr_t edge = 0x20200000UL;
    struct run r;

    alloc_budget = 2; /* L1 and the L0 below the edge, none for the next L0 */
    r = manip(root, SV39_MANIP_MAP, edge - 2 * PGSIZE, edge + 2 * PGSIZE,
              0x80000000UL, USER_RW);
    CHECK(r.err == ENOMEM);
    CHECK(r.start == edge && r.paddr == 0x80002000UL && r.changed);
    CHECK(*slot(root, edge - PGSIZE) == (PA2PTE(0x80001000UL) | USER_RW));
    CHECK(!slot(root, edge)); /* no table: the walk ends with ENOENT */

    /* No table at all for the first page: ENOMEM with no progress. */
    alloc_budget = 0;
    r = manip(root, SV39_MANIP_MAP, 0x50000000UL, 0x50001000UL, 0x1000UL,
              USER_RW);
    CHECK(r.err == ENOMEM && r.start == 0x50000000UL && r.paddr == 0x1000UL);
    CHECK(!r.changed);
}

/* PREALLOC reports what the walk does and writes no leaf. */
static void check_prealloc(void)
{
    pagetable_t root = new_root();
    uintptr_t va = 0x30000000UL;
    struct run r;

    alloc_budget = 1; /* L1 only: the L0 for the first page fails */
    r = manip(root, SV39_MANIP_PREALLOC, va, va + PGSIZE, 0, 0);
    CHECK(r.err == ENOMEM && r.start == va && !r.changed);
    alloc_budget = 4;
    r = manip(root, SV39_MANIP_PREALLOC, va, va + 2 * PGSIZE, 0, 0);
    CHECK(r.err == 0 && r.start == va + 2 * PGSIZE && !r.changed);
    CHECK(slot(root, va) && !*slot(root, va));
}

/* UNMAP and PROT: a missing table is not an error, a valid leaf is changed, an
 * invalid one is left alone, and a superpage is EEXIST.
 */
static void check_unmap_prot(void)
{
    pagetable_t root = new_root();
    uintptr_t va = 0x10000000UL;
    uintptr_t leaf = 0x40400000UL;
    struct run r;
    pte_t *p;
    int before;

    alloc_budget = 8;
    (void) manip(root, SV39_MANIP_MAP, va, va + 2 * PGSIZE, 0x80000000UL,
                 USER_RW);
    before = allocs;

    /* Over a hole (no table): success, nothing changed, nothing allocated. */
    r = manip(root, SV39_MANIP_UNMAP, 0x70000000UL, 0x70003000UL, 0, 0);
    CHECK(r.err == 0 && r.start == 0x70003000UL && !r.changed);
    r = manip(root, SV39_MANIP_PROT, 0x70000000UL, 0x70003000UL, 0, USER_RW);
    CHECK(r.err == 0 && !r.changed && allocs == before);

    /* PROT keeps the frame and the Svpbmt bits, swaps the flag bits. */
    p = slot(root, va);
    *p |= PBMT_NC;
    r = manip(root, SV39_MANIP_PROT, va, va + PGSIZE, 0,
              PTE_V | PTE_A | PTE_R | PTE_U);
    CHECK(r.err == 0 && r.changed);
    CHECK(*p == (PA2PTE(0x80000000UL) | PBMT_NC | PTE_V | PTE_A | PTE_R |
                 PTE_U));
    CHECK(PTE2PA(*p) == 0x80000000UL);

    /* UNMAP clears both pages; the second pass finds them invalid. */
    r = manip(root, SV39_MANIP_UNMAP, va, va + 2 * PGSIZE, 0, 0);
    CHECK(r.err == 0 && r.changed && !*slot(root, va) &&
          !*slot(root, va + PGSIZE));
    r = manip(root, SV39_MANIP_UNMAP, va, va + 2 * PGSIZE, 0, 0);
    CHECK(r.err == 0 && !r.changed);

    /* Over a 2 MiB leaf: EEXIST at its first page, the page before it done. */
    (void) manip(root, SV39_MANIP_MAP, leaf - PGSIZE, leaf, 0x80000000UL,
                 USER_RW);
    plant_megapage(root, leaf);
    r = manip(root, SV39_MANIP_UNMAP, leaf - PGSIZE, leaf + PGSIZE, 0, 0);
    CHECK(r.err == EEXIST && r.start == leaf && r.changed);
    CHECK(!*slot(root, leaf - PGSIZE));
    r = manip(root, SV39_MANIP_PROT, leaf, leaf + PGSIZE, 0, USER_RW);
    CHECK(r.err == EEXIST && r.start == leaf && !r.changed);
}

int main(void)
{
    check_map_ok();
    check_map_over_superpage();
    check_map_over_gigapage();
    check_map_enomem_midway();
    check_prealloc();
    check_unmap_prot();
    puts("sv39-manip: PASS");
    return 0;
}
