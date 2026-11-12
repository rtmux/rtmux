/* SPDX-License-Identifier: Apache-2.0 */
#include <core/globals.h>
#include <cpu-elf.h>
void cpu_reloc_hi20_reset(void) {}
int cpu_apply_rel_relative(uint32_t type,
                           uintptr_t p,
                           uintptr_t bias,
                           int64_t addend)
{
    if (type != 1027)
        return 0;
    *(uint64_t *) p = bias + (uint64_t) addend;
    return 1;
}
int cpu_apply_reloc(const Elf64_Rela *r,
                    uint32_t type,
                    uintptr_t s,
                    uintptr_t p,
                    int resolved,
                    const char *name)
{
    (void) name;
    if (!type)
        return 1;
    if (!resolved)
        return 0;
    if (type == 257 || type == 1025 || type == 1026) {
        *(uint64_t *) p = s + (uint64_t) r->r_addend;
        return 1;
    }
    if (type == 261) {
        *(uint32_t *) p = (uint32_t) (s + (uint64_t) r->r_addend - p);
        return 1;
    }
    return -1;
}
