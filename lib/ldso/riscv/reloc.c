/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/cdefs.h>
#include <sys/types.h>

#include <stdlib.h>

#include "debug.h"
#include "dynlink.h"
#include "printf.h"

uint64_t set_gp(Obj_Entry *obj)
{
    uint64_t old;
    SymLook req;
    uint64_t gp;
    int res;

    __asm __volatile("mv    %0, gp" : "=r"(old));

    symlook_init(&req, "__global_pointer$");
    req.ventry = NULL;
    req.flags = SYMLOOK_EARLY;
    res = symlook_obj(&req, obj);

    if (res == 0) {
        gp = req.sym_out->st_value;
        __asm __volatile("mv    gp, %0" ::"r"(gp));
    }

    return (old);
}

void init_pltgot(Obj_Entry *obj)
{
    if (obj->pltgot) {
        obj->pltgot[0] = (Elf_Addr) &ldso_bind_start;
        obj->pltgot[1] = (Elf_Addr) obj;
    }
}

int do_copy_relocations(Obj_Entry *dstobj)
{
    const Obj_Entry *srcobj, *defobj;
    const Elf_Rela *relalim;
    const Elf_Rela *rela;
    const Elf_Sym *srcsym;
    const Elf_Sym *dstsym;
    const void *srcaddr;
    const char *name;
    void *dstaddr;
    SymLook req;
    size_t size;
    int res;

    /* COPY relocs are invalid outside of the main program */
    assert(dstobj->mainprog);

    relalim =
        (const Elf_Rela *) ((const char *) dstobj->rela + dstobj->relasize);
    for (rela = dstobj->rela; rela < relalim; rela++) {
        if (ELF_R_TYPE(rela->r_info) != R_RISCV_COPY)
            continue;

        /* Bound the symbol index against the symbol table, as reloc_plt does —
         * a malformed COPY reloc would otherwise index symtab[]/strtab past it.
         */
        if (ELF_R_SYM(rela->r_info) >= dstobj->dynsymcount)
            continue;

        dstaddr = (void *) (dstobj->relocbase + rela->r_offset);
        dstsym = dstobj->symtab + ELF_R_SYM(rela->r_info);
        name = dstobj->strtab + dstsym->st_name;
        size = dstsym->st_size;

        symlook_init(&req, name);
        req.ventry = fetch_ventry(dstobj, ELF_R_SYM(rela->r_info));
        req.flags = SYMLOOK_EARLY;

        for (srcobj = globallist_next(dstobj); srcobj;
             srcobj = globallist_next(srcobj)) {
            res = symlook_obj(&req, srcobj);
            if (res == 0) {
                srcsym = req.sym_out;
                defobj = req.defobj_out;
                break;
            }
        }
        if (!srcobj) {
            _ldso_error(
                "Undefined symbol \"%s\" referenced from COPY relocation in %s",
                name, dstobj->path);
            return (-1);
        }

        srcaddr = (const void *) (defobj->relocbase + srcsym->st_value);
        memcpy(dstaddr, srcaddr, size);
    }

    return (0);
}

/* Process the PLT relocations. */
int reloc_plt(Obj_Entry *obj,
              int flags __unused,
              LdsoLockState *lockstate __unused)
{
    const Elf_Rela *relalim;
    const Elf_Rela *rela;

    relalim =
        (const Elf_Rela *) ((const char *) obj->pltrela + obj->pltrelasize);
    for (rela = obj->pltrela; rela < relalim; rela++) {
        Elf_Addr *where;
        const Elf_Sym *sym;
        unsigned long symnum;

        where = (Elf_Addr *) (obj->relocbase + rela->r_offset);

        switch (ELF_R_TYPE(rela->r_info)) {
        case R_RISCV_JUMP_SLOT:
            symnum = ELF_R_SYM(rela->r_info);
            if (symnum < obj->dynsymcount) {
                sym = obj->symtab + symnum;
                if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0) {
                    if (ELF_ST_TYPE(sym->st_info) == STT_GNU_IFUNC)
                        obj->gnu_ifunc = true;
                    else
                        *where = (Elf_Addr) (obj->relocbase + sym->st_value);
                    break;
                }
            }
            *where += (Elf_Addr) obj->relocbase;
            break;
        case R_RISCV_IRELATIVE:
            obj->irelative = true;
            break;
        default:
            _ldso_error("Unknown relocation type %u in PLT",
                        (unsigned int) ELF_R_TYPE(rela->r_info));
            return (-1);
        }
    }

    return (0);
}

