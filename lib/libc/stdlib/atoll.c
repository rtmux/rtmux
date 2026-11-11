/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdlib.h>

long long atoll(const char *str)
{
    return strtoll(str, (char **) NULL, 10);
}
