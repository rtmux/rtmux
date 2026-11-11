/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>

/* See user-slice.h. The out slice is zeroed on failure, so a site that ignores
 * the status copies nothing.
 */
int user_slice_init(thread_t *act,
                    const void *p,
                    size_t len,
                    size_t max,
                    unsigned dir,
                    struct user_slice *out)
{
    int err;

    out->base = 0;
    out->len = 0;
    err =
        user_slice_check((uintptr_t) p, len, max, act->process->boundary_addr);
    if (err || !len)
        return err;
    if (dir == USER_SLICE_WR) {
        /* First and last byte only; the copy recovers faults in between. */
        volatile uint8_t *first = (volatile uint8_t *) p;
        volatile uint8_t *last = first + (len - 1);

        if (xfer_memcpy((void *) first, (const void *) first, 1) ||
            xfer_memcpy((void *) last, (const void *) last, 1))
            return EFAULT;
    }
    out->base = (uintptr_t) p;
    out->len = len;
    return 0;
}

int user_slice_copyin(void *dst, const struct user_slice *s)
{
    return s->len ? xfer_memcpy(dst, (const void *) s->base, s->len) : 0;
}

int user_slice_copyout(const struct user_slice *s, const void *src)
{
    return s->len ? xfer_memcpy((void *) s->base, src, s->len) : 0;
}

int user_slice_get(thread_t *act, void *dst, const void *p, size_t len)
{
    struct user_slice s;

    return user_slice_init(act, p, len, len, USER_SLICE_RD, &s)
               ?: user_slice_copyin(dst, &s);
}

int user_slice_put(thread_t *act, void *p, const void *src, size_t len)
{
    struct user_slice s;

    return user_slice_init(act, p, len, len, USER_SLICE_WR, &s)
               ?: user_slice_copyout(&s, src);
}
