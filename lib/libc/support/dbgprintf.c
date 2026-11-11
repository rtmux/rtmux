/*
 * dbgprintf — formatted debug print via kernel console.
 *
 * Convenience wrapper: vsnprintf into a stack buffer, then dbgprint. Safe to
 * call from any libc / I/O service / roottask context.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/rtmux-core.h>

void dbgprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        if ((size_t) n >= sizeof(buf))
            n = sizeof(buf) - 1;
        dbgprint(buf, (size_t) n);
    }
}
