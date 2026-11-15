/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * env-lock.h — the libc environment lock (lib/libc/stdlib/getenv.c).
 *
 * setenv, unsetenv, and putenv change environ under this lock: they may point
 * environ at a new vector, edit the current one, or rewrite a value in place.
 * Anything in libc that copies the environment or a variable's value out
 * (spawn, exec, getlogin_r) must hold it for the whole copy.
 *
 * Lock order: the environment lock is taken before the allocator lock, never
 * after. Holding it across malloc is allowed: the allocator never touches the
 * environment, and the memory it may ask the roottask for is served without it.
 * Do not hold it across a spawn or any other request that waits on another
 * process.
 */

#ifndef _RTMUX_LIBC_ENV_LOCK_H
#define _RTMUX_LIBC_ENV_LOCK_H

#include <sys/cdefs.h>

void __env_lock(void) __hidden;
void __env_unlock(void) __hidden;

#endif /* _RTMUX_LIBC_ENV_LOCK_H */
