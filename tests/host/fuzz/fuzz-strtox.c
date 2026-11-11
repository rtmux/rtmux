/*
 * Fuzz harness for the libc string-to-integer parsers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* strtoul and strtol scan a NUL-terminated number with sign, base prefix, and
 * overflow saturation, over input that can come from a client. The fuzz bytes
 * become the string and each base exercises a different branch: 0 auto-detects
 * the prefix, 8 and 16 fix octal and hex, 10 is plain decimal.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fuzz-util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *s = fuzz_cstr(data, size);
    char *end;
    int base;

    if (!s)
        return 0;
    for (base = 0; base <= 16; base += 8) { /* 0, 8, 16 */
        (void) strtoul(s, &end, base);
        (void) strtol(s, &end, base);
    }
    (void) strtoul(s, &end, 10);
    (void) strtol(s, &end, 10);
    free(s);
    return 0;
}
