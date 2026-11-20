/*
 * Host-side tests for the Sv39 leaf lookup and leaf address arithmetic that
 * cpu_pte_lookup and mm_vaddrinfo use.
 *
 * Builds a real three-level table in host memory with one 1 GiB, one 2 MiB and
 * one 4 KiB leaf, then checks that each VA at a nonzero offset into its leaf
 * walks to the right level and translates to the right physical address and
 * remaining length. Table "physical addresses" are host pointers, so PA_TO_VA
 * is the identity.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PA_TO_VA(pa) ((void *) (uintptr_t) (pa))
#include <sv39-lookup.h>

#define GIB sv39_leaf_size(2)
#define MIB2 sv39_leaf_size(1)
#define LEAF_RW (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D)

static pagetable_t new_table(void)
{
    pagetable_t t = aligned_alloc(PGSIZE, PGSIZE);

    assert(t);
    memset(t, 0, PGSIZE);
    return t;
}

static void link_table(pagetable_t parent, unsigned idx, pagetable_t child)
{
    parent[idx] = PA2PTE((uintptr_t) child) | PTE_V;
}

struct leaf_case {
    const char *name;
    uintptr_t va;
    int level;
    uint64_t pa;
    size_t len;
};

static void check(pagetable_t root, const struct leaf_case *c)
{
    int level = -1;
    pte_t e = 0;
    size_t len = 0;
    pte_t *pte = sv39_lookup_leaf(root, c->va, &level, &e);

    if (!pte || level != c->level) {
        fprintf(stderr, "sv39-lookup: %s: va %lx gave level %d, want %d\n",
                c->name, (unsigned long) c->va, pte ? level : -1, c->level);
        exit(1);
    }
    assert(e == *pte); /* ep carries the entry the walk checked */
    uint64_t pa = sv39_leaf_paddr(e, level, c->va, &len);
    if (pa != c->pa || len != c->len) {
        fprintf(
            stderr,
            "sv39-lookup: %s: va %lx gave pa %lx len %zx, want %lx len %zx\n",
            c->name, (unsigned long) c->va, (unsigned long) pa, len,
            (unsigned long) c->pa, c->len);
        exit(1);
    }
}

static int allocs;

/* Allocator for sv39_walk_slot: a zeroed host table, linked as the kernel does
 * (valid, PPN of the table).
 */
static pte_t test_alloc(void)
{
    pagetable_t t = new_table();

    allocs++;
    return PA2PTE((uintptr_t) t) | PTE_V;
}

/* Walkers that stop at a superpage leaf: with and without alloc, the slot walk
 * returns no L0 slot with EEXIST (a missing table is ENOENT); and it allocates
 * nothing.
 */
static void check_walk_slot(pagetable_t root)
{
    int err = 0;
    int before = allocs;

    /* 2 MiB leaf at L1[3] and 1 GiB leaf at L2[2] (set up by main). */
    assert(!sv39_walk_slot(root, 0x40600000UL + 0x1000UL, NULL, &err));
    assert(err == EEXIST);
    assert(!sv39_walk_slot(root, 0x40600000UL + 0x1000UL, test_alloc, &err));
    assert(err == EEXIST);
    assert(!sv39_walk_slot(root, 0x80000000UL + 0x5000UL, NULL, &err));
    assert(err == EEXIST);
    assert(!sv39_walk_slot(root, 0x80000000UL + 0x5000UL, test_alloc, &err));
    assert(err == EEXIST);
    assert(allocs == before);

    /* A mapped 4 KiB page: both walks reach its L0 slot. */
    pte_t *slot = sv39_walk_slot(root, 0x40805000UL, NULL, &err);
    assert(slot && slot == sv39_lookup_leaf(root, 0x40805000UL, NULL, NULL));
    assert(sv39_walk_slot(root, 0x40805000UL, test_alloc, &err) == slot);
    assert(allocs == before);

    /* A hole: no alloc ends with ENOENT, alloc builds the tables and returns
     * an empty slot the caller can fill.
     */
    assert(!sv39_walk_slot(root, 0xC0000000UL, NULL, &err) && err == ENOENT);
    slot = sv39_walk_slot(root, 0xC0000000UL, test_alloc, &err);
    assert(slot && !*slot && allocs == before + 2);
}

