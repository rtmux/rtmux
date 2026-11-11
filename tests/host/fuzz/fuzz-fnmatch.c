/*
 * Fuzz harness for the libc fnmatch glob matcher.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* fnmatch runs a client-supplied glob against a candidate string, so both of
 * its string arguments can be untrusted. The first fuzz byte selects the flag
 * combination and the rest splits at the first newline into the pattern and the
 * string; both are copied out NUL terminated.
 */

#include <fnmatch.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int flags;
    const uint8_t *sep;
    size_t plen, slen;
    char *pat, *str;

    if (size == 0)
        return 0;
    flags = data[0] & (FNM_PATHNAME | FNM_PERIOD | FNM_NOESCAPE);
    data++;
    size--;

    sep = memchr(data, '\n', size);
    plen = sep ? (size_t) (sep - data) : size;
    slen = sep ? size - plen - 1 : 0;
    pat = malloc(plen + 1);
    str = malloc(slen + 1);
    if (pat && str) {
        memcpy(pat, data, plen);
        pat[plen] = '\0';
        if (sep)
            memcpy(str, sep + 1, slen);
        str[slen] = '\0';
        (void) fnmatch(pat, str, flags);
    }
    free(pat);
    free(str);
    return 0;
}
