/*
 * Fuzz harness for the libc getsubopt option parser.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* getsubopt walks a comma-separated key=value option string in place, splicing
 * NUL terminators into the buffer and matching each key against a token table.
 * That is the shape of a mount-style option list, which can be untrusted. The
 * fuzz bytes become a mutable option string parsed against a fixed token table
 * until the string is consumed.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fuzz-util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *const tokens[] = {"ro", "rw", "size", "mode", NULL};
    char *buf = fuzz_cstr(data, size);
    char *value;
    char *p;

    if (!buf)
        return 0;
    p = buf;
    while (*p != '\0')
        (void) getsubopt(&p, tokens, &value);
    free(buf);
    return 0;
}
