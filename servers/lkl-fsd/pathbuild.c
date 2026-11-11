/*
 * Untrusted-path join for the RTMux LKL filesystem server.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "pathbuild.h"

int build_lkl_path(char *dst, size_t dstsz, const char *mount, const char *rel)
{
    size_t mlen = strlen(mount);
    const char *p;

    if (rel[0] == '\0')
        return snprintf(dst, dstsz, "%s", mount) < (int) dstsz ? 0 : -1;
    if (rel[0] == '/')
        return -1;
    for (p = rel; *p != '\0';) {
        size_t len = strcspn(p, "/");

        if (len == 2 && p[0] == '.' && p[1] == '.')
            return -1;
        p += len;
        while (*p == '/')
            p++;
    }
    if (mlen + 1 + strlen(rel) + 1 > dstsz)
        return -1;
    snprintf(dst, dstsz, "%s/%s", mount, rel);
    return 0;
}
