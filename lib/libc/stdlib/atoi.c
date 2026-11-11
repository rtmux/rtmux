/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdlib.h>

int atoi(const char *str)
{
    return (int) strtol(str, NULL, 10);
}
