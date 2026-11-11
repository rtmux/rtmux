/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * In FreeBSD, ldso avoids libc by redirecting standard C functions to __sys_*
 * direct syscall stubs. In RTMux, the kernel uses message passing instead of
 * syscall numbers, so we redirect to our own rtmux_ldso_* wrappers declared in
 * rtmux-shim.h.
 *
 * These wrappers are thin shims that call into RTMux's own libc implementations
 * (open, close, read, write, mmap, etc.), which are statically linked into the
 * ldso binary.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DYNLINK_LIBC_H_
#define _DYNLINK_LIBC_H_

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#include "rtmux-shim.h"

/* Avoid dependencies on libthr (used by closedir/opendir/readdir). RTMux does
 * not have libthr; threads are managed by the microkernel.
 */
#define __isthreaded 0
#define _pthread_mutex_lock(mtx) (void) 0
#define _pthread_mutex_unlock(mtx) (void) 0
#define _pthread_mutex_destroy(mtx) (void) 0

/* Redirect standard POSIX functions to RTMux ldso wrappers.
 *
 * FreeBSD's ldso uses __sys_* naming for direct syscalls. We map both the
 * __sys_* names and the standard names to our rtmux_ldso_* implementations,
 * which use RTMux message passing internally.
 */

/* File I/O */
#define __sys_open(path, ...) rtmux_ldso_open(path, __VA_ARGS__)
#define __sys_close(fd) rtmux_ldso_close(fd)
#define __sys_read(fd, buf, n) rtmux_ldso_read(fd, buf, n)
#define __sys_write(fd, buf, n) rtmux_ldso_write(fd, buf, n)
#define __sys_pread(fd, buf, n, off) rtmux_ldso_pread(fd, buf, n, off)
#define __sys_fstat(fd, sb) rtmux_ldso_fstat(fd, sb)
#define __sys_fcntl(fd, cmd, arg) rtmux_ldso_fcntl(fd, cmd, arg)

/* openat / fstatat: FreeBSD ldso uses these for fdlopen() and
 * file-descriptor-relative lookups. Map to our stubs for now.
 */
#define __sys_openat(dirfd, path, flags, ...) \
    rtmux_ldso_openat(dirfd, path, flags, ##__VA_ARGS__)
#define __sys_fstatat(dirfd, path, sb, flags) \
    rtmux_ldso_fstatat(dirfd, path, sb, flags)

/* getcwd */
#define __sys___getcwd(buf, size) rtmux_ldso_getcwd(buf, size)

/* Signals */
#define __sys_sigprocmask(how, set, oset) rtmux_ldso_sigprocmask(how, set, oset)

/* Thread identity (FreeBSD thr_self / thr_kill) */
#define __sys_thr_self(idp) rtmux_ldso_thr_self(idp)
#define __sys_thr_kill(tid, sig) rtmux_ldso_thr_kill(tid, sig)

/* Process control */
#define __sys_exit(status) rtmux_ldso_exit(status)

/* Redirect the standard POSIX names used throughout dynlink.c (same approach as
 * FreeBSD's libc.h).
 */
#define close(fd) rtmux_ldso_close(fd)
#define _close(fd) rtmux_ldso_close(fd)
#define exit(status) rtmux_ldso_exit(status)
#define _exit(status) rtmux_ldso_exit(status)
#define fcntl(fd, cmd, arg) rtmux_ldso_fcntl(fd, cmd, arg)
#define _fcntl(fd, cmd, arg) rtmux_ldso_fcntl(fd, cmd, arg)
#define _fstat(fd, sb) rtmux_ldso_fstat(fd, sb)
#define open(path, ...) rtmux_ldso_open(path, __VA_ARGS__)
#define pread(fd, buf, n, off) rtmux_ldso_pread(fd, buf, n, off)
#define read(fd, buf, n) rtmux_ldso_read(fd, buf, n)
#define sigprocmask(how, set, oset) rtmux_ldso_sigprocmask(how, set, oset)
#define strerror(err) ldso_strerror(err)
#define _write(fd, buf, n) rtmux_ldso_write(fd, buf, n)
#define write(fd, buf, n) rtmux_ldso_write(fd, buf, n)

/* Redirect errno to use ldso's own private storage (__error() in
 * rtmux-ldso-stubs.c) instead of the libc __get_errno_ptr() which would
 * generate a PLT call — fatal during self-relocation when .got.plt entries have
 * not yet been resolved.
 */
int *__error(void);
#undef errno
#define errno (*__error())
#define __get_errno_ptr __error

/* Prevent any accidental use of __libc_interposing (FreeBSD concept that does
 * not exist in RTMux).
 */
#define __libc_interposing error_must_not_use_this_variable_inside_ldso

/* Declarations used by dynlink.c. */
const char *ldso_strerror(int errnum);
char *realpath(const char *path, char *resolved);

#endif /* _DYNLINK_LIBC_H_ */
