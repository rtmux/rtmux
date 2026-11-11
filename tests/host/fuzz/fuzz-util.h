/*
 * Shared helpers for the fuzz harnesses.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_HOST_FUZZ_UTIL_H
#define RTMUX_HOST_FUZZ_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Copy the fuzz bytes into a freshly allocated NUL-terminated string, so a
 * harness can hand untrusted input to a C-string parser.
 *
 * Returns NULL when the allocation fails; the caller owns the result and frees
 * it.
 */
static inline char *fuzz_cstr(const uint8_t *data, size_t size)
{
    char *s = malloc(size + 1);

    if (!s)
        return NULL;
    memcpy(s, data, size);
    s[size] = '\0';
    return s;
}

#endif /* RTMUX_HOST_FUZZ_UTIL_H */
