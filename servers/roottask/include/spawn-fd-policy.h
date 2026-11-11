/*
 * Admission policy for _PROC_SPAWN_FD (the loader's per-fd key request).
 *
 * Pure decisions, no I/O, so a host test can pin every refusal. The caller
 * supplies the facts (the loader context, the parent's spawn request, the
 * parent connection's flags) and acts on the verdict.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPAWN_FD_POLICY_H
#define SPAWN_FD_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

/* Verdict of spawn_fd_offer(): the parent's request does not offer this fd, does
 * offer it, or offers it only if the parent's fd at that number is live and
 * inheritable (the caller checks, with spawn_fd_scan_ok()).
 */
enum { SPAWN_OFFER_NO = 0, SPAWN_OFFER_YES = 1, SPAWN_OFFER_SCAN = 2 };

/* Does the spawn request (nfds list entries, list[], SPAWN_* flags) let the
 * child's fd `base` inherit the parent's fd `parent_fd`? An explicit list entry
 * is honored, SPAWN_FDCLOSED never is. Past the list (or with no list) only
 * the scan-tail/scan modes apply, and only at the fd's own number.
 */
static inline int spawn_fd_offer(unsigned nfds,
                                 const int32_t *list,
                                 uint32_t spawn_flags,
                                 int base,
                                 int parent_fd)
{
    if (base < 0 || parent_fd < 0)
        return SPAWN_OFFER_NO;
    if ((unsigned) base < nfds)
        return list[base] == parent_fd && parent_fd != SPAWN_FDCLOSED
                   ? SPAWN_OFFER_YES
                   : SPAWN_OFFER_NO;
    if (nfds != 0 && !(spawn_flags & SPAWN_FD_SCAN_TAIL))
        return SPAWN_OFFER_NO;
    return base == parent_fd ? SPAWN_OFFER_SCAN : SPAWN_OFFER_NO;
}

/* A scanned parent fd is inheritable only while live and not close-on-exec. */
static inline int spawn_fd_scan_ok(uint32_t conn_flags)
{
    return (conn_flags & (RTMUX_FLG_COF_DEAD | RTMUX_FLG_COF_CLOEXEC)) == 0;
}

/* Is a _PROC_SPAWN_FD request well formed and still timely? One listed fd per
 * call, only while fd inheritance is open (not after _PROC_SPAWN_DONE/EXEC).
 */
static inline int spawn_fd_request_ok(int fds_done,
                                      uint16_t flags,
                                      int nfds,
                                      size_t msglen)
{
    return !fds_done && (flags & _PROC_SPAWN_FD_LIST) && nfds == 1 &&
           msglen >= sizeof(struct _proc_spawn_fd) + sizeof(int32_t);
}

#endif
