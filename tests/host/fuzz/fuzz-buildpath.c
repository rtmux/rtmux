/*
 * Fuzz harness for the LKL filesystem server path join build_lkl_path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* build_lkl_path (servers/lkl-fsd/pathbuild.c) joins a trusted mount prefix and
 * a client-supplied relative path, and is the guard that keeps an untrusted
 * connect path from escaping the mount. This harness feeds the fuzz bytes in as
 * that relative path and, beyond looking for a crash, asserts the security
 * property on every accepted join: the result stays under the mount prefix and
 * carries no parent-directory escape. A format that slips a ".." component past
 * the check therefore fails the run rather than passing silently.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pathbuild.h"

#define MOUNT "/mnt/fs"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char rel[256];
    char out[512];
    size_t n = size < sizeof(rel) - 1 ? size : sizeof(rel) - 1;
    int r;

    memcpy(rel, data, n);
    rel[n] = '\0';

    r = build_lkl_path(out, sizeof out, MOUNT, rel);
    if (r == 0) {
        size_t olen = strlen(out);

        /* Exactly the mount, or a child under it: a raw prefix check would let
         * a sibling like "/mnt/fs_evil" pass.
         */
        assert(strcmp(out, MOUNT) == 0 ||
               strncmp(out, MOUNT "/", strlen(MOUNT) + 1) == 0);
        assert(strstr(out, "/../") == NULL);
        assert(olen < 3 || strcmp(out + olen - 3, "/..") != 0);
    }
    return 0;
}
