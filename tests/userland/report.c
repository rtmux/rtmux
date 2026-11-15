/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * report.c — the outcome hooks the suites report through (see test.h). Shared
 * by the guest suite and the host build of the pure-algorithm suites.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"

struct test_counts suite_counts;

void test_ok(const char *name)
{
    tc_ok(&suite_counts, name);
}

void test_fail(const char *name, const char *detail)
{
    tc_fail(&suite_counts, name, detail);
}

void test_gap(const char *name, const char *anchor, const char *expected, const char *observed)
{
    tc_gap(&suite_counts, name, anchor, expected, observed);
}

void test_skip(const char *name, const char *reason)
{
    tc_skip(&suite_counts, name, reason);
}

void check(int cond, const char *name, const char *detail)
{
    if (cond)
        test_ok(name);
    else
        test_fail(name, detail);
}
