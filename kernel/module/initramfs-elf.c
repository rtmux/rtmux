/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * ELF64 parsing for CPIO initramfs modules
 *
 * Parses ELF64 binaries from the CPIO archive. PT_LOAD segments are mapped into
 * the user process's Sv39 address space. Writable segments (.data, .bss) are
 * copied to freshly allocated memory so the CPIO original stays intact.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/elf.h>
#include <elf/dyn.h>
#include <cpu-elf.h>
#include <sys/cpio.h>
#include <sys/storage.h> /* struct _thread_local_storage */
#include <pthread.h>     /* RTMUX_SYNC_WAITING */
#include <stddef.h>      /* offsetof */
#include <paging.h>
#include <cpu-pgtable.h>
#include <module/initramfs-elf.h>
#include <elf-range.h>

/* Load address for ld-rtmux.so (matches ET_DYN_LOAD_ADDR in ldso machine/elf.h)
 */
#define LDSO_LOAD_BASE 0x100000UL


struct initramfs_module initramfs_modules[INITRAMFS_MAX_MODULES];
int initramfs_nmodules;

/* initramfs_elf_check and initramfs_elf_find_rtmux live in elf-validate.c, the
 * paging-free trust-boundary half of this loader (host-fuzzed separately).
 */

static int initramfs_elf_phdr_table_valid(const Elf64_Ehdr *ehdr, size_t size)
{
    return ehdr->e_phoff != 0 && ehdr->e_phnum != 0 &&
           ehdr->e_phentsize == sizeof(Elf64_Phdr) &&
           !elf_phdr_table_overflows(ehdr->e_phoff, ehdr->e_phnum,
                                     sizeof(Elf64_Phdr), size);
}

/* elf64_dyn_view — reusable ET_DYN parsing + relocation engine. See
 * module/initramfs-elf.h for the API contract and the three consumers.
 */

#ifndef DT_RELACOUNT
#define DT_RELACOUNT 0x6ffffff9
#endif

/* Open an ET_DYN/ET_EXEC ELF image into a reusable parsing view.
 *
 * Validates the ELF header, records the program-header table, computes the
 * page-aligned PT_LOAD span (va_lo/va_hi), and locates PT_DYNAMIC. Parameters:
 *   v    : view to populate (zeroed on entry).
 *   file : ELF image buffer.
 *   size : buffer size in bytes.
 *   name : module name, used in error messages.
 * Return value: 0 on success, -1 on validation failure.
 */
int elf64_dyn_open(struct elf64_dyn_view *v,
                   const void *file,
                   size_t size,
                   const char *name)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) file;

    memset(v, 0, sizeof(*v));

    if (size < sizeof(Elf64_Ehdr)) {
        kprintf("elf64_dyn: %s: too small for ELF header\n", name);
        return -1;
    }
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf("elf64_dyn: %s: not an ELF file\n", name);
        return -1;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        !cpu_elf_machine_valid(ehdr->e_machine)) {
        kprintf("elf64_dyn: %s: not a valid ELF64 LE binary\n", name);
        return -1;
    }
    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC) {
        kprintf("elf64_dyn: %s: not ET_DYN/ET_EXEC (type=%d)\n", name,
                ehdr->e_type);
        return -1;
    }
    if (!initramfs_elf_phdr_table_valid(ehdr, size)) {
        kprintf("elf64_dyn: %s: no program headers\n", name);
        return -1;
    }

    v->file_base = (const uint8_t *) file;
    v->file_size = size;
    v->ehdr = ehdr;
    v->phdr = (const Elf64_Phdr *) (v->file_base + ehdr->e_phoff);
    v->phnum = ehdr->e_phnum;

    /* Compute PT_LOAD span and locate PT_DYNAMIC. */
    uintptr_t lo = ~(uintptr_t) 0;
    uintptr_t hi = 0;
    for (int i = 0; i < v->phnum; i++) {
        const Elf64_Phdr *p = &v->phdr[i];
        if (p->p_type == PT_LOAD) {
            uintptr_t s = (uintptr_t) p->p_vaddr;
            uintptr_t e;
            if (elf_addr_range_overflows(p->p_vaddr, p->p_memsz, &e)) {
                kprintf("elf64_dyn: %s: PT_LOAD %d address overflow\n", name,
                        i);
                return -1;
            }
            for (int j = 0; j < i; j++) {
                uintptr_t ps;
                uintptr_t pe;

                if (v->phdr[j].p_type != PT_LOAD)
                    continue;
                ps = (uintptr_t) v->phdr[j].p_vaddr;
                if (elf_addr_range_overflows(v->phdr[j].p_vaddr,
                                             v->phdr[j].p_memsz, &pe)) {
                    kprintf("elf64_dyn: %s: PT_LOAD %d address overflow\n",
                            name, j);
                    return -1;
                }
                if (s < pe && ps < e) {
                    kprintf("elf64_dyn: %s: PT_LOAD %d overlaps %d\n", name, i,
                            j);
                    return -1;
                }
            }
            if (s < lo)
                lo = s;
            if (e > hi)
                hi = e;
        } else if (p->p_type == PT_DYNAMIC) {
            v->dyn_phdr = p;
        }
    }
    if (lo == ~(uintptr_t) 0 || lo >= hi) {
        kprintf("elf64_dyn: %s: no PT_LOAD segments\n", name);
        return -1;
    }
    /* Page-align the span: lo down, hi up. */
    v->va_lo = lo & ~(uintptr_t) 0xFFF;
    if (elf_roundup_overflows(hi, 0x1000, &v->va_hi)) {
        kprintf("elf64_dyn: %s: PT_LOAD span round-up overflow\n", name);
        return -1;
    }

    return 0;
}

/* Parse the PT_DYNAMIC array into the view's dynsym/dynstr/rela fields.
 *
 * Resolves DT_SYMTAB/STRTAB/RELA/JMPREL pointers through the supplied
 * kernel-accessible alias bias, de-overlaps RELASZ against PLTRELSZ, and
 * derives the dynamic-symbol count. Parameters:
 *   v             : view to populate (must be opened first).
 *   dyn           : the PT_DYNAMIC entry array (kernel-readable).
 *   dyn_count     : number of entries in dyn.
 *   ptr_to_kalias : bias added to ELF d_ptr values to reach a
 *                   kernel-readable address.
 * Return value: 0 (always). A dynamic section this cannot trust leaves
 * dynsym_count at zero rather than failing loudly, so a caller that skips the
 * return value still gets no symbol resolution instead of a bad bound.
 */
