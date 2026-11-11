/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdlib.h>

long atol(const char *str)
{
    return strtol(str, (char **) NULL, 10);
}
