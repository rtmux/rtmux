/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* <ctype.h> — Minimal character classification for RTMux libc.
 *
 * Only the C/POSIX locale is supported (single-byte, ASCII).
 */
#ifndef _CTYPE_H
#define _CTYPE_H

static inline int isspace(int c)
{
    return c == ' ' || (unsigned) c - '\t' < 5; /* \t \n \v \f \r */
}

static inline int isdigit(int c)
{
    return (unsigned) c - '0' < 10;
}

static inline int isalpha(int c)
{
    return ((unsigned) c | 32) - 'a' < 26;
}

static inline int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

static inline int isupper(int c)
{
    return (unsigned) c - 'A' < 26;
}

static inline int islower(int c)
{
    return (unsigned) c - 'a' < 26;
}

static inline int toupper(int c)
{
    return islower(c) ? c & 0x5f : c;
}

static inline int tolower(int c)
{
    return isupper(c) ? c | 0x20 : c;
}

static inline int isprint(int c)
{
    return (unsigned) c - 0x20 < 0x5f;
}

static inline int isxdigit(int c)
{
    return isdigit(c) || ((unsigned) c | 32) - 'a' < 6;
}

#endif /* _CTYPE_H */
