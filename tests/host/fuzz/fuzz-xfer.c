/*
 * Fuzz harness for the RTMux kernel iov bounds checker xfer_memchk.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* xfer_memchk only returns a verdict, it never touches the memory it vets. A
 * bounds bug in it is therefore invisible unless the caller acts on the
 * verdict. So this harness models a small address space, mem[XFER_MEM_SIZE],
 * with bound = XFER_MEM_SIZE - 1, and copies only the iov entries the checker
 * approves. A correct xfer_memchk approves only ranges fully inside [0, bound],
 * so every approved copy stays inside mem. A wrong approval (a planted bounds
 * bug) would read past mem and ASan would trap on it.
 */

#include <core/globals.h>

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The stub declares this global; the harness owns its storage. */
jmp_buf *host_xfer_env;

/* Referenced by check.c's fault_handlers globals, never called here. */
void xfer_restart(thread_t *thp, CPU_REGISTERS *regs)
{
    (void) thp;
    (void) regs;
}

void xfer_async_restart(thread_t *thp, CPU_REGISTERS *regs)
{
    (void) thp;
    (void) regs;
}

#define XFER_MEM_SIZE 65536
#define XFER_MAX_IOV 64

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static unsigned char mem[XFER_MEM_SIZE];
    iov_t iov[XFER_MAX_IOV];
    uintptr_t bound = XFER_MEM_SIZE - 1;
    size_t off = 0, n = 0;

    /* Parse the input into (base, len) pairs of host-endian uintptr_t. */
    while (n < XFER_MAX_IOV && size - off >= 2 * sizeof(uintptr_t)) {
        uintptr_t base, len;

        memcpy(&base, data + off, sizeof(uintptr_t));
        off += sizeof(uintptr_t);
        memcpy(&len, data + off, sizeof(uintptr_t));
        off += sizeof(uintptr_t);

        iov[n].iov_base = (void *) base;
        iov[n].iov_len = len;
        n++;
    }

    if (xfer_memchk(bound, iov, n) != 0) {
        return 0;
    }

    /* Every entry was approved: exercise each read the checker vouched for.
     * Copy in capped chunks so a huge but wrongly approved len still faults on
     * the first out-of-range byte instead of needing a matching allocation. The
     * last copied byte is folded into a volatile sink so the compiler cannot
     * elide the copy, which would drop the sanitizer check for the tail byte
     * and let a one-past-the-end approval slip through.
     */
    static volatile unsigned char sink;

    for (size_t i = 0; i < n; i++) {
        uintptr_t base = (uintptr_t) iov[i].iov_base;
        size_t len = iov[i].iov_len;
        unsigned char scratch[256];

        while (len) {
            size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);

            memcpy(scratch, mem + base, chunk);
            sink = scratch[chunk - 1];
            base += chunk;
            len -= chunk;
        }
    }

    return 0;
}
