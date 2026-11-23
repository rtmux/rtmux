/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* shm_past_end: a shared memory mapping that runs past the end of its object.
 *
 * POSIX mmap: "References within the address range starting at pa and
 * continuing for len bytes to whole pages following the end of an object shall
 * result in delivery of a SIGBUS signal." Pages the object covers are ordinary
 * memory, and the object can change size under a mapping (ftruncate, or
 * F_ALLOCSP / F_FREESP here), so whether a page of an existing mapping is past
 * the end is decided by the object's size at the time of the access.
 *
 * A fault here kills the process (no user handler runs for a synchronous
 * fault), so the faults are taken in children and the parent reads the signal
 * from waitpid. One child, the holder, maps two pages of a one-page object and
 * keeps that one mapping throughout:
 *   1. a second child maps the same two pages and touches the second: SIGBUS;
 *   2. the parent grows the object to two pages; the holder reads the parent's
 *      pattern from its second page and writes its own back, through the
 *      mapping it made before the growth;
 *   3. the parent shrinks the object back to one page; the holder's next touch
 *      of its second page must be SIGBUS again.
 * A second holder maps the same two pages MAP_PRIVATE and writable: after the
 * growth it must reach its second page too, and what it writes there must stay
 * its own.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <devctl.h>
#include <devctl/proc.h>

#include "test.h"

#define SPE_PAGE 4096UL
#define SPE_NAME "/rtmux-shm-past-end"
#define SPE_WAIT_MS 20000
#define SPE_CTL_NAME "/rtmux-shm-past-end-ctl"
#define SPE_SHMCTL_ANON 0x1 /* SHMCTL_ANON: give the object anonymous memory */

/* The handshake lives in the object's first page, which every mapping of it
 * covers all along.
 */
struct spe_shared {
    atomic_int holder; /* holder progress: 1 mapped, 2 read and wrote page 1 */
    atomic_int parent; /* parent progress: 1 grown, 2 shrunk */
    atomic_int priv;   /* private holder: 1 mapped, 2 wrote and read page 1 */
    unsigned seen;     /* what the holder read from page 1 */
    atomic_int ctl;    /* shm_ctl holder: 1 mapped, 2 read and wrote */
    atomic_int sized;  /* parent: the shm_ctl object has its size */
    unsigned ctl_seen; /* what the shm_ctl holder read */
};

#define SPE_PARENT_WORD 0x4b383250U /* the parent writes this into page 1 */
#define SPE_HOLDER_WORD 0x484f4c44U /* the holder writes this back */
#define SPE_PRIVATE_WORD 0x50525654U /* the private holder writes this */

static int spe_space(int fd, int cmd, off_t start, off_t len)
{
    flock_t space;

    memset(&space, 0, sizeof space);
    space.l_whence = SEEK_SET;
    space.l_start = start;
    space.l_len = len;
    return fcntl(fd, cmd, &space);
}

static void *spe_map(int fd, size_t len)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    return p == MAP_FAILED ? NULL : p;
}

/* The private holder: its handshake goes through a shared mapping of the first
 * page, since its own private mapping sees no later store of the parent's.
 */
static int spe_private(int fd)
{
    struct spe_shared *sh = spe_map(fd, SPE_PAGE);
    unsigned char *p = mmap(NULL, 2 * SPE_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    volatile unsigned *page1;

    if (!sh || p == MAP_FAILED)
        return 30;
    page1 = (volatile unsigned *) (p + SPE_PAGE);
    atomic_store(&sh->priv, 1);
    if (!test_wait_flag(&sh->parent, 1, SPE_WAIT_MS))
        return 31;
    *page1 = SPE_PRIVATE_WORD;
    if (*page1 != SPE_PRIVATE_WORD)
        return 32;
    atomic_store(&sh->priv, 2);
    return 0;
}

/* Child side: "touch" maps two pages and reads the second; "hold" is the
 * holder described above, "private" the private one, "unmapped" maps a page
 * past the end and unmaps it again. Exit codes other than by
 * signal name the step that went wrong.
 */
int shm_past_end_child(const char *what)
{
    volatile unsigned *page1;
    struct spe_shared *sh;
    unsigned char *p;
    int fd;

    fd = shm_open(SPE_NAME, O_RDWR, 0600);
    if (fd == -1)
        return 20;
    if (!strcmp(what, "private"))
        return spe_private(fd);
    if (!strcmp(what, "ctl")) {
        /* Maps a page of the size-0 object, past its end, then reads and
         * writes it once shm_ctl has given the object its size.
         */
        int cfd = shm_open(SPE_CTL_NAME, O_RDWR, 0600);
        volatile unsigned *q;

        sh = spe_map(fd, SPE_PAGE);
        if (cfd == -1 || !sh)
            return 50;
        q = spe_map(cfd, SPE_PAGE);
        (void) close(cfd);
        if (!q)
            return 51;
        atomic_store(&sh->ctl, 1);
        if (!test_wait_flag(&sh->sized, 1, SPE_WAIT_MS))
            return 52;
        sh->ctl_seen = *q; /* a fault here is the failure */
        *q = SPE_HOLDER_WORD;
        atomic_store(&sh->ctl, 2);
        return 0;
    }
    if (!strcmp(what, "unmapped")) {
        /* A mapping wholly past the end, unmapped again: the range is no
         * mapping at all, so a touch is SIGSEGV, not SIGBUS.
         */
        volatile unsigned *q = mmap(NULL, SPE_PAGE, PROT_READ, MAP_SHARED, fd, (off_t) SPE_PAGE);

        if (q == MAP_FAILED || munmap((void *) q, SPE_PAGE) == -1)
            return 27;
        (void) *q;
        return 28;
    }
    p = spe_map(fd, 2 * SPE_PAGE);
    (void) close(fd);
    if (!p)
        return 21;
    sh = (struct spe_shared *) p;
    page1 = (volatile unsigned *) (p + SPE_PAGE);

    if (!strcmp(what, "touch")) {
        (void) *page1; /* past the end: must not return */
        return 22;
    }
    if (strcmp(what, "hold"))
        return 23;

    atomic_store(&sh->holder, 1);
    if (!test_wait_flag(&sh->parent, 1, SPE_WAIT_MS))
        return 24;
    sh->seen = *page1;
    *page1 = SPE_HOLDER_WORD;
    atomic_store(&sh->holder, 2);
    if (!test_wait_flag(&sh->parent, 2, SPE_WAIT_MS))
        return 25;
    (void) *page1; /* past the end again: must not return */
    return 26;
}

static pid_t spe_spawn(const char *what)
{
    char *argv[] = {"syscall_testing", "--shm-past-end-child", (char *) what, NULL};
    pid_t pid;

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0)
        return -1;
    return pid;
}

