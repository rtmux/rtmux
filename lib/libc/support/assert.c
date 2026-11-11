/*
 * __assert_fail — called by assert() macro on failure.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/rtmux-core.h>

void __assert_fail(const char *expr,
                   const char *file,
                   unsigned line,
                   const char *func)
{
    dbgprintf("ASSERT FAILED: %s (%s:%u, %s)\n", expr, file, line, func);
    abort();
}
