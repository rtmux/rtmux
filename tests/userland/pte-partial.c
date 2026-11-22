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
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/wait.h>
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
    long before = kstats_value("vm.rollback_pages");
    errno = 0;
    m = mmap(at, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    err = errno;
    check(before >= 0 && kstats_value("vm.rollback_pages") == before + 2,
          "pte_map rolled back exactly two pages", "successful rollback count");
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

/* anon_map_failure: a loader context whose mapping fails part of the way
 * gives its physical memory back.
 *
 * roottask maps each new loader context with kext_root_mmap_anon_user, which
 * takes a contiguous run of physical memory and maps it at the next address of
 * a grow-only window. fail-pte-map on roottask makes that MAP stop after one
 * page, as if a page table could not be had: the spawn fails with EAGAIN, and
 * roottask asks again, at the same address, for the next spawn of that size.
 * Each failure must leave pa.free_bytes where it was (it used to keep the run,
 * and the page it had mapped). Other memory moves as processes come and go,
 * either way, so the test allows a drop smaller than one argument's bytes: a
 * leaked run is a whole context, larger than its argument, each round, so the
 * leaking kernel lost AMF_ROUNDS of them (655360 bytes in 8 rounds of 24 KiB
 * spawns, an 80 KiB class).
 */
#define AMF_ROUNDS 8

/* Spawn /bin/true with one argument of `bytes` bytes, which sets the size class
 * of its loader context. Returns posix_spawn's error, or 0 once it has exited.
 */
static int amf_spawn(size_t bytes)
{
    char *argv[] = {"true", NULL, NULL};
    char *envp[] = {NULL};
    pid_t pid;
    int rc;

    if (!(argv[1] = malloc(bytes)))
        return ENOMEM;
    memset(argv[1], 'a', bytes - 1);
    argv[1][bytes - 1] = '\0';
    rc = posix_spawn(&pid, "/bin/true", NULL, NULL, argv, envp);
    free(argv[1]);
    if (rc == 0 && waitpid(pid, NULL, 0) != pid)
        rc = ECHILD;
    return rc;
}

/* Arm the failure for roottask's next MAP of more than one page. */
static int amf_arm(void)
{
    char pid[16];

    snprintf(pid, sizeof pid, "%d", ROOTTASK_PID);
    return inject_run("fail-pte-map", pid, "1");
}

/* Move an arm that was not taken off roottask: one for this process, for a MAP
 * at a page it never maps.
 */
static void amf_disarm(void)
{
    (void) ptp_inject("fail-pte-map", 0x1001);
}

/* One spawn of `bytes` with the failure armed: 1 if its loader context map
 * took it and the spawn failed, 0 if the spawn did not map a context (it had a
 * free one), -1 on anything else.
 */
static int amf_fail_once(size_t bytes, int *errp)
{
    int rc;

    if (amf_arm() != 0)
        return -1;
    rc = amf_spawn(bytes);
    *errp = rc;
    if (inject_run("alloc-status", "consumed", NULL) != 0) {
        amf_disarm();
        return rc == 0 ? 0 : -1;
    }
    return rc == EAGAIN ? 1 : -1;
}

int anon_map_failure(void)
{
    /* One argument for each of the size classes past the standard one. */
    static const size_t sizes[] = {24 * 1024, 56 * 1024, 88 * 1024};
    char why[192];
    size_t bytes = 0;
    long before, after = -1;
    int err = 0;

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(ptp_pid, sizeof ptp_pid, "%d", (int) getpid());

    /* A class that has no free context: its spawn maps one and takes the arm.
     * The first failure there is the warm-up: it may make the page tables of
     * the window, which stay.
     */
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0] && !bytes; i++) {
        int got = amf_fail_once(sizes[i], &err);

        if (got < 0) {
            snprintf(why, sizeof why, "spawn of %zu bytes: errno %d", sizes[i], err);
            test_fail("anon map failure fixture", why);
            return 0;
        }
        if (got)
            bytes = sizes[i];
    }
    if (!bytes) {
        test_fail("anon map failure fixture", "every size class had a free loader context");
        return 0;
    }

    before = kstats_value("pa.free_bytes");
    for (int r = 0; r < AMF_ROUNDS; r++) {
        int got = amf_fail_once(bytes, &err);

        if (got != 1) {
            snprintf(why, sizeof why, "round %d: %s, errno %d", r,
                     got ? "spawn did not fail as armed" : "a context was free", err);
            test_fail("anon map failure rounds", why);
            return 0;
        }
    }
    /* Give memory a moment to come back from processes that are ending. */
    for (int i = 0; i < 40 && (after = kstats_value("pa.free_bytes")) <= before - (long) bytes; i++)
        sleep_ms(50);
    snprintf(why, sizeof why,
             "pa.free_bytes %ld before, %ld after %d failed maps of %zu-byte spawns (a drop of "
             "%zu or more is a leak)",
             before, after, AMF_ROUNDS, bytes, bytes);
    check(before > 0 && after > before - (long) bytes, "a failed loader context map frees its memory",
          why);

    /* The failure is gone: the same spawn works. */
    err = amf_spawn(bytes);
    snprintf(why, sizeof why, "errno %d", err);
    check(err == 0, "the spawn works after its context map failed", why);
    return 0;
}
