/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>

#include "debug.h"
#include "dynlink.h"
#include "machdep.h"

#define LDSO_LOCK_WRITER 1U
#define LDSO_LOCK_READER 2U
#define LDSO_LOCK_COUNT 2

static char dlerror_msg[512];
static int dlerror_seen_val = 1;

struct ldso_lock {
    volatile unsigned state;
};

static struct ldso_lock ldso_locks[LDSO_LOCK_COUNT];

ldso_lock_t ldso_bind_lock = &ldso_locks[0];
ldso_lock_t ldso_phdr_lock = &ldso_locks[1];

char *ldso_dlerror_loc(void)
{
    return dlerror_msg;
}

int *ldso_dlerror_seen(void)
{
    return &dlerror_seen_val;
}

static void spin_pause(void)
{
    __asm volatile("" ::: "memory");
}

static bool cas_lock(volatile unsigned *p, unsigned old, unsigned new)
{
    return __atomic_compare_exchange_n(p, &old, new, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

static unsigned load_lock(const volatile unsigned *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void rlock(struct ldso_lock *lock)
{
    unsigned state;

    for (;;) {
        state = load_lock(&lock->state);
        if (!(state & LDSO_LOCK_WRITER) &&
            cas_lock(&lock->state, state, state + LDSO_LOCK_READER))
            return;
        spin_pause();
    }
}

static void wlock(struct ldso_lock *lock)
{
    while (!cas_lock(&lock->state, 0, LDSO_LOCK_WRITER))
        spin_pause();
}

void rlock_acquire(ldso_lock_t lock, LdsoLockState *lockstate)
{
    if (!lockstate)
        return;
    rlock(lock);
    lockstate->lockstate = LDSO_LOCK_RLOCKED;
}

void wlock_acquire(ldso_lock_t lock, LdsoLockState *lockstate)
{
    if (!lockstate)
        return;
    wlock(lock);
    lockstate->lockstate = LDSO_LOCK_WLOCKED;
}

void lock_release(ldso_lock_t lock, LdsoLockState *lockstate)
{
    if (!lockstate)
        return;

    switch (lockstate->lockstate) {
    case LDSO_LOCK_UNLOCKED:
        break;
    case LDSO_LOCK_RLOCKED:
        __atomic_fetch_sub(&lock->state, LDSO_LOCK_READER, __ATOMIC_RELEASE);
        lockstate->lockstate = LDSO_LOCK_UNLOCKED;
        break;
    case LDSO_LOCK_WLOCKED:
        __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
        lockstate->lockstate = LDSO_LOCK_UNLOCKED;
        break;
    default:
        abort();
    }
}

void lock_restart_for_upgrade(LdsoLockState *lockstate)
{
    if (!lockstate)
        return;
    if (lockstate->lockstate == LDSO_LOCK_RLOCKED)
        abort();
}

bool lockstate_wlocked(const LdsoLockState *lockstate)
{
    return lockstate && lockstate->lockstate == LDSO_LOCK_WLOCKED;
}

void dlerror_dflt_init(void)
{
    dlerror_msg[0] = '\0';
    dlerror_seen_val = 1;
}