int elf64_dyn_parse_dynamic(struct elf64_dyn_view *v,
                            const Elf64_Dyn *dyn,
                            size_t dyn_count,
                            uintptr_t ptr_to_kalias)
{
    size_t syment = sizeof(Elf64_Sym);

    for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
        case DT_SYMTAB:
            v->dynsym = (const Elf64_Sym *) (dyn[i].d_un.d_ptr + ptr_to_kalias);
            break;
        case DT_STRTAB:
            v->dynstr = (const char *) (dyn[i].d_un.d_ptr + ptr_to_kalias);
            break;
        case DT_SYMENT:
            syment = (size_t) dyn[i].d_un.d_val;
            break;
        case DT_RELA:
            v->rela_dyn =
                (const Elf64_Rela *) (dyn[i].d_un.d_ptr + ptr_to_kalias);
            break;
        case DT_RELASZ:
            v->rela_dyn_sz = (size_t) dyn[i].d_un.d_val;
            break;
        case DT_JMPREL:
            v->rela_plt =
                (const Elf64_Rela *) (dyn[i].d_un.d_ptr + ptr_to_kalias);
            break;
        case DT_PLTRELSZ:
            v->rela_plt_sz = (size_t) dyn[i].d_un.d_val;
            break;
        }
    }

    /* RELASZ may include PLTRELSZ (depending on linker convention). Subtract to
     * avoid double-applying.
     */
    if (v->rela_dyn && v->rela_dyn_sz > v->rela_plt_sz)
        v->rela_dyn_sz -= v->rela_plt_sz;

    if (v->dynsym && v->dynstr) {
        /* Symbol count: dynsym and dynstr are adjacent in the standard layout,
         * so (dynstr - dynsym) / syment gives the count.
         *
         * All three inputs come straight out of the object's dynamic section,
         * so none of them bounds anything on its own. A zero syment is the
         * sharpest case: RISC-V division by zero does not trap, it yields
         * all-ones, so dynsym_count would saturate and the symbol-index test in
         * apply_one_rela() would admit any index. Accept only the ABI entry
         * size, and keep the count to the symbols that fit inside the PT_LOAD
         * span. Anything else leaves dynsym_count at zero, which fails closed:
         * no symbol-bearing relocation resolves.
         */
        const uint8_t *lo = (const uint8_t *) (v->va_lo + ptr_to_kalias);
        const uint8_t *hi = (const uint8_t *) (v->va_hi + ptr_to_kalias);
        const uint8_t *sym = (const uint8_t *) v->dynsym;

        if (syment == sizeof(Elf64_Sym) &&
            (const char *) v->dynstr > (const char *) v->dynsym && sym >= lo &&
            sym < hi) {
            size_t span =
                (size_t) ((const char *) v->dynstr - (const char *) v->dynsym);

            if (span <= (size_t) (hi - sym))
                v->dynsym_count = span / syment;
        }
    }

    return 0;
}

/* Helper: apply a single relocation, dispatching through the supplied ops.
 *
 * Returns 1 on applied, 0 on skipped/unresolved (and bumps *unresolved if the
 * symbol couldn't be resolved).
 */
static int apply_one_rela(const struct elf64_dyn_view *v,
                          const Elf64_Rela *rel,
                          uintptr_t runtime_bias,
                          const struct elf64_reloc_ops *ops,
                          void *ctx,
                          unsigned *unresolved,
                          const char *name)
{
    uint32_t rtype = ELF64_R_TYPE(rel->r_info);
    uint32_t sym_idx = ELF64_R_SYM(rel->r_info);
    uintptr_t P = ops->locate(ctx, (uintptr_t) rel->r_offset);

    if (P == 0) {
        kprintf("elf64_dyn: %s: reloc r_offset=%lx not in any loaded segment\n",
                name, (unsigned long) rel->r_offset);
        return 0;
    }

    /* RELATIVE-class relocations don't need a symbol resolution. */
    if (cpu_apply_rel_relative(rtype, P, runtime_bias, rel->r_addend))
        return 1;

    /* Symbol-bearing relocation. */
    uintptr_t S = 0;
    if (sym_idx != 0 && sym_idx < v->dynsym_count) {
        const Elf64_Sym *sym = &v->dynsym[sym_idx];
        const char *sname =
            (sym->st_name != 0) ? (v->dynstr + sym->st_name) : "";
        S = ops->resolve(ctx, sname, sym, runtime_bias);
        if (S == 0) {
            kprintf("elf64_dyn: %s: unresolved: %s (rtype=%u)\n", name,
                    sname[0] ? sname : "<null>", rtype);
            (*unresolved)++;
            return 0;
        }
    }

    int rc = cpu_apply_reloc(rel, rtype, S, P, /*resolved=*/1, name);
    return (rc > 0) ? 1 : 0;
}

/* Apply all .rela.dyn and .rela.plt relocations of a parsed view. Parameters:
 *   v              : parsed view (dynamic section already parsed).
 *   runtime_bias   : load bias for RELATIVE relocations.
 *   ops            : locate/resolve callbacks.
 *   ctx            : opaque context passed to the callbacks.
 *   out_applied    : if non-NULL, receives the count of applied relocs.
 *   out_unresolved : if non-NULL, receives the count of unresolved syms.
 *   name           : module name, used in error messages.
 * Return value: 0 if all relocations resolved, -1 if any were unresolved.
 */
int elf64_dyn_apply_relocs(const struct elf64_dyn_view *v,
                           uintptr_t runtime_bias,
                           const struct elf64_reloc_ops *ops,
                           void *ctx,
                           unsigned *out_applied,
                           unsigned *out_unresolved,
                           const char *name)
{
    unsigned applied = 0;
    unsigned unresolved = 0;

    if (v->rela_dyn && v->rela_dyn_sz > 0) {
        unsigned n = (unsigned) (v->rela_dyn_sz / sizeof(Elf64_Rela));
        for (unsigned j = 0; j < n; j++)
            applied += (unsigned) apply_one_rela(
                v, &v->rela_dyn[j], runtime_bias, ops, ctx, &unresolved, name);
    }
    if (v->rela_plt && v->rela_plt_sz > 0) {
        unsigned n = (unsigned) (v->rela_plt_sz / sizeof(Elf64_Rela));
        for (unsigned j = 0; j < n; j++)
            applied += (unsigned) apply_one_rela(
                v, &v->rela_plt[j], runtime_bias, ops, ctx, &unresolved, name);
    }

    if (out_applied)
        *out_applied = applied;
    if (out_unresolved)
        *out_unresolved = unresolved;
    return (unresolved == 0) ? 0 : -1;
}

/* find_ldso_module — locate ld-rtmux.so in initramfs_modules[]. */
static const struct initramfs_module *find_ldso_module(void)
{
    for (int i = 0; i < initramfs_nmodules; i++) {
        if (initramfs_modules[i].type != RTMUX_TYPE_SHLIB)
            continue;
        const char *bn = initramfs_modules[i].name;
        const char *p;
        for (p = bn; *p; p++)
            if (*p == '/')
                bn = p + 1;
        if (!strcmp(bn, "ld-rtmux.so"))
            return &initramfs_modules[i];
    }
    return NULL;
}

