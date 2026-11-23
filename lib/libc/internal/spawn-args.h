/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_LIBC_SPAWN_ARGS_H
#define RTMUX_LIBC_SPAWN_ARGS_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/spawn-limits.h>

/* Add one vector to the counts: its entries to *count and its strings, with
 * their NULs, to *strings. Stops with E2BIG as soon as the total is past
 * ARG_MAX, so an oversized or unterminated string is not scanned to its end.
 */
static inline int spawn_measure_vector(char *const *vec,
                                       unsigned *count,
                                       size_t *strings)
{
    for (; vec && *vec; vec++) {
        const char *s = *vec;
        size_t len = 0;

        while (*strings + len < ARG_MAX && s[len])
            len++;
        *strings += len + 1; /* at least one byte an entry bounds *count */
        ++*count;
        if (*strings > ARG_MAX)
            return E2BIG;
    }
    return EOK;
}

/* Measure argv and envp for a spawn request: the two counts and the bytes of
 * all strings with their NULs.
 *
 * Returns E2BIG when spawn_args_fit() refuses them. The caller holds the
 * environment lock, which also covers argv entries that point into the
 * environment.
 */
static inline int spawn_measure_args(char *const *argv,
                                     char *const *envp,
                                     uint16_t *argc,
                                     uint16_t *envc,
                                     size_t *strings)
{
    unsigned nargv = 0, narge = 0;

    *strings = 0;
    if (spawn_measure_vector(argv, &nargv, strings) ||
        spawn_measure_vector(envp, &narge, strings) ||
        !spawn_args_fit(*strings, nargv, narge))
        return E2BIG;
    *argc = (uint16_t) nargv; /* spawn-limits.h asserts that they fit */
    *envc = (uint16_t) narge;
    return EOK;
}

/* Copy the strings of argv, then envp, into the `size` bytes at dst that
 * spawn_measure_args() counted for them.
 *
 * Returns EOK when they fill the buffer exactly. A caller's thread that changes
 * a string between the two passes can make them differ; the copy never runs
 * past the buffer, and the spawn fails with E2BIG.
 */
static inline int spawn_copy_args(char *dst,
                                  size_t size,
                                  char *const *argv,
                                  char *const *envp)
{
    char *const *vecs[2] = {argv, envp};
    size_t used = 0;

    for (int v = 0; v < 2; v++) {
        for (char *const *vec = vecs[v]; vec && *vec; vec++) {
            const char *s = *vec;

            do {
                if (used == size)
                    return E2BIG;
                dst[used++] = *s;
            } while (*s++);
        }
    }
    return used == size ? EOK : E2BIG;
}

#endif
