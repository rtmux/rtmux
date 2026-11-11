/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

char *strcat(char *__restrict s, const char *__restrict append)
{
    char *save = s;

    for (; *s; ++s)
        ;
    while ((*s++ = *append++))
        ;
    return save;
}
