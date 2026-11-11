/* SPDX-License-Identifier: BSD-3-Clause */

#include <stddef.h>
#include <string.h>

char *__strtok_r(char *, const char *, char **) __hidden;

__weak_reference(__strtok_r, strtok_r);

__hidden char *__strtok_r(char *s, const char *delim, char **last)
{
    char *spanp, *tok;
    int c, sc;

    if (!s && !(s = *last))
        return NULL;

    /* Skip (span) leading delimiters (s += strspn(s, delim), sort of). */
cont:
    c = *s++;
    for (spanp = (char *) delim; (sc = *spanp++) != 0;) {
        if (c == sc)
            goto cont;
    }

    if (c == 0) { /* no non-delimiter characters */
        *last = NULL;
        return NULL;
    }
    tok = s - 1;

    /* Scan token (scan for delimiters: s += strcspn(s, delim), sort of). Note
     * that delim must have one NUL; we stop if we see that, too.
     */
    for (;;) {
        c = *s++;
        spanp = (char *) delim;
        do {
            if ((sc = *spanp++) == c) {
                if (c == 0)
                    s = NULL;
                else
                    s[-1] = '\0';
                *last = s;
                return tok;
            }
        } while (sc != 0);
    }
    /* NOTREACHED */
}

char *strtok(char *s, const char *delim)
{
    static char *last;

    return __strtok_r(s, delim, &last);
}