/* load_ldso_sv39 — load ld-rtmux.so into a user address space at
 * LDSO_LOAD_BASE.
 *
 * Parses PT_LOAD segments from the raw ELF data recorded in
 * initramfs_modules[], allocates physical pages, copies file data + zeroes BSS,
 * and maps them into pgdir at LDSO_LOAD_BASE + p_vaddr.
 *
 * Returns the user-space entry point (LDSO_LOAD_BASE + e_entry), or 0 on
 * failure.
 */
static uintptr_t load_ldso_sv39(pagetable_t pgdir,
                                const struct initramfs_module *ldso_mod)
{
    const void *base = (const void *) ldso_mod->load_base;
    size_t size = ldso_mod->load_size;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;
    const Elf64_Phdr *phdr;

    if (!initramfs_elf_check(base, size))
        return 0;
    if (!initramfs_elf_phdr_table_valid(ehdr, size))
        return 0;

    phdr = (const Elf64_Phdr *) ((const uint8_t *) base + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        uintptr_t seg_va;
        uintptr_t tmp;
        size_t seg_memsz = (size_t) phdr[i].p_memsz;
        size_t seg_filesz = (size_t) phdr[i].p_filesz;
        size_t page_off;
        uintptr_t alloc_sz_u;
        size_t alloc_sz;

        if (seg_filesz > seg_memsz ||
            elf_addr_range_overflows(LDSO_LOAD_BASE, phdr[i].p_vaddr,
                                     &seg_va) ||
            elf_addr_range_overflows(seg_va, phdr[i].p_memsz, &tmp)) {
            kprintf("initramfs_elf: ld-rtmux.so: segment %d overflows VA\n", i);
            return 0;
        }
        page_off = seg_va & (PGSIZE - 1);
        if (elf_addr_range_overflows(page_off, seg_memsz, &alloc_sz_u) ||
            elf_roundup_overflows(alloc_sz_u, PGSIZE, &alloc_sz_u)) {
            kprintf("initramfs_elf: ld-rtmux.so: segment %d size overflow\n",
                    i);
            return 0;
        }
        alloc_sz = (size_t) alloc_sz_u;
        uint8_t *phys = (uint8_t *) _scalloc_pgalign(alloc_sz);
        if (!phys) {
            kprintf(
                "initramfs_elf: ld-rtmux.so: segment alloc failed (%lu "
                "bytes)\n",
                (unsigned long) alloc_sz);
            return 0;
        }

        if (seg_filesz > 0) {
            if (elf_file_range_overflows(phdr[i].p_offset, seg_filesz, size)) {
                kprintf(
                    "initramfs_elf: ld-rtmux.so: segment %d overflows file\n",
                    i);
                return 0;
            }
            const uint8_t *src = (const uint8_t *) base + phdr[i].p_offset;
            memcpy(phys + page_off, src, seg_filesz);
        }

        if (seg_memsz > seg_filesz)
            memset(phys + page_off + seg_filesz, 0, seg_memsz - seg_filesz);

        /* Pre-set PTE_A (and PTE_D on writable pages): the SiFive U74 uses
         * trap-based A/D management and page-faults on first access when these
         * are clear. QEMU sets them in hardware, which masked this on the
         * emulator. Mirrors map_flags_to_pte() in arch/riscv/startup/mmu.c.
         */
        uint64_t flags = PTE_V | PTE_U | PTE_A;
        if (phdr[i].p_flags & PF_R)
            flags |= PTE_R;
        if (phdr[i].p_flags & PF_W)
            flags |= PTE_W | PTE_D;
        if (phdr[i].p_flags & PF_X)
            flags |= PTE_X;

        paddr_t seg_pa = PTR_TO_PHYS(phys);
        if (cpu_map_pages(pgdir, PGROUNDDOWN(seg_va), seg_pa, alloc_sz,
                          flags) != 0) {
            kprintf("initramfs_elf: ld-rtmux.so: map failed for segment %d\n",
                    i);
            return 0;
        }

        LOGF_D("  ldso segment %d: VA %lx-%lx -> PA %lx flags=%lx\n", i,
               (unsigned long) PGROUNDDOWN(seg_va),
               (unsigned long) (PGROUNDDOWN(seg_va) + alloc_sz),
               (unsigned long) seg_pa, (unsigned long) flags);
    }

    /* Apply RELATIVE relocations so the ldso's GOT entries are valid before it
     * can self-relocate. JUMP_SLOT relocations are NOT applied here — the ldso
     * handles those itself via reloc_plt(). With -Bsymbolic, no PLT calls occur
     * during self-relocation.
     *
     * Find PT_DYNAMIC → parse DT_RELA/DT_RELASZ.
     */
    const Elf64_Phdr *dyn_phdr = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn_phdr = &phdr[i];
            break;
        }
    }

    if (dyn_phdr) {
        const Elf64_Dyn *dyn =
            (const Elf64_Dyn *) ((const uint8_t *) base + dyn_phdr->p_offset);
        const Elf64_Rela *rela = NULL;
        size_t relasz = 0;

        for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
            if (dyn[i].d_tag == DT_RELA)
                rela = (const Elf64_Rela *) ((const uint8_t *) base +
                                             dyn[i].d_un.d_ptr);
            else if (dyn[i].d_tag == DT_RELASZ)
                relasz = (size_t) dyn[i].d_un.d_val;
        }

        /* Apply RELATIVE relocations from .rela.dyn */
        if (rela && relasz > 0) {
            size_t nrelocs = relasz / sizeof(Elf64_Rela);
            int applied = 0;
            for (size_t i = 0; i < nrelocs; i++) {
                uint32_t type = ELF64_R_TYPE(rela[i].r_info);
                uintptr_t target_va = LDSO_LOAD_BASE + rela[i].r_offset;
                paddr_t target_pa = cpu_va2pa(pgdir, target_va);
                if (target_pa == 0) {
                    kprintf(
                        "initramfs_elf: ld-rtmux.so: reloc VA %lx not mapped\n",
                        (unsigned long) target_va);
                    continue;
                }
                uintptr_t P = (uintptr_t) PHYS_TO_PTR(target_pa);
                if (cpu_apply_rel_relative(type, P, LDSO_LOAD_BASE,
                                           rela[i].r_addend))
                    applied++;
            }
            (void) applied;
        }
    }

    uintptr_t entry = LDSO_LOAD_BASE + (uintptr_t) ehdr->e_entry;
    return entry;
}

/* Auxv entry as expected by FreeBSD ldso (both fields are long = 8 bytes on
 * RV64).
 */
struct ldso_auxv {
    long a_type;
    long a_val;
};

