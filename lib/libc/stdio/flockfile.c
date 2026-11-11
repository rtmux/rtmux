/*
 * flockfile/ftrylockfile/funlockfile and the _unlocked stdio accessors —
 * per-FILE recursive locking for _POSIX_THREAD_SAFE_FUNCTIONS.
 *
 * Each FILE carries a lazily-attached mutex (_fl_mutex) plus owner and
 * recursion fields (_fl_owner, _fl_count) reserved in the FILE layout. A thread
 * that already owns the FILE bumps the count instead of re-locking, so nested
 * locks (e.g. fseek() calling ftell()) do not deadlock.
 *
 * Concurrency notes:
 *   - The mutex is published with a lock-free compare-exchange; the malloc()
 *     happens outside any lock, so the allocator can never re-enter this path
 *     while a lock is held.
 *   - _fl_owner is read locklessly on the recursion fast path, so it is
 *     accessed atomically to avoid a data race with the owner's own stores.
 *     Relaxed order suffices: the mutex provides the real synchronization, and
 *     a non-owner can never observe its own (never-zero) thread id there.
 *   - _fl_count is touched only by the current owner, so it needs no atomics.
 *   - If a per-FILE mutex cannot be allocated (OOM) the FILE shares a single
 *     recursive fallback lock: coarse but correct, and recursive so a thread
 *     locking several malloc-starved streams at once cannot self-deadlock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "local.h"

static pthread_mutex_t fl_fallback;
static pthread_once_t fl_fallback_once = PTHREAD_ONCE_INIT;

static void fl_fallback_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&fl_fallback, &attr);
    pthread_mutexattr_destroy(&attr);
}

static pthread_mutex_t *fl_ensure(FILE *fp)
{
    void *cur, *spare;
    pthread_mutex_t *nm;

    cur = __atomic_load_n(&fp->_fl_mutex, __ATOMIC_ACQUIRE);
    if (cur)
        return cur;

    nm = malloc(sizeof *nm);
    if (nm) {
        pthread_mutex_init(nm, NULL);
        spare = nm;
    } else {
        pthread_once(&fl_fallback_once, fl_fallback_init);
        spare = &fl_fallback;
    }

    cur = NULL; /* expected value for the publish */
    if (__atomic_compare_exchange_n(&fp->_fl_mutex, &cur, spare, 0,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return spare;

    /* Lost the race: cur holds the winner; discard our private spare. */
    if (spare != (void *) &fl_fallback) {
        pthread_mutex_destroy(spare);
        free(spare);
    }
    return cur;
}

static inline pthread_t fl_owner(FILE *fp)
{
    return (pthread_t) (intptr_t) __atomic_load_n(&fp->_fl_owner,
                                                  __ATOMIC_RELAXED);
}

static inline void fl_set_owner(FILE *fp, pthread_t self)
{
    __atomic_store_n(&fp->_fl_owner, (void *) (intptr_t) self,
                     __ATOMIC_RELAXED);
}

void __flockfile(FILE *fp)
{
    pthread_t self = pthread_self();

    if (fl_owner(fp) == self) {
        fp->_fl_count++;
        return;
    }
    pthread_mutex_lock(fl_ensure(fp));
    fl_set_owner(fp, self);
    fp->_fl_count = 1;
}

int __ftrylockfile(FILE *fp)
{
    pthread_t self = pthread_self();

    if (fl_owner(fp) == self) {
        fp->_fl_count++;
        return 0;
    }
    if (pthread_mutex_trylock(fl_ensure(fp)) != 0)
        return -1;
    fl_set_owner(fp, self);
    fp->_fl_count = 1;
    return 0;
}

void __funlockfile(FILE *fp)
{
    if (fl_owner(fp) != pthread_self())
        return; /* not the owner: POSIX leaves this undefined; ignore */
    if (--fp->_fl_count > 0)
        return;
    fl_set_owner(fp, 0);
    pthread_mutex_unlock(fp->_fl_mutex);
}

void flockfile(FILE *fp)
{
    __flockfile(fp);
}

int ftrylockfile(FILE *fp)
{
    return __ftrylockfile(fp);
}

void funlockfile(FILE *fp)
{
    __funlockfile(fp);
}

int getc_unlocked(FILE *fp)
{
    return __sgetc(fp);
}

int getchar_unlocked(void)
{
    return getc_unlocked(stdin);
}

int putc_unlocked(int c, FILE *fp)
{
    return __sputc(c, fp);
}

int putchar_unlocked(int c)
{
    return putc_unlocked(c, stdout);
}
