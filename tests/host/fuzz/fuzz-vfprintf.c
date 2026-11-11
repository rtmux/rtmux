/*
 * Fuzz harness for the libc formatted-output core __vfprintf.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* __vfprintf (lib/libc/stdio/vfprintf.c) is the conversion engine behind the
 * whole printf family: it parses the format string, decoding flags, field
 * width, precision, length modifiers, and conversion specifiers, then sizes and
 * pads an output buffer. That parser is the interesting surface, so this
 * harness feeds the fuzz bytes in as the format string and drives it through
 * vsnprintf, which sets up a bounded string sink and calls __vfprintf. A fixed,
 * generous argument set backs the conversions.
 *
 * The stdio sources build in their own RTMux header world (see the vfprintf
 * target in tools/host/Makefile), so this host-world harness reaches vsnprintf
 * by plain prototype. The libc stdio internals the string-sink path references
 * but never runs (locking, errno, the real-descriptor write, flush) are stubbed
 * here so the link resolves; __sfvwrite copies straight into the sink for a
 * __SSTR FILE without touching any of them.
 *
 * The integer, string, pointer, and %n arguments are all one value: a pointer
 * to a writable, NUL-terminated buffer, which keeps every such conversion safe
 * whatever the fuzzed format asks for (a string reads a valid string, a %n
 * writes into the buffer, a numeric conversion reads the pointer bits). Because
 * a double argument travels in the separate floating-point save area rather
 * than alongside the pointers, the tail arguments are real doubles so a float
 * conversion reads a populated slot instead of an uninitialized one. The format
 * length is capped so that even the densest consumer (a starred width plus
 * value) cannot outrun the fixed argument count, so no conversion lands on an
 * out-of-range argument.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

static int g_errno;

int *__get_errno_ptr(void)
{
    return &g_errno;
}

void __flockfile(void *fp)
{
    (void) fp;
}

void __funlockfile(void *fp)
{
    (void) fp;
}

int __swsetup(void *fp)
{
    (void) fp;
    return 0;
}

int _swrite(void *fp, const char *buf, int n)
{
    (void) fp;
    (void) buf;
    return n;
}

int __fflush(void *fp)
{
    (void) fp;
    return 0;
}

#undef vsnprintf
extern int vsnprintf(char *str, size_t n, const char *fmt, va_list ap);

static int call_vsnprintf(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}

/* Each format byte consumes at most one argument (a run of '*' takes one width
 * argument per star), so the pointer argument count below is kept above this
 * cap with margin. A float conversion instead draws from the doubles, and
 * reading past them only yields an unused placeholder value, so those need not
 * be capped.
 */
#define FMT_MAX 64

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static volatile int sink;
    /* Aligned so a %n conversion, which stores an int or long long through the
     * argument pointer, writes to a naturally aligned address as a well-behaved
     * caller would supply.
     */
    static _Alignas(long long) char argbuf[64];
    char fmt[FMT_MAX];
    char out[256];
    size_t n = size < FMT_MAX - 1 ? size : FMT_MAX - 1;
    void *p = argbuf;
    double d = 1.5;

    memcpy(argbuf, "fuzz", 5);
    memcpy(fmt, data, n);
    fmt[n] = '\0';

    /* 80 pointer arguments (above FMT_MAX) for the integer, string, pointer,
     * and %n conversions, then 8 real doubles for float conversions.
     */
#define P8 p, p, p, p, p, p, p, p
    sink = call_vsnprintf(out, sizeof out, fmt, P8, P8, P8, P8, P8, P8, P8, P8,
                          P8, P8, d, d, d, d, d, d, d, d);
#undef P8
    return 0;
}
