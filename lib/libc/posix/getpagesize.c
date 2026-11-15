/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * getpagesize() — the legacy BSD spelling of sysconf(_SC_PAGESIZE). The loader
 * publishes the page size as AT_PAGESZ, which saves the message to roottask
 * that sysconf() sends; fall back to sysconf() for a start file that left the
 * auxiliary vector unset. The answer never changes, so it is cached.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/auxv.h>

int getpagesize(void)
{
    static _Atomic int cached;
    int size = atomic_load_explicit(&cached, memory_order_relaxed);
    unsigned long at;

    if (size)
        return size;
    at = __libc_auxv_get(AT_PAGESZ);
    size = at && at <= INT32_MAX ? (int) at : (int) sysconf(_SC_PAGESIZE);
    if (size > 0)
        atomic_store_explicit(&cached, size, memory_order_relaxed);
    return size;
}
