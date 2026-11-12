/* SPDX-License-Identifier: Apache-2.0 */
#include <cpu-elf.h>

int cpu_elf_machine_valid(uint16_t machine)
{
    return machine == EM_AARCH64;
}

/* Relocation and process-loader hooks are intentionally not stubbed out.
 * The bring-up fixture is static ET_EXEC; a full kernel link must supply the
 * remaining cpu-elf.h interfaces before claiming dynamic-loader support. */
