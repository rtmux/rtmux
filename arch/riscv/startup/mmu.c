/**
 * \file mmu.c
 *
 * RISC-V Sv39 page table setup for startup.
 *
 * Builds 3-level page tables and enables the MMU during startup. The kernel is
 * identity-mapped (VA == PA) so it continues to run at the same addresses after
 * the MMU is turned on.
 *
 *         SPDX-License-Identifier: BSD-2-Clause
 */

#include <startup.h>
#include <startup-api.h>
#include <paging.h>
#include <cpu-inline.h>

paddr_t page_directory;
uint64_t boot_satp_val;

#define MEGAPAGE_SIZE (2UL * 1024UL * 1024UL)
#define ALIGN_UP_2M(x) (((x) + MEGAPAGE_SIZE - 1) & ~(MEGAPAGE_SIZE - 1))
#define ALIGN_UP_4K(x) (((x) + 0xFFFUL) & ~0xFFFUL)


/**
 * \brief Initialize the page directory (root page table).
 *
 * Allocates one 4KB page table per CPU and records it in the syspage.
 */
static void riscv_init_pgdir(void)
{
    unsigned num = lsp.syspage.p->num_cpu;
    set_syspage_section(&lsp.cpu.pgdir, sizeof(paddr_t) * num);

    /* Page directory size is one page (512 entries, 8 bytes each) */
    unsigned pd_size = __PAGESIZE;
    page_directory = calloc_ram(pd_size * num, __PAGESIZE);

    /* Store per-CPU page directory physical addresses */
    for (unsigned i = 0; i < num; i++) {
        lsp.cpu.pgdir.p[i] = page_directory + i * __PAGESIZE;
    }
}

/**
 * \brief Convert RISCV_MAP_* flags to Sv39 PTE flags.
 */
static uint64_t map_flags_to_pte(unsigned flags)
{
    uint64_t pte_flags = PTE_V;

    if (flags == RISCV_MAP_SYSPAGE) {
        /* Kernel RW, no exec */
        pte_flags |= PTE_R | PTE_W | PTE_A | PTE_D | PTE_G;
    } else if (flags == RISCV_MAP_SYSPAGE_RO) {
        /* User read-only, no execute */
        pte_flags |= PTE_R | PTE_U | PTE_A;
    } else if (flags == RISCV_MAP_USER_RX) {
        /* User read + execute (kcall trampoline) */
        pte_flags |= PTE_R | PTE_X | PTE_U | PTE_A;
    } else {
        /* General mapping: default to kernel RWX */
        pte_flags |= PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_G;

        if (flags & RISCV_MAP_NOEXEC)
            pte_flags &= ~PTE_X;
        if (flags & RISCV_MAP_DEVICE)
            pte_flags &= ~PTE_X;
        /* RISCV_MAP_USER is reserved for narrow per-region exposure (e.g. the
         * kcall trampoline, the syspage's read-only view). The kernel heap is
         * NEVER mapped with PTE_U=1: it holds kernel objects (channels,
         * threads, page tables, ELF images) and the kernel L2 is shared across
         * every process's pgdir, so PTE_U=1 on the heap would expose every
         * kernel object to every U-mode process. The kernel reaches its own
         * heap through the kernel VA in S-mode without needing PTE_U. Things
         * U-mode roottask legitimately needs to dereference (process_t /
         * channel_t / thread_t fields surfaced by query_object) must be copied
         * into a U-mode buffer or read via a PRIV kernel extension that uses
         * SUM after switching as_regions — not by exposing the heap wholesale.
         */
        if (flags & RISCV_MAP_USER)
            pte_flags |= PTE_U;
    }

    return pte_flags;
}

/**
 * \brief Check if a PTE is a leaf (has any of R, W, X set).
 */
static inline int pte_is_leaf(pte_t entry)
{
    return (entry & (PTE_R | PTE_W | PTE_X)) != 0;
}

/**
 * \brief Split a megapage (2MB leaf at L1) into 512 individual 4KB PTEs.
 *
 * Allocates a new L0 page table, fills it with 512 entries that reproduce the
 * same mapping as the original megapage, then replaces the L1 leaf with a
 * pointer to the new L0 table.
 */
