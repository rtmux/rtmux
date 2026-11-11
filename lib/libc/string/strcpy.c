/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

char *strcpy(char *__restrict to, const char *__restrict from)
{
    char *save = to;

    for (; (*to = *from); ++from, ++to)
        ;
    return save;
}
