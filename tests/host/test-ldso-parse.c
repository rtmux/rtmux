/*
 * Host-side tests for ldso startup parsers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include <parse.h>

int main(void)
{
    char max_buf[32];
    char overflow_buf[32];

    snprintf(max_buf, sizeof(max_buf), "%d", INT_MAX);
    snprintf(overflow_buf, sizeof(overflow_buf), "%lld",
             (long long) INT_MAX + 1);

    assert(ldso_parse_decimal_int(NULL) == -1);
    assert(ldso_parse_decimal_int("") == -1);
    assert(ldso_parse_decimal_int("x") == -1);
    assert(ldso_parse_decimal_int("12x") == -1);
    assert(ldso_parse_decimal_int("-1") == -1);
    assert(ldso_parse_decimal_int("0") == 0);
    assert(ldso_parse_decimal_int("0012") == 12);
    assert(ldso_parse_decimal_int(max_buf) == INT_MAX);
    assert(ldso_parse_decimal_int(overflow_buf) == -1);

    return 0;
}