static void split_megapage(pte_t *l1_table, unsigned l1_idx, pte_t entry)
{
    paddr_t mega_pa = PTE2PA(entry);
    uint64_t flags =
        entry & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_V | PTE_A | PTE_D | PTE_G);

    paddr_t new_pt = calloc_ram(__PAGESIZE, __PAGESIZE);
    pte_t *l0_table = MAKE_1TO1_PTR(new_pt);

    for (unsigned i = 0; i < 512; i++) {
        l0_table[i] = PA2PTE(mega_pa + i * PGSIZE) | flags;
    }

    /* Replace the megapage leaf with a pointer to the L0 table */
    l1_table[l1_idx] = PA2PTE(new_pt) | PTE_V;

    /* Flush stale TLB entries for the old megapage */
    sfence_vma();
}

/**
 * \brief Walk the page table, creating intermediate levels as needed,
 *        and install a leaf PTE.
 *
 * For 4KB pages: walks all 3 levels (L2 -> L1 -> L0). For 2MB megapages: walks
 * 2 levels (L2 -> L1), installs leaf at L1. An existing megapage is split when
 * 4KB granularity is required.
 *
 * \param root Physical address of the root page table
 * \param va Virtual address to map
 * \param pa Physical address to map to
 * \param pte_flags PTE flags (PTE_V | PTE_R | etc.)
 * \param megapage If true, install a 2MB megapage at L1
 */
static void map_page(paddr_t root,
                     uintptr_t va,
                     paddr_t pa,
                     uint64_t pte_flags,
                     int megapage)
{
    pte_t *pgtable = MAKE_1TO1_PTR(root);

    /* Walk levels 2 and 1 (creating intermediate tables as needed) */
    int stop_level = megapage ? 1 : 0;

    for (int level = 2; level > stop_level; level--) {
        unsigned idx = PX(level, va);
        pte_t entry = pgtable[idx];

        if (entry & PTE_V) {
            if (level == 1 && !megapage && pte_is_leaf(entry)) {
                /* Existing 2MB megapage leaf, but we need 4KB granularity.
                 * Split it into 512 individual page entries.
                 */
                split_megapage(pgtable, idx, entry);
                entry = pgtable[idx];
            }
            /* Descend to next level */
            pgtable = MAKE_1TO1_PTR(PTE2PA(entry));
        } else {
            /* Allocate a new page table */
            paddr_t new_pt = calloc_ram(__PAGESIZE, __PAGESIZE);
            pgtable[idx] = PA2PTE(new_pt) | PTE_V;
            pgtable = MAKE_1TO1_PTR(new_pt);
        }
    }

    /* Install the leaf PTE */
    unsigned idx = PX(stop_level, va);
    pgtable[idx] = PA2PTE(pa) | pte_flags;
}

/**
 * \brief Invalidate the PTE for a single 4KB page, creating a guard page.
 *
 * If the page is part of a 2MB megapage, splits it first. After this call, any
 * access to \a va will trigger a page fault.
 */
void kernel_guard_page(uintptr_t va)
{
    paddr_t root = (r_satp() & 0xFFFFFFFFFFF) << PGSHIFT;
    pte_t *pgtable = MAKE_1TO1_PTR(root);

    for (int level = 2; level > 0; level--) {
        unsigned idx = PX(level, va);
        pte_t entry = pgtable[idx];

        if (!(entry & PTE_V))
            return; /* not mapped at all — already a guard */

        if (level == 1 && pte_is_leaf(entry)) {
            split_megapage(pgtable, idx, entry);
            entry = pgtable[idx];
        }
        pgtable = MAKE_1TO1_PTR(PTE2PA(entry));
    }

    /* Clear the L0 PTE — makes the page inaccessible */
    unsigned idx = PX(0, va);
    pgtable[idx] = 0;
    sfence_vma();
}

/**
 * \brief Map [paddr, paddr+size) with RISC-V specific attributes.
 *
 * If vaddr is ~0, auto-assign a virtual address. Otherwise, the mapping is made
 * at the specified virtual address.
 *
 * Uses the boot CPU's page table. SMP page tables will be synchronized later
 * when VM is initialized.
 *
 * \return The virtual address of the mapping.
 */
