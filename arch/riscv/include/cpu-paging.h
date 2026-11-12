/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_PAGING_H
#define _RISCV_PAGING_H

/* Start of Sv39 upper canonical half — kernel direct-map base */
#define KERNEL_VIRT_BASE 0xFFFFFFC000000000UL

#define PGSIZE 4096UL /* bytes per page */
#define PGSHIFT 12    /* bits of offset within a page */

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

#define PTE_V (1UL << 0) /* valid */
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4) /* 1 -> user can access */
#define PTE_G (1UL << 5) /* global */
#define PTE_A (1UL << 6) /* accessed */
#define PTE_D (1UL << 7) /* dirty */

/* shift a physical address to the right place for a PTE. */
#define PA2PTE(pa) ((((uint64_t) pa) >> 12) << 10)

#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)

/* extract the three 9-bit page table indices from a virtual address. */
#define PXMASK 0x1FF /* 9 bits */
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64_t) (va)) >> PXSHIFT(level)) & PXMASK)

/* one beyond the highest possible virtual address. MAXVA is actually one bit
 * less than the max allowed by Sv39, to avoid having to sign-extend virtual
 * addresses that have the high bit set.
 */
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))

/* Trampoline page: mapped at the highest user VA in both kernel and user
 * address spaces. Contains the SATP-switching trap entry/exit code.
 */
#define TRAMPOLINE (MAXVA - PGSIZE)

/* Per-process trapframe: one page below the trampoline. Mapped in user page
 * table but NOT accessible from U-mode (no PTE_U flag). The trampoline code
 * reads/writes it via the sscratch CSR.
 */
#define TRAPFRAME (TRAMPOLINE - PGSIZE)

/* Fixed user-space VAs for syspage and boot CPU cpupage. These live in VPN[2]=1
 * (0x40000000–0x7FFFFFFF), which is copied from the boot page table into every
 * new process page table by cpu_vmm_mcreate().
 * Mapped read-only (PTE_V|PTE_R|PTE_U) during startup.
 */
#define RTMUX_SYSPAGE_UADDR 0x40001000UL
#define RTMUX_CPUPAGE_UADDR 0x40002000UL
#define RTMUX_KCALL_UADDR                          \
    0x40003000UL /* ecall trampoline (R+X, one page) \
                  */

/* Reserved VA range for the U-mode roottask image (text/rodata + data/bss).
 * Lives in L2[3] = 0xC0000000-0xFFFFFFFF (1 GiB slot, Sv39 user half).
 *
 * Two kernel-internal callers use these:
 *   - kernel/initramfs-elf.c (B1 loader) uses BASE as roottask's load
 *     address when constructing pid 1's pgdir at boot.
 *   - kernel/mm/<helper> copies roottask's L2[3] entry into a child
 *     pgdir during loader-thread setup, and clears it back out before
 *     the child enters U-mode.
 *
 * Roottask's own loader (servers/roottask/sys/loader-elf.c) refuses to map
 * PT_LOAD segments that fall in this range — a malformed user ELF specifying
 * p_vaddr in here would otherwise rewrite L0 PTEs in the shared L1 page and
 * corrupt roottask itself.
 */
#define RTMUX_ROOTTASK_UADDR_BASE 0xC0000000UL
#define RTMUX_ROOTTASK_UADDR_END 0x100000000UL /* exclusive */

typedef uint64_t pde_t;
typedef uint64_t pte_t;
typedef uint64_t *pagetable_t; /* 512 PTEs */

static inline int cpu_pte_leaf(pte_t p,int level) { (void)level; return (p & (PTE_R|PTE_W|PTE_X)) != 0; }
static inline unsigned cpu_pte_prot(pte_t p) { return (unsigned)(p & (PTE_R|PTE_W|PTE_X)); }
#endif
