/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* iomsg.h Non-portable low-level IO definitions */
#ifndef __IOMSG_H_INCLUDED
#define __IOMSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _RTMUX_CORE_H
#include <sys/rtmux-core.h>
#endif

#ifndef _STDINT_H_INCLUDED
#include <stdint.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef __STAT_H_INCLUDED
#include <sys/stat.h>
#endif

#ifndef __STATVFS_H_INCLUDED
#include <sys/statvfs.h>
#endif

#ifndef _UTIME_H_INCLUDED
#include <utime.h>
#endif

#ifndef __FTYPE_H_INCLUDED
#include <sys/ftype.h>
#endif

#ifndef _LIMITS_H_INCLUDED
#include <limits.h>
#endif

enum _iomsg_protocols {
    _IOMSG_PCI = 0x0d,          /* PCI server private protocol */
    _IOMSG_RTMUX_NATIVE = 0x17, /* RTMux native file-message subset */
    _IOMSG_VIRTIO_MMIO = 0x18,  /* virtio-mmio delegation service */
};

/* flags definitions */
#define _NOTIFY_COND_EXTEN 0x80000000

/* Same as RDBAND, WRNORM, RDNORM below respectively */
#define _NOTIFY_COND_OUTPUT 0x20000000 /* Room for more output */
#define _NOTIFY_COND_INPUT 0x10000000  /* Data is available */

/* Masks for (io_notify_t).i.event.sigev_value.
 *
 * Requested conditions, extended or non, set in (io_notify_t).i.flags. Returned
 * conditions, extended or non, returned at poll in (io_notify_t).o.flags. If
 * extended conditions are requested, and they need to be returned in an armed
 * event, the negative of the satisfied conditions are returned in
 * (io_notify_t).i.event.sigev_code.
 */
#define _NOTIFY_COND_MASK 0xf0000000 /* Mask for conditions */
#define _NOTIFY_DATA_MASK (~_NOTIFY_COND_MASK)

/* If _NOTIFY_COND_EXTEN set See POLL* flags in <sys/poll.h> */
#define _NOTIFY_CONDE_RDNORM 0x00000001 /* Normal data is available */
/* endif */

/* If returning an armed event, the negative of the satisfied conditions is
 * returned in a short. Therefor at max 15 conditions if extended versions are
 * requested.
 */
#define _NOTIFY_MAXCOND 15

#define _NOTIFY_FLAGS_EXTEN_FIRST 0x00000001

/* action definitions 00 Always arm for data transition. [EOK, EBUSY] 01 Arm if
 * no data. Useful in that it may be combined with a read. [EOK, EAGAIN, EBUSY]
 * 10 Poll and disarm. [EOK:bits, EBUSY] 11 Poll and arm if no data. Useful for
 * select(). [EOK:bits, EBUSY]
 *
 * For actions which arm an event it will be disarmed when triggered.
 *
 * POLL - Never arm. Never trig.
 *
 * Return cond.
 *
 * POLLARM - Arm if cond not met. Never trig immed.
 *
 * Return cond.
 *
 * TRANARM - Always arm. Never trig immed. Trig only when new data
 *              arrives in an empty buffer/queue. Return 0.
 *
 * CONDARM - Arm if cond not met and return -1, errno = EAGAIN. If cond
 *              met return 0.
 */
#define _NOTIFY_ACTION_TRANARM 0x0
#define _NOTIFY_ACTION_CONDARM 0x1
#define _NOTIFY_ACTION_POLL 0x2

struct _fdinfo {
    uint32_t mode;   /* File mode */
    uint32_t ioflag; /* Current io flags */
    uint64_t offset; /* Current seek position */
    uint64_t size;   /* Current size of file */
    uint32_t flags;  /* _FDINFO_* */
    uint16_t sflag;  /* reserved */
    uint16_t count;  /* File use count */
    uint16_t rcount; /* File reader count */
    uint16_t wcount; /* File writer count */
    uint16_t rlocks; /* reserved */
    uint16_t wlocks; /* reserved */
    uint32_t zero[6];
};

