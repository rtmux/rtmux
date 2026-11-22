/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOGF_FLAG_OSYNC 0x00000001

struct waiting {
    struct waiting *next;
    int rcvid;
    int priority;
};

struct ocbs {
    struct ocbs *next;
    struct slog_ocb *ocb;
};

struct slogdev {
    struct ocbs *ocbs;
    struct waiting *waiting;
    ioserv_notify_t notify[IOSERV_NOTIFY_COUNT];
    unsigned cnt; /* Number of ints starting at get */
    int *put;     /* Points into buf for putting data */
    int *get;     /* Points into buf for getting data */
    int *beg;     /* Pointer to begining of buf */
    int *end;     /* Pointer to end of buf + 1 */
    struct {
        dev_t device;
        ino_t inode;
    } identity[2]; /* slog, console */
    int id;       /* Contains id of /dev/slog */
};

struct slog_ocb {
    struct slogdev *dev;
    unsigned ioflag;
    off_t offset;
};

/* The offset in a ocb is 64 bits. We remap it as follows. The offset is on a
 * per open basis. A dup() will share the existing offset for the dupped fd.
 */
struct ocbget {
    int *get;
};

#define OCBGET(ocb) ((struct ocbget *) &ocb->offset)->get

static inline int slog_is_nonblock(int ioflag, int xtype)
{
    if (ioflag & O_NONBLOCK) {
        if (xtype & _IO_XFLAG_BLOCK)
            ioflag &= ~O_NONBLOCK;
    } else if (xtype & _IO_XFLAG_NONBLOCK) {
        ioflag |= O_NONBLOCK;
    }
    return ioflag & O_NONBLOCK;
}
