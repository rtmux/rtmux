/*
 * Host-side tests for loader_end(): what a spawner is told when its child's
 * loader ended before the child ran.
 *
 * A loader step that gives up with errno 0 cannot be made to happen from a
 * guest test, so this pins the mapping instead.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include "loader-end.h"

static int end(int signo, int code, int status, int *error)
{
    siginfo_t si;

    memset(&si, 0, sizeof si);
    si.si_signo = signo;
    si.si_code = code;
    si.si_status = status;
    *error = -1;
    return loader_end(&si, error);
}

int main(void)
{
    int error;

    /* An exit carries its errno; an exit with 0 is not a success. */
    assert(end(SIGCHLD, CLD_EXITED, ENOMEM, &error) == 0 && error == ENOMEM);
    assert(end(SIGCHLD, CLD_EXITED, ENOENT, &error) == 0 && error == ENOENT);
    assert(end(SIGCHLD, CLD_EXITED, 0, &error) == 0 && error == EIO);

    /* A fault, recorded by the kernel or in an exit message, is EFAULT. */
    assert(end(SIGSEGV, SEGV_MAPERR, 0, &error) == SIGSEGV && error == EFAULT);
    assert(end(SIGCHLD, CLD_KILLED, SIGBUS, &error) == SIGBUS && error == EFAULT);
    assert(end(SIGCHLD, CLD_DUMPED, SIGILL, &error) == SIGILL && error == EFAULT);
    assert(end(SIGFPE, FPE_INTDIV, 0, &error) == SIGFPE && error == EFAULT);

    /* Killed while loading: EINTR. No record at all: EINTR, and not an exit. */
    assert(end(SIGCHLD, CLD_KILLED, SIGKILL, &error) == SIGKILL && error == EINTR);
    assert(end(0, 0, 0, &error) == -1 && error == EINTR);

    printf("loader-end-test: ok\n");
    return 0;
}
