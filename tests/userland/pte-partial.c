/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* pte_partial_map: a MAP that fails part of the way reports its progress, and
 * leaves the address space as it found it.
 *
 * A CONFIG_DEBUG_INJECT kernel can make that failure happen on demand, in the
 * two ways cpu_pte_manipulate can fail after it has mapped some pages:
 *
 * - EEXIST: /bin/inject pte-superpage turns one empty 2 MiB slot of this
 *   process's page table into a 2 MiB leaf, and a mapping that runs from the
 *   pages below it into the slot must fail without mapping anything inside the
 *   slot or leaving the pages below it behind.
 * - ENOMEM: /bin/inject fail-pte-map makes the next mapping of more than a few
 *   pages run dry after that many.
 *
 * In both, the failed call must say how far it got (/bin/inject manip-partial):
 * roottask's pte_map unmaps what a failed MAP already mapped, and can only do
 * so if it is told. Afterwards no page of the failed mapping may be left in the
 * page table, and the same range must map and work. Without the injection
 * call the case is skipped.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test.h"

#define PTP_PAGE 4096UL
#define PTP_SLOT (2UL * 1024 * 1024)
#define PTP_BELOW 8UL /* pages of the mapping below the superpage */
#define PTP_ABOVE 2UL /* pages of it inside the superpage */
#define PTP_TRIES 12

static char ptp_pid[24];

static int ptp_inject(const char *op, uintptr_t va)
{
    char arg[24];

    snprintf(arg, sizeof arg, "%#lx", (unsigned long) va);
    return inject_run(op, ptp_pid, arg);
}

/* Whether the page table holds a leaf at va: 1, 0, or -1 if the probe failed. */
static int ptp_mapped(uintptr_t va)
{
    int rc = ptp_inject("pte-probe", va);

    return rc == 0 ? 0 : rc == 3 ? 1 : -1;
}

/* Check that the first and last of n pages from lo are out of the page table;
 * each probe is a process, so not every page.
 */
static void ptp_check_unmapped(uintptr_t lo, unsigned n, const char *name)
{
    check(ptp_mapped(lo) == 0 && ptp_mapped(lo + (n - 1) * PTP_PAGE) == 0, name,
          "a page of the failed mapping is still in the page table");
}

/* Whether the pages below slot are free: a hint mapping that lands exactly
 * there, given back at once. It does not touch the slot, whose table an
 * ordinary mapping would create, so the slot stays empty for the injection.
 */
static int ptp_below_free(uintptr_t slot)
{
    uintptr_t lo = slot - PTP_BELOW * PTP_PAGE;
    size_t len = PTP_BELOW * PTP_PAGE;
    void *p = mmap((void *) lo, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);

    if (p == MAP_FAILED)
        return 0;
    (void) munmap(p, len);
    return (uintptr_t) p == lo;
}

/* A mapping that runs dry after two pages: ENOMEM, with the two pages reported
 * and none of the range left, and the same mapping works afterwards.
 */
