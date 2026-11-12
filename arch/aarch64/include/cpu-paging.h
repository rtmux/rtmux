/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_PAGING_H
#define RTMUX_AARCH64_PAGING_H
#define KERNEL_VIRT_BASE 0xffffffc000000000UL
#define PGSIZE 4096UL
#define PGSHIFT 12
#define PGROUNDUP(x) (((x) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(x) ((x) & ~(PGSIZE - 1))
#define PXMASK 0x1ff
#define PXSHIFT(level) (12 + 9 * (level))
#define PX(level, va) (((uint64_t) (va) >> PXSHIFT(level)) & PXMASK)
#define MAXVA (1UL << 38)
#define TRAMPOLINE (MAXVA - PGSIZE)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
/* Logical mapping attributes consumed by cpu_map_pages; not hardware bits. */
#define PTE_V 1UL
#define PTE_R 2UL
#define PTE_W 4UL
#define PTE_X 8UL
#define PTE_U 16UL
#define PTE_G 32UL
#define PTE_A 64UL
#define PTE_D 128UL
/* Native descriptor access. */
#define PTE2PA(p) ((p) & 0x0000fffffffff000UL)
#define PA2PTE(p) ((uint64_t) (p) & 0x0000fffffffff000UL)
#define RTMUX_SYSPAGE_UADDR 0x40001000UL
#define RTMUX_CPUPAGE_UADDR 0x40002000UL
#define RTMUX_KCALL_UADDR 0x40003000UL
#define RTMUX_ROOTTASK_UADDR_BASE 0xc0000000UL
#define RTMUX_ROOTTASK_UADDR_END 0x100000000UL
typedef uint64_t pde_t;
typedef uint64_t pte_t;
typedef uint64_t *pagetable_t;
static inline int cpu_pte_leaf(pte_t p, int level)
{
    return (p & 1) && (level == 0 || !(p & 2));
}
static inline unsigned cpu_pte_prot(pte_t p)
{
    if (!(p & 1))
        return 0;
    unsigned f = PTE_R;
    if (!(p & 128))
        f |= PTE_W;
    if (!(p & (1UL << 54)))
        f |= PTE_X;
    return f;
}
/* AF, inner-shareable normal memory; AP/UXN/PXN enforce EL0 permissions. */
static inline pte_t aarch64_page_attrs(unsigned f)
{
    pte_t p = 3 | (1UL << 10) | (3UL << 8) | (1UL << 2);
    if (!(f & PTE_W))
        p |= 1UL << 7;
    if (f & PTE_U)
        p |= (1UL << 6) | (1UL << 11) | (1UL << 53);
    else
        p |= 1UL << 54;
    if (!(f & PTE_X))
        p |= (3UL << 53);
    return p;
}
#endif
