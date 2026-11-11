/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Debug-only userspace lock-rank (lockdep-style) checker hooks.
 *
 * The mutex acquire/release fast paths call these to record a per-thread stack
 * of held locks and a global graph of "held-before" edges keyed by lock
 * address; a reverse edge is an AB/BA deadlock and aborts. With
 * CONFIG_ULOCK_DEBUG unset the hooks are no-op macros, so a normal build pays
 * no code-size or runtime cost.
 */

#ifndef __PTHREAD_ULOCK_DEBUG_H_INCLUDED
#define __PTHREAD_ULOCK_DEBUG_H_INCLUDED

#ifdef CONFIG_ULOCK_DEBUG

void __ulock_debug_acquire(const void *lock, const void *pc);
void __ulock_debug_release(const void *lock);

#else

#define __ulock_debug_acquire(lock, pc) ((void) 0)
#define __ulock_debug_release(lock) ((void) 0)

#endif /* CONFIG_ULOCK_DEBUG */

#endif /* __PTHREAD_ULOCK_DEBUG_H_INCLUDED */