static void ptp_enomem_midway(void)
{
    const size_t len = 8 * PTP_PAGE;
    char arm[24];
    void *at;
    void *m;
    int err;

    /* A range that is free now stays free: nothing else maps in this process. */
    at = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (at == MAP_FAILED) {
        test_fail("pte_partial_map fixture", strerror(errno));
        return;
    }
    (void) munmap(at, len);
    /* Two pages, only for the MAP that starts at this range. */
    snprintf(arm, sizeof arm, "%#lx", (unsigned long) at | 2);

    if (inject_run("fail-pte-map", ptp_pid, arm) != 0) {
        test_fail("pte_partial_map fixture", "fail-pte-map refused");
        return;
    }
    errno = 0;
    m = mmap(at, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    err = errno;
    check(m == MAP_FAILED && err == ENOMEM, "pte_partial_map ENOMEM midway fails",
          "a MAP that runs dry must fail with ENOMEM");
    check(inject_run("alloc-status", NULL, NULL) == 0,
          "pte_partial_map ENOMEM was injected", "the armed failure was not taken");
    check(inject_run("manip-partial", "2", NULL) == 0,
          "pte_partial_map reports the pages done",
          "the failed MAP must say it mapped the two pages before it ran dry");
    if (m != MAP_FAILED)
        (void) munmap(m, len);
    ptp_check_unmapped((uintptr_t) at, 8, "pte_partial_map ENOMEM leaves no page");
    m = mmap(at, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(m == at, "pte_partial_map maps after ENOMEM",
          "the same mapping must work once the failure is gone");
    if (m != MAP_FAILED) {
        ((volatile unsigned char *) m)[len - 1] = 7;
        (void) munmap(m, len);
    }
}

int pte_partial_map(void)
{
    const size_t len = (PTP_BELOW + PTP_ABOVE) * PTP_PAGE;
    uintptr_t slot = 0;
    uintptr_t lo;
    void *p0;
    uintptr_t base;
    int failed_errno;
    void *m;

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(ptp_pid, sizeof ptp_pid, "%d", (int) getpid());

    /* A free address to build around, then the nearest empty slot around it. */
    p0 = mmap(NULL, PTP_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
              -1, 0);
    if (p0 == MAP_FAILED) {
        test_fail("pte_partial_map fixture", strerror(errno));
        return 0;
    }
    base = ((uintptr_t) p0 + PTP_SLOT) & ~(PTP_SLOT - 1);
    (void) munmap(p0, PTP_PAGE);
    for (unsigned i = 2; i < PTP_TRIES && !slot; i++) {
        uintptr_t cand = (i & 1) ? base + (i / 2) * PTP_SLOT
                                 : base - (i / 2) * PTP_SLOT;

        /* Only an empty slot under an existing table takes the injection. */
        if (ptp_inject("pte-superpage", cand) != 0)
            continue;
        if (ptp_below_free(cand)) {
            slot = cand;
        } else {
            (void) ptp_inject("pte-clear-superpage", cand);
        }
    }
    if (!slot) {
        test_fail("pte_partial_map fixture", "no empty 2 MiB slot to plant a superpage in");
        return 0;
    }
    lo = slot - PTP_BELOW * PTP_PAGE;

    /* The mapping starts below the slot and ends inside the superpage. */
    errno = 0;
    m = mmap((void *) lo, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    failed_errno = errno;
    check(m == MAP_FAILED && failed_errno == EEXIST,
          "pte_partial_map over a superpage fails",
          "a MAP into a 2 MiB leaf must fail with EEXIST");
    if (m != MAP_FAILED)
        (void) munmap(m, len);
    else
        printf("  note  the failed mapping reported errno %d\n", failed_errno);

    /* A mapping wholly inside the superpage fails too. */
    m = mmap((void *) (slot + PTP_PAGE), 2 * PTP_PAGE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    check(m == MAP_FAILED, "pte_partial_map inside a superpage fails",
          "a MAP wholly inside a 2 MiB leaf must not succeed");
    if (m != MAP_FAILED)
        (void) munmap(m, 2 * PTP_PAGE);

    ptp_check_unmapped(lo, PTP_BELOW, "pte_partial_map superpage leaves no page");

    /* Take the superpage out: the whole range maps and is usable. */
    check(ptp_inject("pte-clear-superpage", slot) == 0,
          "pte_partial_map fixture clear", "the injected leaf could not be removed");
    m = mmap((void *) lo, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    check(m == (void *) lo, "pte_partial_map maps once the superpage is gone",
          "the range must map after the failed attempt");
    if (m != MAP_FAILED) {
        volatile unsigned char *b = m;

        b[0] = 1;
        b[len - 1] = 2;
        check(b[0] == 1 && b[len - 1] == 2, "pte_partial_map range is usable",
              "stores to the remapped range must read back");
        (void) munmap(m, len);
    }
    ptp_enomem_midway();
    return 0;
}
