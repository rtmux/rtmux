/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cpucfg.h"

/* value */                          /* Assumption */
#define _IOSERV_CLIENT_FD_MIN (16)   /* Most clients will have < this */
#define _IOSERV_CLIENT_FD_MAX (1024) /* _SC_OPEN_MAX == 1000 by default */
#define _IOSERV_CLIENT_MIN                                             \
    (8) /* Most I/O services will probably be serving < this number of \
           simultaneous clients */

#define _IOSERV_OBJ_LOWAT                                          \
    (8) /* Always try to keep this many handles and buckets around \
     to avoid allocator thrashing when the above are conservative */

#define _IOSERV_LINK_OTHERFUNC 0x00000001
#define _IOSERV_LINK_DETACHWAIT 0x00000002
#define _IOSERV_LINK_HALFOPEN \
    0x00000004 /* This link is not fully ready for use yet */

struct link {
    struct link *next;
    int id;      /* numeric id for this item in the linked list */
    int link_id; /* fd connection for the pathname to proc */
    const ioserv_connect_ops_t *connect_funcs;
    const ioserv_ops_t *io_funcs;
    void *handle;   /* user data handle to provide for connect functions */
    unsigned flags; /* _IOSERV_LINK_XXX flags */
    unsigned count; /* reference count on this structure */
};

struct binding {
    void *ocb;                 /* user allocated data handle (per open) */
    const ioserv_ops_t *funcs; /* functions from the link structure */
    int id;                    /* numeric id of the link structure */
    unsigned count;            /* reference count on this structure */
};

enum _ioserv_handle_type {
    _IOSERV_HANDLE_FIND,
    _IOSERV_HANDLE_SET,
    _IOSERV_HANDLE_REMOVE,
    _IOSERV_HANDLE_DISCONNECT,
    _IOSERV_HANDLE_UNLOCK,
    _IOSERV_HANDLE_LOCK = (int) (~0u ^ (~0u >> 1)),
    _IOSERV_HANDLE_FIND_LOCK = _IOSERV_HANDLE_FIND | _IOSERV_HANDLE_LOCK,
    _IOSERV_HANDLE_DISCONNECT_LOCK =
        _IOSERV_HANDLE_DISCONNECT | _IOSERV_HANDLE_LOCK,
    _IOSERV_HANDLE_REMOVE_LOCK = _IOSERV_HANDLE_REMOVE | _IOSERV_HANDLE_LOCK
};

struct _ioserv_handle_entry {
    struct _ioserv_handle_entry *next;
    void *handle;
    int coid;
    struct _ioserv_handle_entry **prev;
};

union _ioserv_entry_bucket {
    struct _ioserv_handle_entry *lists;
    union _ioserv_entry_bucket *next;
};

struct _ioserv_handle_list {
    struct _ioserv_handle_entry *list;
    int waiting;
    pid_t pid;
    uint32_t nd;
    struct _ioserv_handle_entry **lists;
    int nlists;
    int nlists_max;
};

struct _ioserv_handle_table {
    struct _ioserv_handle_list *vector; /* pointer to an array of lists */
    struct _ioserv_handle_entry *free_list;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned nentries;
    unsigned total;
    unsigned free;
    unsigned min;
    union _ioserv_entry_bucket *free_buckets;
    unsigned total_buckets;
    unsigned nfree_buckets;
    unsigned min_buckets;
    unsigned nlists_max;
    unsigned reserved;
};

extern struct link *_ioserv_link_list __hidden;
extern struct _ioserv_handle_table _ioserv_io_table __hidden;
extern pthread_key_t _ioserv_thread_key __hidden;

extern __hidden void *_ioserv_handle(struct _msg_info *rep,
                                     void *handle,
                                     enum _ioserv_handle_type type);

extern __hidden int _ioserv_connect_handler(ioserv_context_t *ctp,
                                            ioserv_msg_t *msg);
extern __hidden int _ioserv_disconnect_handler(ioserv_context_t *ctp,
                                               ioserv_msg_t *msg,
                                               int scoid);
extern __hidden int _ioserv_dup_handler(ioserv_context_t *ctp,
                                        ioserv_msg_t *msg);
extern __hidden void _ioserv_handler(ioserv_context_t *ctp);
extern __hidden _ioserv_func_t _ioserv_io_func(const ioserv_ops_t *funcs,
                                               unsigned type);
extern __hidden int _ioserv_io_handler(ioserv_context_t *ctp,
                                       ioserv_msg_t *msg,
                                       struct binding *binding);
extern __hidden struct link *_ioserv_link_alloc(void);
extern __hidden int _ioserv_link_free(int id, unsigned flags);

/* Every _ioserv_link_query() call should be followed up with a
 * _ioserv_link_return() call
 */
extern __hidden struct link *_ioserv_link_query(int id, int countadj);
extern __hidden void _ioserv_link_return(struct link *link, int countadj);

extern __hidden int _ioserv_mmap_handler(ioserv_context_t *ctp,
                                         ioserv_msg_t *msg);
extern __hidden int _ioserv_openfd_handler(ioserv_context_t *ctp,
                                           ioserv_msg_t *msg);
extern __hidden int _ioserv_unblock_handler(ioserv_context_t *ctp,
                                            ioserv_msg_t *msg,
                                            int rcvid);
extern __hidden int _ioserv_detach_id(ioserv_context_t *ctp,
                                      int id,
                                      unsigned flags);
extern __hidden int _ioserv_notify_handler(ioserv_context_t *ctp,
                                           ioserv_msg_t *msg);
extern __hidden int _ioserv_access(ioserv_context_t *ctp,
                                   struct _msg_info *msginfo,
                                   unsigned key1,
                                   void *msg,
                                   int size);
extern __hidden void _ioserv_close_handler(ioserv_context_t *ctp,
                                           struct binding *binding);
