/*
 * Fuzz harness for the kernel iov length summer xferlen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* xferlen sums the lengths of a run of iov entries and returns the total as an
 * int, so it is the length companion to the xfer_memchk bounds checker. Its
 * accumulator is a size_t, so the running total wraps rather than overflowing;
 * this harness feeds it arbitrary per-entry lengths and a fuzz-chosen part
 * count (bounded to the entries it actually filled, so xferlen never walks past
 * the array) and folds the result into a volatile sink so the call is not
 * elided. With the accumulator kept unsigned the sum is sanitizer-clean;
 * narrowing it to a signed type would make the same inputs trip the overflow
 * sanitizer.
 *
 * kernel/xfer/length.c compiles with NDEBUG so its debug-only address-space
 * assertion, which reaches into per-thread and per-CPU state, drops out and
 * only the length-sum path under test remains.
 */

#include <core/globals.h>

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* xferlen installs its fault environment through SET_XFER_ENV, which the stub
 * backs with this pointer; the harness owns its storage.
 */
jmp_buf *host_xfer_env;

#define XFER_MAX_IOV 64

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static volatile int sink;
    iov_t iov[XFER_MAX_IOV];
    thread_t thp = {0};
    size_t off = 0;
    int n = 0;
    int parts;

    while (n < XFER_MAX_IOV && size - off >= sizeof(uintptr_t)) {
        uintptr_t len;

        memcpy(&len, data + off, sizeof(uintptr_t));
        off += sizeof(uintptr_t);
        iov[n].iov_base = NULL;
        iov[n].iov_len = (size_t) len;
        n++;
    }

    /* A trailing byte, if any, chooses how many of the filled entries to sum.
     */
    parts = n;
    if (off < size)
        parts = (int) ((unsigned) data[off] % (unsigned) (n + 1));

    sink = xferlen(&thp, iov, parts);
    return 0;
}