__BEGIN_DECLS

/* Message types */
enum _msg_bases { _IO_BASE = 0x100, _IO_MAX = 0x1FF };

enum _io__Uint16types {
    _IO_CONNECT = _IO_BASE,
    _IO_READ,
    _IO_WRITE,
    _IO_RSVD_CLOSE_OCB, /* Place holder in jump table */
    _IO_STAT,
    _IO_NOTIFY,
    _IO_DEVCTL,
    _IO_RSVD_UNBLOCK, /* Place holder in jump table */
    _IO_PATHCONF,
    _IO_LSEEK,
    _IO_CHMOD,
    _IO_CHOWN,
    _IO_UTIME,
    _IO_OPENFD,
    _IO_FDINFO,
    _IO_LOCK,
    _IO_SPACE,
    _IO_RSVD_SHUTDOWN,
    _IO_MMAP,
    _IO_MSG,
    _IO_RSVD_UNUSED,
    _IO_DUP,
    _IO_CLOSE,
    _IO_RSVD_LOCK_OCB,   /* Place holder in jump table */
    _IO_RSVD_UNLOCK_OCB, /* Place holder in jump table */
    _IO_SYNC
};


enum _io_msg_xtypes {
    _IO_XTYPE_NONE = 0,
    _IO_XTYPE_OFFSET = 5,
    _IO_XTYPE_MASK = 0x000000ff,

    /* The _IO_XFLAG_DIR_EXTRA_HINT flag is only valid when reading from a
     * directory. The filesystem should normally return extra directory
     * information when it is easy to get. If this flag is set, it is a hint to
     * the filesystem to try harder (possibly causing media lookups) to return
     * the extra information. The most common use would be to return
     * _DTYPE_LSTAT information.
     */
    _IO_XFLAG_DIR_EXTRA_HINT = 0x00000100,

    _IO_XFLAG_NONBLOCK = 0x00004000,
    _IO_XFLAG_BLOCK = 0x00008000
    /* Upper 16 bits are for use by the specific xtype */
};

struct _xtype_offset {
    off64_t offset;
};

/** IO message types which are pathname based * */

/* Message of _IO_CONNECT */
struct _io_connect {
    uint16_t type;
    uint16_t subtype;   /* _IO_CONNECT_? */
    uint32_t file_type; /* _FTYPE_? in sys/ftype.h */
    uint16_t reply_max;
    uint16_t entry_max;
    uint32_t key;
    uint32_t handle;
    uint32_t ioflag; /* O_? in fcntl.h & _IO_FLAG_? */
    uint32_t mode;   /* S_IF? in sys/stat.h */
    uint16_t sflag;  /* reserved */
    uint16_t access; /* S_I in sys/stat.h */
    uint32_t path_len;
    uint8_t eflag;      /* _IO_CONNECT_EFLAG_? */
    uint8_t extra_type; /* _IO_CONNECT_EXTRA_? */
    uint16_t extra_len;
    char path[1]; /* path_len, null, extra_len */
};

/* struct _io_connect subtype */
enum _io_connect_subtypes {
    _IO_CONNECT_COMBINE,       /* Combine with IO msg */
    _IO_CONNECT_COMBINE_CLOSE, /* Combine with IO msg and always close */
    _IO_CONNECT_OPEN,
    _IO_CONNECT_UNLINK,
    _IO_CONNECT_READLINK,
    _IO_CONNECT_LINK,
    _IO_CONNECT_RSVD_UNBLOCK /* Place holder in jump table */
};

/* struct _io_connect extra_type */
enum _io_connect_extra_type {
    _IO_CONNECT_EXTRA_NONE = 0,
    _IO_CONNECT_EXTRA_SEM = 6,
    _IO_CONNECT_EXTRA_SERVICE_LINK = 7,
    _IO_CONNECT_EXTRA_PROC_SYMLINK = 8,
    _IO_CONNECT_EXTRA_TYMEM = 12
};