/* build_user_stack — construct the initial stack for ldso_bootstrap().
 *
 * Layout (growing downward from stack_va_top):
 *   [string data: prog_name]
 *   [auxv entries + AT_NULL terminator]
 *   [NULL]                 ← end of envp
 *   [NULL]                 ← end of argv
 *   [argv[0] pointer]      ← points to prog_name string on stack
 *   [argc = 1]
 *   ^^^ sp points here (16-byte aligned)
 *
 * Returns user VA of sp, or 0 on failure.
 */
static uintptr_t build_user_stack(paddr_t stack_pa,
                                  uintptr_t stack_va_top,
                                  const char *prog_name,
                                  uintptr_t phdr_va,
                                  int phnum,
                                  uintptr_t prog_entry,
                                  uintptr_t ldso_base)
{
    uint8_t *page = (uint8_t *) PHYS_TO_PTR(stack_pa);
    uintptr_t stack_va_base = stack_va_top - PGSIZE;
    size_t name_len = strlen(prog_name) + 1;

    /* Work from the top of the page downward. pos tracks offset within the
     * physical page.
     *
     * Layout (high to low):
     *   [string data]           ← unaligned, variable length
     * 16-byte alignment boundary
     *   [auxv entries]          ← 16 bytes each, naturally aligned
     *   [envp NULL]             ← 8 bytes
     *   [argv NULL]             ← 8 bytes
     *   [argv[0]]               ← 8 bytes
     *   [argc]                  ← 8 bytes
     *   ^^^ sp points here (16-byte aligned because 4 * 8 = 32
     *       below the aligned auxv base)
     */
    size_t pos = PGSIZE;

    /* Place prog_name string at top of stack */
    pos -= name_len;
    memcpy(page + pos, prog_name, name_len);
    uintptr_t name_va = stack_va_base + pos;

    /* Align down to 16 bytes before placing structured data */
    pos &= ~(size_t) 15;

    /* Auxv entries (6 entries + AT_NULL terminator = 7) */
    struct ldso_auxv auxv[] = {
        {7 /* AT_BASE */, (long) ldso_base},
        {3 /* AT_PHDR */, (long) phdr_va},
        {5 /* AT_PHNUM */, phnum},
        {4 /* AT_PHENT */, sizeof(Elf64_Phdr)},
        {9 /* AT_ENTRY */, (long) prog_entry},
        {6 /* AT_PAGESZ */, PGSIZE},
        {0 /* AT_NULL */, 0},
    };
    size_t auxv_sz = sizeof(auxv);
    pos -= auxv_sz;
    memcpy(page + pos, auxv, auxv_sz);

    /* NULL (end of envp) */
    pos -= sizeof(long);
    *(long *) (page + pos) = 0;

    /* NULL (end of argv) */
    pos -= sizeof(long);
    *(long *) (page + pos) = 0;

    /* argv[0] = pointer to prog_name string */
    pos -= sizeof(long);
    *(long *) (page + pos) = (long) name_va;

    /* argc = 1 */
    pos -= sizeof(long);
    *(long *) (page + pos) = 1;

    uintptr_t user_sp = stack_va_base + pos;
    return user_sp;
}

/* User-mode roottask loader — initramfs_elf_load_roottask
 *
 * Loads the static ET_EXEC roottask binary into pid 1's private as at a fixed
 * base address (ROOTTASK_LOAD_BASE). Sets up the initial user stack with TLS
 * and an empty argv frame.
 *
 * Strategy:
 *    1. Allocate ONE contiguous physical block big enough to cover
 *       the full PT_LOAD span (text + data + bss, page-aligned).
 *       Copy each PT_LOAD's file content at the correct intra-block
 *       offset; zero the BSS tail.  Single bias (kalias = block_base)
 *       handles all relocation lookups.
 *    2. Install per-segment PTEs into the new pgdir at base+vaddr,
 *       splitting the contiguous block into RX vs RW page ranges
 *       per-segment.
 *    3. Allocate a 16-page user stack just below TRAPFRAME, build
 *       TLS + argv frame at the top.
 *
 * Returns the user-space entry point (ROOTTASK_LOAD_BASE + e_entry), or 0 on
 * failure. Populates m->pgdir, m->user_sp, m->tls_uva, m->load_base,
 * m->load_size on success.
 */

/* ROOTTASK_LOAD_BASE — fixed user VA where roottask is laid down. Defined in
 * cpu-paging.h as RTMUX_ROOTTASK_UADDR_BASE so the loader
 * (servers/roottask/sys/loader-elf.c) and the loader-as helpers use the same
 * constant. See cpu-paging.h for the rationale (cannot collide with the L2[1]
 * syspage mapping shared by every U-mode process).
 */
#define ROOTTASK_LOAD_BASE RTMUX_ROOTTASK_UADDR_BASE

/* User stack: 16 pages = 64 KiB just below TRAPFRAME. */
#define ROOTTASK_STACK_PAGES 16

uintptr_t initramfs_elf_load_roottask(const void *file,
                                      size_t size,
                                      const char *name,
                                      struct initramfs_module *m)
{
    struct elf64_dyn_view v;
    int i;

    if (elf64_dyn_open(&v, file, size, name) != 0) {
        kprintf("%s: ELF header validation failed\n", name);
        return 0;
    }
    if (v.ehdr->e_type != ET_EXEC) {
        kprintf("%s: roottask must be static ET_EXEC\n", name);
        return 0;
    }
    uintptr_t load_bias = 0;

    /* (1) Fresh user pgdir (kernel high-half L2 entries already shared). */
    pagetable_t pgdir = cpu_pgdir_create();
    if (!pgdir) {
        kprintf("%s: cpu_pgdir_create failed\n", name);
        return 0;
    }

    /* (2) Allocate one contiguous block to hold the full PT_LOAD span. va_lo /
     * va_hi are page-aligned by elf64_dyn_open.
     */
    size_t total = v.va_hi - v.va_lo;
    uint8_t *block = (uint8_t *) _scalloc_pgalign(total);
    if (!block) {
        kprintf("%s: _scalloc_pgalign(%lu) failed\n", name,
                (unsigned long) total);
        cpu_pgdir_free(pgdir);
        return 0;
    }

    /* kalias_bias: a kernel-writable pointer for any ELF-VA in the loaded
     * image. ELF-VA "va" → kalias = (va - va_lo) + block.
     */
    uintptr_t kalias_bias = (uintptr_t) block - v.va_lo;

