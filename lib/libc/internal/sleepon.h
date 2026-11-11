/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Internal sleepon handle definition. */

#ifndef _RTMUX_LIBC_SLEEPON_H
#define _RTMUX_LIBC_SLEEPON_H

#include <pthread.h>
#include <stdint.h>
#include <sys/cdefs.h>

/* Forward declare this */
struct _sleepon_entry;

struct _sleepon_handle {
    pthread_mutex_t mutex;
    struct _sleepon_entry *list;
    struct _sleepon_entry *free;
    unsigned flags;
    unsigned inuse;
    int reserved[6];
};

typedef struct _sleepon_handle sleepon_t;

extern sleepon_t _sleepon_default __hidden;
int _sleepon_init(sleepon_t **list, unsigned flags) __hidden;
int _sleepon_destroy(sleepon_t *handle) __hidden;
int _sleepon_lock(sleepon_t *handle) __hidden;
int _sleepon_unlock(sleepon_t *handle) __hidden;
int _sleepon_wait(sleepon_t *handle,
                  const volatile void *addr,
                  uint64_t nsec) __hidden;
int _sleepon_signal(sleepon_t *handle, const volatile void *addr) __hidden;
int _sleepon_broadcast(sleepon_t *handle, const volatile void *addr) __hidden;

#endif /* _RTMUX_LIBC_SLEEPON_H */
