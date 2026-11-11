/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/ioserv-private.h */

#ifndef __IOSERV_PRIVATE_H_INCLUDED
#define __IOSERV_PRIVATE_H_INCLUDED

#include <sys/iomsg.h>

typedef struct _notification io_notification_t;

struct _dispatch;
typedef struct _dispatch dispatch_t;

typedef union _ioserv_msg {
    unsigned short type;

    io_notification_t notification;

    struct _io_connect connect;
    io_open_t open;
    io_unlink_t unlink;
    io_readlink_t readlink;
    io_link_t link;

    struct _io_combine combine;
    io_close_t close;
    io_write_t write;
    io_read_t read;
    io_stat_t stat;
    io_notify_t notify;
    io_devctl_t devctl;
    io_pathconf_t pathconf;
    io_lseek_t lseek;
    io_chmod_t chmod;
    io_chown_t chown;
    io_utime_t utime;
    io_openfd_t openfd;
    io_fdinfo_t fdinfo;
    io_lock_t lock;
    io_space_t space;
    io_msg_t msg;
    struct _io_native_attach native_attach;
    struct _io_native_walk native_walk;
    struct _io_native_open native_open;
    struct _io_native_read native_read;
    struct _io_native_write native_write;
    struct _io_native_clunk native_clunk;
    struct _io_native_stat native_stat;
    struct _io_native_ctl_write native_ctl_write;
    io_mmap_t mmap;
    io_dup_t dup;
    io_sync_t sync;
} ioserv_msg_t;

typedef struct _ioserv_context {
    int rcvid;
    struct _msg_info info;
    ioserv_msg_t *msg;
    dispatch_t *dpp;
    int id;
    unsigned msg_max_size;
    int status;
    int offset;
    int size;
    iov_t iov[1];
} ioserv_context_t;

#ifndef IOSERV_HANDLE_T
#define IOSERV_HANDLE_T void
#endif

typedef int (*_ioserv_func_t)(ioserv_context_t *ctp,
                              ioserv_msg_t *msg,
                              void *ocb);

typedef struct _ioserv_connect_ops {
    unsigned nfuncs;
    int (*open)(ioserv_context_t *ctp,
                io_open_t *msg,
                IOSERV_HANDLE_T *handle,
                void *extra);
    int (*unlink)(ioserv_context_t *ctp,
                  io_unlink_t *msg,
                  IOSERV_HANDLE_T *handle,
                  void *reserved);
    int (*readlink)(ioserv_context_t *ctp,
                    io_readlink_t *msg,
                    IOSERV_HANDLE_T *handle,
                    void *reserved);
    int (*link)(ioserv_context_t *ctp,
                io_link_t *msg,
                IOSERV_HANDLE_T *handle,
                io_link_extra_t *extra);
    int (*unblock)(ioserv_context_t *ctp,
                   io_notification_t *msg,
                   IOSERV_HANDLE_T *handle,
                   void *reserved);
} ioserv_connect_ops_t;
#define IOSERV_CONNECT_NFUNCS \
    ((sizeof(ioserv_connect_ops_t) - sizeof(unsigned)) / sizeof(void *))

#ifndef IOSERV_OCB_T
#define IOSERV_OCB_T void
#endif

