/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * getenv, setenv, unsetenv, and putenv.
 *
 * environ starts out pointing at the vector the loader built (on the initial
 * stack). The first change copies it into a vector libc allocates and never
 * writes the original; after that the vector grows geometrically, so n
 * insertions cost O(n) allocation in total. A side array, parallel to libc's
 * vector, records for each entry the size of the buffer when libc allocated the
 * string (setenv) and 0 when it did not (putenv, or inherited).
 *
 * Nothing libc allocated for the environment is freed, except the side array. A
 * vector that is outgrown is retired, not freed: a reader still walking it sees
 * a stale but intact snapshot, and the retired vectors of a geometric series
 * add up to less than the live one. Each change after the program reassigns
 * environ also retires libc's vector, since the program may assign it back
 * later, so a program that keeps reassigning environ and changing it leaks one
 * vector per round.
 *
 * Lifetime of a string returned by getenv. A string libc allocated (setenv)
 * stays allocated for the life of the process. Overwriting the variable with a
 * value that fits rewrites that buffer in place, so the old pointer then reads
 * the new value; a longer value goes into a new buffer and the old one is left
 * as it was. unsetenv leaves removed strings allocated too. The cost is that
 * replacing a variable with a longer value, or setting it again after unsetenv,
 * leaks the old string. A string libc did not allocate (one passed to putenv,
 * or in a vector the loader or the program supplied) lives exactly as long as
 * its owner keeps it.
 *
 * A program may assign environ directly. libc notices that environ is no longer
 * its own vector and, on the next change, copies the new vector instead of
 * growing the old one, treating every string in it as not owned.
 *
 * Changes run under the environment lock (env-lock.h), which spawn and exec
 * also take while they copy environ. getenv does not take it: as POSIX allows,
 * getenv is not safe against a concurrent change.
 *
 * Extensions: a NULL name to setenv or unsetenv fails with EINVAL, like an
 * empty one or one containing '='; a NULL value to setenv is taken as "";
 * putenv of a string without '=' or with an empty name fails with EINVAL.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "env-lock.h"

#define ENV_MIN_SLOTS 16

/* Weak, so the program's own definition (crt0.S) is the one every object binds
 * to. The dynamic linker prefers a strong definition over a weak one in search
 * order, so a strong one here would leave the program writing its copy while
 * libc read this one.
 */
char **environ __attribute__((weak));

/* libc's own vector and its side array of owned buffer sizes, each with room
 * for slots pointers including the terminating NULL. Valid only while environ
 * still equals vec.
 */
static struct {
    char **vec;
    size_t *owned;
    size_t slots;
} env;

#ifdef RTMUX_SYNC_INITIALIZER
/* Priority-inheriting, like the allocator lock it is taken before. */
static pthread_mutex_t env_mutex = {RTMUX_SYNC_NONRECURSIVE,
                                    RTMUX_SYNC_INITIALIZER};
#else
static pthread_mutex_t env_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

void __env_lock(void)
{
    (void) pthread_mutex_lock(&env_mutex);
}

void __env_unlock(void)
{
    (void) pthread_mutex_unlock(&env_mutex);
}

/* POSIX.1-2024: a name for setenv or unsetenv is non-empty and has no '='. */
static int bad_name(const char *name)
{
    if (!name || *name == '\0' || strchr(name, '=')) {
        errno = EINVAL;
        return 1;
    }
    return 0;
}

static size_t env_count(void)
{
    size_t n = 0;

    if (environ)
        while (environ[n])
            n++;
    return n;
}

/* Whether entry defines the variable name[0..len). */
static int env_match(const char *entry, const char *name, size_t len)
{
    return strncmp(entry, name, len) == 0 && entry[len] == '=';
}

/* Index of the first entry for name[0..len), or -1. */
static long env_find(const char *name, size_t len)
{
    if (!environ)
        return -1;
    for (long i = 0; environ[i]; i++)
        if (env_match(environ[i], name, len))
            return i;
    return -1;
}

