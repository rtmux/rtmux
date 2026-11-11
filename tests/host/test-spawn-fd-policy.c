/*
 * Host-side tests for the _PROC_SPAWN_FD admission policy.
 *
 * The refusals here are reached only by a child's own loader, so no ordinary
 * process can drive them from a guest test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include "spawn-fd-policy.h"

#define LISTED(...) \
    (const int32_t[]) { __VA_ARGS__ }

int main(void)
{
    const int32_t *l = LISTED(0, 5, SPAWN_FDCLOSED, 7);

    /* Explicit list: only the offered pairing, never a closed slot. */
    assert(spawn_fd_offer(4, l, 0, 1, 5) == SPAWN_OFFER_YES);
    assert(spawn_fd_offer(4, l, 0, 1, 6) == SPAWN_OFFER_NO);
    assert(spawn_fd_offer(4, l, 0, 0, 5) == SPAWN_OFFER_NO);
    assert(spawn_fd_offer(4, l, 0, 2, SPAWN_FDCLOSED) == SPAWN_OFFER_NO);
    assert(spawn_fd_offer(4, l, 0, 2, 0) == SPAWN_OFFER_NO);
    assert(spawn_fd_offer(4, l, 0, -1, 5) == SPAWN_OFFER_NO);
    assert(spawn_fd_offer(4, l, 0, 1, -2) == SPAWN_OFFER_NO);

    /* Past a list: refused unless the scan tail was requested. */
    assert(spawn_fd_offer(4, l, 0, 4, 4) == SPAWN_OFFER_NO);
    assert(spawn_fd_offer(4, l, SPAWN_FD_SCAN_TAIL, 4, 4) == SPAWN_OFFER_SCAN);
    assert(spawn_fd_offer(4, l, SPAWN_FD_SCAN_TAIL, 4, 5) == SPAWN_OFFER_NO);

    /* No list: scan mode, same-number fds only. */
    assert(spawn_fd_offer(0, NULL, 0, 3, 3) == SPAWN_OFFER_SCAN);
    assert(spawn_fd_offer(0, NULL, 0, 3, 4) == SPAWN_OFFER_NO);

    /* A scanned fd must be live and not close-on-exec. */
    assert(spawn_fd_scan_ok(0));
    assert(!spawn_fd_scan_ok(RTMUX_FLG_COF_CLOEXEC));
    assert(!spawn_fd_scan_ok(RTMUX_FLG_COF_DEAD));
    assert(!spawn_fd_scan_ok(RTMUX_FLG_COF_DEAD | RTMUX_FLG_COF_CLOEXEC));

    /* Request shape and timing. */
    {
        size_t ok = sizeof(struct _proc_spawn_fd) + sizeof(int32_t);

        assert(spawn_fd_request_ok(0, _PROC_SPAWN_FD_LIST, 1, ok));
        assert(!spawn_fd_request_ok(1, _PROC_SPAWN_FD_LIST, 1, ok));
        assert(!spawn_fd_request_ok(0, 0, 1, ok));
        assert(!spawn_fd_request_ok(0, _PROC_SPAWN_FD_LIST, 0, ok));
        assert(!spawn_fd_request_ok(0, _PROC_SPAWN_FD_LIST, 2, ok));
        assert(!spawn_fd_request_ok(0, _PROC_SPAWN_FD_LIST, 1, ok - 1));
    }

    puts("spawn-fd-policy-test: PASS");
    return 0;
}
