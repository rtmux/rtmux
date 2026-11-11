/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LDSO_PAGE_H
#define LDSO_PAGE_H

#include <stddef.h>
#include <stdint.h>

#include <align.h>

extern size_t page_size;

static inline uintptr_t ldso_round_page(uintptr_t x)
{
    return lib_align_up(x, (uintptr_t) page_size);
}

static inline uintptr_t ldso_trunc_page(uintptr_t x)
{
    return lib_align_down(x, (uintptr_t) page_size);
}

#endif /* LDSO_PAGE_H */