/* Bits 54..63 of a PTE are attributes (Svpbmt, Svnapot), never address. */
static void check_ppn_mask(void)
{
    pte_t e = PA2PTE(0x90007000UL) | LEAF_RW | (3UL << 61) | (1UL << 63) |
              (1UL << 54);

    assert(PTE2PA(e) == 0x90007000UL);
    /* The top PPN bit (PA bit 55) survives the mask. */
    assert(PTE2PA(PA2PTE(1UL << 55) | PTE_V) == (1UL << 55));
}

int main(void)
{
    pagetable_t root = new_table();
    pagetable_t l1 = new_table();
    pagetable_t l0 = new_table();

    /* L2[2]: 1 GiB leaf for VA 0x80000000 -> PA 0x1_4000_0000. */
    root[2] = PA2PTE(0x140000000UL) | LEAF_RW;

    /* L2[1] points at L1, whose slot 3 is a 2 MiB leaf for VA 0x40600000 -> PA
     * 0x8820_0000 and whose slot 4 points at L0. L0 slot 5 is a 4 KiB leaf for
     * VA 0x40805000 -> PA 0x9000_7000.
     */
    link_table(root, 1, l1);
    l1[3] = PA2PTE(0x88200000UL) | LEAF_RW;
    link_table(l1, 4, l0);
    l0[5] = PA2PTE(0x90007000UL) | LEAF_RW | PTE_X;

    const struct leaf_case cases[] = {
        {"1 GiB", 0x80000000UL + 0x12345678UL, 2, 0x140000000UL + 0x12345678UL,
         GIB - 0x12345678UL},
        {"2 MiB", 0x40600000UL + 0x1abcdUL, 1, 0x88200000UL + 0x1abcdUL,
         MIB2 - 0x1abcdUL},
        {"4 KiB", 0x40805000UL + 0x9a8UL, 0, 0x90007000UL + 0x9a8UL,
         PGSIZE - 0x9a8UL},
        /* Last byte of each superpage: the length left is one byte. */
        {"1 GiB end", 0x80000000UL + GIB - 1, 2, 0x140000000UL + GIB - 1, 1},
        {"2 MiB end", 0x40600000UL + MIB2 - 1, 1, 0x88200000UL + MIB2 - 1, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check(root, &cases[i]);

    /* NULL levelp is accepted. */
    assert(sv39_lookup_leaf(root, 0x40805000UL, NULL, NULL) == &l0[5]);

    /* Unmapped at each level: empty L2 slot, empty L1 slot, empty L0 slot. */
    assert(!sv39_lookup_leaf(root, 0xC0000000UL, NULL, NULL));
    assert(!sv39_lookup_leaf(root, 0x40000000UL, NULL, NULL));
    assert(!sv39_lookup_leaf(root, 0x40806000UL, NULL, NULL));

    /* A valid entry without R/W/X at L0 is not a leaf. */
    l0[6] = PTE_V;
    assert(!sv39_lookup_leaf(root, 0x40806000UL, NULL, NULL));

    /* Encodings the hardware faults on are not mappings: W without R, and a
     * superpage whose PPN is not aligned to its size.
     */
    l0[7] = PA2PTE(0x90008000UL) | PTE_V | PTE_W;
    assert(!sv39_lookup_leaf(root, 0x40807000UL, NULL, NULL));
    l1[3] = PA2PTE(0x88200000UL | 0x5000UL) | LEAF_RW;
    assert(!sv39_lookup_leaf(root, 0x40600010UL, NULL, NULL));
    root[2] = PA2PTE(0x140000000UL | MIB2) | LEAF_RW;
    assert(!sv39_lookup_leaf(root, 0x80000010UL, NULL, NULL));

    /* Restore the superpages and compare the slot walks against them. */
    l1[3] = PA2PTE(0x88200000UL) | LEAF_RW;
    root[2] = PA2PTE(0x140000000UL) | LEAF_RW;
    check_walk_slot(root);
    check_ppn_mask();

    free(l0);
    free(l1);
    free(root);
    printf("sv39-lookup: ok\n");
    return 0;
}
