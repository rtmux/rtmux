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
 * Nothing libc allocated for the environment is freed, except the side array
 * and what is described next. A vector that is outgrown is retired, not freed:
 * a reader still walking it sees a stale but intact snapshot, and the retired
 * vectors of a geometric series add up to less than the live one. When the
 * program has reassigned environ, though, libc's next change copies the
 * program's vector and frees libc's old one, together with every string libc
 * allocated that nothing holds: not the program's vector (by its start or by a
 * pointer into it) and not an argument of the call, so a loop that assigns
 * environ and changes it stays flat. A string the program's vector holds by its
 * start stays libc's, to reuse or free later. libc finds what holds what by
 * sorting its strings by address and searching them, so the cost is O(n log n)
 * however large the environment, and the replacement runs only after every
 * allocation of the change has succeeded: a setenv that fails leaves libc's
 * vector, and every string in it, as they were. A program must not assign
 * environ back to a libc vector it saved before that change, and, as POSIX
 * allows, a pointer from getenv does not survive a later setenv, unsetenv or
 * putenv.
 *
 * Lifetime of a string returned by getenv. A string libc allocated (setenv)
 * stays allocated until the program reassigns environ and libc then releases it
 * (see above), and otherwise for the life of the process. Overwriting the
 * variable with a value that fits rewrites that buffer in place, so the old
 * pointer then reads the new value; a longer value goes into a new buffer and
 * the old one is left as it was. unsetenv leaves removed strings allocated too.
 * The cost is that replacing a variable with a longer value, or setting it
 * again after unsetenv, leaks the old string. A string libc did not allocate
 * (one passed to putenv, or in a vector the loader or the program supplied)
 * lives exactly as long as its owner keeps it.
 *
 * A program may assign environ directly. libc notices that environ is no longer
 * its own vector and, on the next change, copies the new vector instead of
 * growing the old one. A string in it counts as not owned, except one that libc
 * allocated and the vector holds by its start in exactly one entry (see above).
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
#include <stdint.h>
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

/* Whether p (which may be NULL) points into the n bytes at base. */
static int env_within(const char *p, const char *base, size_t n)
{
    return p && (uintptr_t) p >= (uintptr_t) base &&
           (uintptr_t) p - (uintptr_t) base < n;
}

/* The strings libc allocated, as vec[0..m) with sizes size[0..m), ordered by
 * address: they are disjoint buffers, so the one a pointer lies in is found by
 * a binary search. A string's size word also carries, above ENV_HITS_SHIFT, how
 * many entries of the program's vector point into it (0, 1, or 2 for two or
 * more).
 */
#define ENV_HITS_SHIFT 62
#define ENV_SIZE_MASK (((size_t) 1 << ENV_HITS_SHIFT) - 1)
_Static_assert(sizeof(size_t) * 8 >= ENV_HITS_SHIFT + 2,
               "the hit count needs two spare high bits in a size");

static void env_swap(char **vec, size_t *size, size_t a, size_t b)
{
    char *s = vec[a];
    size_t z = size[a];

    vec[a] = vec[b];
    size[a] = size[b];
    vec[b] = s;
    size[b] = z;
}

/* Restore the max-heap on address for the subtree at root, in [0, end). */
static void env_sift(char **vec, size_t *size, size_t root, size_t end)
{
    for (size_t child; (child = 2 * root + 1) < end; root = child) {
        if (child + 1 < end &&
            (uintptr_t) vec[child + 1] > (uintptr_t) vec[child])
            child++;
        if ((uintptr_t) vec[root] >= (uintptr_t) vec[child])
            return;
        env_swap(vec, size, root, child);
    }
}

/* Heapsort: O(m log m) in place and with no allocation, so it cannot fail. */
static void env_sort(char **vec, size_t *size, size_t m)
{
    for (size_t i = m / 2; i-- > 0;)
        env_sift(vec, size, i, m);
    for (size_t end = m; end-- > 1;) {
        env_swap(vec, size, 0, end);
        env_sift(vec, size, 0, end);
    }
}