    /* Copy each PT_LOAD's file bytes into the right offset in block. */
    for (i = 0; i < v.phnum; i++) {
        const Elf64_Phdr *p = &v.phdr[i];
        if (p->p_type != PT_LOAD)
            continue;
        size_t filesz = (size_t) p->p_filesz;
        size_t memsz = (size_t) p->p_memsz;
        if (filesz > memsz) {
            kprintf("%s: segment %d filesz exceeds memsz\n", name, i);
            _sfree(block, total);
            cpu_pgdir_free(pgdir);
            return 0;
        }
        if (filesz > 0) {
            if (elf_file_range_overflows(p->p_offset, filesz, size)) {
                kprintf("%s: segment %d overflows file\n", name, i);
                _sfree(block, total);
                cpu_pgdir_free(pgdir);
                return 0;
            }
            uint8_t *dst = (uint8_t *) kalias_bias + (uintptr_t) p->p_vaddr;
            memcpy(dst, v.file_base + p->p_offset, filesz);
        }
        /* BSS already zeroed by _scalloc_pgalign. */
        (void) memsz;
    }

    /* (3) Install per-segment PTEs into pgdir at ROOTTASK_LOAD_BASE+vaddr.
     *     Segments may overlap at page granularity (text page-rounded
     *     up may meet data page-rounded down); install per-segment so
     *     each segment's flags are honoured.  PT_LOAD ordering in
     *     a sane linker output ensures no real overlap.
     */
    for (i = 0; i < v.phnum; i++) {
        const Elf64_Phdr *p = &v.phdr[i];
        if (p->p_type != PT_LOAD)
            continue;
        uintptr_t va_lo;
        uintptr_t va_hi;
        uintptr_t pg_hi;
        if (elf_addr_range_overflows(load_bias, p->p_vaddr, &va_lo) ||
            elf_addr_range_overflows(va_lo, p->p_memsz, &va_hi) ||
            elf_roundup_overflows(va_hi, PGSIZE, &pg_hi)) {
            kprintf("%s: segment %d VA overflow\n", name, i);
            _sfree(block, total);
            cpu_pgdir_free(pgdir);
            return 0;
        }
        uintptr_t pg_lo = va_lo & ~(uintptr_t) (PGSIZE - 1);
        size_t map_sz = pg_hi - pg_lo;

        /* Phys for the first page of this segment in our contiguous block. The
         * block is laid out so that ELF-VA "va" lives at (block + (va -
         * va_lo_of_image)). In ET_DYN that's (pg_lo - ROOTTASK_LOAD_BASE); in
         * ET_EXEC that's (pg_lo - v.va_lo).
         */
        uintptr_t kpg = ((uintptr_t) block) + (pg_lo - load_bias - v.va_lo);
        paddr_t pa = PTR_TO_PHYS((void *) kpg);

        /* Pre-set PTE_A (and PTE_D on writable pages): the SiFive U74 uses
         * trap-based A/D management and page-faults on first access when these
         * are clear. QEMU sets them in hardware, which masked this on the
         * emulator. Mirrors map_flags_to_pte() in arch/riscv/startup/mmu.c.
         */
        uint64_t flags = PTE_V | PTE_U | PTE_A;
        if (p->p_flags & PF_R)
            flags |= PTE_R;
        if (p->p_flags & PF_W)
            flags |= PTE_W | PTE_D;
        if (p->p_flags & PF_X)
            flags |= PTE_X;

        if (cpu_map_pages(pgdir, pg_lo, pa, map_sz, flags) != 0) {
            kprintf("%s: cpu_map_pages failed for segment %d\n", name, i);
            _sfree(block, total);
            cpu_pgdir_free(pgdir);
            return 0;
        }
    }

    cpu_icache_sync();

    /* (6) User stack — 16 pages just below TRAPFRAME, with TLS + empty argv
     * frame at the top so roottask's crt0 sees argc=0.
     */
    size_t stack_size = ROOTTASK_STACK_PAGES * PGSIZE;
    uintptr_t stack_va = TRAPFRAME - stack_size;
    uint8_t *stack_ptr = (uint8_t *) _scalloc_pgalign(stack_size);
    if (!stack_ptr ||
        cpu_map_pages(pgdir, stack_va, PTR_TO_PHYS(stack_ptr), stack_size,
                      PTE_V | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D) != 0) {
        kprintf("%s: stack mapping failed\n", name);
        _sfree(block, total);
        cpu_pgdir_free(pgdir);
        return 0;
    }

    /* Lay out the topmost stack page (mirrors initramfs_elf_load_sv39 static
     * path). Top-down:
     *
     *    TRAPFRAME -----------------------------------------
     *      TLS               (struct _thread_local_storage)
     *
     *      argv string pool  (argv[0]\0 argv[1]\0 ...)
     *
     *      argv frame        argc, argv[0..argc-1], NULL,
     *                        envp[0]=NULL                     <-- user_sp
     *
     * crt0.S reads "0(sp) = argc, 8(sp) = &argv[0]", so user_sp must be 16-byte
     * aligned and point at argc. __rtmux_syschan_init's "I am roottask"
     * early-return reads tls->__pid == ROOTTASK_PID so we prefill that. The
     * argv strings are taken from the pristine boot command line
     * (boot_command_line_orig — the snapshot saved before setup_cmdline()'s
     * in-place tokenisation), so roottask's own arg parser in roottask_main()
     * sees the unmodified "-vvv" the user typed on the kernel command line.
     */
    extern char boot_command_line_orig[];
    {
        size_t tls_sz =
            (sizeof(struct _thread_local_storage) + 15) & ~(size_t) 15;

        /* Pass 1 over the cmdline: count argc and total string bytes (including
         * a NUL after each token).
         */
        int argc = 0;
        size_t strings_sz = 0;
        {
            const char *p = boot_command_line_orig;
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                const char *start = p;
                while (*p && *p != ' ')
                    p++;
                argc++;
                strings_sz += (size_t) (p - start) + 1;
            }
        }

        /* Frame: argc(8) + argv[argc] pointers + argv NULL + envp NULL. */
        size_t frame_sz = ((size_t) argc + 3) * sizeof(uint64_t);
        size_t strings_a = (strings_sz + 15) & ~(size_t) 15;
        size_t frame_a = (frame_sz + 15) & ~(size_t) 15;
        size_t total_top = tls_sz + strings_a + frame_a;

