/*
 * SPDX-License-Identifier: Apache-2.0
 * TLS accessors and errno support.
 *
 * __tls() reads the thread pointer via LIBC_TLS(). __get_errno_ptr() returns
 * the per-thread errno pointer from TLS. __stackavail() computes remaining
 * stack from sp and TLS stack base.
 */

#include <sys/storage.h>
#include <stddef.h>
#include <stdint.h>
#include "cpucfg.h"

struct _thread_local_storage *__tls(void)
{
    return LIBC_TLS();
}

int *__get_errno_ptr(void)
{
    return LIBC_TLS()->__errptr;
}

static inline uintptr_t read_sp(void)
{
    uintptr_t sp;
    sp = r_sp();
    return sp;
}

size_t __stackavail(void)
{
    return read_sp() - (uintptr_t) LIBC_TLS()->__stackaddr;
}
