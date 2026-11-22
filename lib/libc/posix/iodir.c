/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* opendir/readdir/closedir — directory I/O via message-passing. */

#include <stddef.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/iomsg.h>
#include <devctl/common.h>
#include <align.h>

#include "connect.h"

#define BUF_SIZE 4096

#define DIR_FLAG_FILTER 0x00000001U

struct _fd_block {
    int count, index;
    int *fds;
};

#define DATA_COUNT 50
typedef struct _cursor {
    struct _cursor *next;
    unsigned count;
    char *data[DATA_COUNT];
} cursor_t;

struct _dir {
    struct _fd_block dd_fd_block;
    struct _cursor *dd_cursor;
    unsigned dd_flags;
    int dd_pos;
    int dd_loc;
    int dd_size;
    char *dd_buf;
    char *dd_path;
};

static int compare(const void *p1, const void *p2)
{
    const char *p1c = (const char *) p1;
    const char **p2c = (const char **) p2;
    return strcmp(p1c, *p2c);
}

static int find_with_insert(cursor_t **chead, char *name)
{
    cursor_t *ctarget;
    unsigned indx;

    while ((ctarget = *chead)) {
        if (bsearch(name, ctarget->data, ctarget->count, sizeof(char *),
                    compare))
            return 1;
        if (ctarget->count < DATA_COUNT)
            break;
        chead = &ctarget->next;
    }

    /* Allocate before publishing a node or shifting its sorted entries. */
    char *copy = strdup(name);
    if (!copy)
        return -1;
    if (!ctarget) {
        ctarget = calloc(1, sizeof(*ctarget));
        if (!ctarget) {
            free(copy);
            errno = ENOMEM;
            return -1;
        }
        *chead = ctarget;
    }
    for (indx = 0; indx < ctarget->count; indx++) {
        if (strcmp(name, ctarget->data[indx]) < 0)
            break;
    }
    memmove(&ctarget->data[indx + 1], &ctarget->data[indx],
            (ctarget->count - indx) * sizeof(char *));
    ctarget->data[indx] = copy;
    ctarget->count++;
    return 0;
}

static void free_cursor(cursor_t **chead)
{
    cursor_t *cursor;
    unsigned i;

    for (cursor = *chead; cursor; cursor = *chead) {
        *chead = cursor->next;
        for (i = 0; i < cursor->count; i++)
            free(cursor->data[i]);
        free(cursor);
    }
}


DIR *opendir(const char *path)
{
    DIR *dirp;
    struct fcntl_stat {
        struct {
            struct _io_devctl i;
            int flags;
        } devctl;
        struct _io_stat stat;
    } msg;
    struct stat buff;

    if (!(dirp = malloc(sizeof *dirp + 8 + BUF_SIZE))) {
        errno = ENOMEM;
        return 0;
    }
    memset(dirp, 0, sizeof(*dirp) + BUF_SIZE);
    dirp->dd_flags = DIR_FLAG_FILTER;
    dirp->dd_buf = (char *) lib_align_up((uintptr_t) (dirp + 1), 8);

    dirp->dd_fd_block.fds = NULL;
    dirp->dd_fd_block.index = 0;
    dirp->dd_fd_block.count = 0;

    msg.devctl.i.type = _IO_DEVCTL;
    msg.devctl.i.combine_len =
        offsetof(struct fcntl_stat, stat) | _IO_COMBINE_FLAG;
    msg.devctl.i.devctl_cmd = DEVCTL_SET_FLAGS;
    msg.devctl.i.nbytes = sizeof msg.devctl.flags;
    msg.devctl.i.zero = 0;
    msg.devctl.flags = O_CLOEXEC | O_NOCTTY;
    msg.stat.type = _IO_STAT;
    msg.stat.combine_len = sizeof msg.stat;
    msg.stat.zero = 0;

    if (_connect_fd(0, path, 0, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY, 0,
                    _IO_CONNECT_COMBINE, 1, _IO_FLAG_RD, 0, 0, sizeof msg, &msg,
                    sizeof buff, &buff, 0, &dirp->dd_fd_block.count,
                    &dirp->dd_fd_block.fds) == -1) {
        goto bad_alloc;
    }

    if (!S_ISDIR(buff.st_mode)) {
        dirp->dd_fd_block.count--;
        while (dirp->dd_fd_block.fds && dirp->dd_fd_block.count >= 0) {
            close(dirp->dd_fd_block.fds[dirp->dd_fd_block.count--]);
        }
        errno = ENOTDIR;
        goto bad_alloc;
    }

    dirp->dd_size = dirp->dd_loc = 0;
    dirp->dd_fd_block.index = 0;

    if (dirp->dd_fd_block.count <= 1)
        dirp->dd_flags &= ~DIR_FLAG_FILTER;

    return dirp;

bad_alloc:
    if (dirp) {
        if (dirp->dd_fd_block.fds)
            free(dirp->dd_fd_block.fds);
        free(dirp);
    }
    return 0;
}