        if (total_top > PGSIZE) {
            kprintf("%s: argv frame (%lu bytes) overflows stack page\n", name,
                    (unsigned long) total_top);
            _sfree(block, total);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        uintptr_t tls_uva = TRAPFRAME - tls_sz;
        uintptr_t strings_uva = tls_uva - strings_a;
        uintptr_t frame_uva = strings_uva - frame_a;
        size_t tls_off = PGSIZE - tls_sz;
        size_t strings_off = tls_off - strings_a;
        size_t frame_off = strings_off - frame_a;

        uint8_t *page = (uint8_t *) PHYS_TO_PTR(PTR_TO_PHYS(stack_ptr) +
                                                stack_size - PGSIZE);

        struct _thread_local_storage *tls =
            (struct _thread_local_storage *) (page + tls_off);
        tls->__pid = ROOTTASK_PID;
        tls->__tid = 1;
        tls->__errptr =
            (int *) ((uintptr_t) tls_uva +
                     offsetof(struct _thread_local_storage, __errval));
        tls->__owner =
            (unsigned) ((ROOTTASK_PID << 16) | 1) & ~RTMUX_SYNC_WAITING;
        tls->__stackaddr = (void *) stack_va;

        /* Pass 2: copy strings into the pool, build argv pointer table. */
        char *strings_dst = (char *) (page + strings_off);
        uint64_t *frame = (uint64_t *) (page + frame_off);
        frame[0] = (uint64_t) argc;
        {
            const char *p = boot_command_line_orig;
            int idx = 0;
            size_t soff = 0;
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                const char *start = p;
                while (*p && *p != ' ')
                    p++;
                size_t tlen = (size_t) (p - start);
                memcpy(strings_dst + soff, start, tlen);
                strings_dst[soff + tlen] = '\0';
                frame[1 + idx] = (uint64_t) (strings_uva + soff);
                soff += tlen + 1;
                idx++;
            }
        }
        frame[1 + argc] = 0;     /* argv terminator */
        frame[1 + argc + 1] = 0; /* envp terminator (no envp) */

        m->tls_uva = tls_uva;
        m->user_sp = frame_uva;
    }

    /* (7) Populate module record + return entry point. For ET_EXEC
     *     e_entry is already the final user VA; for ET_DYN we apply
     * the same bias used to map the segments.
     */
    m->pgdir = pgdir;
    m->load_base = load_bias;
    m->load_size = total;
    uintptr_t entry = load_bias + (uintptr_t) v.ehdr->e_entry;
    return entry;
}

/* initramfs_elf_load_sv39 — load a user ELF binary with Sv39 page tables.
 *
 * Allocates a user page table, copies each PT_LOAD segment into freshly
 * allocated physical pages, maps them at the ELF virtual addresses, and sets up
 * the user stack.
 *
 * For dynamically-linked executables (!is_static), also loads ld-rtmux.so into
 * the same address space and builds an initial stack with auxv so the ldso can
 * relocate the executable.
 *
 * Returns the entry point address, or 0 on failure. The initramfs_module is
 * populated with pgdir and user_sp.
 */
static uintptr_t initramfs_elf_load_sv39(const void *base,
                                         size_t size,
                                         const char *name,
                                         struct initramfs_module *m)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;
    const Elf64_Phdr *phdr;
    int i;

    phdr = (const Elf64_Phdr *) ((const uint8_t *) base + ehdr->e_phoff);

    /* Create user root page table */
    pagetable_t pgdir = cpu_pgdir_create();
    if (!pgdir) {
        kprintf("initramfs_elf: %s: failed to allocate page table\n", name);
        return 0;
    }

    uintptr_t load_lo = ~(uintptr_t) 0;
    uintptr_t load_hi = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        uintptr_t seg_va = (uintptr_t) phdr[i].p_vaddr;
        size_t seg_memsz = (size_t) phdr[i].p_memsz;
        size_t seg_filesz = (size_t) phdr[i].p_filesz;
        uintptr_t seg_end;
        size_t page_off = seg_va & (PGSIZE - 1);
        uintptr_t alloc_sz_u;
        size_t alloc_sz;

        if (seg_filesz > seg_memsz ||
            elf_addr_range_overflows(seg_va, seg_memsz, &seg_end)) {
            kprintf("initramfs_elf: %s: segment %d VA overflow\n", name, i);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        if (seg_va < load_lo)
            load_lo = seg_va;
        if (seg_end > load_hi)
            load_hi = seg_end;

        /* Allocate page-aligned physical pages for this segment */
        if (elf_addr_range_overflows(page_off, seg_memsz, &alloc_sz_u) ||
            elf_roundup_overflows(alloc_sz_u, PGSIZE, &alloc_sz_u)) {
            kprintf("initramfs_elf: %s: segment %d size overflow\n", name, i);
            cpu_pgdir_free(pgdir);
            return 0;
        }
        alloc_sz = (size_t) alloc_sz_u;
        uint8_t *phys = (uint8_t *) _scalloc_pgalign(alloc_sz);
        if (!phys) {
            kprintf("initramfs_elf: %s: segment alloc failed (%lu bytes)\n",
                    name, (unsigned long) alloc_sz);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        /* Copy file data at correct sub-page offset */
        if (seg_filesz > 0) {
            if (elf_file_range_overflows(phdr[i].p_offset, seg_filesz, size)) {
                kprintf("initramfs_elf: %s: segment %d overflows file\n", name,
                        i);
                cpu_pgdir_free(pgdir);
                return 0;
            }
            const uint8_t *src = (const uint8_t *) base + phdr[i].p_offset;
            memcpy(phys + page_off, src, seg_filesz);
        }

        /* Zero BSS */
        if (seg_memsz > seg_filesz)
            memset(phys + page_off + seg_filesz, 0, seg_memsz - seg_filesz);

        /* Determine PTE flags Pre-set PTE_A (and PTE_D on writable pages): the
         * SiFive U74 uses trap-based A/D management and page-faults on first
         * access when these are clear. QEMU sets them in hardware, which masked
         * this on the emulator. Mirrors map_flags_to_pte() in
         * arch/riscv/startup/mmu.c.
         */
        uint64_t flags = PTE_V | PTE_U | PTE_A;
        if (phdr[i].p_flags & PF_R)
            flags |= PTE_R;
        if (phdr[i].p_flags & PF_W)
            flags |= PTE_W | PTE_D;
        if (phdr[i].p_flags & PF_X)
            flags |= PTE_X;

        /* Map segment pages at page-aligned address */
        paddr_t seg_pa = PTR_TO_PHYS(phys);
        if (cpu_map_pages(pgdir, PGROUNDDOWN(seg_va), seg_pa, alloc_sz,
                          flags) != 0) {
            kprintf("initramfs_elf: %s: map failed for segment %d\n", name, i);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        LOGF_D("  segment %d: VA %lx-%lx -> PA %lx flags=%lx\n", i,
               (unsigned long) PGROUNDDOWN(seg_va),
               (unsigned long) (PGROUNDDOWN(seg_va) + alloc_sz),
               (unsigned long) seg_pa, (unsigned long) flags);
    }

    if (load_lo == ~(uintptr_t) 0) {
        kprintf("initramfs_elf: %s: no PT_LOAD segments\n", name);
        cpu_pgdir_free(pgdir);
        return 0;
    }

    /* Extract PHDR virtual address (for auxv AT_PHDR) */
    uintptr_t phdr_va = 0;
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_PHDR) {
            phdr_va = (uintptr_t) phdr[i].p_vaddr;
            break;
        }
    }

    /* Allocate and map user stack (16 pages = 64KB, below TRAPFRAME) */
