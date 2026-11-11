/*
 * Sanitizer self-test: a deliberate out-of-bounds read.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* This harness proves the sanitizer build actually traps rather than silently
 * passing. host-sanitize runs it and requires a nonzero exit: with
 * AddressSanitizer wired in the one-past-the-end read aborts, and if the
 * sanitizer were missing the read would pass and the target would fail. It is
 * never part of the real corpus sweep, only the wiring check.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    volatile uint8_t sink = 0;
    uint8_t *heap = malloc(4);

    (void) data;
    (void) size;
    if (heap) {
        sink = heap[4]; /* one past the 4-byte allocation */
        free(heap);
    }
    return (int) sink;
}