/* LD_BIND_NOW was set - force relocation for all jump slots */
int reloc_jmpslots(Obj_Entry *obj, int flags, LdsoLockState *lockstate)
{
    const Obj_Entry *defobj;
    const Elf_Rela *relalim;
    const Elf_Rela *rela;
    const Elf_Sym *def;

    relalim =
        (const Elf_Rela *) ((const char *) obj->pltrela + obj->pltrelasize);
    for (rela = obj->pltrela; rela < relalim; rela++) {
        Elf_Addr *where;

        where = (Elf_Addr *) (obj->relocbase + rela->r_offset);
        switch (ELF_R_TYPE(rela->r_info)) {
        case R_RISCV_JUMP_SLOT:
            def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
                              SYMLOOK_IN_PLT | flags, NULL, lockstate);
            if (!def) {
                dbg("reloc_jmpslots: sym not found");
                return (-1);
            }

            if (ELF_ST_TYPE(def->st_info) == STT_GNU_IFUNC) {
                obj->gnu_ifunc = true;
                continue;
            }

            *where = (Elf_Addr) (defobj->relocbase + def->st_value);
            break;
        default:
            _ldso_error("Unknown relocation type %x in jmpslot",
                        (unsigned int) ELF_R_TYPE(rela->r_info));
            return (-1);
        }
    }

    return (0);
}

static void reloc_iresolve_one(Obj_Entry *obj,
                               const Elf_Rela *rela,
                               LdsoLockState *lockstate)
{
    Elf_Addr *where, target, *ptr;

    ptr = (Elf_Addr *) (obj->relocbase + rela->r_addend);
    where = (Elf_Addr *) (obj->relocbase + rela->r_offset);
    lock_release(ldso_bind_lock, lockstate);
    target = call_ifunc_resolver(ptr);
    wlock_acquire(ldso_bind_lock, lockstate);
    *where = target;
}

int reloc_iresolve(Obj_Entry *obj, struct Struct_LdsoLockState *lockstate)
{
    const Elf_Rela *relalim;
    const Elf_Rela *rela;

    if (!obj->irelative)
        return (0);

    obj->irelative = false;
    relalim =
        (const Elf_Rela *) ((const char *) obj->pltrela + obj->pltrelasize);
    for (rela = obj->pltrela; rela < relalim; rela++) {
        if (ELF_R_TYPE(rela->r_info) == R_RISCV_IRELATIVE)
            reloc_iresolve_one(obj, rela, lockstate);
    }
    return (0);
}

int reloc_iresolve_nonplt(Obj_Entry *obj,
                          struct Struct_LdsoLockState *lockstate)
{
    const Elf_Rela *relalim;
    const Elf_Rela *rela;

    if (!obj->irelative_nonplt)
        return (0);

    obj->irelative_nonplt = false;
    relalim = (const Elf_Rela *) ((const char *) obj->rela + obj->relasize);
    for (rela = obj->rela; rela < relalim; rela++) {
        if (ELF_R_TYPE(rela->r_info) == R_RISCV_IRELATIVE)
            reloc_iresolve_one(obj, rela, lockstate);
    }
    return (0);
}

int reloc_gnu_ifunc(Obj_Entry *obj,
                    int flags,
                    struct Struct_LdsoLockState *lockstate)
{
    const Elf_Rela *relalim;
    const Elf_Rela *rela;
    Elf_Addr *where, target;
    const Elf_Sym *def;
    const Obj_Entry *defobj;

    if (!obj->gnu_ifunc)
        return (0);

    relalim =
        (const Elf_Rela *) ((const char *) obj->pltrela + obj->pltrelasize);
    for (rela = obj->pltrela; rela < relalim; rela++) {
        if (ELF_R_TYPE(rela->r_info) == R_RISCV_JUMP_SLOT) {
            where = (Elf_Addr *) (obj->relocbase + rela->r_offset);
            def = find_symdef(ELF_R_SYM(rela->r_info), obj, &defobj,
                              SYMLOOK_IN_PLT | flags, NULL, lockstate);
            if (!def)
                return (-1);
            if (ELF_ST_TYPE(def->st_info) != STT_GNU_IFUNC)
                continue;

            lock_release(ldso_bind_lock, lockstate);
            target = (Elf_Addr) ldso_resolve_ifunc(defobj, def);
            wlock_acquire(ldso_bind_lock, lockstate);
            reloc_jmpslot(where, target, defobj, obj, (const Elf_Rel *) rela);
        }
    }
    obj->gnu_ifunc = false;
    return (0);
}

Elf_Addr reloc_jmpslot(Elf_Addr *where,
                       Elf_Addr target,
                       const Obj_Entry *defobj __unused,
                       const Obj_Entry *obj __unused,
                       const Elf_Rel *rel)
{
    assert(ELF_R_TYPE(rel->r_info) == R_RISCV_JUMP_SLOT ||
           ELF_R_TYPE(rel->r_info) == R_RISCV_IRELATIVE);

    if (*where != target && !ld_bind_not)
        *where = target;
    return (target);
}