typedef struct _ioserv_ops {
    unsigned nfuncs;
    int (*read)(ioserv_context_t *ctp, io_read_t *msg, IOSERV_OCB_T *ocb);
    int (*write)(ioserv_context_t *ctp, io_write_t *msg, IOSERV_OCB_T *ocb);
    int (*close_ocb)(ioserv_context_t *ctp, void *reserved, IOSERV_OCB_T *ocb);
    int (*stat)(ioserv_context_t *ctp, io_stat_t *msg, IOSERV_OCB_T *ocb);
    int (*notify)(ioserv_context_t *ctp, io_notify_t *msg, IOSERV_OCB_T *ocb);
    int (*devctl)(ioserv_context_t *ctp, io_devctl_t *msg, IOSERV_OCB_T *ocb);
    int (*unblock)(ioserv_context_t *ctp,
                   io_notification_t *msg,
                   IOSERV_OCB_T *ocb);
    int (*pathconf)(ioserv_context_t *ctp,
                    io_pathconf_t *msg,
                    IOSERV_OCB_T *ocb);
    int (*lseek)(ioserv_context_t *ctp, io_lseek_t *msg, IOSERV_OCB_T *ocb);
    int (*chmod)(ioserv_context_t *ctp, io_chmod_t *msg, IOSERV_OCB_T *ocb);
    int (*chown)(ioserv_context_t *ctp, io_chown_t *msg, IOSERV_OCB_T *ocb);
    int (*utime)(ioserv_context_t *ctp, io_utime_t *msg, IOSERV_OCB_T *ocb);
    int (*openfd)(ioserv_context_t *ctp, io_openfd_t *msg, IOSERV_OCB_T *ocb);
    int (*fdinfo)(ioserv_context_t *ctp, io_fdinfo_t *msg, IOSERV_OCB_T *ocb);
    int (*lock)(ioserv_context_t *ctp, io_lock_t *msg, IOSERV_OCB_T *ocb);
    int (*space)(ioserv_context_t *ctp, io_space_t *msg, IOSERV_OCB_T *ocb);
    int (*reserved_shutdown)(ioserv_context_t *ctp,
                             void *msg,
                             IOSERV_OCB_T *ocb);
    int (*mmap)(ioserv_context_t *ctp, io_mmap_t *msg, IOSERV_OCB_T *ocb);
    int (*msg)(ioserv_context_t *ctp, io_msg_t *msg, IOSERV_OCB_T *ocb);
    int (*reserved_unused)(ioserv_context_t *ctp, void *msg, IOSERV_OCB_T *ocb);
    int (*dup)(ioserv_context_t *ctp, io_dup_t *msg, IOSERV_OCB_T *ocb);
    int (*close_dup)(ioserv_context_t *ctp, io_close_t *msg, IOSERV_OCB_T *ocb);
    int (*lock_ocb)(ioserv_context_t *ctp, void *reserved, IOSERV_OCB_T *ocb);
    int (*unlock_ocb)(ioserv_context_t *ctp, void *reserved, IOSERV_OCB_T *ocb);
    int (*sync)(ioserv_context_t *ctp, io_sync_t *msg, IOSERV_OCB_T *ocb);
} ioserv_ops_t;
#define IOSERV_IO_NFUNCS \
    ((sizeof(ioserv_ops_t) - sizeof(unsigned)) / sizeof(void *))

#define IOSERV_NOREPLY \
    ((int) (~0u ^       \
            (~0u >>     \
             1))) /* Do not reply to sender (no reply is sent to the caller) */
#define IOSERV_DEFAULT          \
    ((int) ((~0u ^ (~0u >> 1)) | \
            1)) /* Default action (usually return ENOSYS) */
#define IOSERV_NPARTS(_num) \
    (-(int) (_num)) /* Reply with this many parts from ctp->iov */
#define IOSERV_ERRNO(_err) \
    (_err) /* Reply with a short status containing an errno (can be EOK) */
#define IOSERV_PTR(_h, _p, _n) \
    (SETIOV((_h)->iov + 0, (_p), (_n)), IOSERV_NPARTS(1))
#define IOSERV_STATUS(_c, _s) ((_c)->status = (int) (_s))

/* flags for service attach. Must match NAMESPACE_FLAG_*. */
#define IOSERV_FLAG_BEFORE                                                   \
    0x0001 /* Force path to be resolved before others at the same mountpoint. \
            */
#define IOSERV_FLAG_AFTER                                                   \
    0x0002 /* Force path to be resolved after others at the same mountpoint. \
            */
#define IOSERV_FLAG_OPAQUE \
    0x0004 /* Don't resolve to mountpoints with shorter pathname matches. */
#define IOSERV_FLAG_FTYPEONLY \
    0x0008 /* Only attach at "/", and only match ftype */
#define IOSERV_FLAG_FTYPEALL \
    0x0010 /* Matching all ftypes (for redirecting servers) */
#define IOSERV_FLAG_DIR 0x0100  /* Allow resolving of longer pathnames. */
#define IOSERV_FLAG_SELF 0x0200 /* Allow resolving names to itself. */
#define IOSERV_FLAG_UNCACHEABLE \
    0x0400 /* Directory lookups bypass any namespace dentry cache */
#define IOSERV_FLAG_MASK 0x071f /* Flags allowed by service attach. */

/* flags for service detach. */
#define IOSERV_DETACH_ALL                                               \
    0x0000 /* Detach the name from the namespace and invalidate all open \
              bindings. */
#define IOSERV_DETACH_PATHNAME \
    0x0001 /* Only detach the name from the namespace */
#define IOSERV_DETACH_CLOSE                        \
    0x8000 /* Call close on bindings when detaching \
            */

/* If using thread pool functions, default to passing ioserv_context_t */
#ifndef THREAD_POOL_PARAM_T
#define THREAD_POOL_PARAM_T ioserv_context_t
#endif

#ifndef THREAD_POOL_HANDLE_T
#define THREAD_POOL_HANDLE_T dispatch_t
#endif

#endif
