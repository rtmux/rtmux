/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __HWRES_PRIVATE_H_INCLUDED
#define __HWRES_PRIVATE_H_INCLUDED

#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <hwres-msg.h>
#include <hwres-types.h>
#include "externs.h"

/* This generic block is a construct which is used so that we can loop through
 * either create blocks or resource blocks in a generic fashion. This layout
 * (start, end, next) should be at the head of all of the blocks.
 */
#define GENERIC_BLOCK(nexttype, othertype, othermember) \
    uint64_t start;                                     \
    uint64_t end;                                       \
    nexttype *next;                                     \
    othertype *othermember;

/* These blocks are maintained in an active (ie free and used) resource listing.
 * We intermingle free blocks with used block which are cross threaded back to
 * the process they came from.
 */
typedef struct _rsrc_block {
    GENERIC_BLOCK(struct _rsrc_block, struct _rsrc_block, prev)
    uint32_t flags;
    pid_t owner;
} rsrc_block_t;

/* These blocks are maintained in a created resource listing. We maintain a
 * linear list of these blocks sorted by start point which makes priority
 * queries easier.
 */
typedef struct _rsrc_create {
    GENERIC_BLOCK(struct _rsrc_create, struct _rsrc_node, node)
    pid_t creator;
    uint16_t flags;
    uint16_t zero;
} rsrc_create_t;

/* This is the structure of a node: it contains the priority (ie the level in
 * the tree) that it was created at for convenient access.
 */
typedef struct _rsrc_node {
    struct _rsrc_node *sibling; /* Root node uses this as a next pointer */
    struct _rsrc_node *child;
    uint8_t zero[2];
    uint8_t priority;
    char name[1]; /* Null terminated name allocated at creation */
} rsrc_node_t;

typedef struct _rsrc_root_node {
    rsrc_block_t *active;
    rsrc_create_t *created;
    union {
        struct _rsrc_root_node *_next;
        rsrc_node_t _head;
    } data;
#define next_root data._next
#define head_node data._head
} rsrc_root_node_t;

/* Device-number resource data structures. */
#define MAX_NUM_MINORS 1024
#define MAX_NUM_MAJORS 64

typedef struct _rsrc_devno {
    char *name;
    struct _devno_minors {
        uint8_t count;
        uint8_t bits[1];
    } *minors;
} rsrc_devno_t;

/* Per-process resource tracking. */
typedef struct hwres_process_state {
    uint16_t haversrc;
} hwres_process_state_t;

/* Function prototypes and globals. */
extern pthread_mutex_t g_rsrc_mutex;
extern rsrc_root_node_t *g_rsrc_root;

int _rsrc_minor(const char *name, int *major, int *minor_request, int flags);

static inline int hwres_dev_major(dev_t device)
{
    return (int) ((device >> 10) & 0x3f);
}

static inline int hwres_dev_minor(dev_t device)
{
    return (int) (device & 0x3ff);
}

static inline dev_t hwres_dev_make(int major, int minor)
{
    return (dev_t) ((major << 10) | minor);
}

/* hwres-list.c — low-level list operations */
rsrc_create_t *rsrc_create_allocate(void);
void rsrc_create_free(rsrc_create_t *create);
void rsrc_block_free(rsrc_block_t *blockp);
rsrc_root_node_t *rsrc_root_locate(const char *name, int create);
rsrc_node_t *rsrc_node_locate(const char *name, int create);
rsrc_create_t *rsrc_insert_create(rsrc_create_t **head, rsrc_create_t *create);
int rsrc_block_add_range_pid(rsrc_root_node_t *root,
                             uint64_t start,
                             uint64_t end,
                             pid_t owner,
                             unsigned flags);
int rsrc_block_remove_range(rsrc_root_node_t *root,
                            uint64_t start,
                            uint64_t end,
                            pid_t pid);
int _remove_callout(rsrc_root_node_t *root,
                    uint64_t start,
                    uint64_t end,
                    void *data);
int _add_callout(rsrc_root_node_t *root,
                 uint64_t start,
                 uint64_t end,
                 void *data);
int _rsrc_block_limit_op(
    rsrc_root_node_t *root,
    uint64_t start,
    uint64_t end,
    void *odata,
    int (*overlap)(rsrc_root_node_t *, uint64_t, uint64_t, void *),
    void *nodata,
    int (*nonoverlap)(rsrc_root_node_t *, uint64_t, uint64_t, void *));

/* hwres-api.c — high-level API */
int _hwres_create(hwres_range_t *rsrc, pid_t pid);
int _hwres_destroy(hwres_range_t *rsrc, pid_t pid);
int _hwres_find(hwres_claim_t *rsrc, pid_t pid, int *priority);
int _hwres_remove(hwres_claim_t *rsrc, pid_t pid, int *priority);
int _hwres_attach(hwres_claim_t *rsrc, pid_t pid);
int _hwres_detach(hwres_claim_t *rsrc, pid_t pid);

int rsrc_find_range(rsrc_root_node_t *root,
                    uint64_t length,
                    uint64_t *start,
                    uint64_t *end,
                    uint64_t alignment,
                    pid_t owner,
                    unsigned flags,
                    int *priority);

int _hwres_pid_clear(pid_t pid);
int _hwres_pid_mark(pid_t pid);

#define FLAG_QUERY_NEXT 0x1
#define FLAG_QUERY_COUNT 0x2
#define FLAG_QUERY_CREATE 0x4
int _hwres_query(const char *name,
                 pid_t pid,
                 unsigned flags,
                 int start,
                 hwres_range_t *array,
                 uint32_t nbytes,
                 char **newname);

#endif