/* How a child ended: its signal number when it was killed, else 1000 plus its
 * exit status, or -1 when waitpid failed.
 */
static int spe_reap(pid_t pid)
{
    int status = 0;

    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WIFSIGNALED(status))
        return WTERMSIG(status);
    return 1000 + WEXITSTATUS(status);
}

static void spe_report(const char *name, int how, int want)
{
    char detail[96];

    snprintf(detail, sizeof detail, "child ended with %d, want %d (signal, or 1000 + exit status)",
             how, want);
    check(how == want, name, detail);
}

/* The cost of the marks: a 64 MiB mapping of the one-page object marks 16383
 * pages past its end. Whole 2 MiB regions are marked in their level-one slot,
 * so the mapping takes a few page-table pages, not one per 2 MiB (32 here,
 * 128 KiB). Another process can allocate meanwhile, so the best of three
 * tries is judged; the mappings stay until the end, so each try lands on
 * fresh addresses (an unmap leaves its page tables in place).
 */
#define SPE_BIG (64UL << 20)
#define SPE_BIG_TRIES 3
#define SPE_BIG_TABLE_MAX (8 * SPE_PAGE)

static void spe_mark_cost(int fd)
{
    void *big[SPE_BIG_TRIES];
    char detail[96];
    long best = -1;
    int n = 0;

    while (n < SPE_BIG_TRIES) {
        long before = kstats_value("pa.free_bytes");
        void *m = mmap(NULL, SPE_BIG, PROT_READ, MAP_SHARED, fd, 0);
        long after = kstats_value("pa.free_bytes");

        if (m == MAP_FAILED || before < 0 || after < 0) {
            if (m != MAP_FAILED)
                (void) munmap(m, SPE_BIG);
            test_fail("shm past end large mapping fixture", "mmap or kstats failed");
            break;
        }
        big[n++] = m;
        if (best < 0 || before - after < best)
            best = before - after;
    }
    while (n > 0)
        (void) munmap(big[--n], SPE_BIG);
    if (best < 0)
        return;
    snprintf(detail, sizeof detail, "a 64 MiB mapping took %ld bytes, want <= %lu", best,
             SPE_BIG_TABLE_MAX);
    check(best <= (long) SPE_BIG_TABLE_MAX, "shm past end large mapping costs few tables",
          detail);
}

/* The other locked growth path: shm_ctl (DEVCTL_MEM_OBJECT_SET) gives a size-0
 * object its size, holding the object lock, while a holder has a page of it
 * mapped (marked past the end). The holder must then read and write that page
 * through the mapping it had.
 */
