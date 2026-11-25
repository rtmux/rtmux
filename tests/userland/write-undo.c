/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* write_undo: a failed write takes back only its own growth.
 *
 * A write past the end of a /dev/shmem object grows the object first and then
 * copies the data in; when the copy fails, the write gives the growth back. A
 * CONFIG_DEBUG_INJECT roottask can hold such a write between its growth and its
 * copy (PGRP_WINDOW_WRITE_GROWN). While it is held, a second write lands inside
 * the grown range and succeeds; then the first write's source buffer is
 * unmapped, so its copy fails. The object must keep the size and the bytes the
 * second write gave it. Before the undo checked for other writers, the
 * failed write shrank the object back and the second write's bytes were gone.
 * Without the window the case is skipped.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/procmsg.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <unistd.h>

#include "outcome.h"
#include "test.h"

#define WU_PAGE 4096
#define WU_NAME "/rtmux-write-undo"
#define WU_HOLD_MS 1500

struct wu_writer {
    int fd;
    void *buf;
    ssize_t rc;
    int err;
};

/* Grows the one-page object to three pages, from a buffer that is unmapped
 * while the write is held.
 */
static void *wu_grow(void *raw)
{
    struct wu_writer *w = raw;

    w->rc = pwrite(w->fd, w->buf, 2 * WU_PAGE, WU_PAGE);
    w->err = errno;
    return NULL;
}

int test_write_undo(void)
{
    struct wu_writer w = {.fd = -1};
    char mark[WU_PAGE], got[WU_PAGE];
    char detail[128];
    struct stat st;
    pthread_t th;
    int held = 0;
    int fd;

    (void) shm_unlink(WU_NAME);
    fd = shm_open(WU_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        snprintf(detail, sizeof detail, "shm_open errno %d", errno);
        test_fail("write_undo fixture", detail);
        return 1;
    }
    memset(mark, 'A', sizeof mark);
    w.fd = fd;
    w.buf = mmap(NULL, 2 * WU_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, NOFD, 0);
    if (pwrite(fd, mark, WU_PAGE, 0) != WU_PAGE || w.buf == MAP_FAILED) {
        test_fail("write_undo fixture", "one-page object");
        goto out;
    }
    memset(w.buf, 'G', 2 * WU_PAGE);
    if (!test_window_arm(PGRP_WINDOW_WRITE_GROWN, WU_HOLD_MS)) {
        (void) close(fd);
        (void) shm_unlink(WU_NAME);
        return TC_EXIT_SKIP;
    }
    if (pthread_create(&th, NULL, wu_grow, &w) != 0) {
        test_fail("write_undo fixture", "pthread_create");
        goto out;
    }
    for (int i = 0; i < 200 && !held; i++) {
        held = test_window_arm(PGRP_WINDOW_ACTIVE, PGRP_WINDOW_WRITE_GROWN);
        if (!held)
            sleep_ms(5);
    }

    /* Inside the held write's growth: no growth of its own. */
    memset(mark, 'B', sizeof mark);
    ssize_t second = pwrite(fd, mark, WU_PAGE, 2 * WU_PAGE);
    (void) munmap(w.buf, 2 * WU_PAGE); /* the held write's copy now fails */
    (void) pthread_join(th, NULL);

    check(held, "write_undo the growing write was held", "the window never opened");
    check(second == WU_PAGE, "write_undo the second write succeeds", "short or failed write");
    snprintf(detail, sizeof detail, "held write returned %zd errno %d", w.rc, w.err);
    check(w.rc == -1, "write_undo the held write fails", detail);
    st.st_size = -1;
    (void) fstat(fd, &st);
    memset(got, 0, sizeof got);
    snprintf(detail, sizeof detail, "size %lld after the failed write, want %d",
             (long long) st.st_size, 3 * WU_PAGE);
    check(st.st_size == 3 * WU_PAGE, "write_undo keeps the other writer's size", detail);
    check(pread(fd, got, WU_PAGE, 2 * WU_PAGE) == WU_PAGE && !memcmp(got, mark, WU_PAGE),
          "write_undo keeps the other writer's bytes",
          "the failed write's undo must not take bytes another write put there");
    (void) close(fd);
    (void) shm_unlink(WU_NAME);
    return 0;

out:
    if (w.buf != MAP_FAILED && w.buf)
        (void) munmap(w.buf, 2 * WU_PAGE);
    (void) close(fd);
    (void) shm_unlink(WU_NAME);
    return 1;
}
