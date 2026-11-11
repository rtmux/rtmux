/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CBMC harness for the typed user-pointer boundary
 * (docs/user-slice-inventory.md).
 *
 * It includes the real kernel/include/user-slice.h with USER_SLICE_MODEL, so
 * user_slice_check() is the shipped predicate, and proves for every base, len,
 * max and boundary_addr:
 *   1. NULL with a nonzero length is rejected.
 *   2. A range whose end wraps past UINTPTR_MAX is rejected.
 *   3. A range with any byte at or above the boundary is rejected (the model
 *      WITHIN_BOUNDARY is the form arch/riscv uses, kernel in high memory:
 *      first <= boundary && last <= boundary, last = one past the end).
 *   4. A length over max is rejected.
 *   5. An accepted slice satisfies all four. The slice is a plain value, so
 *      the copy helpers (which read only the slice) cannot see later changes
 *      to the caller's arguments; that is a structural property of
 *      user_slice_copyin/copyout, modeled here by mirroring user_slice_init's
 *      assignment, not proven against the C function itself.
 * Only the high-memory WITHIN_BOUNDARY variant (arch/riscv) is modeled; the
 * low-memory variant in kernel/include/paging.h is not covered.
 * Address-space assumptions modeled: flat 64-bit user addresses and one
 * boundary per process. Page mappings are not modeled; the copy itself is
 * fault-recovering (kernel/xfer/copy.c).
 *
 * scripts/check-user-slice.sh also builds this against copies of the header
 * with one rule removed (-I mutant dir) and with -DPLANT_BOUNDARY, and requires
 * each build to FAIL.
 */

#include <stddef.h>
#include <stdint.h>

#define USER_SLICE_MODEL
#define EFAULT 14
#define EMSGSIZE 90
#ifdef PLANT_BOUNDARY
#define WITHIN_BOUNDARY(first, last, boundary) 1
#else
#define WITHIN_BOUNDARY(first, last, boundary) \
    ((first) <= (boundary) && (last) <= (boundary))
#endif
#include <user-slice.h>

uintptr_t nondet_uintptr(void);
size_t nondet_size(void);

/* Mirror of the slice assignment in user_slice_init (kernel/xfer/user-slice.c).
 */
static int model_init(uintptr_t p,
                      size_t len,
                      size_t max,
                      uintptr_t boundary,
                      struct user_slice *out)
{
    int err = user_slice_check(p, len, max, boundary);

    out->base = 0;
    out->len = 0;
    if (err || !len)
        return err;
    out->base = p;
    out->len = len;
    return 0;
}

int main(void)
{
    uintptr_t p = nondet_uintptr(), boundary = nondet_uintptr();
    size_t len = nondet_size(), max = nondet_size();
    struct user_slice s;
    int err = model_init(p, len, max, boundary, &s);

    if (len && !p)
        __CPROVER_assert(err != 0, "NULL with nonzero length rejected");
    if (len && p + len < p)
        __CPROVER_assert(err != 0, "wrapping range rejected");
    if (len && p + len >= p && p + len > boundary)
        __CPROVER_assert(err != 0, "range past boundary rejected");
    if (len > max)
        __CPROVER_assert(err != 0, "overlength rejected");

    if (!err && len) {
        __CPROVER_assert(p && p + len >= p && p + len <= boundary && len <= max,
                         "accepted slice satisfies every rule");

        /* Arguments rewritten after validation (a sibling thread, or a re-read
         * of the syscall frame) never reach the slice.
         */
        uintptr_t vbase = p;
        size_t vlen = len;

        p = nondet_uintptr();
        len = nondet_size();
        __CPROVER_assert(s.base == vbase && s.len == vlen,
                         "copy extent equals validated extent");
    }
    if (err)
        __CPROVER_assert(s.len == 0, "rejected slice copies nothing");
    return 0;
}
