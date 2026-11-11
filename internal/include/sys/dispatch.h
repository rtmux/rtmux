/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/dispatch.h */
#ifndef __DISPATCH_H_INCLUDED
#define __DISPATCH_H_INCLUDED

#ifndef _PTHREAD_H_INCLUDED
#include <pthread.h>
#endif

#ifndef __IOSERV_PRIVATE_H_INCLUDED
#include <sys/ioserv-private.h>
#endif

typedef struct _ioserv_attr {
    unsigned flags;
    unsigned nparts_max;
    unsigned msg_max_size;
    int (*other_func)(ioserv_context_t *, void *msg);
} ioserv_attr_t;

/* Name dispatch functions */
#define NAME_FLAG_ATTACH_MOUNTABLE 0x00000004 /* Endpoint may be mounted */

#define NAME_FLAG_DETACH_SAVEDPP 0x00000001 /* Dispatch ptr saved */

typedef struct _name_attach {
    dispatch_t *dpp;
    int chid;
    int mntid;
    int zero[2];
} name_attach_t;

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags);
int name_detach(name_attach_t *attach, unsigned flags);

int name_open(const char *name, int flags);
int name_close(int fd);

/* Message dispatch functions */
#define MSG_FLAG_TYPE_NOTIFICATION 0x00000001
#define MSG_FLAG_ALLOC_NOTIFICATION 0x00000002
#define MSG_FLAG_TYPE_IOSERV 0x00000008
#define MSG_FLAG_SIDE_CHANNEL 0x00000200

typedef struct _ioserv_context message_context_t;

typedef struct _message_attr {
    unsigned flags;
    unsigned nparts_max;
    unsigned msg_max_size;
} message_attr_t;

int message_attach(
    dispatch_t *dpp,
    message_attr_t *attr,
    int low,
    int high,
    int (*func)(message_context_t *ctp, int code, unsigned flags, void *handle),
    void *handle);
int notification_attach(
    dispatch_t *dpp,
    int flags,
    int code,
    int (*func)(message_context_t *ctp, int code, unsigned flags, void *handle),
    void *handle);
int message_connect(dispatch_t *dpp, int flags);

/* General dispatch functions */

#define DISPATCH_FLAG_NOLOCK 0x00000001

enum dispatch_type {
    DISPATCH_IOSERV = 1,
    DISPATCH_MESSAGE,
};

typedef union _dispatch_context {
    ioserv_context_t ioserv_context;
    message_context_t message_context;
} dispatch_context_t;

dispatch_t *dispatch_create(void);
dispatch_t *dispatch_create_channel(int chid, unsigned reserved);
dispatch_context_t *dispatch_block(dispatch_context_t *ctp);
int dispatch_handler(dispatch_context_t *ctp);
dispatch_context_t *dispatch_context_alloc(dispatch_t *dpp);
void dispatch_context_free(dispatch_context_t *ctp);
int dispatch_destroy(dispatch_t *dpp);

/* Thread pool functions */

#define POOL_FLAG_EXIT_SELF 0x00000001
#define POOL_FLAG_RESERVE 0x00000010

typedef struct _thread_pool thread_pool_t;

#ifndef THREAD_POOL_PARAM_T
#define THREAD_POOL_PARAM_T void
#endif

#ifndef THREAD_POOL_HANDLE_T
#define THREAD_POOL_HANDLE_T dispatch_t
#endif

typedef struct _thread_pool_attr {
    THREAD_POOL_HANDLE_T *handle;
    THREAD_POOL_PARAM_T *(*block_func)(THREAD_POOL_PARAM_T *ctp);
    void (*unblock_func)(THREAD_POOL_PARAM_T *ctp);
    int (*handler_func)(THREAD_POOL_PARAM_T *ctp);
    THREAD_POOL_PARAM_T *(*context_alloc)(THREAD_POOL_HANDLE_T *handle);
    void (*context_free)(THREAD_POOL_PARAM_T *ctp);
    pthread_attr_t *attr;
    unsigned short lo_water;
    unsigned short increment;
    unsigned short hi_water;
    unsigned short maximum;
} thread_pool_attr_t;

struct _thread_pool {
    thread_pool_attr_t pool_attr;
    unsigned created;
    unsigned waiting;
    unsigned flags;
    void *props;
};

thread_pool_t *thread_pool_create(thread_pool_attr_t *attr, unsigned flags);
int thread_pool_start(void *pool);

__END_DECLS
#endif
