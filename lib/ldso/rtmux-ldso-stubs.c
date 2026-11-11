/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux ldso stubs for FreeBSD-specific interfaces.
 *
 * FreeBSD's dynlink.c references various libc internals and BSD-specific
 * functions that do not exist (and will never exist) in RTMux. This file
 * provides minimal stubs so the ldso port compiles and links without pulling in
 * large swathes of FreeBSD libc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/storage.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>

#include "rtmux-shim.h"

/* Kernel console trace (syscall 3, __KSYS_DBGPRINT) */

/* ldso_trace — emit a string to the kernel console via dbgprint(). Compiled out
 * unless LDSO_VERBOSE is defined (see local.h).
 */
#ifdef LDSO_VERBOSE
extern void dbgprint(const char *, size_t);
void ldso_trace(const char *msg)
{
    size_t len = 0;
    while (msg[len] != '\0')
        len++;
    dbgprint(msg, len);
}
#endif

/* errno support */

/* FreeBSD's dynamic-linker libc shim provides __error() to give ldso its own
 * private errno storage, avoiding any dependency on libthr. RTMux threads get
 * errno via TLS (__tls()->__errval), but for the initial ldso port we use a
 * simple static variable.
 */
static int ldso_errno;

int *__error(void)
{
    return &ldso_errno;
}

/* __get_errno_ptr: RTMux libc's errno accessor. Libc objects compiled into ldso
 * (lc_*.o) reference this instead of __error(). Provide a local definition so
 * these references resolve within the ldso binary, avoiding a JUMP_SLOT that
 * would crash during self-relocation.
 */
int *__get_errno_ptr(void)
{
    return &ldso_errno;
}

/* __tls: RTMux TLS accessor. During ldso bootstrap, no real TLS is available.
 * Provide a static dummy so libc objects (posix-id.c, syscalls.c) compiled into
 * ldso don't generate a JUMP_SLOT. Only pid/tid fields are meaningful at this
 * stage.
 */
static struct _thread_local_storage ldso_tls;

struct _thread_local_storage *__tls(void)
{
    return &ldso_tls;
}

/* ELF TLS TCB pointer for ldso.
 *
 * FreeBSD's ldso uses _tcb_set()/_tcb_get() which on RISC-V would normally
 * write to the tp (thread pointer) register. RTMux uses tp for the
 * kernel-provided libc TLS block (_thread_local_storage). We must NOT let ldso
 * overwrite it.
 *
 * Instead, machine/tls.h's _tcb_set()/_tcb_get() store the ELF TCB pointer
 * here. ldso's own __tls_get_addr() uses this pointer correctly, and the
 * kernel's tp value is preserved for the user process.
 */
struct tcb *_ldso_tcb_ptr;

/* abort (used by assert in debug builds) */

void abort(void)
{
    /* NYI: raise SIGABRT via signal_kill, then trap */
    __builtin_trap();
}

/* __stackavail (TLS not available during ldso bootstrap) */

/* __stackavail: used by alloca() to check remaining stack space. The real
 * implementation reads TLS (__stackaddr), but TLS is not set up during ldso
 * bootstrap.
 *
 * Return a generous estimate.
 */
size_t __stackavail(void)
{
    return 65536;
}

/* fstat */

/* dynlink.c uses plain POSIX fstat(). Forward to the real IPC implementation in
 * fd.c.
 */
int fstat(int fd, struct stat *sb)
{
    return rtmux_ldso_fstat(fd, sb);
}

char *realpath(const char *restrict path, char *restrict resolved)
{
    ldso_trace("ldso_stub: realpath\n");
    /* Identity stub: copy path unchanged */
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    size_t len = 0;
    while (path[len] != '\0')
        len++;
    if (len >= 1024) { /* PATH_MAX */
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved, path, len + 1);
    return resolved;
}

char *getcwd(char *buf, size_t size)
{
    ldso_trace("ldso_stub: getcwd\n");
    if (!buf || size < 2) {
        errno = ERANGE;
        return NULL;
    }
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}
