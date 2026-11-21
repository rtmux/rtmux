/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DYNLINK_MACHDEP_H
#define DYNLINK_MACHDEP_H 1

#include <sys/types.h>
#include <align.h>
#include <machine/atomic.h>
#include <machine/tls.h>

struct Struct_Obj_Entry;

#define MD_OBJ_ENTRY

uint64_t set_gp(struct Struct_Obj_Entry *obj);

/* Return the address of the .dynamic section in the dynamic linker. */
#define dynlink_dynamic(obj)                                              \
    ({                                                                    \
        Elf_Addr _dynamic_addr;                                           \
        __asm __volatile("lla       %0, _DYNAMIC" : "=r"(_dynamic_addr)); \
        (const Elf_Dyn *) _dynamic_addr;                                  \
    })

Elf_Addr reloc_jmpslot(Elf_Addr *where,
                       Elf_Addr target,
                       const struct Struct_Obj_Entry *defobj,
                       const struct Struct_Obj_Entry *obj,
                       const Elf_Rel *rel);

#define make_function_pointer(def, defobj) \
    ((defobj)->relocbase + (def)->st_value)

#define call_initfini_pointer(obj, target)            \
    ({                                                \
        uint64_t old0;                                \
        old0 = set_gp(obj);                           \
        (((InitFunc) (target))());                    \
        __asm __volatile("mv    gp, %0" ::"r"(old0)); \
    })

#define call_init_pointer(obj, target)                             \
    ({                                                             \
        uint64_t old1;                                             \
        old1 = set_gp(obj);                                        \
        (((InitArrFunc) (target))(main_argc, main_argv, environ)); \
        __asm __volatile("mv    gp, %0" ::"r"(old1));              \
    })

extern unsigned long elf_hwcap;
#define call_ifunc_resolver(ptr)                                               \
    (((Elf_Addr (*)(unsigned long, unsigned long, unsigned long,               \
                    unsigned long, unsigned long, unsigned long,               \
                    unsigned long, unsigned long)) ptr)(elf_hwcap, 0, 0, 0, 0, \
                                                        0, 0, 0))

/* TLS */

#define calculate_first_tls_offset(size, align, offset) TLS_TCB_SIZE
#define calculate_tls_offset(prev_offset, prev_size, size, align, offset) \
    lib_align_up_size((prev_offset) + (prev_size), (align))
#define calculate_tls_post_size(align) 0

typedef struct {
    unsigned long ti_module;
    unsigned long ti_offset;
} tls_index;

extern void *__tls_get_addr(tls_index *ti);

#define md_abi_variant_hook(x)

#endif
