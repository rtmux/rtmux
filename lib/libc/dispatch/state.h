/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Internal definitions for the dispatch interface */

#include <sys/cdefs.h>

#define _DISPATCH_BLOCK_RECEIVE 1

#define _DPP(dpp) ((dispatch_t *) dpp)
/* Special-case flags */

#define _DISPATCH_ONLY_IOSERV 0x00000001

#define _VEC_VALID 0x80000000

/* I/O-service interface */

typedef struct {
    int (*other_func)(ioserv_context_t *ctp, void *msg);
    unsigned nparts_max;
    unsigned msg_max_size;
    unsigned context_size;
    pthread_mutex_t mutex;
} _ioserv_control;

__hidden int _ioserv_msg_handler(message_context_t *ctp,
                                 int code,
                                 unsigned flags,
                                 void *handle);

/* Message interface */

#define _MESSAGE_IOSERV_ENTRY MSG_FLAG_TYPE_IOSERV
#define _MESSAGE_NOTIFICATION_ENTRY MSG_FLAG_TYPE_NOTIFICATION

typedef struct {
    void *message_vec;
    unsigned num_elements;
    unsigned num_entries;
    unsigned context_size;
    unsigned msg_max_size;
    unsigned nparts_max;
    pthread_mutex_t mutex;
    unsigned reserved; /* flags; */
} _message_control;

typedef struct _message_vec {
    unsigned flags;
    short lo; /* Stay unsigned, see message.c */
    short high;
    int (*func)(message_context_t *ctp, int code, unsigned flags, void *handle);
    void *handle;
} message_vec_t;

int _message_handler(dispatch_context_t *ctp) __hidden;
void _message_unblock(dispatch_context_t *ctp) __hidden;

/* Dispatch internal structures */
int _dispatch_attach(dispatch_t *dpp, void *ctrl, unsigned type) __hidden;
int _dispatch_set_contextsize(dispatch_t *dpp, unsigned type) __hidden;

#define _DISPATCH_CHANNEL_COIDDEATH 0x40000000
#define _DISPATCH_CONTEXT_ALLOCED 0x80000000

struct _dispatch {
    /* enum dispatch_type		type; */
    int (*other_func)(ioserv_context_t *ctp, void *msg);
    unsigned nparts_max;
    unsigned msg_max_size;
    int block_type;
    unsigned int flags;
    int chid;
    unsigned int context_size;
    _ioserv_control *ioserv_ctrl;
    _message_control *message_ctrl;
};

struct _pool_properties {
    pthread_mutex_t inline_lock;
    unsigned newthreads;
    unsigned reserved_threads;
};

void *_thread_pool_context_thread(void *data) __hidden;