/* struct _io_connect ioflag (non-masked values match O_? in fcntl.h) */
#define _IO_FLAG_RD 0x00000001   /* read permission */
#define _IO_FLAG_WR 0x00000002   /* write permission */
#define _IO_FLAG_MASK 0x00000003 /* permission mask */

/* struct _io_connect eflag */
#define _IO_CONNECT_EFLAG_DIR 0x01    /* Path referenced a directory    */
#define _IO_CONNECT_EFLAG_DOT 0x02    /* Last component was . or ..     */
#define _IO_CONNECT_EFLAG_DOTDOT 0x04 /* Last component was ..          */

/* return status from connect (These are continuation cases that always negative
 * (RET_FLAG is set). To avoid conflicting io msgs must never return a negative
 * status
 */
#define _IO_CONNECT_RET_UMASK \
    0x00020000 /* umask field in link reply is valid */
#define _IO_CONNECT_RET_NOCTTY 0x00040000 /* No controling terminal defined */
#define _IO_CONNECT_RET_CHROOT \
    0x00080000 /* chroot_len field in link reply is valid */
#define _IO_CONNECT_RET_MSG \
    0x00100000 /* Connect to server and send new message */

#define _IO_CONNECT_RET_TYPE_MASK 0x0001e000 /* Mask for returned file type */
#define _IO_CONNECT_RET_FTYPE \
    0x00008000 /* File type was matched, _io_connect_ftype_reply expected */
#define _IO_CONNECT_RET_LINK 0x00010000 /* Not fully resolved, follow link */

#define _IO_CONNECT_RET_FLAG \
    0x80000000 /* Must be set to signify connect is returing */

#define _IO_STATUS(_c, _s) ((_c)->status = (int) (_s))
#define _IO_SET_CONNECT_RET(_c, _s) \
    _IO_STATUS(_c, _IO_CONNECT_RET_FLAG | (_s)) /* Sets connect return code */

/* _io_connect reply redirecting resolution to other entries */
struct _io_connect_link_reply {
    uint32_t reserved1;
    uint32_t file_type; /*_FTYPE_? in sys/ftype.h */
    uint8_t eflag;      /* _IO_CONNECT_EFLAG_? */
    uint8_t reserved2[1];
    uint16_t chroot_len; /* len of chroot in returned path */
    uint32_t umask;      /* S_IF? in sys/stat.h */
    uint16_t nentries;   /* If zero, path is a symbolic link */
    uint16_t path_len; /* len includs null, if zero, path is null terminated */
    /* struct _io_connect_entry server[nentries]; char path[path_len]; or struct
     * _server_info info; io_?_t msg;
     */
};

/* _io_connect reply indicating a change/reply of a certain ftype and errno */
struct _io_connect_ftype_reply {
    uint16_t status; /* Typically an errno */
    uint16_t reserved;
    uint32_t file_type; /* _FTYPE_? in sys/ftype.h */
};

/* used in _io_connect_link_reply */
struct _io_connect_entry {
    uint32_t nd;
    int32_t pid;
    int32_t chid;
    uint32_t handle;
    uint32_t key;
    uint32_t file_type;
    uint16_t prefix_len;
    uint16_t zero[3];
};

typedef union {
    struct _io_connect connect;
    struct _io_connect_link_reply link_reply;
    struct _io_connect_ftype_reply ftype_reply;
} io_open_t;


typedef union {
    struct _io_connect connect;
    struct _io_connect_link_reply link_reply;
    struct _io_connect_ftype_reply ftype_reply;
} io_unlink_t;


typedef union {
    struct _io_connect connect;
    struct _io_connect_link_reply link_reply;
    struct _io_connect_ftype_reply ftype_reply;
} io_readlink_t;


typedef union {
    struct _io_connect connect;
    struct _io_connect_link_reply link_reply;
    struct _io_connect_ftype_reply ftype_reply;
} io_link_t;

struct _ioserv_link_extra {
    uint32_t nd;
    int32_t pid;
    int32_t chid;
    uint32_t handle;
    uint32_t flags;
    uint32_t file_type;
    uint32_t reserved[2];
};

