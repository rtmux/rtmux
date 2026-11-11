/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* uname() -- identify the running system.
 *
 * release and version come from the kernel's syspage build_info record, so
 * they describe the kernel actually booted rather than the libc that was
 * linked against it.
 */

#include <sys/utsname.h>
#include <sys/syspage.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

extern struct syspage_entry *_syspage_ptr;

/* Copy src into a zeroed dst of size cap, always leaving a terminating NUL. */
static void uname_copy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);

    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Read the first line of /etc/hostname into dst; leave dst alone on failure. */
static void uname_hostname(char *dst, size_t cap)
{
    char buf[64];
    ssize_t n;
    size_t i;
    int fd = open("/etc/hostname", O_RDONLY);

    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    for (i = 0; i < (size_t) n && buf[i] != '\n' && buf[i] != '\r' &&
                buf[i] != '\0';
         i++)
        ;
    if (i == 0 || i >= cap)
        return;
    memcpy(dst, buf, i);
    dst[i] = '\0';
}

int uname(struct utsname *u)
{
    const struct build_info_entry *bi;

    if (!u) {
        errno = EFAULT;
        return -1;
    }

    memset(u, 0, sizeof(*u));
    uname_copy(u->sysname, "RTMux", sizeof(u->sysname));
    uname_copy(u->nodename, "rtmux", sizeof(u->nodename));
    uname_copy(u->machine, "riscv64", sizeof(u->machine));

    bi = syspage_build_info_of(_syspage_ptr);
    if (bi) {
        /* The record is not trusted to terminate its strings. */
        memcpy(u->release, bi->release, sizeof(u->release) - 1);
        memcpy(u->version, bi->build_id, sizeof(bi->build_id));
        u->release[sizeof(u->release) - 1] = '\0';
        u->version[sizeof(u->version) - 1] = '\0';
    } else {
        uname_copy(u->release, "unknown", sizeof(u->release));
        uname_copy(u->version, "unknown", sizeof(u->version));
    }

    uname_hostname(u->nodename, sizeof(u->nodename));
    return 0;
}
