/*
 * RTMux libc — POSIX thread spin locks (_POSIX_SPIN_LOCKS).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A spin lock is pure userspace state: one atomic word busy-waited on, with no
 * kernel sync object. That is what lets it live in shared memory and be
 * PTHREAD_PROCESS_SHARED without a per-process kernel handle, unlike the
 * mutex/cond types. C11 atomics order the acquire on lock and release on
 * unlock; there is no owner tracking, so re-locking a held lock deadlocks by
 * design, exactly as POSIX leaves it.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

/* The lock word must stay a bare, always-lock-free atomic int: a libatomic
 * fallback would take an internal lock and turn every spin op into a call, and
 * any size/alignment drift would break the public ABI (_pthreadtypes.h reaches
 * far beyond pthread.h).
 */
_Static_assert(sizeof(pthread_spinlock_t) == sizeof(int),
               "pthread_spinlock_t must stay a bare atomic word");
_Static_assert(_Alignof(pthread_spinlock_t) == _Alignof(int),
               "pthread_spinlock_t alignment must match int");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "spin lock requires a lock-free atomic int");

#define SPIN_UNLOCKED 0
#define SPIN_LOCKED 1

/* Plain test-and-set, matching the in-tree allocator spinlock; a
 * test-and-test-and-set body and a pause hint are deferred until a contention
 * benchmark justifies them (measure-or-revert).
 */

int pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
    if (!lock)
        return EINVAL;
    if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)
        return EINVAL;
    atomic_store_explicit(&lock->__lock, SPIN_UNLOCKED, memory_order_relaxed);
    return EOK;
}

int pthread_spin_destroy(pthread_spinlock_t *lock)
{
    if (!lock)
        return EINVAL;
    return EOK;
}

int pthread_spin_lock(pthread_spinlock_t *lock)
{
    if (!lock)
        return EINVAL;
    while (atomic_exchange_explicit(&lock->__lock, SPIN_LOCKED,
                                    memory_order_acquire) != SPIN_UNLOCKED)
        ;
    return EOK;
}

int pthread_spin_trylock(pthread_spinlock_t *lock)
{
    if (!lock)
        return EINVAL;
    if (atomic_exchange_explicit(&lock->__lock, SPIN_LOCKED,
                                 memory_order_acquire) != SPIN_UNLOCKED)
        return EBUSY;
    return EOK;
}

int pthread_spin_unlock(pthread_spinlock_t *lock)
{
    if (!lock)
        return EINVAL;
    atomic_store_explicit(&lock->__lock, SPIN_UNLOCKED, memory_order_release);
    return EOK;
}
