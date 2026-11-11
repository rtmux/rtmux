/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Various utility routines.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>

/**
 * \brief Convert a string to a number.
 *
 * \note This function, in addition to straight numeric massaging,
 *        accepts suffixes (like 'K' or 'G') to scale the quantity.
 */
paddr_t getsize(char *str, char **dst)
{
    paddr_t v;

    v = strtopaddr(str, &str, 0);

    switch (*str) {
    case 'G':
    case 'g':
        v *= 1024UL;
        fallthrough;
    case 'M':
    case 'm':
        v *= 1024UL;
        fallthrough;
    case 'K':
    case 'k':
        v *= 1024UL;
        ++str;
        break;
    }
    if (dst)
        *dst = str;
    return v;
}
