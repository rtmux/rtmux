/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

char *strrchr(const char *p, int ch)
{
    char *save;
    char c;

    c = (char) ch;
    for (save = NULL;; ++p) {
        if (*p == c)
            save = (char *) p;
        if (*p == '\0')
            return save;
    }
    /* NOTREACHED */
}