typedef union _io_link_extra {
    void *ocb;                             /* from I/O service functions */
    char path[1];                          /* symbolic link target */
    struct _ioserv_link_extra service; /* EXTRA_SERVICE_LINK */
} io_link_extra_t;

/** IO message types which are fd based * */

/* Common header for combining io messages */
struct _io_combine {
    uint16_t type;
    uint16_t combine_len;
};

#define _IO_COMBINE_FLAG 0x8000 /* Ored with combine len to cause combine */


/* Message of _IO_CLOSE */
struct _io_close {
    uint16_t type;
    uint16_t combine_len;
};

typedef union {
    struct _io_close i;
} io_close_t;

/* Message of _IO_WRITE */
struct _io_write {
    uint16_t type;
    uint16_t combine_len;
    int32_t nbytes;
    uint32_t xtype;
    uint32_t zero;
    /*	unsigned char				data[nbytes];	*/
};


typedef union {
    struct _io_write i;
    /*	nbytes is returned with msg_reply */
} io_write_t;

#define _IO_SET_WRITE_NBYTES(_c, _s) _IO_STATUS((_c), (_s))

/* Message of _IO_READ */
struct _io_read {
    uint16_t type;
    uint16_t combine_len;
    size_t nbytes;
    uint16_t xtype;
    uint16_t zero;
};

typedef union {
    struct _io_read i;
    /*	unsigned char				data[nbytes];	*/
    /*	nbytes is returned with msg_reply */
} io_read_t;

#define _IO_SET_READ_NBYTES(_c, _s) _IO_STATUS((_c), (_s))

/* Message of _IO_STAT */
struct _io_stat {
    uint16_t type;
    uint16_t combine_len;
    uint32_t zero;
};

typedef union {
    struct _io_stat i;
    struct stat o;
} io_stat_t;


/* Message of _IO_NOTIFY */
struct _io_notify {
    uint16_t type;
    uint16_t combine_len;
    int32_t action;
    uint32_t flags;
    struct sigevent event;

    /* Following fields only valid if (flags & _NOTIFY_COND_EXTEN) */
    int32_t service_data[2]; /* Service-private state */
    uint32_t flags_extra_mask;
    uint32_t flags_exten;
    int32_t nfds;
    int32_t fd_first;
    int32_t nfds_ready;
    int64_t timo;
    /*	struct pollfd				fds[nfds]; */
};

struct _io_notify_reply {
    uint32_t zero;
    uint32_t flags; /* action */

    /* Following fields only updated by services that opt in. */
    uint32_t flags2; /* flags above */
    struct sigevent event;
    int32_t service_data[2];
    uint32_t flags_extra_mask;
    uint32_t flags_exten;
    int32_t nfds;
    int32_t fd_first;
    int32_t nfds_ready;
    int64_t timo;
    /*	struct pollfd				fds[nfds]; */
};

typedef union {
    struct _io_notify i;
    struct _io_notify_reply o;
} io_notify_t;


/* Message of _IO_DEVCTL */
struct _io_devctl {
    uint16_t type;
    uint16_t combine_len;
    uint32_t devctl_cmd;
    int32_t nbytes;
    int32_t zero;
    /*	char						data[nbytes]; */
};

struct _io_devctl_reply {
    uint32_t zero;
    int32_t ret_val;
    int32_t nbytes;
    int32_t zero2;
    /*	char						data[nbytes]; */
};

typedef union {
    struct _io_devctl i;
    struct _io_devctl_reply o;
} io_devctl_t;


/* Message of _IO_PATHCONF */
struct _io_pathconf {
    uint16_t type;
    uint16_t combine_len;
    short name;
    uint16_t zero;
};

typedef union {
    struct _io_pathconf i;
    /*	value is returned with msg_reply */
} io_pathconf_t;

#define _IO_SET_PATHCONF_VALUE(_c, _s) _IO_STATUS((_c), (_s))


/* Message of _IO_LSEEK */
struct _io_lseek {
    uint16_t type;
    uint16_t combine_len;
    short whence;
    uint16_t zero;
    uint64_t offset;
};