/* Process non-PLT relocations */
int reloc_non_plt(Obj_Entry *obj,
                  Obj_Entry *obj_ldso,
                  int flags,
                  LdsoLockState *lockstate)
{
    const Obj_Entry *defobj;
    const Elf_Rela *relalim;
    const Elf_Rela *rela;
    const Elf_Sym *def;
    SymCache *cache;
    Elf_Addr *where, symval;
    unsigned long symnum;


    /* The dynamic loader may be called from a thread, we have limited amounts
     * of stack available so we cannot use alloca().
     */
    ldso_trace("ldso: reloc_non_plt: enter\n");
    if (obj == obj_ldso)
        cache = NULL;
    else
        cache = calloc(obj->dynsymcount, sizeof(SymCache));
    /* No need to check for NULL here */

    ldso_trace("ldso: reloc_non_plt: loop start\n");
    relalim = (const Elf_Rela *) ((const char *) obj->rela + obj->relasize);
    for (rela = obj->rela; rela < relalim; rela++) {
        where = (Elf_Addr *) (obj->relocbase + rela->r_offset);
        symnum = ELF_R_SYM(rela->r_info);

        switch (ELF_R_TYPE(rela->r_info)) {
        case R_RISCV_JUMP_SLOT:
            /* This will be handled by the plt/jmpslot routines */
            break;
        case R_RISCV_NONE:
            break;
        case R_RISCV_64:
            def = find_symdef(symnum, obj, &defobj, flags, cache, lockstate);
            if (!def)
                return (-1);

            /* If symbol is IFUNC, only perform relocation when caller allowed
             * it by passing SYMLOOK_IFUNC flag. Skip the relocations otherwise.
             */
            if (ELF_ST_TYPE(def->st_info) == STT_GNU_IFUNC) {
                if ((flags & SYMLOOK_IFUNC) == 0) {
                    obj->non_plt_gnu_ifunc = true;
                    continue;
                }
                symval = (Elf_Addr) ldso_resolve_ifunc(defobj, def);
            } else {
                if ((flags & SYMLOOK_IFUNC) != 0)
                    continue;
                symval = (Elf_Addr) (defobj->relocbase + def->st_value);
            }

            *where = symval + (Elf_Addr) rela->r_addend;
            break;
        case R_RISCV_TLS_DTPMOD64:
            def = find_symdef(symnum, obj, &defobj, flags, cache, lockstate);
            if (!def)
                return -1;

            *where += (Elf_Addr) defobj->tlsindex;
            break;
        case R_RISCV_COPY:
            /* These are deferred until all other relocations have been done.
             * All we do here is make sure that the COPY relocation is not in a
             * shared library. They are allowed only in executable files.
             */
            if (!obj->mainprog) {
                _ldso_error(
                    "%s: Unexpected R_RISCV_COPY "
                    "relocation in shared library",
                    obj->path);
                return (-1);
            }
            break;
        case R_RISCV_TLS_DTPREL64:
            def = find_symdef(symnum, obj, &defobj, flags, cache, lockstate);
            if (!def)
                return (-1);

            *where += (Elf_Addr) (def->st_value + (Elf_Addr) rela->r_addend -
                                  TLS_DTV_OFFSET);
            break;
        case R_RISCV_TLS_TPREL64:
            def = find_symdef(symnum, obj, &defobj, flags, cache, lockstate);
            if (!def)
                return (-1);

            /* We lazily allocate offsets for static TLS as we see the first
             * relocation that references the TLS block. This allows us to
             * support (small amounts of) static TLS in dynamically loaded
             * modules. If we run out of space, we generate an error.
             */
            if (!defobj->tls_static) {
                if (!allocate_tls_offset(__DECONST(Obj_Entry *, defobj))) {
                    _ldso_error(
                        "%s: No space available for static "
                        "Thread Local Storage",
                        obj->path);
                    return (-1);
                }
            }

            *where = (def->st_value + (Elf_Addr) rela->r_addend +
                      defobj->tlsoffset - TLS_TP_OFFSET - TLS_TCB_SIZE);
            break;
        case R_RISCV_RELATIVE:
            *where = (Elf_Addr) (obj->relocbase + rela->r_addend);
            break;
        case R_RISCV_IRELATIVE:
            obj->irelative_nonplt = true;
            break;
        default:
            ldso_printf("%s: Unhandled relocation %lu\n", obj->path,
                        ELF_R_TYPE(rela->r_info));
            return (-1);
        }
    }

    return (0);
}

unsigned long elf_hwcap;

void ifunc_init(Elf_Auxinfo *aux_info[__min_size(AT_COUNT)])
{
    if (aux_info[AT_HWCAP])
        elf_hwcap = (unsigned long) aux_info[AT_HWCAP]->a_un.a_val;
}

void allocate_initial_tls(Obj_Entry *objs)
{
    /* Fix the size of the static TLS block by using the maximum offset
     * allocated so far and adding a bit for dynamic modules to use.
     */
    tls_static_space = tls_last_offset + tls_last_size + ld_static_tls_extra;

    _tcb_set(allocate_tls(objs, NULL, TLS_TCB_SIZE, TLS_TCB_ALIGN));
}

void *__tls_get_addr(tls_index *ti)
{
    uintptr_t **dtvp;
    void *p;

    dtvp = &_tcb_get()->tcb_dtv;
    p = tls_get_addr_common(dtvp, (int) ti->ti_module, ti->ti_offset);

    return ((char *) p + TLS_DTV_OFFSET);
}
