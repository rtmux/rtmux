/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>

/* System-invariant _PC_* values are identical for every descriptor, so answer
 * them from the limit/capability macros rather than an I/O service. Only names
 * whose value a service can legitimately vary per descriptor are left out and
 * forwarded -- notably _PC_PIPE_BUF, which the pipe service answers with its
 * (runtime-configurable) atomic write size.
 *
 * Returns 1 and fills *value for a handled name, 0 otherwise.
 */
static int static_pathconf(int name, long *value)
{
    switch (name) {
    case _PC_LINK_MAX:
        *value = LINK_MAX;
        return 1;
    case _PC_NAME_MAX:
        *value = NAME_MAX;
        return 1;
    case _PC_PATH_MAX:
        *value = PATH_MAX;
        return 1;
    case _PC_SYMLOOP_MAX:
        *value = SYMLOOP_MAX;
        return 1;
    case _PC_NO_TRUNC:
        *value = _POSIX_NO_TRUNC;
        return 1;
    case _PC_VDISABLE:
        *value = _POSIX_VDISABLE;
        return 1;
    case _PC_CHOWN_RESTRICTED:
        *value = 1;
        return 1;
    case _PC_ASYNC_IO:
        *value = _POSIX_ASYNC_IO;
        return 1;
    case _PC_PRIO_IO:
        *value = _POSIX_PRIO_IO;
        return 1;
    case _PC_SYNC_IO:
        *value = _POSIX_SYNC_IO;
        return 1;
    case _PC_FILESIZEBITS:
        *value = (long) (sizeof(off_t) * CHAR_BIT);
        return 1;
    case _PC_SYMLINK_MAX:
        *value = _POSIX_SYMLINK_MAX;
        return 1;
    default:
        return 0;
    }
}

static int valid_pathconf_name(int name, int allow_private)
{
    switch (name) {
    case 11: /* roottask-private ROOT_PC_IMAGE_VADDR, used by loader-elf */
        return allow_private;
    case _PC_LINK_MAX:
    case _PC_MAX_CANON:
    case _PC_MAX_INPUT:
    case _PC_NAME_MAX:
    case _PC_PATH_MAX:
    case _PC_PIPE_BUF:
    case _PC_NO_TRUNC:
    case _PC_VDISABLE:
    case _PC_CHOWN_RESTRICTED:
    case _PC_ASYNC_IO:
    case _PC_PRIO_IO:
    case _PC_SYNC_IO:
    case _PC_FILESIZEBITS:
    case _PC_SYMLINK_MAX:
    case _PC_SYMLOOP_MAX:
    case _PC_2_SYMLINKS:
    case _PC_ALLOC_SIZE_MIN:
    case _PC_REC_INCR_XFER_SIZE:
    case _PC_REC_MAX_XFER_SIZE:
    case _PC_REC_MIN_XFER_SIZE:
    case _PC_REC_XFER_ALIGN:
        return 1;
    default:
        return 0;
    }
}

static int indeterminate_pathconf_name(int name)
{
    switch (name) {
    case _PC_MAX_CANON:
    case _PC_MAX_INPUT:
    case _PC_ALLOC_SIZE_MIN:
    case _PC_REC_INCR_XFER_SIZE:
    case _PC_REC_MAX_XFER_SIZE:
    case _PC_REC_MIN_XFER_SIZE:
    case _PC_REC_XFER_ALIGN:
        return 1;
    default:
        return 0;
    }
}

long fpathconf(int fd, int name)
{
    io_pathconf_t msg;
    long value;
    int ret;

    /* POSIX requires a valid descriptor for every query. A negative or
     * side-channel handle is not a POSIX fd (EBADF, matching the dup family),
     * and a closed positive descriptor fails F_GETFD. Validate up front so both
     * the answered-here and forwarded names reject a bad descriptor uniformly
     * -- a bare msg_sendnc would happily transmit over a side-channel
     * connection.
     */
    if (fd < 0 || ((unsigned) fd & RTMUX_SIDE_CHANNEL) ||
        fcntl(fd, F_GETFD) == -1) {
        errno = EBADF;
        return -1;
    }

    if (static_pathconf(name, &value))
        return value;
    if (!valid_pathconf_name(name, 1)) {
        errno = EINVAL;
        return -1;
    }

    msg.i.type = _IO_PATHCONF;
    msg.i.combine_len = sizeof msg.i;
    msg.i.name = (short) name;
    msg.i.zero = 0;
    ret = msg_sendnc(fd, &msg.i, sizeof msg.i, 0, 0);
    if (ret == -1 && errno == ENOSYS) {
        /* The descriptor's service does not answer this name. Supply the system
         * default for names that have one, and report POSIX indeterminate
         * values without treating them as errors.
         */
        if (name == _PC_PIPE_BUF) {
            errno = 0;
            return PIPE_BUF;
        }
        if (name == _PC_2_SYMLINKS) {
            errno = 0;
            return 0;
        }
        if (indeterminate_pathconf_name(name)) {
            errno = 0;
            return -1;
        }
    }
    return ret;
}

long pathconf(const char *path, int name)
{
    long value;

    /* pathconf() is a pathname metadata query: it requires only search
     * permission on the path prefix, not read/write/exec on the target. Resolve
     * with access(F_OK) rather than a read-open, so a search-only caller is
     * never rejected with a spurious EACCES and no device or server open path
     * runs for a read-only query.
     */
    if (access(path, F_OK) != 0)
        return -1;

    if (static_pathconf(name, &value))
        return value;
    if (!valid_pathconf_name(name, 0)) {
        errno = EINVAL;
        return -1;
    }

    /* No filesystem service registers an _IO_PATHCONF handler and RTMux has no
     * named FIFOs, so a pathname never resolves to a service that varies these
     * per file -- an fd forward would only reach ENOSYS. Supply the same
     * defaults fpathconf falls back to when a service does not answer, and
     * report POSIX indeterminate values without treating them as errors.
     */
    if (name == _PC_PIPE_BUF)
        return PIPE_BUF;
    if (name == _PC_2_SYMLINKS)
        return 0;
    if (indeterminate_pathconf_name(name))
        errno = 0;
    else
        errno = ENOSYS;
    return -1;
}
