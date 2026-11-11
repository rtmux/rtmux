/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* RTMux binary identification section
 *
 * Every RTMux binary (executables and shared libraries) must contain a ".rtmux"
 * ELF section identifying it as a RTMux binary. See RTMUX_NT_* in notes.h.
 *
 * Usage (source-level):
 *
 *   #include <sys/rtmux-ident.h>
 *   RTMUX_IDENT(RTMUX_TYPE_EXEC, 0, 1);
 *
 * Usage (post-link, for pre-built binaries):
 *
 *   objcopy --add-section .rtmux=rtmux_ident.bin \
 *           --set-section-flags .rtmux=noload,readonly binary
 */

#ifndef _SYS_RTMUX_IDENT_H
#define _SYS_RTMUX_IDENT_H

#include <stdint.h>

#define RTMUX_MAGIC 0x4d555800 /* "MUX\0" */

/* Binary types */
#define RTMUX_TYPE_EXEC 1  /* Executable */
#define RTMUX_TYPE_SHLIB 2 /* Shared library */

/* Flags */
#define RTMUX_FLAG_STATIC 0x00000001 /* Statically linked */

/* The .rtmux section contents (32 bytes, fixed size) */
struct rtmux_ident {
    uint32_t qi_magic;       /* RTMUX_MAGIC */
    uint16_t qi_version;     /* Struct version (1) */
    uint16_t qi_type;        /* RTMUX_TYPE_* */
    uint32_t qi_flags;       /* RTMUX_FLAG_* */
    uint32_t qi_abi_version; /* Minimum kernel ABI version required */
    uint8_t qi_reserved[16]; /* Reserved, must be zero */
};

/* Source-level macro to embed the .rtmux section.
 *
 *   type:  RTMUX_TYPE_EXEC or RTMUX_TYPE_SHLIB
 *   flags: RTMUX_FLAG_* or 0
 *   abi:   minimum kernel ABI version (start with 1)
 */
#define RTMUX_IDENT(type, flags, abi)                                      \
    __attribute__((section(".rtmux"),                                      \
                   used)) static const struct rtmux_ident _rtmux_ident = { \
        .qi_magic = RTMUX_MAGIC,                                           \
        .qi_version = 1,                                                   \
        .qi_type = (type),                                                 \
        .qi_flags = (flags),                                               \
        .qi_abi_version = (abi),                                           \
    }

#endif /* _SYS_RTMUX_IDENT_H */