/* Index in the sorted vec[0..m) of the string p points into, or -1. */
static long env_owner(char *const *vec,
                      const size_t *size,
                      size_t m,
                      const char *p)
{
    size_t lo = 0, hi = m; /* the answer is in [lo - 1, hi - 1] */

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if ((uintptr_t) vec[mid] <= (uintptr_t) p)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo && env_within(p, vec[lo - 1], size[lo - 1] & ENV_SIZE_MASK))
        return (long) lo - 1;
    return -1;
}

/* Free libc's old vector, which the program has replaced, and the strings libc
 * allocated in it that nothing holds: neither live[0..n) (the program's vector,
 * copied into the new vector with side array owned) nor keep (an argument of
 * the call in progress), as a whole string or as a pointer into it. A string
 * that live holds by its start passes on its buffer size to owned, so a later
 * change can reuse or free it.
 *
 * The old vector and side array are sorted in place (they are freed here), so
 * the work is O((n + m) log m) for n live and m owned entries, however large
 * the environment, and no allocation can fail halfway.
 */
static void env_release(char *const *live,
                        size_t n,
                        size_t *owned,
                        const void *keep)
{
    char **vec = env.vec;
    size_t *size = env.owned;
    size_t m = 0;

    for (size_t i = 0; i < env.slots && vec[i]; i++) {
        if (size[i]) {
            env_swap(vec, size, i, m);
            m++;
        }
    }
    env_sort(vec, size, m);

    for (size_t k = 0; k < n; k++) {
        long e = env_owner(vec, size, m, live[k]);

        if (e >= 0 && (size[e] >> ENV_HITS_SHIFT) < 2)
            size[e] += (size_t) 1 << ENV_HITS_SHIFT;
    }
    for (size_t k = 0; k < n; k++) {
        long e = env_owner(vec, size, m, live[k]);

        /* One entry, at the start: the buffer is that entry's alone. Any other
         * alias would see a reuse in place.
         */
        if (e >= 0 && size[e] >> ENV_HITS_SHIFT == 1 && live[k] == vec[e])
            owned[k] = size[e] & ENV_SIZE_MASK;
    }
    for (size_t e = 0; e < m; e++) {
        size_t sz = size[e] & ENV_SIZE_MASK;

        if (!(size[e] >> ENV_HITS_SHIFT) && !env_within(keep, vec[e], sz))
            free(vec[e]);
    }
    free(vec);
}

/* Make environ libc's own vector, with room for extra more entries.
 *
 * Returns the entry count, or -1 with errno ENOMEM and nothing changed. An
 * outgrown vector of libc's is retired, not freed (see the top). keep is a
 * pointer the caller still needs, which a replacement must not free.
 */
static long env_reserve(size_t extra, const void *keep)
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
    else if (env.vec)
        env_release(vec, n, owned, keep);
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
    long n = env_reserve(i < 0 ? 1 : 0, s);

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

    /* Everything that can fail runs before libc's old vector and strings are
     * released, so a failure leaves every variable, and libc's own storage, as
     * it was: the string first, then the vector.
     */
    if (!(s = malloc(size))) {
        errno = ENOMEM;
        rc = -1;
        goto out;
    }
    memcpy(s, name, nlen);
    s[nlen] = '=';
    memcpy(s + nlen + 1, value, vlen + 1);

    /* A variable in a vector the program supplied: take over libc's copy of it,
     * so that a buffer libc allocated there can be reused in place.
     */
    if (i >= 0 && environ != env.vec) {
        if (env_reserve(0, s) < 0) {
            free(s);
            rc = -1;
            goto out;
        }
        if (env.owned[i] >= size) {
            memcpy(environ[i], s, size);
            free(s);
            goto out;
        }
    }
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
    if (env_reserve(0, name) < 0) {
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
