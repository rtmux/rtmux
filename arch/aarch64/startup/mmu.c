/* SPDX-License-Identifier: Apache-2.0 */
#include <startup.h>
#include <startup-api.h>
#include <paging.h>
#include <cpu-inline.h>
paddr_t page_directory;
extern uint64_t boot_lower[], boot_upper[];
static pte_t *startup_walk(pagetable_t table, uintptr_t va)
{
    for (int level = 2; level > 0; level--) {
        pte_t *p = &table[PX(level, va)];
        if (!(*p & 1))
            *p = calloc_ram(4096, 4096) | 3;
        if (cpu_pte_leaf(*p, level))
            crash("startup mapping over block");
        table = (void *) (uintptr_t) PTE2PA(*p);
    }
    return &table[PX(0, va)];
}
uintptr_t aarch64_map(uintptr_t va, paddr_t pa, size_t size, unsigned flags)
{
    if (va == ~(uintptr_t) 0)
        return pa; /* startup's identity alias */
    unsigned f = PTE_R | PTE_U;
    if (flags == AARCH64_MAP_USER_RX)
        f |= PTE_X;
    else if (flags != AARCH64_MAP_SYSPAGE_RO)
        f |= PTE_W;
    for (size_t n = 0; n < size; n += 4096)
        *startup_walk(boot_lower, va + n) =
            PA2PTE(pa + n) | aarch64_page_attrs(f);
    cpu_tlb_flush();
    return va;
}
void init_mmu(void)
{
    page_directory = STARTUP_V2P(boot_lower);
    set_syspage_section(&lsp.cpu.pgdir,
                        sizeof(paddr_t) * lsp.syspage.p->num_cpu);
    for (unsigned c = 0; c < lsp.syspage.p->num_cpu; c++)
        lsp.cpu.pgdir.p[c] = page_directory;
    paddr_t upper_pa = calloc_ram(4096, 4096);
    pagetable_t upper = (void *) (uintptr_t) upper_pa;
    memcpy(upper, boot_upper, 4096);
    /* Preallocate all high-half slots so future kernel mappings are shared. */
    for (unsigned i = 256; i < 512; i++) {
        if (!upper[i])
            upper[i] = calloc_ram(4096, 4096) | 3 | (1UL << 61) | (1UL << 60);
        boot_lower[i] = upper[i];
    }
    /* Replace bootstrap RAM blocks with pages for the linked image. */
    extern char _text_start[], _text_end[], _rodata_start[], _rodata_end[];
    paddr_t ram_pa = calloc_ram(4096, 4096);
    pagetable_t ram = (void *) (uintptr_t) ram_pa;
    memcpy(ram, (void *) (uintptr_t) PTE2PA(upper[256]), 4096);
    /* The direct map is writable data. Only linked text is executable. */
    for (unsigned i = 0; i < 480; i++)
        ram[i] |= 1UL << 53;
    upper[256] = ram_pa | 3 | (1UL << 61) | (1UL << 60);
    boot_lower[256] = upper[256];
    paddr_t image_end = STARTUP_V2P(_end);
    for (paddr_t base = 0; base < image_end; base += 0x200000) {
        paddr_t pt = calloc_ram(4096, 4096);
        pagetable_t leaf = (void *) (uintptr_t) pt;
        for (unsigned j = 0; j < 512; j++) {
            paddr_t pa = base + j * 4096;
            unsigned f = PTE_R | PTE_W;
            if (pa >= STARTUP_V2P(_text_start) && pa < STARTUP_V2P(_text_end))
                f = PTE_R | PTE_X;
            else if (pa >= STARTUP_V2P(_rodata_start) &&
                     pa < STARTUP_V2P(_rodata_end))
                f = PTE_R;
            leaf[j] = pa | aarch64_page_attrs(f);
        }
        ram[base >> 21] = pt | 3;
    }
    extern uint64_t aarch64_kernel_ttbr1;
    aarch64_kernel_ttbr1 = upper_pa;
    __asm__ volatile("dc cvac,%0; dsb sy" ::"r"(&aarch64_kernel_ttbr1)
                     : "memory");
    __asm__ volatile("dsb ish; msr ttbr1_el1,%0; isb" ::"r"(upper_pa)
                     : "memory");
    cpu_tlb_flush();
}