typedef union {
    struct _io_lseek i;
    uint64_t o;
} io_lseek_t;


/* Message of _IO_CHMOD */
struct _io_chmod {
    uint16_t type;
    uint16_t combine_len;
    mode_t mode;
};

typedef union {
    struct _io_chmod i;
} io_chmod_t;


/* Message of _IO_CHOWN */
struct _io_chown {
    uint16_t type;
    uint16_t combine_len;
    int32_t gid;
    int32_t uid;
};

typedef union {
    struct _io_chown i;
} io_chown_t;


/* Message of _IO_UTIME */
struct _io_utime {
    uint16_t type;
    uint16_t combine_len;
    int32_t cur_flag; /*  If set, ignore times and set to "now"   */
    struct utimbuf times;
};

typedef union {
    struct _io_utime i;
} io_utime_t;


/* Message of _IO_OPENFD */

enum _io_openfd_xtypes { _IO_OPENFD_NONE, _IO_OPENFD_PIPE };

struct _io_openfd {
    uint16_t type;
    uint16_t combine_len;
    uint32_t ioflag; /* O_? in fcntl.h & _IO_FLAG_? */
    uint16_t sflag;  /* reserved */
    uint16_t xtype;
    struct _msg_info info;
    uint32_t reserved2;
    uint32_t key;
};

typedef union {
    struct _io_openfd i;
} io_openfd_t;


/* Message of _IO_FDINFO */
struct _io_fdinfo {
    uint16_t type;
    uint16_t combine_len;
    uint32_t flags;
    int32_t path_len;
    uint32_t reserved;
};

struct _io_fdinfo_reply {
    uint32_t zero[2];
    struct _fdinfo info;
    /* 	char						path[path_len + 1];	 */
};

typedef union {
    struct _io_fdinfo i;
    struct _io_fdinfo_reply o;
} io_fdinfo_t;

#define _IO_SET_FDINFO_LEN(_c, _s) _IO_STATUS((_c), (_s))

/* Message of _IO_LOCK */
struct _io_lock {
    uint16_t type;
    uint16_t combine_len;
    uint32_t subtype;
    int32_t nbytes;
    /*	char        				data[1];	*/ /* for F_*LK this will be
                                                      flock_t
                                                      */
};

struct _io_lock_reply {
    uint32_t zero[3];
    /*	char        				data[1];	*/ /* for F_*LK this will be
                                                      flock_t
                                                      */
};

typedef union {
    struct _io_lock i;
    struct _io_lock_reply o;
} io_lock_t;


/* Message of _IO_SPACE */
struct _io_space {
    uint16_t type;
    uint16_t combine_len;
    uint16_t subtype; /* F_ALLOCSP or F_FREESP */
    short whence;
    uint64_t start;
    uint64_t len; /* zero means to end of file */
};

typedef union {
    struct _io_space i;
    uint64_t o; /* file size */
} io_space_t;


/* Message of _IO_MMAP */
struct _io_mmap {
    uint16_t type;
    uint16_t combine_len;
    uint32_t prot;
    uint64_t offset;
    struct _msg_info info;
    uint32_t zero[6];
};

struct _io_mmap_reply {
    uint32_t zero;
    uint32_t flags;
    uint64_t offset;
    int32_t coid;
    int32_t fd;
};

typedef union {
    struct _io_mmap i;
    struct _io_mmap_reply o;
} io_mmap_t;


/* Message of _IO_DUP */
struct _io_dup {
    uint16_t type;
    uint16_t combine_len;
    struct _msg_info info;
    uint32_t reserved;
    uint32_t key;
};

typedef union {
    struct _io_dup i;
} io_dup_t;


/* Message of _IO_MSG */
struct _io_msg {
    uint16_t type;
    uint16_t combine_len;
    uint16_t protocol; /* I/O message protocol id */
    uint16_t subtype;  /* protocol-specific subtype */
};

typedef union {
    struct _io_msg i;
} io_msg_t;

