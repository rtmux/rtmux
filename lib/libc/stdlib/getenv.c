/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1987, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

char **environ;

char *getenv(const char *name)
{
    size_t len;
    char **p;
    const char *c;

    if (!name || !environ)
        return NULL;

    for (c = name, len = 0; *c && *c != '='; c++, len++)
        ;

    for (p = environ; *p; p++) {
        if (strncmp(*p, name, len) == 0 && (*p)[len] == '=')
            return &(*p)[len + 1];
    }
    return NULL;
}

int putenv(char *string)
{
    char **p;
    size_t len, cnt;
    const char *c;

    if (!string)
        return -1;

    for (c = string; *c && *c != '='; c++)
        ;
    len = (size_t) (c - string);
    if (*c != '=')
        return -1;

    /* Search for existing */
    if (environ) {
        for (p = environ; *p; p++) {
            if (strncmp(*p, string, len + 1) == 0) {
                *p = string;
                return 0;
            }
        }
    }

    /* Not found — grow environ by one slot. environ may point into the stack
     * (set by crt0), so we must malloc+copy on first addition, not realloc.
     */
    cnt = 0;
    if (environ)
        for (p = environ; *p; p++)
            cnt++;

    p = malloc((cnt + 2) * sizeof(char *));
    if (!p)
        return -1;
    if (environ)
        memcpy(p, environ, cnt * sizeof(char *));
    p[cnt] = string;
    p[cnt + 1] = NULL;
    environ = p;
    return 0;
}

int unsetenv(const char *name)
{
    char **p, **q;
    size_t len;

    if (!name || !environ)
        return -1;

    len = strlen(name);

    for (p = q = environ; *p; p++) {
        if (strncmp(*p, name, len) == 0 && (*p)[len] == '=')
            continue;
        *q++ = *p;
    }
    *q = NULL;
    return 0;
}

/* setenv: malloc a "name=value" string and hand it to putenv. If "rewrite" is 0
 * and the variable already exists, leave it alone. The malloc'd storage is
 * leaked on overwrite — same as glibc; OK for the small handful of variables
 * login(1) sets.
 */
int setenv(const char *name, const char *value, int rewrite)
{
    size_t nlen, vlen;
    char *p;

    if (!name || *name == '\0' || strchr(name, '='))
        return -1;
    if (!value)
        value = "";

    if (!rewrite && getenv(name))
        return 0;

    nlen = strlen(name);
    vlen = strlen(value);
    p = malloc(nlen + vlen + 2);
    if (!p)
        return -1;
    memcpy(p, name, nlen);
    p[nlen] = '=';
    memcpy(p + nlen + 1, value, vlen + 1); /* includes NUL */

    return putenv(p);
}
