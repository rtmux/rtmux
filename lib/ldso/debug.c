/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 *
 * Support for printing debugging messages.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdarg.h>

#include "debug.h"
#include "dynlink.h"
#include "printf.h"

static const char rel_header[] =
    " symbol name               r_info r_offset st_value st_size    address    "
    "value\n"
    " -------------------------------------------------------------------------"
    "-----\n";
static const char rel_format[] = " %-25s %6lx %08lx %08lx %7d %10p %08lx\n";

int debug = 0;

static void dump_obj_relocations(Obj_Entry *obj);
static void dump_elf_rel(Obj_Entry *obj, const Elf_Rel *rel0, u_long relsize);
static void dump_elf_rela(Obj_Entry *obj,
                          const Elf_Rela *rela0,
                          u_long relasize);

void debug_printf(const char *format, ...)
{
    if (debug) {
        va_list ap;
        va_start(ap, format);

        ldso_vfdprintf(STDERR_FILENO, format, ap);
        ldso_fdputchar(STDERR_FILENO, '\n');

        va_end(ap);
    }
}

void dump_relocations(Obj_Entry *obj0)
{
    Obj_Entry *obj;

    for (obj = globallist_curr(obj0); obj; obj = globallist_next(obj)) {
        dump_obj_relocations(obj);
    }
}

static void dump_obj_relocations(Obj_Entry *obj)
{
    ldso_printf("Object \"%s\", relocbase %p\n", obj->path, obj->relocbase);

    if (obj->relsize) {
        ldso_printf("Non-PLT Relocations: %ld\n",
                    (obj->relsize / sizeof(Elf_Rel)));
        dump_elf_rel(obj, obj->rel, obj->relsize);
    }

    if (obj->relasize) {
        ldso_printf("Non-PLT Relocations with Addend: %ld\n",
                    (obj->relasize / sizeof(Elf_Rela)));
        dump_elf_rela(obj, obj->rela, obj->relasize);
    }

    if (obj->pltrelsize) {
        ldso_printf("PLT Relocations: %ld\n",
                    (obj->pltrelsize / sizeof(Elf_Rel)));
        dump_elf_rel(obj, obj->pltrel, obj->pltrelsize);
    }

    if (obj->pltrelasize) {
        ldso_printf("PLT Relocations with Addend: %ld\n",
                    (obj->pltrelasize / sizeof(Elf_Rela)));
        dump_elf_rela(obj, obj->pltrela, obj->pltrelasize);
    }
}

static void dump_elf_rel(Obj_Entry *obj, const Elf_Rel *rel0, u_long relsize)
{
    const Elf_Rel *rel;
    const Elf_Rel *rellim;
    const Elf_Sym *sym;
    Elf_Addr *dstaddr;

    ldso_putstr(rel_header);
    rellim = (const Elf_Rel *) ((const char *) rel0 + relsize);
    for (rel = rel0; rel < rellim; rel++) {
        dstaddr = (Elf_Addr *) (obj->relocbase + rel->r_offset);
        sym = obj->symtab + ELF_R_SYM(rel->r_info);
        ldso_printf(rel_format, obj->strtab + sym->st_name,
                    (u_long) rel->r_info, (u_long) rel->r_offset,
                    (u_long) sym->st_value, (int) sym->st_size, dstaddr,
                    (u_long) *dstaddr);
    }
}

static void dump_elf_rela(Obj_Entry *obj,
                          const Elf_Rela *rela0,
                          u_long relasize)
{
    const Elf_Rela *rela;
    const Elf_Rela *relalim;
    const Elf_Sym *sym;
    Elf_Addr *dstaddr;

    ldso_putstr(rel_header);
    relalim = (const Elf_Rela *) ((const char *) rela0 + relasize);
    for (rela = rela0; rela < relalim; rela++) {
        dstaddr = (Elf_Addr *) (obj->relocbase + rela->r_offset);
        sym = obj->symtab + ELF_R_SYM(rela->r_info);
        ldso_printf(rel_format, obj->strtab + sym->st_name,
                    (u_long) rela->r_info, (u_long) rela->r_offset,
                    (u_long) sym->st_value, (int) sym->st_size, dstaddr,
                    (u_long) *dstaddr);
    }
}
