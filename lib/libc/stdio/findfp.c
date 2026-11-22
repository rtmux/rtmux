/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "local.h"

_Atomic int __sdidinit;
__hidden int __isthreaded;


#define std(flags, file)          \
    {                             \
        ._flags = (flags),        \
        ._file = (file),          \
        ._cookie = __sF + (file), \
        ._close = __sclose,       \
        ._read = __sread,         \
        ._seek = __sseek,         \
        ._write = __swrite,       \
    }

enum { STREAMS_PER_BLOCK = FOPEN_MAX - 3 };
struct stream_block {
    struct stream_block *next;
    unsigned used;
    FILE streams[STREAMS_PER_BLOCK];
};
_Static_assert(STREAMS_PER_BLOCK < sizeof(unsigned) * 8, "stream bitmap size");
static struct stream_block usual;
static struct stream_block *blocks = &usual;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE __sF[3] = {
    [STDIN_FILENO] = std(__SRD, STDIN_FILENO),
    [STDOUT_FILENO] = std(__SWR, STDOUT_FILENO),
    [STDERR_FILENO] = std(__SWR | __SNBF, STDERR_FILENO),
};

FILE *stdin = &__sF[0];
FILE *stdout = &__sF[1];
FILE *stderr = &__sF[2];
FILE *__stdinp = &__sF[0];
FILE *__stdoutp = &__sF[1];
FILE *__stderrp = &__sF[2];

/* Blocks stay allocated until process exit. A walker can drop the pool lock
 * before waiting on a stream or writing to its descriptor. */
FILE *__sfp(void)
{
    const unsigned full = (1u << STREAMS_PER_BLOCK) - 1;
    struct stream_block *b;
    pthread_mutex_lock(&pool_lock);
    for (b = blocks; b && b->used == full; b = b->next)
        ;
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) {
            pthread_mutex_unlock(&pool_lock);
            errno = EMFILE;
            return NULL;
        }
        b->next = blocks;
        blocks = b;
    }
    unsigned slot = 0;
    while (b->used & (1u << slot))
        slot++;
    b->used |= 1u << slot;
    pthread_mutex_unlock(&pool_lock);

    FILE *fp = &b->streams[slot];
    FLOCKFILE(fp);
    /* Keep the mutex attached across close and reuse. */
    memset(fp, 0, offsetof(FILE, _fl_mutex));
    fp->_orientation = fp->_flags2 = 0;
    fp->_file = -1;
    __sdidinit = 1;
    return fp;
}

/* Called under the stream lock after clearing the file flags. The three
 * standard streams have permanent slots outside the pool. */
void __sfp_release(FILE *fp)
{
    if (fp == &__sF[0] || fp == &__sF[1] || fp == &__sF[2])
        return;
    pthread_mutex_lock(&pool_lock);
    for (struct stream_block *b = blocks; b; b = b->next) {
        uintptr_t start = (uintptr_t) b->streams;
        uintptr_t addr = (uintptr_t) fp;
        if (addr >= start && addr - start < sizeof(b->streams)) {
            b->used &= ~(1u << ((addr - start) / sizeof(FILE)));
            break;
        }
    }
    pthread_mutex_unlock(&pool_lock);
}

static int visit(FILE *fp, int (*func)(FILE *), int trylock)
{
    if (trylock) {
        if (ftrylockfile(fp))
            return 0;
    } else {
        FLOCKFILE(fp);
    }
    int rc = fp->_flags && !(fp->_flags & __SIGN) ? func(fp) : 0;
    FUNLOCKFILE(fp);
    return rc;
}

int _fwalk(int (*func)(FILE *), int trylock)
{
    int rc = 0;
    for (unsigned i = 0; i < 3; i++)
        rc |= visit(&__sF[i], func, trylock);
    pthread_mutex_lock(&pool_lock);
    for (struct stream_block *b = blocks; b; b = b->next) {
        unsigned used = b->used;
        pthread_mutex_unlock(&pool_lock);
        for (unsigned i = 0; used; used >>= 1, i++)
            if (used & 1u)
                rc |= visit(&b->streams[i], func, trylock);
        pthread_mutex_lock(&pool_lock);
    }
    pthread_mutex_unlock(&pool_lock);
    return rc;
}

void _cleanup(void)
{
    if (__sdidinit)
        (void) _fwalk(__sflush, 0);
}