struct dirent *readdir(DIR *dirp)
{
    const size_t hdr = offsetof(struct dirent, d_name);

    for (;;) {
        while (dirp->dd_loc == dirp->dd_size) {
            if (dirp->dd_fd_block.index >= dirp->dd_fd_block.count)
                return NULL;
            ssize_t n = _readx(dirp->dd_fd_block.fds[dirp->dd_fd_block.index],
                               dirp->dd_buf, BUF_SIZE, 0, "", 0);
            if (n < 0)
                return NULL;
            if (n > BUF_SIZE) {
                errno = EOVERFLOW;
                return NULL;
            }
            dirp->dd_loc = 0;
            dirp->dd_size = (int) n;
            if (n == 0)
                ++dirp->dd_fd_block.index;
        }

        size_t left = (size_t) (dirp->dd_size - dirp->dd_loc);
        struct dirent *d = (void *) (dirp->dd_buf + dirp->dd_loc);
        if (left <= hdr || d->d_reclen <= hdr || d->d_reclen > left ||
            (d->d_reclen < left && d->d_reclen % _Alignof(struct dirent)))
            goto malformed;

        char *nul = memchr(d->d_name, '\0', d->d_reclen - hdr);
        if (!nul)
            goto malformed;
        size_t len = (size_t) (nul - d->d_name);
        /* Older procfs mount entries include the NUL in d_namelen. */
        if (d->d_namelen != len && d->d_namelen != len + 1)
            goto malformed;
        d->d_namelen = (uint16_t) len;
        int duplicate = 0;
        if (dirp->dd_flags & DIR_FLAG_FILTER) {
            duplicate = find_with_insert(&dirp->dd_cursor, d->d_name);
            if (duplicate < 0)
                return NULL;
        }
        dirp->dd_loc += d->d_reclen;
        if (!duplicate) {
            dirp->dd_pos++;
            return d;
        }
    }

malformed:
    dirp->dd_loc = dirp->dd_size;
    errno = EIO;
    return NULL;
}

/* readdir_r -- reentrant readdir: copy the next entry into the caller-supplied
 * buffer instead of leaving it in the DIR's own buffer. readdir() already
 * returns a pointer into the per-DIR buffer (no shared static state), so this
 * only adds the copy the POSIX interface requires. RTMux's on-wire dirent can
 * carry trailing dirent_extra records beyond the name; those are RTMux-specific
 * and not part of the POSIX entry, so only the header plus the NUL-terminated
 * name are copied and d_reclen is set to that name-only length -- which fits a
 * POSIX-sized (offsetof(d_name) + NAME_MAX + 1) caller buffer. As with
 * readdir(), concurrent calls on the SAME DIR still need external
 * serialization; different DIRs are independent.
 */
int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result)
{
    struct dirent *d;
    size_t hdr, namelen;

    errno = 0;
    d = readdir(dirp);
    if (!d) {
        *result = NULL;
        return errno; /* 0 at end-of-directory, else the readdir() errno */
    }

    hdr = offsetof(struct dirent, d_name);
    namelen = d->d_namelen;

    /* readdir checked the wire record. The caller's buffer holds NAME_MAX. */
    if (namelen > NAME_MAX) {
        *result = NULL;
        errno = EOVERFLOW;
        return EOVERFLOW;
    }
    memcpy(entry, d, hdr + namelen);
    entry->d_name[namelen] = '\0';
    entry->d_reclen = (uint16_t) (hdr + namelen + 1);
    *result = entry;
    return 0;
}

int closedir(DIR *dirp)
{
    int ret = 0;

    while (--dirp->dd_fd_block.count >= 0)
        ret |= close(dirp->dd_fd_block.fds[dirp->dd_fd_block.count]);

    if (dirp->dd_fd_block.fds)
        free(dirp->dd_fd_block.fds);
    if (dirp->dd_cursor)
        free_cursor(&dirp->dd_cursor);
    free(dirp);
    return ret;
}

void rewinddir(DIR *dirp)
{
    int i;

    for (i = 0; i < dirp->dd_fd_block.count; i++)
        lseek(dirp->dd_fd_block.fds[i], 0, SEEK_SET);
    dirp->dd_fd_block.index = 0;

    if (dirp->dd_cursor)
        free_cursor(&dirp->dd_cursor);

    dirp->dd_loc = 0;
    dirp->dd_pos = 0;
    dirp->dd_size = 0;
}
