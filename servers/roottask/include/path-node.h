/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PATH_NODE_H
#define PATH_NODE_H

#include <inttypes.h>

#define INODE_XOR(value) ((uintptr_t) (value) ^ 0x19071975)

typedef struct node_entry node_t;
typedef union path_object path_object_t;

typedef struct node_entry {
    node_t *parent;
    node_t *sibling;
    node_t *child;
    path_object_t *object;
    uint16_t links;         /* Number of current access to this node */
    uint16_t child_objects; /* Number of children with objects */
    uint16_t flags;
    uint16_t len;
    uint64_t entry_generation;
    char name[];
} node_t;

#define NODE_FLAG_NOCHILD 0x0001 /* No children allowed on this node */
#define NODE_FLAG_NONSERVER \
    0x0002 /* There is a non-server object on this node */
#define NODE_FLAG_UNLINKING 0x0004 /* The node is being unlinked */
#define NODE_FLAG_DEFERRED 0x0008  /* The node has a deferred open */

#define PATH_LOOKUP_ATTACH 0x00000001 /* increment link count if found */
#define PATH_LOOKUP_CREATE 0x00000002 /* create entries if they don't exist */
#define PATH_LOOKUP_PRUNE 0x00000004  /* only search first path entry */
#define PATH_LOOKUP_NOSERVER                      \
    0x00000008 /* Stop at first non-server object \
                */
#define PATH_LOOKUP_NOEMPTIES \
    0x00000010 /* Avoid reporting empty objects if possible */
#define PATH_LOOKUP_NOAUTO \
    0x00000020 /* Avoid reporting autocreated objects if possible */
#define PATH_LOOKUP_STATS \
    0x00000040 /* Caller represents external pathname resolution: count it */

extern pthread_mutex_t path_mutex;

#endif
