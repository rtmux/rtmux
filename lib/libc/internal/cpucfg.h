/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/syspage.h>
#include <sys/rtmux-core.h>
#include <arch/cpu-inline.h>
#include <arch/smpxchg.h>
#include "atomic.h"

static __inline__ struct _thread_local_storage *__attribute__((__unused__,
                                                               __const__))
LIBC_TLS(void)
{
    void *__tls;

    __asm__("mv %0, tp" : "=r"(__tls));
    return (struct _thread_local_storage *) __tls;
}

#ifndef __RTMUX_PTHREAD_RET_DEFINED
#define __RTMUX_PTHREAD_RET_DEFINED
static __inline__ int __rtmux_pthread_ret(int ret)
{
    return ret < 0 ? -ret : ret;
}
#endif

#ifndef __mutex_smp_cmpxchg
#define __mutex_smp_cmpxchg(d, c, s) _smp_cmpxchg((d), (c), (s))
#endif

#ifndef __mutex_smp_xchg
#define __mutex_smp_xchg(d, n) _smp_xchg((d), (n))
#endif

/* Release a held mutex's owner word WITHOUT ever publishing a transient 0.
 *
 * A plain xchg(owner, 0) momentarily marks the mutex free-with-no-waiters. If a
 * waiter is queued in the kernel, that window is a mutual-exclusion hole: a
 * third thread's fast-path CAS(0 -> self) can win at the same instant the
 * kernel hands the mutex to the queued waiter, giving two simultaneous owners.
 * Clearing only the owner-id bits keeps RTMUX_SYNC_WAITING set when it was set,
 * so the published word is a non-zero sentinel (0x80000000) on which a
 * fast-path CAS(0 -> self) fails; only the kernel handoff -- or a waiter's
 * masked futex re-check -- then grants ownership. When no waiter was present
 * the word had no WAITING bit, so this clears it to 0 (free) exactly as before.
 * Returns the old owner word: if it carried RTMUX_SYNC_WAITING the caller must
 * enter the kernel to wake a waiter. A release barrier must precede this (the
 * clear is a plain amoand.d with no release ordering of its own).
 */
#define __mutex_release_owner(__owner) \
    atomic_clr_value_64((__owner), RTMUX_SYNC_OWNER_MASK)

/* Libc-only mutex fast paths for internal locks that do not need type checks.
 */
#define __mutex_lock(__m)                                       \
    (__mutex_smp_cmpxchg(&(__m)->owner, 0, LIBC_TLS()->__owner) \
         ? __rtmux_pthread_ret(sync_mutex_lock_r(__m))          \
         : 0 /* EOK */)
#define __mutex_unlock(__m)                                      \
    (__cpu_membarrier(),                                         \
     (__mutex_release_owner(&(__m)->owner) & RTMUX_SYNC_WAITING) \
         ? __rtmux_pthread_ret(sync_mutex_unlock_r((__m)))       \
         : EOK)
