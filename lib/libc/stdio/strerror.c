/*
 * strerror — map error number to string.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Minimal error table — covers the most common POSIX errors. Entries with NULL
 * will fall through to "Unknown error".
 */
static const char *const _errlist[] = {
    [0] = "Success",
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "Input/output error",
    [ENXIO] = "No such device or address",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [ENOTBLK] = "Block device required",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [EXDEV] = "Invalid cross-device link",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [ENFILE] = "Too many open files in system",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Inappropriate ioctl for device",
    [EFBIG] = "File too large",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EMLINK] = "Too many links",
    [EPIPE] = "Broken pipe",
    [EDOM] = "Numerical argument out of domain",
    [ERANGE] = "Numerical result out of range",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [ENAMETOOLONG] = "File name too long",
};

static char _unknown_buf[32];

char *strerror(int err)
{
    if (err >= 0 && err < (int) (sizeof(_errlist) / sizeof(_errlist[0])) &&
        _errlist[err])
        return (char *) _errlist[err];

    /* Format unknown error */
    snprintf(_unknown_buf, sizeof(_unknown_buf), "Unknown error %d", err);
    return _unknown_buf;
}

/* Reentrant strerror: writes into the caller's buffer instead of the shared
 * static one, so concurrent callers do not clobber each other's message.
 * Returns 0 on success and ERANGE if the message did not fit.
 */
int strerror_r(int err, char *buf, size_t buflen)
{
    const char *msg = NULL;
    size_t len;

    if (!buf || buflen == 0)
        return EINVAL;

    if (err >= 0 && err < (int) (sizeof(_errlist) / sizeof(_errlist[0])) &&
        _errlist[err])
        msg = _errlist[err];

    if (!msg) {
        /* Unknown error: format straight into the caller's buffer. */
        int n = snprintf(buf, buflen, "Unknown error %d", err);
        if (n < 0 || (size_t) n >= buflen)
            return ERANGE;
        return 0;
    }

    len = strlen(msg);
    if (len >= buflen) {
        memcpy(buf, msg, buflen - 1);
        buf[buflen - 1] = '\0';
        return ERANGE;
    }
    memcpy(buf, msg, len + 1);
    return 0;
}
