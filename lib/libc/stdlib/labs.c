/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdlib.h>

long labs(long j)
{
    return (j < 0 ? -j : j);
}
