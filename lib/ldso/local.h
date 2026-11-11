/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * local dynamic-linker build definitions. Keeps the self-contained ldso build
 * independent of libc.so during early relocation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DYNLINK_LOCAL_H
#define DYNLINK_LOCAL_H

#include <stdint.h>
#include <limits.h>
#include <align.h>

/* BSD utility macros */

#ifndef nitems
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

size_t strlcat(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);

/* Byte-order conversion (RTMux is always little-endian) */

static inline uint32_t __bswap32(uint32_t x)
{
    return ((x >> 24) & 0xff) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) |
           ((x << 24) & 0xff000000u);
}

#define le32toh(x) ((uint32_t) (x))
#define htole32(x) ((uint32_t) (x))
#define be32toh(x) __bswap32((uint32_t) (x))
#define htobe32(x) __bswap32((uint32_t) (x))

/* ELF Program Header types */

#ifndef PT_TLS
#define PT_TLS 7
#endif

#ifndef PT_GNU_EH_FRAME
#define PT_GNU_EH_FRAME 0x6474e550
#endif

#ifndef PT_GNU_STACK
#define PT_GNU_STACK 0x6474e551
#endif

#ifndef PT_GNU_RELRO
#define PT_GNU_RELRO 0x6474e552
#endif

/* ELF Dynamic tag constants */

#ifndef DT_FLAGS_1
#define DT_FLAGS_1 0x6ffffffb
#endif

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

#ifndef DT_RELR
#define DT_RELR 0x24
#endif

#ifndef DT_RELRSZ
#define DT_RELRSZ 0x23
#endif

#ifndef DT_RELRENT
#define DT_RELRENT 0x25
#endif

#ifndef DT_VERSYM
#define DT_VERSYM 0x6ffffff0
#endif

#ifndef DT_VERDEF
#define DT_VERDEF 0x6ffffffc
#endif

#ifndef DT_VERDEFNUM
#define DT_VERDEFNUM 0x6ffffffd
#endif

#ifndef DT_VERNEED
#define DT_VERNEED 0x6ffffffe
#endif

#ifndef DT_VERNEEDNUM
#define DT_VERNEEDNUM 0x6fffffff
#endif

#ifndef DT_AUXILIARY
#define DT_AUXILIARY 0x7ffffffd
#endif

#ifndef DT_FILTER
#define DT_FILTER 0x7fffffff
#endif

/* ELF Flags (DF_*, DF_1_*) */

#ifndef DF_ORIGIN
#define DF_ORIGIN 0x00000001
#endif
#ifndef DF_SYMBOLIC
#define DF_SYMBOLIC 0x00000002
#endif
#ifndef DF_TEXTREL
#define DF_TEXTREL 0x00000004
#endif
#ifndef DF_BIND_NOW
#define DF_BIND_NOW 0x00000008
#endif
#ifndef DF_STATIC_TLS
#define DF_STATIC_TLS 0x00000010
#endif

#ifndef DF_1_BIND_NOW
#define DF_1_BIND_NOW 0x00000001
#endif
#ifndef DF_1_GLOBAL
#define DF_1_GLOBAL 0x00000002
#endif
#ifndef DF_1_NODELETE
#define DF_1_NODELETE 0x00000008
#endif
#ifndef DF_1_LOADFLTR
#define DF_1_LOADFLTR 0x00000010
#endif
#ifndef DF_1_INITFIRST
#define DF_1_INITFIRST 0x00000020
#endif
#ifndef DF_1_NOOPEN
#define DF_1_NOOPEN 0x00000040
#endif
#ifndef DF_1_ORIGIN
#define DF_1_ORIGIN 0x00000080
#endif
#ifndef DF_1_INTERPOSE
#define DF_1_INTERPOSE 0x00000400
#endif
#ifndef DF_1_NODEFLIB
#define DF_1_NODEFLIB 0x00000800
#endif
#ifndef DF_1_PIE
#define DF_1_PIE 0x08000000
#endif

/* ELF Versioning constants */

#define VER_DEF_CURRENT 1
#define VER_NEED_CURRENT 1

#define VER_FLG_BASE 0x1
#define VER_FLG_WEAK 0x2

#define VER_NDX_LOCAL 0
#define VER_NDX_GLOBAL 1
#define VER_NDX_GIVEN 2
#define VER_NDX_HIDDEN 0x8000

#define VER_NDX(x) ((x) & (unsigned short) ~VER_NDX_HIDDEN)
#define VER_NEED_HIDDEN VER_NDX_HIDDEN

#define VER_DEF_IDX(x) VER_NDX(x)
#define VER_NEED_IDX(x) VER_NDX(x)

/* ELF misc constants */

#ifndef STN_UNDEF
#define STN_UNDEF 0
#endif

/* RTLD constants */

#ifndef RTLD_SELF
#define RTLD_SELF ((void *) -3)
#endif

#ifndef RTLD_MODEMASK
#define RTLD_MODEMASK 0x0003
#endif

#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x0004
#endif

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *) -2)
#endif

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *) -3)
#endif

#ifndef RTLD_NODELETE
#define RTLD_NODELETE 0x1000
#endif

#ifndef RTLD_TRACE
#define RTLD_TRACE 0x0200
#endif

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0x0400
#endif

/* dlinfo request codes */
#define RTLD_DI_LINKMAP 2
#define RTLD_DI_ORIGIN 6
#define RTLD_DI_SERINFOSIZE 5
#define RTLD_DI_SERINFO 4
#define RTLD_DI_MAX RTLD_DI_ORIGIN

/* Dl_serinfo structures */
typedef struct dl_serpath {
    char *dls_name;
    unsigned int dls_flags;
} Dl_serpath;

typedef struct dl_serinfo {
    size_t dls_size;
    unsigned int dls_cnt;
    Dl_serpath dls_serpath[1];
} Dl_serinfo;

/* Misc FreeBSD-isms */

#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 5 /* must match include/fcntl.h */
#endif

/* ELF identification macro */

#ifndef IS_ELF
#define IS_ELF(ehdr)                       \
    ((ehdr).e_ident[EI_MAG0] == ELFMAG0 && \
     (ehdr).e_ident[EI_MAG1] == ELFMAG1 && \
     (ehdr).e_ident[EI_MAG2] == ELFMAG2 && (ehdr).e_ident[EI_MAG3] == ELFMAG3)
#endif

/* Visibility fallbacks */

/* BSD dev_t constants */
#ifndef NODEV
#define NODEV ((dev_t) (-1))
#endif

/* dirent d_type constants */
#ifndef DT_REG
#define DT_REG 8
#define DT_DIR 4
#define DT_LNK 10
#endif

/* BSD integer type aliases */
typedef int64_t quad_t;
typedef uint64_t u_quad_t;
typedef unsigned int u_int;
typedef unsigned long u_long;

#ifndef __exported
#define __exported __attribute__((visibility("default")))
#endif
#ifndef __hidden
#define __hidden __attribute__((visibility("hidden")))
#endif

/* ldso_trace — compile out unless LDSO_VERBOSE is defined. The real function
 * lives in rtmux-ldso-stubs.c; without the flag every call expands to nothing.
 */

#ifdef LDSO_VERBOSE
void ldso_trace(const char *msg);
#else
#define ldso_trace(msg) ((void) 0)
#endif

#endif /* DYNLINK_LOCAL_H */
