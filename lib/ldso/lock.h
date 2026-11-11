/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 * Adapted for RTMux.
 */

#ifndef _DYNLINK_LOCK_H_
#define _DYNLINK_LOCK_H_

#ifdef IN_LDSO

struct ldso_lock;
typedef struct ldso_lock *ldso_lock_t;

extern ldso_lock_t ldso_bind_lock;
extern ldso_lock_t ldso_phdr_lock;

#define LDSO_LOCK_UNLOCKED 0
#define LDSO_LOCK_RLOCKED 1
#define LDSO_LOCK_WLOCKED 2

struct Struct_LdsoLockState;
typedef struct Struct_LdsoLockState LdsoLockState;

void rlock_acquire(ldso_lock_t, LdsoLockState *);
void wlock_acquire(ldso_lock_t, LdsoLockState *);
void lock_release(ldso_lock_t, LdsoLockState *);
void lock_restart_for_upgrade(LdsoLockState *);
bool lockstate_wlocked(const LdsoLockState *);

void dlerror_dflt_init(void);

#endif /* IN_LDSO */

#endif
