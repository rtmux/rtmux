/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * machine/elf.h shim for RTMux ldso (RISC-V 64-bit)
 *
 * Maps FreeBSD's generic Elf_* aliases to RTMux's Elf64_* types.
 */
#ifndef _MACHINE_ELF_H
#define _MACHINE_ELF_H

#include <sys/elf.h>

/* Generic ELF type aliases for 64-bit */
typedef Elf64_Addr Elf_Addr;
typedef Elf64_Off Elf_Off;
typedef Elf64_Half Elf_Half;
typedef Elf64_Sword Elf_Sword;
typedef Elf64_Xword Elf_Xword;
typedef Elf64_Sxword Elf_Sxword;
typedef Elf64_Word Elf_Word;
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
typedef Elf64_Shdr Elf_Shdr;
typedef Elf64_Sym Elf_Sym;
typedef Elf64_Rela Elf_Rela;
typedef Elf64_Rel Elf_Rel;

/* Elf_Dyn - check if defined */
#ifndef __ELF_DYN_DEFINED
#include <elf/dyn.h>
typedef Elf64_Dyn Elf_Dyn;
#define __ELF_DYN_DEFINED
#endif

/* Elf_Hashelt — hash table element */
typedef Elf32_Word Elf_Hashelt;

/* Elf_Size — used by FreeBSD ldso for magic/version in Obj_Entry */
typedef Elf64_Xword Elf_Size;

/* Packed relative relocations (RELR) */
typedef Elf64_Xword Elf_Relr;

/* ELF versioning types */
typedef struct {
    Elf64_Half vd_version;
    Elf64_Half vd_flags;
    Elf64_Half vd_ndx;
    Elf64_Half vd_cnt;
    Elf64_Word vd_hash;
    Elf64_Word vd_aux;
    Elf64_Word vd_next;
} Elf_Verdef;

typedef struct {
    Elf64_Word vda_name;
    Elf64_Word vda_next;
} Elf_Verdaux;

typedef struct {
    Elf64_Half vn_version;
    Elf64_Half vn_cnt;
    Elf64_Word vn_file;
    Elf64_Word vn_aux;
    Elf64_Word vn_next;
} Elf_Verneed;

typedef struct {
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
} Elf_Vernaux;

typedef Elf64_Half Elf_Versym;

/* Size and sign macros */
#define ELF_TARG_CLASS ELFCLASS64
#define ELF_TARG_DATA ELFDATA2LSB
#define ELF_TARG_MACH EM_RISCV
#define ELF_TARG_VER 1

#define __ELF_WORD_SIZE 64

/* Machine type checks */
#define ELF_ARCH EM_RISCV
#define ELF_MACHINE_OK(x) ((x) == ELF_ARCH)

/* Dynamic load address */
#define ET_DYN_LOAD_ADDR 0x100000

/* Auxiliary vector entry */
typedef struct {
    long a_type;
    union {
        long a_val;
        void *a_ptr;
        void (*a_fcn)(void);
    } a_un;
} Elf_Auxinfo;

/* ELF macros that FreeBSD's ldso uses */
#define ELF_ST_TYPE(info) ((info) & 0xf)
#define ELF_ST_BIND(info) ((info) >> 4)
#define ELF_ST_VISIBILITY(o) ((o) & 0x3)

#ifndef ELF_R_SYM
#define ELF_R_SYM(info) ((info) >> 32)
#define ELF_R_TYPE(info) ((info) & 0xffffffff)
#endif

#ifndef ELF_ST_INFO
#define ELF_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
#endif

/* Note header */
#ifndef __ELF_NOTE_DEFINED
typedef struct {
    Elf32_Word n_namesz;
    Elf32_Word n_descsz;
    Elf32_Word n_type;
} Elf_Note;
#define __ELF_NOTE_DEFINED
#endif

/* Relocation type test macros */
#define ELF_R_TYPE_EQ(a, b) ((a) == (b))

/* RISC-V specific relocation types */
#ifndef R_RISCV_NONE
#define R_RISCV_NONE 0
#define R_RISCV_32 1
#define R_RISCV_64 2
#define R_RISCV_RELATIVE 3
#define R_RISCV_COPY 4
#define R_RISCV_JUMP_SLOT 5
#define R_RISCV_TLS_DTPMOD32 6
#define R_RISCV_TLS_DTPMOD64 7
#define R_RISCV_TLS_DTPREL32 8
#define R_RISCV_TLS_DTPREL64 9
#define R_RISCV_TLS_TPREL32 10
#define R_RISCV_TLS_TPREL64 11
#define R_RISCV_IRELATIVE 58
#endif

/* Auxiliary vector types */
#define AT_NULL 0
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_NOTELF 10
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_EXECPATH 15
#define AT_CANARY 16
#define AT_CANARYLEN 17
#define AT_OSRELDATE 18
#define AT_NCPUS 19
#define AT_PAGESIZES 20
#define AT_PAGESIZESLEN 21
#define AT_TIMEKEEP 22
#define AT_STACKPROT 23
#define AT_EHDRFLAGS 24
#define AT_HWCAP 25
#define AT_HWCAP2 26
#define AT_BSDFLAGS 27
#define AT_ARGC 28
#define AT_ARGV 29
#define AT_ENVC 30
#define AT_ENVV 31
#define AT_PS_STRINGS 32
#define AT_FXRNG 33
#define AT_KPRELOAD 34
#define AT_USRSTACKBASE 35
#define AT_USRSTACKLIM 36
#define AT_COUNT 37
/* RTMux-private: set-id image, ignore unsecure LD_* variables. */
#define AT_RTMUX_SECURE 50

/* ELF symbol visibility */
#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3

/* ELF symbol types */
#ifndef STT_NOTYPE
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#endif

/* ELF symbol binding */
#ifndef STB_LOCAL
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#endif

/* HWCAP flags */
#define HWCAP_ISA_BIT(c) (1 << ((c) - 'a'))
#define HWCAP_ISA_I HWCAP_ISA_BIT('i')
#define HWCAP_ISA_M HWCAP_ISA_BIT('m')
#define HWCAP_ISA_A HWCAP_ISA_BIT('a')
#define HWCAP_ISA_F HWCAP_ISA_BIT('f')
#define HWCAP_ISA_D HWCAP_ISA_BIT('d')
#define HWCAP_ISA_C HWCAP_ISA_BIT('c')

#endif /* _MACHINE_ELF_H */
