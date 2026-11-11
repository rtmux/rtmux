/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Descriptor -> absolute-path registry backing fd-relative *at resolution.
 *
 * RTMux resolves paths textually through the roottask path server; there is no
 * resolve-relative-to-a-connection support, so openat() and its siblings cannot
 * resolve a relative path against a real dirfd on their own. This registry
 * remembers the absolute path a descriptor was opened with, and the *at
 * wrappers join it with the relative component and let the path server resolve
 * the result (which handles ., .., and symlink components).
 *
 * A descriptor opened with an ABSOLUTE path is tracked with that (already
 * canonical) path; one opened with a RELATIVE path is tracked with the path
 * resolved against a cached cwd (kept in sync by chdir()). A descriptor this
 * libc never saw opened (e.g. an inherited fd) is lazily resolved through
 * _IO_FDINFO if its service can report an absolute path; otherwise the *at
 * wrappers fall back to the EBADF/ENOTDIR/ENOSYS error precedence for it. The
 * tracking is path- not handle-based: a cwd cache miss/stale entry or a
 * directory replaced after the open can misroute a relative *at -- both moot on
 * RTMux's static namespace (no rename on the flat /dev/shmem writable FS).
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/iomsg.h>

#include "posix.h"

static char **atfd_paths;
static int atfd_cap;
static char atfd_cwd[PATH_MAX + 1];
static int atfd_cwd_valid;
static unsigned atfd_cwd_gen; /* bumped on every chdir()/chroot() */
static pthread_mutex_t atfd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Grow the table so index `fd` is valid.  Caller holds atfd_lock. */
static int atfd_reserve(int fd)
{
    int ncap, i;
    char **grown;

    if (fd < atfd_cap)
        return 0;
    ncap = atfd_cap ? atfd_cap * 2 : 16;
    while (ncap <= fd)
        ncap *= 2;
    grown = realloc(atfd_paths, (size_t) ncap * sizeof *grown);
    if (!grown)
        return -1;
    for (i = atfd_cap; i < ncap; i++)
        grown[i] = NULL;
    atfd_paths = grown;
    atfd_cap = ncap;
    return 0;
}

/* Remember `abspath` (must be absolute) for `fd`. Best-effort: on allocation
 * failure the descriptor is simply left untracked.
 */
void __atfd_track(int fd, const char *abspath)
{
    char *copy;

    if (fd < 0 || !abspath || abspath[0] != '/')
        return;
    copy = strdup(abspath);
    if (!copy)
        return;
    pthread_mutex_lock(&atfd_lock);
    if (atfd_reserve(fd) == 0) {
        free(atfd_paths[fd]);
        atfd_paths[fd] = copy;
        copy = NULL;
    }
    pthread_mutex_unlock(&atfd_lock);
    free(copy);
}

/* Drop any path remembered for `fd` (called when it is closed). */
void __atfd_forget(int fd)
{
    if (fd < 0)
        return;
    pthread_mutex_lock(&atfd_lock);
    if (fd < atfd_cap) {
        free(atfd_paths[fd]);
        atfd_paths[fd] = NULL;
    }
    pthread_mutex_unlock(&atfd_lock);
}

/* Copy oldfd's path (if any) onto newfd (called on dup/dup2/F_DUPFD). */
void __atfd_dup(int oldfd, int newfd)
{
    char *copy = NULL;

    if (oldfd < 0 || newfd < 0)
        return;
    pthread_mutex_lock(&atfd_lock);
    if (oldfd < atfd_cap && atfd_paths[oldfd])
        copy = strdup(atfd_paths[oldfd]);
    if (copy && atfd_reserve(newfd) == 0) {
        free(atfd_paths[newfd]);
        atfd_paths[newfd] = copy;
        copy = NULL;
    }
    pthread_mutex_unlock(&atfd_lock);
    free(copy);
}

/* Record the process working directory (absolute) so a subsequent relative open
 * can be tracked without a fresh getcwd(). Called by chdir() on success, and
 * invalidated by chroot(). This cache backs only *at path tracking -- getcwd()
 * itself does not read it, so a stale value can never corrupt getcwd; at worst
 * it misroutes a relative *at on a relative-opened dirfd.
 */
void __atfd_chdir(const char *abspath)
{
    size_t len;

    if (!abspath || abspath[0] != '/')
        return;
    len = strlen(abspath);
    if (len >= sizeof atfd_cwd)
        return;
    pthread_mutex_lock(&atfd_lock);
    memcpy(atfd_cwd, abspath, len + 1);
    atfd_cwd_valid = 1;
    atfd_cwd_gen++;
    pthread_mutex_unlock(&atfd_lock);
}

/* Reset *at tracking after a successful chroot(): the process root moved, so
 * every cached chroot-relative path -- the cwd cache and every tracked
 * descriptor -- would be reinterpreted under the new root. Drop them all; the
 * cwd re-seeds on the next relative open and pre-chroot dirfds fail *at with
 * ENOSYS rather than resolving to the wrong place.
 */
void __atfd_chroot(void)
{
    int i;

    pthread_mutex_lock(&atfd_lock);
    atfd_cwd_valid = 0;
    atfd_cwd_gen++;
    for (i = 0; i < atfd_cap; i++) {
        free(atfd_paths[i]);
        atfd_paths[i] = NULL;
    }
    pthread_mutex_unlock(&atfd_lock);
}

