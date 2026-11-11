/*
 * Host-build shim that forces the RTMux <sys/elf.h> for the ELF validator fuzz
 * target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* glibc ships its own /usr/include/sys/elf.h, which the -idirafter search order
 * lets win over RTMux's internal copy and which lacks the Elf64_Ehdr layout and
 * the EI_/ELFMAG constants elf-validate.c needs. Sitting on -I ahead of the
 * system dirs, this shim pulls the real RTMux header by an explicit repo path;
 * the stub sys/platform.h beside it keeps that header on host integer types,
 * and the header's own include guard makes the direct-path include idempotent.
 */
#include "../../../../../internal/include/sys/elf.h"
