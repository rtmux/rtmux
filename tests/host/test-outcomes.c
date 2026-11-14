/*
 * Host-side test for the guest suite's outcome accounting (tests/userland/outcome.h).
 *
 * The rule under test: a known gap is accepted only while it still fails the way
 * it is documented to. An unexpected pass, a different failure and a gap with no
 * tracking anchor are failures, and a skip needs a reason, so neither a gap list
 * nor a skip list can hide a regression. Each case runs on fresh counters and
 * checks both the counts and the exit-status policy.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "outcome.h"

#define ANCHOR "T0.4"

/* Check the counters and the process status (for a mode that must run a case). */
static void expect(const struct test_counts *c,
                   struct test_counts want,
                   int status,
                   const char *what)
{
    if (c->run != want.run || c->passed != want.passed || c->failed != want.failed ||
        c->gap != want.gap || c->skipped != want.skipped || tc_status(c, 1) != status) {
        fprintf(stderr,
                "test-outcomes: %s: got run=%d passed=%d failed=%d gap=%d skipped=%d "
                "status=%d, want %d/%d/%d/%d/%d status=%d\n",
                what, c->run, c->passed, c->failed, c->gap, c->skipped, tc_status(c, 1),
                want.run, want.passed, want.failed, want.gap, want.skipped, status);
        assert(0);
    }
}

int main(void)
{
    struct test_counts c;

    c = (struct test_counts) {0};
    tc_gap(&c, "known", ANCHOR, "EXPECTED", "EXPECTED");
    expect(&c, (struct test_counts){1, 0, 0, 1, 0}, 0, "matching gap exits clean");

    c = (struct test_counts) {0};
    tc_gap(&c, "different", ANCHOR, "EXPECTED", "OTHER");
    expect(&c, (struct test_counts){1, 0, 1, 0, 0}, 1, "different failure is a failure");

    c = (struct test_counts) {0};
    tc_gap(&c, "stale", ANCHOR, "EXPECTED", NULL);
    expect(&c, (struct test_counts){1, 0, 1, 0, 0}, 1, "unexpected pass is a failure");

    c = (struct test_counts) {0};
    tc_gap(&c, "no-anchor", NULL, "EXPECTED", "EXPECTED");
    tc_gap(&c, "empty-anchor", "", "EXPECTED", "EXPECTED");
    tc_gap(&c, "no-expected", ANCHOR, NULL, "EXPECTED");
    expect(&c, (struct test_counts){3, 0, 3, 0, 0}, 1, "gap without anchor or id is a failure");

    /* A skip with a reason is neither run nor failed; with an ok case it exits clean. */
    c = (struct test_counts) {0};
    tc_ok(&c, "ok");
    tc_skip(&c, "optional", "needs 3 harts");
    expect(&c, (struct test_counts){1, 1, 0, 0, 1}, 0, "skip with reason exits clean");

    /* A mode that only skipped must not pass silently when it requires a run. */
    c = (struct test_counts) {0};
    tc_skip(&c, "optional", "needs 3 harts");
    expect(&c, (struct test_counts){0, 0, 0, 0, 1}, 1, "all-skipped mode fails when it must run");
    assert(tc_status(&c, 0) == 0);

    c = (struct test_counts) {0};
    tc_skip(&c, "silent", NULL);
    tc_skip(&c, "empty", "");
    expect(&c, (struct test_counts){2, 0, 2, 0, 0}, 1, "skip without a reason is a failure");

    /* Plain outcomes still count as before. */
    c = (struct test_counts) {0};
    tc_ok(&c, "ok");
    tc_fail(&c, "bad", "boom");
    expect(&c, (struct test_counts){2, 1, 1, 0, 0}, 1, "pass and fail");

    /* The summary line the harness greps for. */
    c = (struct test_counts) {0};
    tc_ok(&c, "ok");
    tc_gap(&c, "known", ANCHOR, "EXPECTED", "EXPECTED");
    tc_skip(&c, "optional", "needs 3 harts");
    tc_summary(&c);
    expect(&c, (struct test_counts){2, 1, 0, 1, 1}, 0, "clean run");

    puts("test-outcomes: ok");
    return 0;
}
