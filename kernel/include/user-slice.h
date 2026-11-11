/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_USER_SLICE_H
#define _KERNEL_USER_SLICE_H

/* Typed boundary for a caller-supplied (pointer, length) pair.
 *
 * A user_slice is a range that user_slice_init() has already accepted: not NULL
 * with a nonzero length, no wraparound, every byte below the caller's
 * boundary_addr, and no longer than the site's maximum. user_slice_copyin() and
 * user_slice_copyout() read base and len only from the slice, so the extent
 * that was validated is the extent that is copied; the user pointer and length
 * are never re-read from the syscall arguments afterward. Both copies run
 * through xfer_memcpy, so a page unmapped by a sibling hart (or never mapped)
 * returns EFAULT instead of faulting S-mode.
 *
 * Validation is O(1): a handful of compares, plus (for USER_SLICE_WR) a
 * one-byte read-back-write of the first and last byte so that an unwritable
 * output fails before the kcall has side effects. Bytes in between are covered
 * by the copy itself, which is fault-recovering.
 *
 * Build with USER_SLICE_MODEL (tests/cbmc/user-slice.c) to get just the pure
 * range predicate against a model WITHIN_BOUNDARY.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef USER_SLICE_MODEL
#include <paging.h>
#include <errno.h>
struct thread_entry;
typedef struct thread_entry thread_t;
#endif

#define USER_SLICE_RD 0u
#define USER_SLICE_WR 1u

struct user_slice {
    uintptr_t base;
    size_t len;
};

/* Range rule shared by init and the model checker. A zero-length range holds no
 * bytes and is always accepted.
 *
 * Returns 0, EFAULT (NULL, wrap, or out of range), or EMSGSIZE (longer than
 * max).
 */
static inline int user_slice_check(uintptr_t base,
                                   size_t len,
                                   size_t max,
                                   uintptr_t boundary)
{
    if (!len)
        return 0;
    if (len > max)
        return EMSGSIZE;
    if (!base || base + len < base ||
        !WITHIN_BOUNDARY(base, base + len, boundary))
        return EFAULT;
    return 0;
}

#ifndef USER_SLICE_MODEL
int user_slice_init(thread_t *act,
                    const void *p,
                    size_t len,
                    size_t max,
                    unsigned dir,
                    struct user_slice *out);
int user_slice_copyin(void *dst, const struct user_slice *s);
int user_slice_copyout(const struct user_slice *s, const void *src);

/* One-shot forms for a fixed-size object: validate [p, p+len) and copy it. */
int user_slice_get(thread_t *act, void *dst, const void *p, size_t len);
int user_slice_put(thread_t *act, void *p, const void *src, size_t len);
#endif

#endif