/* Copy the cached cwd into `buf`, initializing it from getcwd() on first use.
 * Returns 0 on success, -1 if the cwd is unavailable.
 */
static int atfd_cwd_copy(char *buf, size_t sz)
{
    size_t len;
    unsigned gen;
    int have = 0;

    pthread_mutex_lock(&atfd_lock);
    if (atfd_cwd_valid) {
        len = strlen(atfd_cwd);
        if (len < sz) {
            memcpy(buf, atfd_cwd, len + 1);
            have = 1;
        }
    }
    gen = atfd_cwd_gen;
    pthread_mutex_unlock(&atfd_lock);
    if (have)
        return 0;
    if (!getcwd(buf, sz))
        return -1;

    /* Seed the cache only if it is still cold AND no chdir()/chroot() happened
     * while we queried getcwd (a generation change means our result may predate
     * it). Otherwise leave the descriptor untracked rather than record a
     * possibly-wrong path -- an untracked dirfd fails *at with ENOSYS, not a
     * wrong resolution.
     */
    pthread_mutex_lock(&atfd_lock);
    if (atfd_cwd_valid || atfd_cwd_gen != gen) {
        pthread_mutex_unlock(&atfd_lock);
        return -1;
    }
    len = strlen(buf);
    if (len < sizeof atfd_cwd) {
        memcpy(atfd_cwd, buf, len + 1);
        atfd_cwd_valid = 1;
    }
    pthread_mutex_unlock(&atfd_lock);
    return 0;
}

static int atfd_fdinfo_path(int fd, char *buf, size_t sz)
{
    struct {
        struct _io_fdinfo_reply o;
        char path[PATH_MAX + 1];
    } reply;
    io_fdinfo_t msg;
    int ret;

    if (sz > sizeof reply.path)
        sz = sizeof reply.path;
    memset(&msg.i, 0, sizeof msg.i);
    msg.i.type = _IO_FDINFO;
    msg.i.combine_len = sizeof msg.i;
    msg.i.path_len = (int32_t) sz;
    ret = msg_sendnc(fd, &msg.i, sizeof msg.i, &reply, sizeof reply);
    if (ret == -1)
        return -1;
    if (ret <= 0 || (size_t) ret > sz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!memchr(reply.path, '\0', (size_t) ret)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (reply.path[0] != '/') {
        errno = ENOSYS;
        return -1;
    }
    memcpy(buf, reply.path, strlen(reply.path) + 1);
    return 0;
}

/* Track a just-opened descriptor by the absolute form of its open path so it
 * can serve as an *at dirfd. An absolute path is already canonical; a relative
 * path is resolved against the cwd. Best-effort: an unavailable cwd or an
 * overlong join simply leaves the descriptor untracked.
 */
void __atfd_track_open(int fd, const char *path)
{
    char abs[PATH_MAX + 1];
    size_t clen, plen;
    int saved_errno = errno;

    if (fd < 0 || !path || !path[0])
        return;
    if (path[0] == '/') {
        __atfd_track(fd, path);
        errno = saved_errno;
        return;
    }
    if (atfd_cwd_copy(abs, sizeof abs) != 0) {
        errno = saved_errno;
        return;
    }
    clen = strlen(abs);
    plen = strlen(path);
    if (plen >= sizeof abs - clen - 1) {
        errno = saved_errno;
        return;
    }
    abs[clen] = '/';
    memcpy(abs + clen + 1, path, plen + 1);
    __atfd_track(fd, abs);
    errno = saved_errno;
}

/* Build the absolute path for `path` interpreted relative to the directory
 * `dirfd` was opened as. Validates the descriptor with POSIX error precedence
 * first -- EBADF for an invalid descriptor, ENOTDIR for a non-directory -- then
 * requires a tracked absolute path (ENOSYS for a valid directory this libc
 * never saw opened with one, e.g. an inherited or relative-opened dirfd).
 * Returns 0 on success, or -1 with errno set (also ENAMETOOLONG if the join
 * overflows `out`).
 */
int __atfd_abspath(int dirfd, const char *path, char *out, size_t outsz)
{
    struct stat st;
    char base[PATH_MAX + 1];
    size_t blen = 0, plen;
    int found = 0;

    if (fstat(dirfd, &st) < 0)
        return -1; /* EBADF (or other fstat error) */
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    pthread_mutex_lock(&atfd_lock);
    if (dirfd >= 0 && dirfd < atfd_cap && atfd_paths[dirfd]) {
        blen = strlen(atfd_paths[dirfd]);
        if (blen < sizeof base) {
            memcpy(base, atfd_paths[dirfd], blen + 1);
            found = 1;
        }
    }
    pthread_mutex_unlock(&atfd_lock);

    if (!found) {
        if (atfd_fdinfo_path(dirfd, base, sizeof base) != 0)
            return -1;
        blen = strlen(base);
        __atfd_track(dirfd, base);
    }
    while (blen > 1 && base[blen - 1] == '/')
        base[--blen] = '\0';

    plen = strlen(path);
    if (blen >= outsz || plen >= outsz - blen - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, base, blen);
    out[blen] = '/';
    memcpy(out + blen + 1, path, plen + 1);
    return 0;
}