#define USER_STACK_PAGES 16
    uintptr_t stack_size = USER_STACK_PAGES * PGSIZE;
    uintptr_t stack_va = TRAPFRAME - stack_size;
    uint8_t *stack_ptr = (uint8_t *) _scalloc_pgalign(stack_size);
    if (!stack_ptr) {
        kprintf("initramfs_elf: %s: stack allocation failed\n", name);
        cpu_pgdir_free(pgdir);
        return 0;
    }
    paddr_t stack_pa = PTR_TO_PHYS(stack_ptr);
    if (cpu_map_pages(pgdir, stack_va, stack_pa, stack_size,
                      PTE_V | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D) != 0) {
        kprintf("initramfs_elf: %s: stack mapping failed\n", name);
        cpu_pgdir_free(pgdir);
        return 0;
    }
    /* Lay out the topmost stack page from high to low:
     *
     *   [TRAPFRAME]
     *     pad to 16 B
     *     struct _thread_local_storage  (sizeof ≈ 104 B, padded to 112)
     *     argv/envp/auxv frame          (5 × 8 B zeroed: argc, argv[0]=0,
     *                                    envp[0]=0, auxv AT_NULL = {0,0})
     *   [user_sp]
     *     ...
     *   [TRAPFRAME - stack_size]
     *
     * The TLS *must* live at a mapped, writable user VA — roottask's crt0 calls
     * __rtmux_syschan_init -> getpid() before main(), and getpid() reads
     * __tls()->__pid (offset 0x20 in TLS). With tp=0 the load page-faults at
     * stval=0x20, which is exactly what we saw before this change. We populate
     * __pid = ROOTTASK_PID so __rtmux_syschan_init's "I am roottask" early
     * return at line 202 fires, skipping the channel registration that would
     * otherwise try to msg_send to proc service (which is roottask talking to
     * itself).
     *
     * The pages were _scalloc_pgalign'd so they're already zeroed — we only
     * need to write the non-zero TLS fields.
     */
    {
        size_t tls_sz =
            (sizeof(struct _thread_local_storage) + 15) & ~(size_t) 15;
        size_t argv_sz = 5 * sizeof(uint64_t);
        size_t total = tls_sz + ((argv_sz + 15) & ~(size_t) 15);

        uintptr_t tls_uva = TRAPFRAME - tls_sz;
        size_t tls_off_in_page =
            (PGSIZE - tls_sz); /* tls sits at very top of page */

        /* topmost stack page lives at PA (stack_pa + stack_size - PGSIZE) */
        uint8_t *page = (uint8_t *) PHYS_TO_PTR(stack_pa + stack_size - PGSIZE);
        struct _thread_local_storage *tls =
            (struct _thread_local_storage *) (page + tls_off_in_page);

        /* zero already, but be explicit for the fields we depend on */
        tls->__pid = ROOTTASK_PID; /* roottask is pid 1 */
        tls->__tid = 1;            /* first thread, 1-based POSIX */
        tls->__errptr =
            (int *) ((uintptr_t) tls_uva +
                     offsetof(struct _thread_local_storage, __errval));
        tls->__owner =
            (unsigned) ((ROOTTASK_PID << 16) | 1) & ~RTMUX_SYNC_WAITING;
        tls->__stackaddr = (void *) (uintptr_t) (TRAPFRAME - stack_size);

        m->tls_uva = tls_uva;

        /* argv frame is below the TLS; user_sp points at argc. 16-byte aligned
         * because tls_sz and total are both multiples of 16.
         */
        (void) argv_sz;
        m->user_sp = TRAPFRAME - total;
    }
    uintptr_t user_sp = m->user_sp; /* used by the dynamic path below as base */

    LOGF_D("  user stack: VA %lx-%lx -> PA %lx (%lu pages)\n",
           (unsigned long) stack_va, (unsigned long) TRAPFRAME,
           (unsigned long) stack_pa, (unsigned long) USER_STACK_PAGES);

    uintptr_t entry = (uintptr_t) ehdr->e_entry;

    kprintf("initramfs: loaded %s (sv39) at VA %lx, entry=%lx\n", name,
            (unsigned long) load_lo, (unsigned long) entry);

    /* For dynamically-linked executables, load ld-rtmux.so into the same
     * address space and build an initial stack with auxv so the ldso can
     * relocate the executable before jumping to it.
     */
    if (!m->is_static) {
        const struct initramfs_module *ldso_mod = find_ldso_module();
        if (!ldso_mod) {
            kprintf("initramfs_elf: %s: dynamic but ld-rtmux.so not found\n",
                    name);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        uintptr_t ldso_entry = load_ldso_sv39(pgdir, ldso_mod);
        if (ldso_entry == 0) {
            kprintf("initramfs_elf: %s: failed to load ld-rtmux.so\n", name);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        /* Pass PA of the topmost stack page (where argv/auxv go) */
        user_sp =
            build_user_stack(stack_pa + stack_size - PGSIZE, TRAPFRAME, name,
                             phdr_va, ehdr->e_phnum, entry, LDSO_LOAD_BASE);
        if (user_sp == 0) {
            kprintf("initramfs_elf: %s: failed to build user stack\n", name);
            cpu_pgdir_free(pgdir);
            return 0;
        }

        kprintf(
            "initramfs: %s is dynamic, entering via ld-rtmux.so at %lx, "
            "sp=%lx\n",
            name, (unsigned long) ldso_entry, (unsigned long) user_sp);
        entry = ldso_entry;
    }

    /* Populate module entry */
    m->pgdir = pgdir;
    m->user_sp = user_sp;

    return entry;
}

/* initramfs_elf_load_bare — load a user ELF binary in bare-mode (MMU off).
 *
 * Copies PT_LOAD segments directly to their link addresses (which are physical
 * addresses when linked for bare-mode).
 */
static uintptr_t initramfs_elf_load_bare(const void *base,
                                         size_t size,
                                         const char *name,
                                         struct initramfs_module *m)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;
    const Elf64_Phdr *phdr;
    int i;

    phdr = (const Elf64_Phdr *) ((const uint8_t *) base + ehdr->e_phoff);

    uintptr_t load_lo = ~(uintptr_t) 0;
    uintptr_t load_hi = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        uintptr_t seg_start = (uintptr_t) phdr[i].p_vaddr;
        uintptr_t seg_end;
        if (phdr[i].p_filesz > phdr[i].p_memsz ||
            elf_addr_range_overflows(seg_start, phdr[i].p_memsz, &seg_end)) {
            kprintf("initramfs_elf: %s: segment %d range overflow\n", name, i);
            return 0;
        }

        if (seg_start < load_lo)
            load_lo = seg_start;
        if (seg_end > load_hi)
            load_hi = seg_end;

        uint8_t *dst = (uint8_t *) seg_start;

        /* Copy file data */
        if (phdr[i].p_filesz > 0) {
            if (elf_file_range_overflows(phdr[i].p_offset, phdr[i].p_filesz,
                                         size)) {
                kprintf("initramfs_elf: %s: segment %d overflows file\n", name,
                        i);
                return 0;
            }
            const uint8_t *src = (const uint8_t *) base + phdr[i].p_offset;
            memcpy(dst, src, (size_t) phdr[i].p_filesz);
        }

        /* Zero BSS (memsz > filesz) */
        if (phdr[i].p_memsz > phdr[i].p_filesz) {
            memset(dst + phdr[i].p_filesz, 0,
                   (size_t) (phdr[i].p_memsz - phdr[i].p_filesz));
        }
    }

    if (load_lo == ~(uintptr_t) 0) {
        kprintf("initramfs_elf: %s: no PT_LOAD segments\n", name);
        return 0;
    }

    m->load_base = load_lo;
    m->load_size = (size_t) (load_hi - load_lo);

    kprintf("initramfs: loaded %s at %p, entry=%p\n", name, (void *) load_lo,
            (void *) (uintptr_t) ehdr->e_entry);

    return (uintptr_t) ehdr->e_entry;
}

