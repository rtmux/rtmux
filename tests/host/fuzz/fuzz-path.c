/*
 * Fuzz harness for the roottask path normalizer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* path_normalize collapses client-supplied paths (dot, dot-dot, repeated and
 * trailing slashes) before namespace resolution, so it parses untrusted strings
 * on the open path. The fuzz bytes become a NUL-terminated path fed into a
 * fixed output buffer.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fuzz-util.h"

int path_normalize(const char *path, char *out, size_t outsz, int *dir_out);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *in = fuzz_cstr(data, size);
    char out[1024];
    int is_dir;

    if (!in)
        return 0;
    (void) path_normalize(in, out, sizeof(out), &is_dir);
    free(in);
    return 0;
}
