/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * module/initramfs-elf.h — ELF64 parsing for CPIO modules
 *
 * License: Apache 2.0
 */

#ifndef _INITRAMFS_ELF_H
#define _INITRAMFS_ELF_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/rtmux-ident.h>
#include <sys/elf.h>
#include <elf/dyn.h>

/* Initramfs slot count. Userland binaries that link crt0.o now carry a .rtmux
 * section, so every executable in initramfs.cpio gets recorded. v0.11 ships ~25
 * binaries plus 2 shlibs and roottask; conformance runs can inject dozens more.
 */
#define INITRAMFS_MAX_MODULES 128

/* Module descriptor */
struct initramfs_module {
    char name[64];       /* filename from CPIO (copied) */
    uint16_t type;       /* RTMUX_TYPE_* */
    bool is_static;      /* true = no PT_INTERP (statically linked) */
    uintptr_t entry;     /* e_entry (or 0 for shlibs) */
    uintptr_t load_base; /* lowest PT_LOAD vaddr */
    size_t load_size;    /* span from lowest to highest PT_LOAD */
    /* Sv39 fields (populated when MMU is on) */
    void *pgdir;       /* user root page table (pagetable_t) */
    uintptr_t user_sp; /* initial user stack pointer */
    uintptr_t tls_uva; /* user VA of preallocated TLS (static EXEC); 0 if N/A */
};

/* Module table (populated by initramfs_load_modules) */
extern struct initramfs_module initramfs_modules[];
extern int initramfs_nmodules;

/* Validate that a buffer holds a little-endian ELF64 image for a supported
 * machine.
 *
 * Returns true if it is a valid ELF64 image, false otherwise.
 */
bool initramfs_elf_check(const void *base, size_t size);

/* Find the .rtmux section in the ELF and return a pointer to its contents.
 * Returns NULL if not found.
 */
const struct rtmux_ident *initramfs_elf_find_rtmux(const void *base,
                                                   size_t size);

/* Load an ELF64 binary from CPIO into the supplied module slot. The slot must
 * already have name / type / load_base / load_size populated (typically by
 * initramfs_load_cb in pass 1). This call fills in is_static / pgdir / user_sp
 * / entry — driving the Sv39 page-table build (MMU on) or the bare-mode mapping
 * (MMU off). For writable segments (.data/.bss), copies data to freshly
 * allocated memory so the CPIO original stays intact.
 *
 * Returns the entry point address, or 0 on failure.
 */
uintptr_t initramfs_elf_load(struct initramfs_module *m,
                             const void *base,
                             size_t size,
                             const char *name);

/* Scan the CPIO archive, parse each file as ELF, and populate the
 * initramfs_modules[] table.
 */
void initramfs_load_modules(const uint8_t *cpio_base, size_t cpio_size);

/* Find a loaded module by RTMux type.
 *
 * Returns NULL if not found.
 */
const struct initramfs_module *initramfs_find_module(uint16_t type);

/* elf64_dyn_view — reusable ET_DYN parsing + relocation engine.
 *
 * Two callers share this machinery:
 *
 *    1. load_ldso_sv39 (this file) — loads ld-rtmux.so into a USER
 *       pgdir at LDSO_LOAD_BASE; only RELATIVE relocations.
 *    2. Future dynamic user loaders can reuse the same reloc engine with
 *       caller-supplied locate()/resolve() callbacks.
 *
 * The view holds raw-ELF + cached PT_DYNAMIC fields. Per-consumer logic is
 * supplied via two callbacks: locate() translates an ELF-VA r_offset to a
 * kernel-writable address (where the relocation value is to be stored), and
 * resolve() turns a dynamic-symbol reference into its runtime address.
 */

struct elf64_dyn_view {
    /* Filled by elf64_dyn_open() */
    const uint8_t *file_base;
    size_t file_size;
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    int phnum;
    /* PT_LOAD span (ELF-VAs, before any runtime bias).  Page-aligned. */
    uintptr_t va_lo;
    uintptr_t va_hi;
    const Elf64_Phdr *dyn_phdr; /* PT_DYNAMIC, or NULL */

