/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/elftypes.h */
#ifndef __ELFTYPES_H_INCLUDED
#define __ELFTYPES_H_INCLUDED

#include <sys/platform.h>

__BEGIN_DECLS typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf32_Word;
typedef uint16_t Elf32_Half;

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef int32_t Elf64_Sword;
typedef int64_t Elf64_Sxword;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef uint16_t Elf64_Half;

__END_DECLS
#endif
