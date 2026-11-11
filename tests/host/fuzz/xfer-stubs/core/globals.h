/*
 * Host-side stub of <core/globals.h> for the xfer_memchk fuzz harness.
 *
 * kernel/xfer/check.c includes this to pull in the iov type, the boundary
 * predicate, and the fault-handler machinery. Only the pieces xfer_memchk
 * actually needs are real; the fault-handler types and helpers exist so the
 * unrelated globals in check.c compile and link, they are never called by the
 * checker under test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_HOST_FUZZ_XFER_GLOBALS_H
#define RTMUX_HOST_FUZZ_XFER_GLOBALS_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

/* The iov type xfer_memchk walks. */
typedef struct iovec {
    void *iov_base;
    size_t iov_len;
} iov_t;

#define GETIOVBASE(_iov) ((_iov)->iov_base)
#define GETIOVLEN(_iov) ((_iov)->iov_len)

/* RV64 user space sits below the kernel boundary, so an iov range is in bounds
 * when both its first and last address are at or below the boundary.
 */
#define WITHIN_BOUNDARY(first, last, boundary) \
    ((first) <= (boundary) && (last) <= (boundary))

/* Opaque placeholders for the fault-handler signatures. */
typedef struct {
    int _unused;
} thread_t;

typedef struct {
    int _unused;
} CPU_REGISTERS;

struct fault_handlers {
    void (*fault)(thread_t *, CPU_REGISTERS *, unsigned);
    void (*restart)(thread_t *, CPU_REGISTERS *);
};

/* Defined by the harness to satisfy the link; never invoked. */
void xfer_restart(thread_t *thp, CPU_REGISTERS *regs);
void xfer_async_restart(thread_t *thp, CPU_REGISTERS *regs);

/* Single-threaded transfer environment. */
extern jmp_buf *host_xfer_env;
#define SET_XFER_ENV(env) (host_xfer_env = (env))
#define GET_XFER_ENV() (host_xfer_env)

#define xfer_setjmp(env) setjmp(env)
#define xfer_longjmp(env, val, regs) longjmp((env), (val))

#define FAULT_ISWRITE(flags) ((flags) & 1)
#define XFER_SRC_FAULT 0x01
#define XFER_DST_FAULT 0x02

/* xferlen installs and clears a per-CPU fault handler around its length walk;
 * the harness never faults, so recording the handler is a no-op here. The
 * kernel macro writes get_pcpu()->pc_xfer_handlers.
 */
#define SET_XFER_HANDLER(handler) ((void) (handler))

int xfer_memchk(uintptr_t bound, const iov_t *iov, size_t iov_len);
int xferlen(thread_t *thp, iov_t *iov, int parts);

#endif /* RTMUX_HOST_FUZZ_XFER_GLOBALS_H */