    /* Filled by elf64_dyn_parse_dynamic() */
    const Elf64_Sym *dynsym;
    const char *dynstr;
    size_t dynsym_count;
    const Elf64_Rela *rela_dyn;
    size_t rela_dyn_sz;
    const Elf64_Rela *rela_plt;
    size_t rela_plt_sz;
};

/* Validate an ELF64 ET_DYN file and fill in "v".
 *
 * Returns 0 on success, -1 on failure (the function kprintfs the reason).
 */
int elf64_dyn_open(struct elf64_dyn_view *v,
                   const void *file,
                   size_t size,
                   const char *name);

/* Parse PT_DYNAMIC content. The caller has already loaded the binary somewhere
 * (kernel memory or a user pgdir) and supplies:
 *   dyn          — kernel-readable pointer to the start of the
 *                  PT_DYNAMIC entries (caller already translated)
 *   dyn_count    — entry budget; iteration stops at DT_NULL anyway
 *   ptr_to_kalias — bias to add to ELF-VA "d_ptr" fields (DT_SYMTAB,
 *                  DT_STRTAB, DT_RELA, DT_JMPREL) so they become
 *                  kernel-readable.  For a contiguous _scalloc
 *                  load, this equals (load_mem - va_lo).
 *
 * Returns 0 on success, -1 if required tables are missing.
 */
int elf64_dyn_parse_dynamic(struct elf64_dyn_view *v,
                            const Elf64_Dyn *dyn,
                            size_t dyn_count,
                            uintptr_t ptr_to_kalias);

/* Per-consumer reloc callbacks. */
struct elf64_reloc_ops {
    /* Translate an ELF-VA r_offset to the kernel-writable address where the
     * relocation result must be stored.
     *
     * Return 0 if the offset doesn't lie in any loaded segment (the engine will
     * skip that relocation and tally it as unresolved).
     */
    uintptr_t (*locate)(void *ctx, uintptr_t r_offset);

    /* Resolve a dynamic-symbol reference. "name" is the symbol name (may be
     * empty for STN_UNDEF=0, used by RELATIVE relocs but those don't reach this
     * callback). "sym" is the dynsym entry. "runtime_bias" is the bias applied
     * to internally defined symbols (st_value + bias for st_shndx !=
     * SHN_UNDEF).
     *
     * Return the resolved runtime address, or 0 if unresolved.
     */
    uintptr_t (*resolve)(void *ctx,
                         const char *name,
                         const Elf64_Sym *sym,
                         uintptr_t runtime_bias);
};

/* Apply DT_RELA + DT_JMPREL. RELATIVE relocations get "runtime_bias";
 * symbol-bearing relocations are dispatched through ops->resolve.
 *
 * Returns 0 on success, -1 if any unresolved symbols remain (the engine counts
 * them in *out_unresolved but does not crash itself — the caller decides
 * whether unresolved symbols are fatal).
 */
int elf64_dyn_apply_relocs(const struct elf64_dyn_view *v,
                           uintptr_t runtime_bias,
                           const struct elf64_reloc_ops *ops,
                           void *ctx,
                           unsigned *out_applied,
                           unsigned *out_unresolved,
                           const char *name);

/* initramfs_elf_load_roottask — B1 (in-kernel) loader for static ET_EXEC
 * roottask. Loads roottask into pid 1's private as at a fixed base address and
 * sets up TLS + initial stack. Called from ksys_start_roottask_umode() at boot.
 * See implementation for full contract.
 *
 * Returns the relocated user-space entry point, or 0 on failure. Populates
 * m->pgdir, m->user_sp, m->tls_uva, m->load_base, m->load_size on success.
 */
uintptr_t initramfs_elf_load_roottask(const void *file,
                                      size_t size,
                                      const char *name,
                                      struct initramfs_module *m);

#endif /* _INITRAMFS_ELF_H */
