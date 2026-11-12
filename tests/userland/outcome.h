/*
 * Test outcome accounting for the guest suite and its host test.
 *
 * Four outcomes. PASS and FAIL are the usual pair. SKIP means a case did not
 * run and says why. GAP means a case is a known, tracked failure: it names the
 * stable identifier of the failure it expects. A gap is only accepted while the
 * case still fails *that* way; an unexpected pass or a different failure is a
 * FAIL, so a gap list cannot hide a regression or go stale.
 *
 * Pure C with printf only, so tests/host/test-outcomes.c compiles it unchanged.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_TEST_OUTCOME_H
#define RTMUX_TEST_OUTCOME_H

#include <stdio.h>
#include <string.h>

/*
 * Exit status of a program that ran nothing because its precondition is missing
 * (too few harts, no counter). The init wrapper turns it into "RTMux <mode>:
 * SKIP" and the harness fails a mode that must run, so a skip never reads as a
 * pass.
 */
#define TC_EXIT_SKIP 77

struct test_counts {
    int run;     /* cases that executed: passed + failed + gap */
    int passed;
    int failed;
    int gap;     /* known failures that still fail as documented */
    int skipped; /* cases that did not run (not counted in run) */
};

static inline void tc_ok(struct test_counts *c, const char *name)
{
    c->run++;
    c->passed++;
    printf("  PASS  %s\n", name);
}

static inline void tc_fail(struct test_counts *c, const char *name, const char *detail)
{
    c->run++;
    c->failed++;
    printf("  FAIL  %s: %s\n", name, detail);
}

/*
 * A known gap. `anchor` points at the tracking entry (for example "T1.4"),
 * `expected` is the stable identifier of the known failure, and `observed` is the
 * identifier of the failure seen now, or NULL when the case passed.
 */
static inline void tc_gap(struct test_counts *c,
                          const char *name,
                          const char *anchor,
                          const char *expected,
                          const char *observed)
{
    c->run++;
    if (!anchor || !*anchor || !expected || !*expected) {
        c->failed++;
        printf("  FAIL  %s: known gap needs a tracking anchor and an expected failure id\n", name);
    } else if (!observed) {
        c->failed++;
        printf("  FAIL  %s: known gap %s now passes; remove the gap marker\n", name, anchor);
    } else if (strcmp(observed, expected) != 0) {
        c->failed++;
        printf("  FAIL  %s: known gap %s fails differently: %s (expected %s)\n", name, anchor,
               observed, expected);
    } else {
        c->gap++;
        printf("  GAP   %s (%s: %s)\n", name, anchor, expected);
    }
}

/* A case that did not run. The reason is mandatory. */
static inline void tc_skip(struct test_counts *c, const char *name, const char *reason)
{
    if (!reason || !*reason) {
        tc_fail(c, name, "skipped without a reason");
        return;
    }
    c->skipped++;
    printf("  SKIP  %s (%s)\n", name, reason);
}

/*
 * Process exit status policy: nonzero when anything failed. A mode that must run
 * at least one case (require_run) also fails when nothing passed or was
 * documented as a gap, so a mode that skipped everything cannot pass silently.
 */
static inline int tc_status(const struct test_counts *c, int require_run)
{
    return c->failed || (require_run && c->passed + c->gap == 0);
}

static inline void tc_summary(const struct test_counts *c)
{
    printf("\n=== Results: %d tests, %d passed, %d failed, %d gap, %d skipped ===\n", c->run,
           c->passed, c->failed, c->gap, c->skipped);
}

#endif /* RTMUX_TEST_OUTCOME_H */