/* RTMux native file-message subset. These messages are carried as _IO_MSG with
 * i.protocol == _IOMSG_RTMUX_NATIVE and i.subtype == _IO_NATIVE_*.
 *
 * Path strings are presentation only. Authority is carried by explicit
 * generation-tagged capability ids plus rights in the common header, and by the
 * receive-side message credentials.
 */
#define _IO_NATIVE_VERSION 1U

enum _io_native_subtypes {
    _IO_NATIVE_ATTACH = 1,
    _IO_NATIVE_WALK,
    _IO_NATIVE_OPEN,
    _IO_NATIVE_READ,
    _IO_NATIVE_WRITE,
    _IO_NATIVE_CLUNK,
    _IO_NATIVE_STAT,
    _IO_NATIVE_CTL_WRITE
};

enum _io_native_rights {
    _IO_NATIVE_RIGHT_READ = 0x00000001U,
    _IO_NATIVE_RIGHT_WRITE = 0x00000002U,
    _IO_NATIVE_RIGHT_EXEC = 0x00000004U,
    _IO_NATIVE_RIGHT_ADMIN = 0x00000008U
};

enum _io_native_cap_index {
    _IO_NATIVE_CAP_ROOT,
    _IO_NATIVE_CAP_OBJECT,
    _IO_NATIVE_CAP_IO,
    _IO_NATIVE_CAP_MAX
};

#define _IO_NATIVE_CAP_TRANSFER_MAX 4U

struct _io_native_cap {
    uint64_t id;
    uint32_t rights;
    uint32_t flags;
};

/* A cap transfer marked OPTIONAL is best-effort: if the receiver's table has no
 * room, the reply is still delivered with that transfer dropped instead of
 * failing the whole reply. Used by the spawn process-cap so a full parent table
 * never turns a spawn into an error.
 */
#define _IO_NATIVE_CAP_XFER_OPTIONAL 0x00000001U

struct _io_native_cap_transfer {
    uint64_t id;
    uint32_t rights;
    uint32_t flags;
};

struct _io_native_cred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t ngroups;
};

struct _io_native_hdr {
    struct _io_msg msg;
    uint16_t version;
    uint16_t flags;
    uint32_t body_len;
    uint64_t request_id;
    struct _io_native_cred cred;
    struct _io_native_cap caps[_IO_NATIVE_CAP_MAX];
    uint16_t nxfers;
    uint16_t reserved;
    struct _io_native_cap_transfer xfers[_IO_NATIVE_CAP_TRANSFER_MAX];
};

struct _io_native_attach {
    struct _io_native_hdr hdr;
    uint32_t aname_len;
    uint32_t zero;
    char aname[1];
};

struct _io_native_walk {
    struct _io_native_hdr hdr;
    uint64_t fid;
    uint64_t newfid;
    uint32_t path_len;
    uint32_t zero;
    char path[1];
};

struct _io_native_open {
    struct _io_native_hdr hdr;
    uint64_t fid;
    uint32_t ioflag;
    uint32_t mode;
};

struct _io_native_read {
    struct _io_native_hdr hdr;
    uint64_t fid;
    uint64_t offset;
    uint32_t nbytes;
    uint32_t flags;
};

struct _io_native_write {
    struct _io_native_hdr hdr;
    uint64_t fid;
    uint64_t offset;
    uint32_t nbytes;
    uint32_t flags;
    unsigned char data[1];
};

struct _io_native_clunk {
    struct _io_native_hdr hdr;
    uint64_t fid;
};

struct _io_native_stat {
    struct _io_native_hdr hdr;
    uint64_t fid;
    uint32_t flags;
    uint32_t zero;
};

struct _io_native_ctl_write {
    struct _io_native_hdr hdr;
    uint64_t fid;
    uint32_t op;
    uint32_t nbytes;
    unsigned char data[1];
};

/* Message of _IO_SYNC */
struct _io_sync {
    uint16_t type;
    uint16_t combine_len;
    uint32_t flag; /* O_?SYNC in fcntl.h */
};

typedef union {
    struct _io_sync i;
} io_sync_t;


__END_DECLS
#endif
