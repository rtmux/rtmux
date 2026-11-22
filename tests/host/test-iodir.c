/* SPDX-License-Identifier: Apache-2.0 */

/* Two directory servers at one path, with scripted _IO_READ replies.
 * Run the public DIR operations with the target ABI and allocator. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mallocstats.h>
#include <unistd.h>
#include "connect.h"
#include "malloc-core.h"

static int failure;
static ssize_t reply_size;
static _Alignas(struct dirent) unsigned char reply[4096];

int *__get_errno_ptr(void)
{
    static int err;
    return &err;
}

int _connect_fd(int base,
                const char *path,
                mode_t mode,
                unsigned oflag,
                unsigned sflag,
                unsigned subtype,
                int cancel,
                unsigned access,
                unsigned type,
                unsigned xtype,
                unsigned xlen,
                const void *extra,
                unsigned len,
                void *response,
                int *status,
                int *count,
                void *fds)
{
    int *v = __crt_malloc(2 * sizeof(*v));
    assert(v && len == sizeof(struct stat));
    (void) base;
    (void) path;
    (void) mode;
    (void) oflag;
    (void) sflag;
    (void) subtype;
    (void) cancel;
    (void) access;
    (void) type;
    (void) xtype;
    (void) xlen;
    (void) extra;
    (void) status;
    ((struct stat *) response)->st_mode = S_IFDIR;
    *count = 2;
    *(int **) fds = v;
    v[0] = open("/dev/null", O_RDONLY);
    v[1] = open("/dev/null", O_RDONLY);
    assert(v[0] >= 0 && v[1] >= 0);
    return v[0];
}

ssize_t _readx(int fd,
               void *dst,
               size_t cap,
               unsigned xtype,
               void *x,
               size_t len)
{
    ssize_t n = reply_size;
    (void) fd;
    (void) xtype;
    (void) x;
    (void) len;
    if (n > 0 && (size_t) n <= cap)
        memcpy(dst, reply, (size_t) n);
    reply_size = 0;
    return n;
}

char *dir_test_strdup(const char *name)
{
    size_t n = strlen(name) + 1;
    char *p = __crt_malloc(failure ? SIZE_MAX : n);
    if (p)
        memcpy(p, name, n);
    else
        errno = ENOMEM;
    return p;
}

static struct dirent *record(const char *name)
{
    struct dirent *d = (void *) reply;
    memset(reply, 0, sizeof reply);
    d->d_namelen = strlen(name);
    d->d_reclen = 32;
    strcpy(d->d_name, name);
    reply_size = d->d_reclen;
    return d;
}

int main(void)
{
    struct __rtmux_mallocstats before, after;
    __crt_mallocstats(&before);
    DIR *dir = opendir("/fixture");
    assert(dir);

    for (unsigned i = 0; i < 6; i++) {
        struct dirent *d = record("abc");
        switch (i) {
        case 0:
            d->d_reclen = 0;
            break;
        case 1:
            d->d_reclen = UINT16_MAX;
            break;
        case 2:
            reply_size = offsetof(struct dirent, d_name) - 1;
            break;
        case 3:
            memset(d->d_name, 'x', 32 - offsetof(struct dirent, d_name));
            break;
        case 4:
            d->d_reclen = 31;
            break; /* unaligned next header */
        case 5:
            reply_size = sizeof(reply) + 1;
            break;
        }
        errno = 0;
        assert(!readdir(dir) && (errno == EIO || errno == EOVERFLOW));
    }

    _Alignas(struct dirent) char entry[64];
    struct dirent *result;
    record("abc")->d_reclen = 0;
    assert(readdir_r(dir, (void *) entry, &result) == EIO && !result);
    record("old")->d_namelen++;
    assert(readdir_r(dir, (void *) entry, &result) == 0 && result);
    assert(result->d_namelen == 3 && !strcmp(result->d_name, "old"));
    rewinddir(dir);

    /* Fail both the first insertion and insertion before a live entry.
     * Retrying must yield the same name and preserve duplicate suppression. */
    const char *names[] = {"z", "a"};
    for (unsigned i = 0; i < 2; ++i) {
        record(names[i]);
        failure = 1;
        assert(!readdir(dir) && errno == ENOMEM);
        failure = 0;
        struct dirent *d = readdir(dir);
        assert(d && !strcmp(d->d_name, names[i]));
    }
    record("z");
    errno = 0;
    assert(!readdir(dir) && errno == 0);
    assert(closedir(dir) == 0);
    __crt_mallocstats(&after);
    assert(after.failed == before.failed + 2);
    assert(after.bytes_in_use == before.bytes_in_use);
    assert(after.allocs - before.allocs == after.frees - before.frees);
    return 0;
}
