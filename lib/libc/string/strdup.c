/* SPDX-License-Identifier: BSD-3-Clause */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char *strdup(const char *str)
{
    size_t len;
    char *copy;

    len = strlen(str) + 1;
    if (!(copy = malloc(len)))
        return NULL;
    memcpy(copy, str, len);
    return copy;
}
