/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* elf/notes.h */
#ifndef __ELF_NOTES_H_INCLUDED
#define __ELF_NOTES_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __ELFTYPES_H_INCLUDED
#include <elf/elftypes.h>
#endif

__BEGIN_DECLS
#define RTMUX_NOTE_NAME "RTMux"
enum Elf_rtmux_note_types {
    RTMUX_NT_NULL = 0,
    RTMUX_NT_DEBUG_FULLPATH,
    RTMUX_NT_DEBUG_RELOC,
    RTMUX_NT_STACK,
    RTMUX_NT_GENERATOR,
    RTMUX_NT_DEFAULT_LIB,
    RTMUX_NT_CORE_SYSINFO,
    RTMUX_NT_CORE_INFO,
    RTMUX_NT_CORE_STATUS,
    RTMUX_NT_CORE_GREG,
    RTMUX_NT_CORE_FPREG,
    RTMUX_NT_NUM
};

typedef struct {
    uint8_t major_version;
    uint8_t minor_version;
    uint16_t flags;
} generator_version;

#define RTMUXELF_GEN_MAJOR 1
#define RTMUXELF_GEN_MINOR 0

/* Generator note flags */
enum { RTMUXELF_FLAG_DYNAMIC = 0x0001, RTMUXELF_FLAG_PRIVATES = 0x0002 };

__END_DECLS
#endif