/* Make environ libc's own vector, with room for extra more entries.
 *
 * Returns the entry count, or -1 with errno ENOMEM and nothing changed. An
 * outgrown vector of libc's is retired, not freed (see the top).
 */
static long env_reserve(size_t extra)
{
    size_t n = env_count();
    int mine = environ && environ == env.vec;

    if (mine && n + 1 + extra <= env.slots)
        return (long) n;

    size_t slots = 2 * (n + 1 + extra);
    char **vec;
    size_t *owned;

    if (slots < ENV_MIN_SLOTS)
        slots = ENV_MIN_SLOTS;
    vec = malloc(slots * sizeof *vec);
    owned = calloc(slots, sizeof *owned);
    if (!vec || !owned) {
        free(vec);
        free(owned);
        errno = ENOMEM;
        return -1;
    }
    if (n)
        memcpy(vec, environ, n * sizeof *vec);
    vec[n] = NULL;
    if (mine)
        memcpy(owned, env.owned, n * sizeof *owned);
    free(env.owned); /* only libc reads the side array, so it can go */
    env.vec = vec;
    env.owned = owned;
    env.slots = slots;
    environ = vec;
    return (long) n;
}

/* Put s at index i (appending when i < 0) in libc's own vector, recording owned
 * as its buffer size (0: not libc's).
 *
 * Returns 0, or -1 with errno ENOMEM and nothing changed.
 */
static int env_store(long i, char *s, size_t owned)
{
    long n = env_reserve(i < 0 ? 1 : 0);

    if (n < 0)
        return -1;
    if (i < 0) {
        i = n;
        environ[n + 1] = NULL;
    }
    environ[i] = s; /* a replaced string stays allocated: see the top */
    env.owned[i] = owned;
    return 0;
}

char *getenv(const char *name)
{
    size_t len;
    long i;

    if (!name)
        return NULL;
    len = strcspn(name, "=");
    i = env_find(name, len);
    return i < 0 ? NULL : environ[i] + len + 1;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t nlen, vlen, size;
    long i;
    char *s;
    int rc = 0;

    if (bad_name(name))
        return -1;
    if (!value)
        value = "";
    nlen = strlen(name);
    vlen = strlen(value);
    size = nlen + vlen + 2;

    __env_lock();
    i = env_find(name, nlen);
    if (i >= 0 && !overwrite)
        goto out;
    if (i >= 0 && environ == env.vec && env.owned[i] >= size) {
        /* memmove: value may point into this very string. */
        memmove(environ[i] + nlen + 1, value, vlen + 1);
        goto out;
    }

    /* Allocate the string before touching the vector, so a failure on either
     * leaves the environment as it was.
     */
    if (!(s = malloc(size))) {
        errno = ENOMEM;
        rc = -1;
        goto out;
    }
    memcpy(s, name, nlen);
    s[nlen] = '=';
    memcpy(s + nlen + 1, value, vlen + 1);
    if (env_store(i, s, size) < 0) {
        free(s);
        rc = -1;
    }
out:
    __env_unlock();
    return rc;
}

int putenv(char *string)
{
    size_t len;
    int rc;

    if (!string || (len = strcspn(string, "=")) == 0 || string[len] != '=') {
        errno = EINVAL;
        return -1;
    }

    __env_lock();
    rc = env_store(env_find(string, len), string, 0);
    __env_unlock();
    return rc;
}

int unsetenv(const char *name)
{
    size_t len;
    long i, j;
    int rc = 0;

    if (bad_name(name))
        return -1;
    len = strlen(name);

    __env_lock();

    /* Nothing to remove changes nothing; otherwise work on libc's own copy,
     * never on a vector the loader or the program supplied.
     */
    if (env_find(name, len) < 0)
        goto out;
    if (env_reserve(0) < 0) {
        rc = -1;
        goto out;
    }
    for (i = j = 0; environ[i]; i++) {
        if (env_match(environ[i], name, len))
            continue; /* removed strings stay allocated: see the top */
        environ[j] = environ[i];
        env.owned[j] = env.owned[i];
        j++;
    }
    environ[j] = NULL;
out:
    __env_unlock();
    return rc;
}
