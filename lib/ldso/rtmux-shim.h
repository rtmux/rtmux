/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux ldso platform shim declarations.
 *
 * These functions provide the I/O, memory, signal, and process primitives that
 * the FreeBSD ldso-elf expects. Each one wraps a RTMux libc function that uses
 * microkernel message passing.
 *
 * The ldso binary is statically linked with a minimal libc subset, so these
 * wrappers call the real RTMux functions directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_LDSO_SHIM_H
#define _RTMUX_LDSO_SHIM_H

#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#ifndef __hidden
#define __hidden __attribute__((visibility("hidden")))
#endif

int rtmux_ldso_open(const char *path, int flags, ...) __hidden;
int rtmux_ldso_close(int fd) __hidden;
ssize_t rtmux_ldso_read(int fd, void *buf, size_t nbytes) __hidden;
ssize_t rtmux_ldso_write(int fd, const void *buf, size_t nbytes) __hidden;
ssize_t rtmux_ldso_pread(int fd,
                         void *buf,
                         size_t nbytes,
                         off_t offset) __hidden;
int rtmux_ldso_fstat(int fd, struct stat *sb) __hidden;
int rtmux_ldso_fcntl(int fd, int cmd, ...) __hidden;
int rtmux_ldso_openat(int dirfd, const char *path, int flags, ...) __hidden;
int rtmux_ldso_fstatat(int dirfd,
                       const char *path,
                       struct stat *sb,
                       int flags) __hidden;
int rtmux_ldso_getcwd(char *buf, size_t size) __hidden;
void *rtmux_ldso_mmap(void *addr,
                      size_t len,
                      int prot,
                      int flags,
                      int fd,
                      off_t offset) __hidden;
int rtmux_ldso_mprotect(void *addr, size_t len, int prot) __hidden;
int rtmux_ldso_munmap(void *addr, size_t len) __hidden;
void rtmux_ldso_exit(int status) __attribute__((noreturn)) __hidden;
pid_t rtmux_ldso_getpid(void) __hidden;
int rtmux_ldso_sigprocmask(int how,
                           const sigset_t *set,
                           sigset_t *oset) __hidden;
int rtmux_ldso_thr_self(long *idp) __hidden;
int rtmux_ldso_thr_kill(long tid, int sig) __hidden;

#endif /* _RTMUX_LDSO_SHIM_H */