uintptr_t riscv_map(uintptr_t vaddr, paddr_t paddr, size_t size, unsigned flags)
{
    uint64_t pte_flags = map_flags_to_pte(flags);
    paddr_t root = page_directory; /* boot CPU page table */

    /* Auto-assign virtual address if requested. Use identity mapping (VA == PA)
     * — works both before MMU enable (direct PA access) and after (identity map
     * is in place).
     */
    if (vaddr == ~(uintptr_t) 0) {
        vaddr = (uintptr_t) paddr;
    }

    uintptr_t va = vaddr;
    paddr_t pa = paddr;
    size_t remaining = size;

    LOGF_T("riscv_map: va=%lx pa=%lx size=%lx flags=%x\n", (unsigned long) va,
           (unsigned long) pa, (unsigned long) size, flags);

    while (remaining > 0) {
        /* Use 2MB megapages when:
         * - Both VA and PA are 2MB-aligned
         * - At least 2MB remains
         */
        size_t megapage_size = 1UL << PXSHIFT(1); /* 2MB */

        if (remaining >= megapage_size && (va & (megapage_size - 1)) == 0 &&
            (pa & (megapage_size - 1)) == 0) {
            map_page(root, va, pa, pte_flags, 1);
            va += megapage_size;
            pa += megapage_size;
            remaining -= megapage_size;
        } else {
            map_page(root, va, pa, pte_flags, 0);
            va += PGSIZE;
            pa += PGSIZE;
            remaining -= (remaining >= PGSIZE) ? PGSIZE : remaining;
        }
    }

    return vaddr;
}

/**
 * \brief Identity-map a physical range (VA == PA).
 */
static void identity_map(paddr_t start, paddr_t end, unsigned flags)
{
    size_t size = (size_t) (end - start);
    if (size == 0)
        return;

    LOGF_D("  identity_map: %lx-%lx (%lu KB) flags=%x\n", (unsigned long) start,
           (unsigned long) end, (unsigned long) (size / 1024), flags);

    riscv_map((uintptr_t) start, start, size, flags);
}

/**
 * \brief Map a physical range into the high-VA kernel region.
 *
 * Maps [start, end) at VA = start + KERNEL_VIRT_BASE, creating a mirror of the
 * identity mapping in the upper canonical half.
 */
static void high_va_map(paddr_t start, paddr_t end, unsigned flags)
{
    size_t size = (size_t) (end - start);
    if (size == 0)
        return;

    LOGF_D("  high_va_map: %lx-%lx -> VA %lx (%lu KB) flags=%x\n",
           (unsigned long) start, (unsigned long) end,
           (unsigned long) (start + KERNEL_VIRT_BASE),
           (unsigned long) (size / 1024), flags);

    riscv_map((uintptr_t) start + KERNEL_VIRT_BASE, start, size, flags);
}

/**
 * \brief Enable the Sv39 MMU by writing SATP.
 *
 * Since the kernel is identity-mapped (VA == PA), execution continues
 * seamlessly after the SATP write. kprintf via SBI still works because UART and
 * RAM are both identity-mapped.
 */
static void enable_mmu(void)
{
    uint64_t satp_val = MAKE_SATP(page_directory);
    LOGF_D("enable_mmu: satp=%lx\n", (unsigned long) satp_val);

    /* Flush TLB before switching to Sv39, then again after. FreeBSD does only
     * sfence.vma before csrw satp at boot. We add one after as well for safety.
     */
    sfence_vma();
    w_satp(satp_val);
    sfence_vma();

    /* Save for AP boot — they load this value to enable the same page tables */
    boot_satp_val = satp_val;

    kprintf("enable_mmu: Sv39 paging active\n");
}

/**
 * \brief Initialize the MMU page tables.
 *
 * Allocates the root page table and identity-maps the kernel image so that VA
 * == PA continues to work when the MMU is eventually enabled.
 *
 * Does NOT enable the MMU (write to SATP) — that happens later when the kernel
 * is ready for virtual addressing.
 *
 * Additional mappings (syspage, cpupage) are created by riscv_map() called from
 * cpu_alloc_syspage_memory().
 */