static void spe_shmctl(struct spe_shared *sh)
{
    devctl_mem_object obj = {.size = SPE_PAGE, .flags = SPE_SHMCTL_ANON};
    volatile unsigned *q = NULL;
    char detail[96];
    int info = 0;
    pid_t pid;
    int fd;
    int rc;

    (void) shm_unlink(SPE_CTL_NAME);
    fd = shm_open(SPE_CTL_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        snprintf(detail, sizeof detail, "shm_open errno %d", errno);
        test_fail("shm past end shm_ctl fixture", detail);
        return;
    }
    pid = spe_spawn("ctl");
    if (pid == -1 || !test_wait_flag(&sh->ctl, 1, SPE_WAIT_MS)) {
        test_fail("shm past end shm_ctl fixture", "the holder did not map the object");
        goto out;
    }
    rc = devctl(fd, DEVCTL_MEM_OBJECT_SET, &obj, sizeof obj, &info);
    if (rc != EOK || info != 0 || !(q = spe_map(fd, SPE_PAGE))) {
        snprintf(detail, sizeof detail, "shm_ctl rc %d info %d errno %d", rc, info, errno);
        test_fail("shm past end shm_ctl size", detail);
        goto out;
    }
    *q = SPE_PARENT_WORD;
    atomic_store(&sh->sized, 1);
    spe_report("shm past end page works after shm_ctl sizes the object", spe_reap(pid), 1000);
    pid = -1;
    check(sh->ctl_seen == SPE_PARENT_WORD && *q == SPE_HOLDER_WORD,
          "shm past end shm_ctl page is shared",
          "the holder must read the parent's word and the parent the holder's");
out:
    if (pid != -1) {
        (void) kill(pid, SIGKILL);
        (void) spe_reap(pid);
    }
    if (q)
        (void) munmap((void *) q, SPE_PAGE);
    (void) close(fd);
    (void) shm_unlink(SPE_CTL_NAME);
}

void test_shm_past_end(void)
{
    struct spe_shared *sh = NULL;
    unsigned char *p = NULL;
    pid_t holder = -1;
    pid_t priv = -1;
    pid_t pid;
    char detail[80];
    int fd;

    (void) shm_unlink(SPE_NAME);
    fd = shm_open(SPE_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        snprintf(detail, sizeof detail, "shm_open errno %d", errno);
        test_fail("shm past end fixture", detail);
        return;
    }
    if (spe_space(fd, F_ALLOCSP, 0, (off_t) SPE_PAGE) == -1 ||
        !(sh = spe_map(fd, SPE_PAGE))) {
        snprintf(detail, sizeof detail, "one-page object errno %d", errno);
        test_fail("shm past end fixture", detail);
        goto out;
    }
    memset(sh, 0, sizeof *sh);
    spe_mark_cost(fd);
    spe_shmctl(sh);

    holder = spe_spawn("hold");
    priv = spe_spawn("private");
    if (holder == -1 || priv == -1 || !test_wait_flag(&sh->holder, 1, SPE_WAIT_MS) ||
        !test_wait_flag(&sh->priv, 1, SPE_WAIT_MS)) {
        test_fail("shm past end fixture", "a holder did not map the object");
        goto out;
    }

    pid = spe_spawn("touch");
    spe_report("shm past end page faults SIGBUS", pid == -1 ? -1 : spe_reap(pid), SIGBUS);
    pid = spe_spawn("unmapped");
    spe_report("shm past end page unmapped faults SIGSEGV", pid == -1 ? -1 : spe_reap(pid),
               SIGSEGV);

    if (spe_space(fd, F_ALLOCSP, 0, (off_t) (2 * SPE_PAGE)) == -1 ||
        !(p = spe_map(fd, 2 * SPE_PAGE))) {
        snprintf(detail, sizeof detail, "grow errno %d", errno);
        test_fail("shm past end grow", detail);
        goto out;
    }
    *(volatile unsigned *) (p + SPE_PAGE) = SPE_PARENT_WORD;
    atomic_store(&sh->parent, 1);
    check(test_wait_flag(&sh->holder, 2, SPE_WAIT_MS) && sh->seen == SPE_PARENT_WORD &&
              *(volatile unsigned *) (p + SPE_PAGE) == SPE_HOLDER_WORD,
          "shm past end page works after the object grows",
          "the mapping made before the growth must read and write the new page");
    spe_report("shm past end private page works after the object grows", spe_reap(priv), 1000);
    priv = -1;
    check(*(volatile unsigned *) (p + SPE_PAGE) == SPE_HOLDER_WORD,
          "shm past end private write stays private",
          "a MAP_PRIVATE store must not reach the object");
    (void) munmap(p, 2 * SPE_PAGE);

    if (spe_space(fd, F_FREESP, (off_t) SPE_PAGE, 0) == -1) {
        snprintf(detail, sizeof detail, "shrink errno %d", errno);
        test_fail("shm past end shrink", detail);
        goto out;
    }
    atomic_store(&sh->parent, 2);
    spe_report("shm past end page faults SIGBUS after a shrink", spe_reap(holder), SIGBUS);
    holder = -1;

out:
    if (priv != -1) {
        (void) kill(priv, SIGKILL);
        (void) spe_reap(priv);
    }
    if (holder != -1) {
        (void) kill(holder, SIGKILL);
        (void) spe_reap(holder);
    }
    if (sh)
        (void) munmap(sh, SPE_PAGE);
    (void) close(fd);
    (void) shm_unlink(SPE_NAME);
}
