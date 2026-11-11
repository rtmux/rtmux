/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1989, 1993, 1994
 * The Regents of the University of California. All rights reserved.
 *
 * Simplified for RTMux: single-byte only, no locale/collation.
 */

#include <fnmatch.h>
#include <string.h>

#define EOS '\0'

static int rangematch(const char *, char, int, const char **);

int fnmatch(const char *pattern, const char *string, int flags)
{
    const char *stringstart = string;
    const char *bt_pattern = NULL, *bt_string = NULL;
    char c, sc;

    for (;;) {
        c = *pattern++;
        sc = *string;

        switch (c) {
        case EOS:
            if (sc == EOS)
                return 0;
            goto backtrack;

        case '?':
            if (sc == EOS)
                return FNM_NOMATCH;
            if (sc == '/' && (flags & FNM_PATHNAME))
                goto backtrack;
            if (sc == '.' && (flags & FNM_PERIOD) &&
                (string == stringstart ||
                 ((flags & FNM_PATHNAME) && *(string - 1) == '/')))
                goto backtrack;
            string++;
            break;

        case '*':
            while (*pattern == '*')
                pattern++;

            if (sc == '.' && (flags & FNM_PERIOD) &&
                (string == stringstart ||
                 ((flags & FNM_PATHNAME) && *(string - 1) == '/')))
                goto backtrack;

            if (*pattern == EOS) {
                if (flags & FNM_PATHNAME)
                    return strchr(string, '/') ? FNM_NOMATCH : 0;
                else
                    return 0;
            } else if (*pattern == '/' && (flags & FNM_PATHNAME)) {
                if (!(string = strchr(string, '/')))
                    return FNM_NOMATCH;
                break;
            }

            bt_pattern = pattern;
            bt_string = string;
            break;

        case '[': {
            const char *newp;
            int result;
            if (sc == EOS)
                return FNM_NOMATCH;
            if (sc == '/' && (flags & FNM_PATHNAME))
                goto backtrack;
            if (sc == '.' && (flags & FNM_PERIOD) &&
                (string == stringstart ||
                 ((flags & FNM_PATHNAME) && *(string - 1) == '/')))
                goto backtrack;
            result = rangematch(pattern, sc, flags, &newp);
            if (result == 1) {
                pattern = newp;
                string++;
            } else if (result == 0)
                goto backtrack;
            else
                goto norm;
            break;
        }

        case '\\':
            if (!(flags & FNM_NOESCAPE)) {
                c = *pattern++;
                if (c == EOS)
                    return FNM_NOMATCH;
            }
            /* FALLTHROUGH */
        default:
        norm:
            string++;
            if (c == sc)
                ;
            else {
            backtrack:
                if (!bt_pattern)
                    return FNM_NOMATCH;
                sc = *bt_string;
                if (sc == EOS)
                    return FNM_NOMATCH;
                if (sc == '/' && (flags & FNM_PATHNAME))
                    return FNM_NOMATCH;
                bt_string++;
                pattern = bt_pattern;
                string = bt_string;
            }
            break;
        }
    }
}

/* rangematch — match character against bracket expression.
 * Returns 1 for match, 0 for no match, -1 for error.
 */
static int rangematch(const char *pattern,
                      char test,
                      int flags,
                      const char **newp)
{
    int negate, ok;
    char c, c2;
    const char *origpat;

    if ((negate = (*pattern == '!' || *pattern == '^')))
        ++pattern;

    ok = 0;
    origpat = pattern;
    for (;;) {
        if (*pattern == ']' && pattern > origpat) {
            pattern++;
            break;
        } else if (*pattern == '\0') {
            return -1;
        } else if (*pattern == '/' && (flags & FNM_PATHNAME)) {
            *newp = pattern;
            return 0;
        } else if (*pattern == '\\' && !(flags & FNM_NOESCAPE)) {
            pattern++;

            /* A backslash that escapes the terminator means the bracket is
             * unterminated; stop before consuming the EOS so the read below
             * cannot run past the pattern. The range side already guards this.
             */
            if (*pattern == EOS)
                return -1;
        }
        c = *pattern++;

        if (*pattern == '-' && *(pattern + 1) != EOS && *(pattern + 1) != ']') {
            if (*++pattern == '\\' && !(flags & FNM_NOESCAPE))
                if (*pattern != EOS)
                    pattern++;
            c2 = *pattern++;
            if (c2 == EOS)
                return -1;
            if ((unsigned char) c <= (unsigned char) test &&
                (unsigned char) test <= (unsigned char) c2)
                ok = 1;
        } else if (c == test)
            ok = 1;
    }

    *newp = pattern;
    return ok == negate ? 0 : 1;
}