void init_mmu(void)
{
    kprintf("init_mmu\n");
    LOGF_D("  building Sv39 page tables\n");

    riscv_init_pgdir();

    paddr_t top = ram_top();
    kprintf("  ram_top = %lx\n", (unsigned long) top);

    if (!ram_phys_start)
        crash("ram_phys_start not set — FDT memory@ node missing?\n");

    /* Identity-map RAM and I/O regions (VA == PA). Required for startup to
     * survive enable_mmu() — the PC is still at a PA when satp is written, so
     * the identity mapping must exist for the instructions between satp write
     * and the VA switch.
     */
    kprintf("  identity_map RAM %lx..%lx\n", (unsigned long) ram_phys_start,
            (unsigned long) top);
    identity_map(ram_phys_start, top, 0);

    if (uart_base) {
        kprintf("  identity_map UART %lx\n", (unsigned long) uart_base);
        identity_map(uart_base, uart_base + PGSIZE,
                     RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }
    if (clint_base) {
        kprintf("  identity_map CLINT %lx\n", (unsigned long) clint_base);
        identity_map(clint_base, clint_base + 0x10000,
                     RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }
    if (plic_base) {
        kprintf("  identity_map PLIC %lx\n", (unsigned long) plic_base);
        identity_map(plic_base, plic_base + plic_size,
                     RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }

    /* Identity-map the FDT if it lives outside the RAM range (U-Boot may
     * relocate it to high memory, e.g. just below 4 GiB). Needed because
     * init_hwinfo calls parse_fdt after MMU is on.
     */
    paddr_t fdt_pa = TRUNC((paddr_t) fdt_addr, __PAGESIZE);
    paddr_t fdt_end = ROUNDPG((paddr_t) fdt_addr + fdt_size);
    if (fdt_pa < ram_phys_start || fdt_end > top) {
        kprintf("  identity_map FDT %lx..%lx\n", (unsigned long) fdt_pa,
                (unsigned long) fdt_end);
        identity_map(fdt_pa, fdt_end, RISCV_MAP_NOEXEC);
    }

    /* High-VA mappings: the kernel's real address space. After
     * switch_to_high_va(), ALL kernel code runs here.
     *
     * The RAM map is split into three ranges so U-mode roottask can dereference
     * kernel-allocated objects (process_t, channel_t, thread_t, ...) returned
     * from query_object and friends:
     *
     *   [ram_phys_start, kimg_end_4K):   PTE_U=0  (kernel image —
     *      .text/.rodata/.data/.bss.  S-mode-fetchable;
     *      U-mode fetch from PTE_U=0 pages is illegal regardless
     *      of SUM, so kernel text MUST stay U=0.)
     *   [kimg_end_4K, kimg_end_2M):      PTE_U=1  (heap tail of the
     *      kernel-image megapage — first heap allocations land here,
     *      objects in this region must be U-readable; mapped at 4 KiB
     *      granularity since the boundary is page-aligned not megapage-
     *      aligned.)
     *   [kimg_end_2M, top):              PTE_U=1  (the bulk of heap —
     *      _smalloc'd objects and initramfs metadata. Megapaged for TLB
     *      efficiency.)
     *
     * MMU is OFF here. GCC's mcmodel=medany emits PC-relative addressing for
     * "_end" so the symbol resolves directly to its PA — subtracting
     * KERNEL_VIRT_BASE would underflow by 2^38.
     */
    paddr_t kimg_end_pa = (paddr_t) (uintptr_t) _end;
    paddr_t kimg_end_4k = ALIGN_UP_4K(kimg_end_pa);
    paddr_t kimg_end_2m = ALIGN_UP_2M(kimg_end_pa);

    kprintf("  high_va_map kernel image [%lx..%lx)\n",
            (unsigned long) ram_phys_start, (unsigned long) kimg_end_4k);
    high_va_map(ram_phys_start, kimg_end_4k, 0);

    if (kimg_end_4k < kimg_end_2m) {
        kprintf("  high_va_map heap-tail [%lx..%lx) PTE_U=0 (4K)\n",
                (unsigned long) kimg_end_4k, (unsigned long) kimg_end_2m);
        high_va_map(kimg_end_4k, kimg_end_2m, 0);
    }

    kprintf("  high_va_map heap [%lx..%lx) PTE_U=0\n",
            (unsigned long) kimg_end_2m, (unsigned long) top);
    high_va_map(kimg_end_2m, top, 0);

    if (uart_base) {
        kprintf("  high_va_map UART\n");
        high_va_map(uart_base, uart_base + PGSIZE,
                    RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }
    if (clint_base) {
        kprintf("  high_va_map CLINT\n");
        high_va_map(clint_base, clint_base + 0x10000,
                    RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }
    if (plic_base) {
        kprintf("  high_va_map PLIC\n");
        high_va_map(plic_base, plic_base + plic_size,
                    RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }

#ifdef CONFIG_MONITORING_KTHREAD
    /* Monitor kthread needs direct PCI access (ECAM walk + MMIO write to
     * pci-serial UART) — map ECAM + every pci_fdt window. Both identity (PA)
     * and high-VA (kernel) sides.
     */
    if (pci_fdt.ecam_size) {
        kprintf("  monitor: map PCI ECAM %lx (%lx bytes)\n",
                (unsigned long) pci_fdt.ecam_base,
                (unsigned long) pci_fdt.ecam_size);
        identity_map(pci_fdt.ecam_base, pci_fdt.ecam_base + pci_fdt.ecam_size,
                     RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
        high_va_map(pci_fdt.ecam_base, pci_fdt.ecam_base + pci_fdt.ecam_size,
                    RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }
    for (unsigned i = 0; i < pci_fdt.num_windows; i++) {
        paddr_t base = pci_fdt.windows[i].cpu_addr;
        paddr_t size = pci_fdt.windows[i].size;
        if (size == 0)
            continue;
        kprintf("  monitor: map PCI window[%u] %lx (%lx bytes)\n", i,
                (unsigned long) base, (unsigned long) size);
        identity_map(base, base + size, RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
        high_va_map(base, base + size, RISCV_MAP_DEVICE | RISCV_MAP_NOEXEC);
    }
#endif

    /* Pre-allocate empty L1 page tables for every kernel L2 slot.
     *
     * Sv39 splits the address space into 512 L2 slots of 1 GiB each; with
     * KERNEL_VIRT_BASE = 0xffffffc000000000 the kernel half is slots 256..511.
     * init_mmu has populated whatever subset of those slots needs leaf mappings
     * right now (RAM direct map, MMIO...). Slots that ended up empty are still
     * empty.
     *
     * cpu_pgdir_create() copies the kernel L2 entries into every user process's
     * root page table when that process is created. After that, any kernel
     * mapping installed in a previously-empty L2 slot (e.g. a thread kernel
     * stack allocated in a fresh 1 GiB region after roottask started) becomes
     * visible only through the kernel's own pgdir — user pgdirs created earlier
     * still see the old, empty slot. When the kernel later runs under such a
     * user satp (every trap to a user thread) and pushes registers in the
     * affected region, it store-page-faults on the now-stale L2. This is the
     * fingerprint behind the "romfs hang" documented in
     * doc/tex/PortingStory/chapter06.tex.
     *
     * Cure: install an empty L1 page in every kernel L2 slot now, before any
     * user pgdir is created. Subsequent kernel mappings can only modify L1/L0
     * entries within those L1 pages, and those changes are visible through the
     * (shared) L2 PTE in every user page table — no L2 propagation race.
     *
     * Cost: at most 256 * 4KiB = 1 MiB of pinned RAM. In practice most slots
     * will already have valid L1s, so the real cost is far less.
     */
    {
        pte_t *l2 = MAKE_1TO1_PTR(page_directory);
        unsigned filled = 0;
        for (int i = 256; i < 512; i++) {
            if (l2[i] & PTE_V)
                continue;
            paddr_t l1_pa = calloc_ram(__PAGESIZE, __PAGESIZE);
            l2[i] = PA2PTE(l1_pa) | PTE_V;
            filled++;
        }
        kprintf("  prealloc_kernel_l2: filled %u empty kernel L2 slots\n",
                filled);
    }

    kprintf("  page tables ready, root at %lx\n",
            (unsigned long) page_directory);

    /* Enable paging right away — the identity mapping keeps us running. */
    kprintf("  enable_mmu...\n");
    enable_mmu();
    kprintf("  MMU enabled\n");
}

/**
 * \brief Switch execution from PA to high VA.
 *
 * Called after the MMU is enabled with both identity and high-VA mappings
 * active. Adjusts sp and ra by va_offset (KERNEL_VIRT_BASE), then returns to
 * the caller at the high VA. Both PA and high VA resolve to the same physical
 * memory, so the transition is seamless.
 */
__attribute__((naked)) void switch_to_high_va(uintptr_t va_offset)
{
    __asm__ volatile(
        "add sp, sp, a0\n" /* stack pointer -> high VA */
        "add ra, ra, a0\n" /* return address -> high VA */
        "ret\n"            /* return to caller at high VA */
    );
}
