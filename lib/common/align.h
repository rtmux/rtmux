/*
 * Power-of-two alignment helpers shared by lib components.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIB_COMMON_ALIGN_H
#define LIB_COMMON_ALIGN_H

#include <stddef.h>
#include <stdint.h>

static inline int lib_is_power_of_two(uintptr_t value)
{
    return value && ((value & (value - 1)) == 0);
}

/* Contract for the align_{down,up} helpers below: `align` must be a nonzero
 * power of two, and `value` must not overflow when rounded up. Callers pass
 * compile-time constants or already-masked page sizes; the one exception is ELF
 * p_align, which is validated with lib_is_power_of_two() at load time (see
 * lib/ldso PT_TLS handling). The helpers do NOT self-check -- guard any new
 * untrusted `align` at the trust boundary before calling.
 */
static inline uintptr_t lib_align_down(uintptr_t value, uintptr_t align)
{
    return value & ~(align - 1);
}

static inline uintptr_t lib_align_up(uintptr_t value, uintptr_t align)
{
    return lib_align_down(value + align - 1, align);
}

static inline size_t lib_align_up_size(size_t value, size_t align)
{
    return (size_t) lib_align_up((uintptr_t) value, (uintptr_t) align);
}

#endif /* LIB_COMMON_ALIGN_H */
