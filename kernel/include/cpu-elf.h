/*
 * cpu-elf.h — CPU-independent interfaces for ELF relocation and validation.
 *
 * Each architecture implements these in arch/<cpu>/.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_CPU_ELF_H
#define _KERNEL_CPU_ELF_H

#include <sys/elf.h>

/* cpu_elf_machine_valid — return non-zero if e_machine matches this CPU. */
int cpu_elf_machine_valid(uint16_t machine);

/* cpu_apply_reloc — apply a single ELF relocation.
 * Returns 1 on success, 0 on skip (unresolved symbol), -1 on error.
 */
int cpu_apply_reloc(const Elf64_Rela *rela,
                    uint32_t type,
                    uintptr_t S,
                    uintptr_t P,
                    int sym_resolved,
                    const char *name);

/* cpu_apply_rel_relative — apply a R_*_RELATIVE dynamic relocation.
 * Returns 1 if handled, 0 if the relocation type is not RELATIVE.
 */
int cpu_apply_rel_relative(uint32_t type,
                           uintptr_t P,
                           uintptr_t bias,
                           int64_t addend);

/* cpu_reloc_hi20_reset — reset any per-module relocation state. Called before
 * applying relocations for a new module.
 */
void cpu_reloc_hi20_reset(void);

#endif /* _KERNEL_CPU_ELF_H */
