/*
 * How a loader that did not finish ended: what its spawner is told.
 *
 * Pure, so a host test can pin every case (tests/host/test-loader-end.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LOADER_END_H
#define LOADER_END_H

#include <errno.h>
#include <signal.h>

/* 0 with *error the errno it exited with, or the signal that killed it with
 * *error what the spawner is told. A loader that exited with status 0 gave up
 * on a step that set no errno: EIO, since an error answer of 0 would read as a
 * spawn that worked and name no child. A fault in the loader (a kernel-recorded
 * SIGSEGV, SIGBUS, SIGILL or SIGFPE, or an exit message that names one) is
 * EFAULT: it is a defect or a bad image, not a shortage, and ENOMEM would send
 * the caller looking for memory. Any other signal means somebody killed the
 * child while it loaded: EINTR.
 */
static inline int loader_end(const siginfo_t *si, int *error)
{
    int signo = si->si_signo;

    if (signo == SIGCHLD) {
        if (si->si_code == CLD_EXITED) {
            *error = si->si_status ? si->si_status : EIO;
            return 0;
        }
        signo = si->si_status; /* CLD_KILLED and CLD_DUMPED carry the signal */
    }
    switch (signo) {
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
        *error = EFAULT;
        break;
    default:
        *error = EINTR;
        break;
    }
    return signo ? signo : -1; /* no record at all: not an exit either */
}

#endif