/* initramfs_elf_load — populate an existing initramfs_modules[] slot.
 *
 * The slot has already been recorded by pass 1 (initramfs_load_cb) with name /
 * type / load_base / load_size; this call drives the actual Sv39 / bare load
 * and writes back is_static / pgdir / user_sp / entry.
 *
 * Returns the entry point, or 0 on failure (slot left with entry=0 /
 * pgdir=NULL).
 *
 * Slot recording is done by pass 1 (initramfs_load_cb); this function only
 * fills the load-time fields of the existing slot it is handed.
 */
uintptr_t initramfs_elf_load(struct initramfs_module *m,
                             const void *base,
                             size_t size,
                             const char *name)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;
    uintptr_t entry;

    if (!initramfs_elf_check(base, size))
        return 0;
    if (!initramfs_elf_phdr_table_valid(ehdr, size))
        return 0;

    /* Initialize the load-time fields; pass 1 populated only the metadata (name
     * / type / load_base / load_size).
     */
    m->pgdir = NULL;
    m->user_sp = 0;
    m->entry = 0;

    /* Check for PT_INTERP to determine static vs dynamic */
    {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *) ((const uint8_t *) base + ehdr->e_phoff);
        bool has_interp = false;
        for (int j = 0; j < ehdr->e_phnum; j++) {
            if (ph[j].p_type == PT_INTERP) {
                has_interp = true;
                break;
            }
        }
        m->is_static = !has_interp;
    }

    if (__cpu_flags & CPU_FLAG_MMU)
        entry = initramfs_elf_load_sv39(base, size, name, m);
    else
        entry = initramfs_elf_load_bare(base, size, name, m);

    if (entry == 0)
        return 0;

    m->entry = entry;
    return entry;
}

/* Allocate the next initramfs slot, or crash if the table is full.
 *
 * Silent truncation was the bug: once crt0.o stamps .rtmux into every userland
 * binary, an oversized CPIO would drop trailing modules with no diagnostic and
 * boot would later fail with a misleading "not found". Halt now, name the
 * offending module.
 */
static struct initramfs_module *initramfs_alloc_slot(const char *name,
                                                     uint16_t type)
{
    if (initramfs_nmodules >= INITRAMFS_MAX_MODULES) {
        kprintf(
            "initramfs: too many modules (>%d), cannot record %s — "
            "raise INITRAMFS_MAX_MODULES or shrink initramfs.cpio\n",
            INITRAMFS_MAX_MODULES, name);
        crash();
    }
    struct initramfs_module *m = &initramfs_modules[initramfs_nmodules++];
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = '\0';
    m->type = type;
    m->entry = 0;
    return m;
}

/* CPIO callback: examine each file and load ELF modules. */
static void initramfs_load_cb(const cpio_file_info_t *info, void *data)
{
    (void) data;

    /* Skip directories and empty entries */
    if (info->filesize == 0)
        return;

    /* Check if it's an ELF binary */
    if (!initramfs_elf_check(info->data, info->filesize))
        return;

    const struct rtmux_ident *qi =
        initramfs_elf_find_rtmux(info->data, info->filesize);
    if (!qi) {
        LOGF_D("initramfs: %s: ELF but no .rtmux section, skipping\n",
               info->filename);
        return;
    }

    struct initramfs_module *m;

    switch (qi->qi_type) {
    case RTMUX_TYPE_EXEC:
        m = initramfs_alloc_slot(info->filename, RTMUX_TYPE_EXEC);
        m->load_base = (uintptr_t) info->data;
        m->load_size = info->filesize;
        LOGF_I("initramfs: %s: executable (recorded)\n", info->filename);
        break;

    case RTMUX_TYPE_SHLIB:
        m = initramfs_alloc_slot(info->filename, RTMUX_TYPE_SHLIB);
        m->load_base = (uintptr_t) info->data;
        m->load_size = info->filesize;
        LOGF_I("initramfs: %s: shared library (recorded)\n", info->filename);
        break;

    default:
        kprintf("initramfs: %s: unknown RTMux type %d\n", info->filename,
                qi->qi_type);
        break;
    }
}

/* Scan the CPIO initramfs and load its executable modules.
 *
 * Pass 1 records every module by type (executables and shared libraries). Pass
 * 2 loads the recorded executables in place, skipping the ET_DYN roottask
 * (loaded separately in ksys_start_roottask_umode). Parameters:
 *   cpio_base : base of the CPIO archive.
 *   cpio_size : archive size in bytes.
 */
void initramfs_load_modules(const uint8_t *cpio_base, size_t cpio_size)
{
    initramfs_nmodules = 0;

    /* Pass 1: scan all CPIO entries, record modules by type. Executables are
     * NOT loaded yet — shared libraries must be available first (the loader
     * needs ld-rtmux.so for dynamic exes).
     */
    cpio_iterate(cpio_base, cpio_size, initramfs_load_cb, NULL);

    /* Do not pre-load ordinary executables here. Roottask mounts the CPIO
     * archive as / and spawns programs through the normal loader, so eagerly
     * building a page table, text/data copy, ldso mapping, and stack for every
     * /bin and /sbin program only burns boot memory before pid 1 has even run.
     * The U-mode roottask image is loaded explicitly by
     * ksys_start_roottask_umode() from the metadata recorded above.
     */

    kprintf("initramfs: %d module(s) found\n", initramfs_nmodules);
}

/* Find the first recorded initramfs module of a given type. Parameters:
 *   type : RTMUX_TYPE_* module type to search for.
 * Return value: pointer to the matching module, or NULL if none.
 */
const struct initramfs_module *initramfs_find_module(uint16_t type)
{
    int i;
    for (i = 0; i < initramfs_nmodules; i++) {
        if (initramfs_modules[i].type == type)
            return &initramfs_modules[i];
    }
    return NULL;
}
