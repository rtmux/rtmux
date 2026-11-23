/*
 * Host-side tests for the startup library's strtoul and strtopaddr.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* The startup headers pull in the target's world; strtoul.c needs only the
 * type and the two prototypes below, so their guards are set and the headers
 * read empty.
 */
#define _STARTUP_H
#define _STARTUP_API_H
typedef uint64_t paddr_t;
paddr_t strtopaddr(const char *nptr, char **endptr, int base);
#define strtoul startup_strtoul
unsigned long startup_strtoul(const char *nptr, char **endptr, int base);
#include "../../kernel/startup/lib/strtoul.c"

static void expect(const char *s, int base, paddr_t want, size_t used)
{
    char *end;
    paddr_t v = strtopaddr(s, &end, base);

    assert(v == want);
    assert((size_t) (end - s) == used);
}

int main(void)
{
    expect("0", 10, 0, 1);
    expect("42", 10, 42, 2);
    expect("010", 10, 10, 3);
    expect("010", 0, 8, 3);
    expect("0x1f", 0, 0x1f, 4);
    expect("", 10, 0, 0);
    expect("x", 10, 0, 0);

    /* The largest value is exact; one more saturates instead of wrapping, and
     * every digit is still consumed.
     */
    expect("18446744073709551615", 10, UINT64_MAX, 20);
    expect("18446744073709551616", 10, UINT64_MAX, 20);
    expect("18446744073709551626", 10, UINT64_MAX, 20);
    expect("99999999999999999999999", 10, UINT64_MAX, 23);
    expect("0xffffffffffffffff", 0, UINT64_MAX, 18);
    expect("0x10000000000000000", 0, UINT64_MAX, 19);

    {
        char *end;
        const char *s = "18446744073709551626";

        assert(startup_strtoul(s, &end, 10) == UINT64_MAX);
        assert(end == s + strlen(s));
    }
    return 0;
}
