/*
 * Host-build stub for <sys/platform.h> used by the ELF validator fuzz target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The real sys/platform.h pulls in RTMux's freestanding LP64 typedefs, which
 * define uint64_t as unsigned long; the host libc defines it as unsigned long
 * long, and the two collide when elf-validate.c is compiled against host
 * headers. This stub gives elf/elftypes.h and sys/elf.h the same integer types
 * the harness and standalone driver see (plain host stdint) plus the couple of
 * decoration macros those headers expect, so no RTMux platform chain is pulled.
 */

#ifndef __PLATFORM_H_INCLUDED
#define __PLATFORM_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifndef __BEGIN_DECLS
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#ifndef __FLEXARY
#define __FLEXARY(__type, __name) __type __name[1]
#endif

#endif
