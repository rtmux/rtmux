/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This file contains the main interpretive interface to the resource database
 * layer from outside to in.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <hwres.h>

#define CONTAINSE(s1, e1, p) ((s1) <= (p) && (p) <= (e1))
#define OVERLAPE(s1, e1, s2, e2) \
    (CONTAINSE((s1), (e1), (s2)) || CONTAINSE((s2), (e2), (s1)))

/** *
 * Allocation Functions
 */
rsrc_create_t *rsrc_create_allocate(void)
{
    return calloc(1, sizeof(rsrc_create_t));
}

void rsrc_create_free(rsrc_create_t *create)
{
    free(create);
}

static rsrc_block_t *rsrc_block_allocate(void)
{
    return calloc(1, sizeof(rsrc_block_t));
}

void rsrc_block_free(rsrc_block_t *blockp)
{
    free(blockp);
}

/** *
 * Searching Functions
 */
/* This will return the root node for a given class. If NULL is passed in then
 * it will return the head of the list of roots.
 */
rsrc_root_node_t *rsrc_root_locate(const char *name, int create)
{
    rsrc_root_node_t *root;
    size_t len;

    if (!name) {
        return g_rsrc_root;
    }

    for (len = 0; name[len] && name[len] != '/'; len++) {
        ;
    }

    for (root = g_rsrc_root; root; root = root->next_root) {
        if (strncmp(root->head_node.name, name, len) == 0 &&
            strlen(root->head_node.name) == len) {
            break;
        }
    }

    if (!root && create) {
        /* TODO: Replace with a pool call? */
        if (!(root = calloc(1, sizeof(*root) + len))) {
            errno = ENOMEM;
            return NULL;
        }
        memcpy(root->head_node.name, name, len);
        root->next_root = g_rsrc_root;
        g_rsrc_root = root;
    }

    if (!root) {
        errno = EINVAL;
    }

    return root;
}

/* This will return the node which is responsible for this name */
rsrc_node_t *rsrc_node_locate(const char *name, int create)
{
    rsrc_root_node_t *root;
    rsrc_node_t *parent, *node;
    const char *start, *end;
    uint8_t level;
    size_t len;

    if (!(root = rsrc_root_locate(name, create))) {
        return NULL;
    }
    node = NULL;

    /* Move to the first "/", if it isn't there return w/ root */
    end = start = name;
    while (*start && *start != '/') {
        start++;
    }
    while (*start && *start == '/') {
        start++;
    }

    if (!*start) {
        return &root->head_node;
    }

    for (level = 1, parent = &root->head_node; parent; level++, parent = node) {
        /* Eat any extra slashes in the name, then find the end */
        while (*start && *start == '/') {
            start++;
        }
        end = start;
        while (*end && *end != '/') {
            end++;
        }

        /* No more name to process, return this node */
        len = (size_t) (end - start);
        if (len == 0) {
            break;
        }

        for (node = parent->child; node; node = node->sibling) {
            if (strncmp(node->name, start, len) == 0 &&
                strlen(node->name) == len) {
                break;
            }
        }

        /* Create ourselves a node if that is required */
        if (!node && create && (node = calloc(1, sizeof(*node) + len))) {
            memcpy(node->name, start, len);
            node->priority = level;
            node->sibling = parent->child;
            parent->child = node;
        } else {
            errno = (!node && create) ? ENOMEM : EINVAL;
        }

        start = end;
    }

    return node;
}


/* Return the priority information about this range (max/?min?) */
static int16_t find_range_priority(rsrc_root_node_t *root,
                                   uint64_t start,
                                   uint64_t end,
                                   unsigned flags)
{
    rsrc_create_t *create;
    int16_t priority;

    priority = 0;
    for (create = root->created; create; create = create->next) {
        if (create->start > end) {
            break;
        }

        if ((create->start <= start && start <= create->end) ||
            (start <= create->start && create->start <= end)) {
            /* HACK: We need to support the RSVP flags in some way */
            int16_t p = (int16_t) (create->node->priority +
                                   ((create->flags & HWRES_FLAG_RSVP) ? 1 : 0));
            if (p > priority)
                priority = p;
        }
    }

    return priority;
}

/* Find the priority of this block. Then find the next lowest priority block
 * that starts before (backward = 1) or starts after (backwards = 0). We then
 * stuff the appropriate end point (backward = 1) or startpoint (backwards = 0)
 * in value when we find it. If the priority is 0, then we stuff it with the
 * start/end values respectively
 */
static int find_next_priority(rsrc_root_node_t *root,
                              uint64_t start,
                              uint64_t end,
                              int backward,
                              uint64_t *value)
{
    rsrc_create_t *create;
    int16_t curpriority, priority;


    curpriority = find_range_priority(root, start, end, 0);
    *value = (backward) ? ((start == 0) ? 0 : start - 1)
                        : ((end == ULONGLONG_MAX) ? ULONGLONG_MAX : end + 1);

    if (curpriority == 0) {
        return (backward) ? (*value != end) : (*value != start);
    }

    priority = 0;
    for (create = root->created; create; create = create->next) {
        if (create->start > end) {
            break;
        }

        if ((create->start <= start && start <= create->end) ||
            (start <= create->start && create->start <= end)) {
            /* HACK: Support the RSVP flag */
            uint16_t nodeprio;

            nodeprio = create->node->priority +
                       ((create->flags & HWRES_FLAG_RSVP) ? 1 : 0);
            if (nodeprio > priority)
                priority = (int16_t) nodeprio;

            if (priority >= curpriority && nodeprio >= curpriority) {
                *value = (backward) ? __min(*value, create->start - 1)
                                    : __max(*value, create->end + 1);
            }
        }
    }

    return (backward) ? (*value != end) : (*value != start);
}

/* Find the lowest priority resource based on a set of parameters. Optional
 * parameters, other than length which is required, include:
 * - The block must have a certain alignment (default 1)
 * - The search should continue from the top of the range down (default up)
 * - The block may be sharable (default not shared)
 *
 * TODO: There is a limitation with this code in that it will only fit shared
 * blocks inside a totally free range or inside an existing share block which is
 * greater than the request. This means that a request in the following
 * situation will fail: 5-10 -> 11s-15s -> 16-20 A share request similar to
 * 10-11 or 15-16 would fail, even though technically it should succeed. This is
 * a very infrequent case however since most shares contain the same range, but
 * should be addressed at some point in the future.
 */
#define BLOCK_CONTAINS_PT(blk, pt) ((blk)->start <= (pt) && (pt) <= (blk)->end)
#define RSRC_FIND_BACK HWRES_FLAG_TOPDOWN
#define RSRC_FIND_SHARE HWRES_FLAG_SHARE

#define BLOCK_SHARE_COMPATIBLE(blk, flags, owner)       \
    (((blk)->owner == owner) ||                         \
     ((blk)->owner && (blk)->flags & RSRC_FIND_SHARE && \
      (flags) & RSRC_FIND_SHARE))

int rsrc_find_range(rsrc_root_node_t *root,
                    uint64_t length,
                    uint64_t *start,
                    uint64_t *end,
                    uint64_t alignment,
                    pid_t owner,
                    unsigned flags,
                    int *priority)
{
    uint64_t limit_start, limit_end;
    uint64_t tmp_start, tmp_end, extraamount;
    uint64_t savedstart, savedend;
    rsrc_block_t *curblock, *tmpblock;
    int16_t savedprio, curprio;
    uint8_t forward;


    /* Save the limiting start and end points */
    limit_end = *end;
    limit_start = *start;

    /* Indicate that the savedrange is not valid */
    savedstart = savedend = 0;
    savedprio = -1;

    /* Where are we looking forward or back? */
    forward = (flags & RSRC_FIND_BACK) ? 0 : 1;

    /* Get us to a point where we can start looking for blocks */
    tmpblock = NULL;
    for (curblock = root->active; curblock; curblock = curblock->next) {
        if (forward && (BLOCK_CONTAINS_PT(curblock, limit_start) ||
                        limit_start < curblock->start)) {
            tmpblock = curblock;
            break;
        } else if (!forward && (limit_end >= curblock->start)) {
            tmpblock = curblock;
        } else if (!forward) {
            break;
        }
    }

    if (!(curblock = tmpblock)) {
        return -1;
    }

    /* While we have a valid range iterate through the possibilities */
    while (curblock && curblock->start <= limit_end) {
        /* Determine the real range that we have to work with */
        tmp_start = __max(curblock->start, limit_start);
        tmp_end = __min(curblock->end, limit_end);

    restart:

        /* Look for something that is valid and might fit */
        if (BLOCK_SHARE_COMPATIBLE(curblock, flags, owner) &&
            tmp_start <= tmp_end && length <= ((tmp_end - tmp_start) + 1)) {
            tmp_start = (forward) ? tmp_start : tmp_end - (length - 1);

            /* Attempt to fit ourselves to the aligment we need */
            if ((extraamount = (tmp_start % alignment)) != 0) {
                tmp_start += (forward) ? alignment - extraamount : -extraamount;
            }

            tmp_end = tmp_start + (length - 1);

            /* Re-check the limits after a possible alignment adjust */
            if (/*extraamount && We iterate below ... so always check */
                (tmp_start < __max(curblock->start, limit_start) ||
                 tmp_end > __min(curblock->end, limit_end))) {
                ; /* Invalid block ... just continue */
            } else {
                /* Check range priority against saved block */
                curprio = find_range_priority(root, tmp_start, tmp_end, 0);
                if (curprio == 0 || savedprio == -1 || curprio < savedprio) {
                    savedstart = tmp_start;
                    savedend = tmp_end;
                    savedprio = curprio;
                    if (curprio == 0) {
                        break;
                    }
                }

                /* Find the next possible starting block based on the priority
                 */
                if (find_next_priority(root, tmp_start, tmp_end, !forward,
                                       &extraamount)) {
                    if (forward) {
                        tmp_start = extraamount;
                        tmp_end = __min(curblock->end, limit_end);
                    } else {
                        tmp_start = __max(curblock->start, limit_start);
                        tmp_end = extraamount;
                    }
                    goto restart;
                }
            }
        }

        curblock = (forward) ? curblock->next : curblock->prev;
    }

    /* Check and see if what was found was better than what was saved */
    if (savedprio != -1) {
        *start = savedstart;
        *end = savedend;
        if (priority) {
            *priority = savedprio;
        }
        return EOK;
    }

    errno = ERANGE;
    return -1;
}

/** *
 * Insertion Functions
 */

/* Insert a create block into a list kept sorted by start address. Parameters:
 *   head   : address of the list head pointer
 *   create : the create block to insert
 * Return value: the inserted block (create).
 */
rsrc_create_t *rsrc_insert_create(rsrc_create_t **head, rsrc_create_t *create)
{
    rsrc_create_t **prev;

    prev = head;
    while (*prev && (*prev)->start <= create->start) {
        prev = &((*prev)->next);
    }
    create->next = *prev;
    *prev = create;

    return create;
}

/* Insert a new block (start, end) into the list before/after curblk. */
static rsrc_block_t *rsrc_insert_block(rsrc_block_t **head,
                                       rsrc_block_t *curblk,
                                       uint64_t start,
                                       uint64_t end,
                                       pid_t owner,
                                       unsigned flags,
                                       int after)
{
    rsrc_block_t *newblk;

    if (!(newblk = rsrc_block_allocate())) {
        return NULL;
    }
    newblk->start = start;
    newblk->end = end;
    newblk->owner = owner;
    newblk->flags = flags;

    /* We are the first block in the list */
    if (!curblk || (!after && !curblk->prev)) {
        newblk->prev = NULL;
        if ((newblk->next = *head)) {
            newblk->next->prev = newblk;
        }
        *head = newblk;
    } else {
        /* newblk->prev should alway be set to something */
        newblk->prev = (after) ? curblk : curblk->prev;
        if ((newblk->next = newblk->prev->next)) {
            newblk->next->prev = newblk;
        }
        newblk->prev->next = newblk;
    }

    return newblk;
}

/* Add a new allocation block into the single linked list of existing allocated
 * blocks. Then add the intersection of this block and existing blocks into the
 * active pool of resources (make this another function?).
 */

/* Block are compatible (ie can be joined) if they have the same owner and if
 * they have the same share flags (ie can be shared, or can't be shared) #define
 * JOIN_COMPATIBLE(blk, pid, flags) ((blk)->owner == (pid) && \ ((blk)->flags &
 * HWRES_FLAG_SHARE) == (flags & HWRES_FLAG_SHARE)) Shared blocks should always
 * get their own blocks, even if they overlap ?
 */
#define JOIN_COMPATIBLE(blk, pid, flags)                            \
    ((blk)->owner == (pid) && !((blk)->flags & HWRES_FLAG_SHARE) && \
     !(flags & HWRES_FLAG_SHARE))

/* TODO: We need to roll these block_add_range* functions into one! */
/* Add an allocated block [newstart, newend] owned by pid into the active list,
 * absorbing or extending join-compatible neighbours. Parameters:
 *   root     : the resource root
 *   newstart : start of the range to add
 *   newend   : end of the range to add
 *   pid      : owning process
 *   flags    : block flags (e.g. HWRES_FLAG_SHARE)
 * Return value: 0.
 */
int rsrc_block_add_range_pid(rsrc_root_node_t *root,
                             uint64_t newstart,
                             uint64_t newend,
                             pid_t pid,
                             unsigned flags)
{
    rsrc_block_t *curblk, *absorb;

    absorb = NULL;
    /* TODO: Find an early out condition here */
    for (curblk = root->active; curblk; curblk = curblk->next) {
        if (newstart <= curblk->end + 1) {
            uint8_t overlaps, canjoin;

            overlaps = OVERLAPE((newstart != 0) ? newstart - 1 : newstart,
                                (newend != ULONGLONG_MAX) ? newend + 1 : newend,
                                curblk->start, curblk->end);
            canjoin = JOIN_COMPATIBLE(curblk, pid, flags);

            /* If we start before this current block then create a new block
             * which will then absorb in any overlapping blocks which arise in
             * the future
             */
            if (!absorb && newstart < curblk->start) {
                absorb = rsrc_insert_block(&root->active, curblk, newstart,
                                           newend, pid, flags, 0);

                /* TODO: If we are maintaining a taken list, then we need to
                 * make sure that things are properly marked here.
                 */
            }

            /* Absorb or extend this block as required */
            if (canjoin && overlaps) {
                newstart = __min(newstart, curblk->start);
                newend = __max(newend, curblk->end);
                if (!absorb) {
                    absorb = curblk;
                } else {
                    if ((absorb->next = curblk->next)) {
                        absorb->next->prev = absorb;
                    }
                    rsrc_block_free(curblk);
                    curblk = absorb;
                }
                absorb->start = newstart;
                absorb->end = newend;
            }
        }

        if (!curblk->next) {
            break;
        }
    }

    /* If we still have an outstanding block, create it at the end of the list
     */
    if (!absorb && newstart <= newend) {
        absorb = rsrc_insert_block(&root->active, curblk, newstart, newend, pid,
                                   flags, 1);
    }

    return 0;
}

/* This function should have no intelligence other than determining that you
 * want to insert a block in a "start point first" scenario or that you want to
 * insert a block to be merged with other like minded blocks. Currently this
 * routine does a lot of extra work to limit ranges and the like. This should be
 * done by an external function.
 */
static int rsrc_block_add_range(rsrc_root_node_t *root,
                                uint64_t newstart,
                                uint64_t newend,
                                pid_t pid,
                                unsigned flags)
{
    rsrc_block_t *curblk, *absorb;

    /* Determine the overlap with existing blocks and add the missing bits */
    absorb = NULL;
    for (curblk = root->active; curblk && newstart <= newend;
         curblk = curblk->next) {
        uint8_t overlaps;

        /* Skip blocks that aren't even close to us, */
        if (curblk->end + 1 < newstart) {
            if (!curblk->next) {
                break;
            }
            continue;
        }
        /* Create a new block if it ends before the next block */
        if (newend + 1 < curblk->start) {
            if (!absorb) {
                absorb = rsrc_insert_block(&root->active, curblk, newstart,
                                           newend, pid, flags, 0);
            }
            newstart = newend + 1;
            break;
        }

        overlaps = OVERLAPE((newstart != 0) ? newstart - 1 : newstart,
                            (newend != ULONGLONG_MAX) ? newend + 1 : newend,
                            curblk->start, curblk->end);

        /* If the next block has an owner and we overlap then create a new block
         * before
         */
        if (curblk->owner && overlaps) {
            if (newstart < curblk->start) {
                if (absorb) {
                    absorb->end = curblk->start - 1;
                } else {
                    absorb =
                        rsrc_insert_block(&root->active, curblk, newstart,
                                          curblk->start - 1, pid, flags, 0);
                }
            }
            if (curblk->end >= newend) { /* Handle ULONGLONG_MAX case */
                break;
            }
            newstart = curblk->end + 1;
            absorb = NULL;
        } else if (overlaps) {
            newstart = __min(newstart, curblk->start);
            newend = __max(newend, curblk->end);
            if (!absorb) {
                absorb = curblk;
            } else {
                if ((absorb->next = curblk->next)) {
                    absorb->next->prev = absorb;
                }
                rsrc_block_free(curblk);
                curblk = absorb;
            }
            absorb->start = newstart;
            absorb->end = newend;
        } else {
            /* I don't know what condition got us here? */
            root_log("WUZZUP!\n");
        }

        if (!curblk->next) {
            break;
        }
    }

    /* If we still have an outstanding block, create it here */
    if (!absorb && newstart <= newend) {
        absorb = rsrc_insert_block(&root->active, curblk, newstart, newend, pid,
                                   flags, 1);
    }

    return 0;
}

/* Options for removal are: 1- Just delete the blocks owned by pid in this range
 * (pid != -1) 2- Just delete the blocks not owned in this range (pid == 0)
 */
int rsrc_block_remove_range(rsrc_root_node_t *root,
                            uint64_t newstart,
                            uint64_t newend,
                            pid_t pid)
{
    rsrc_block_t *target, **curblk;

    /* Determine the overlap with existing blocks and add the missing bits */
    curblk = &root->active;
    while ((target = *curblk) && target->start <= newend) {
        if (target->owner != pid ||
            !(OVERLAPE(newstart, newend, target->start, target->end))) {
            curblk = &(*curblk)->next;
            continue;
        }
        /* Remove entire block, split the block or shrink block */
        if (newstart <= target->start && target->end <= newend) {
            if (target->next) {
                target->next->prev = target->prev;
            }
            *curblk = target->next;
            rsrc_block_free(target);
        } else if (target->start < newstart && newend < target->end) {
            if (!(target = rsrc_block_allocate())) {
                return ENOMEM;
            }
            /* Duplicate this block and insert w/ adjusted end point */
            memcpy(target, *curblk, sizeof(*target));
            target->next = *curblk;
            target->next->prev = target;

            /* TODO: If the block is owned/shared then we need to do some extra
             * fiddling. if(target->owner) { target->prevtaken =
             * (*curblk)->prevtaken; target->nexttaken = *curblk;
             * (*curblk)->prevtaken = &target->nexttaken; *target->prevtaken =
             * target; }
             */

            *curblk = target; /* target->prev->next */

            target->end = newstart - 1;
            target->next->start = newend + 1;
            /* Advance the pointer to the next,next block */
            curblk = &target->next->next;
        } else {
            target->start =
                (newstart <= target->start) ? newend + 1 : target->start;
            target->end = (newend >= target->end) ? newstart - 1 : target->end;
            curblk = &target->next;
        }
    }
    return 0;
}

/** *
 * Middle Layer Helper Functions
 */
/* _rsrc_block_limit_op callout: free the system-owned blocks in a range.
 * Parameters:
 *   root  : the resource root
 *   start : range start
 *   end   : range end
 *   data  : unused callout context
 * Return value: 0 on success, or an errno.
 */
int _remove_callout(rsrc_root_node_t *root,
                    uint64_t start,
                    uint64_t end,
                    void *data)
{
    return rsrc_block_remove_range(root, start, end, 0);
}

/* _rsrc_block_limit_op callout: mark a range as free (system-owned).
 * Parameters:
 *   root  : the resource root
 *   start : range start
 *   end   : range end
 *   data  : unused callout context
 * Return value: 0.
 */
int _add_callout(rsrc_root_node_t *root,
                 uint64_t start,
                 uint64_t end,
                 void *data)
{
    return rsrc_block_add_range(root, start, end, 0, 0);
}

/* This function is used when you want to do some sort of limiting on a range
 * and apply a function to that range (ie add/remove etc).
 */
int _rsrc_block_limit_op(rsrc_root_node_t *root,
                         uint64_t start,
                         uint64_t end,
                         void *odata,
                         int (*overlap)(rsrc_root_node_t *root,
                                        uint64_t start,
                                        uint64_t end,
                                        void *data),
                         void *nodata,
                         int (*nonoverlap)(rsrc_root_node_t *root,
                                           uint64_t start,
                                           uint64_t end,
                                           void *data))
{
    rsrc_create_t *tmpinfo;
    uint64_t overlapstart, overlapend;
    uint64_t purgestart, purgeend;
    int ret, overlapvalid;

    overlapstart = overlapend = 0;
    purgestart = start;
    purgeend = end;
    overlapvalid = 0;
    ret = EOK;

    for (tmpinfo = root->created; tmpinfo && purgestart <= purgeend;
         tmpinfo = tmpinfo->next) {
        /* If this block doesn't overlap, then don't bother looking at it */
        if (!OVERLAPE(tmpinfo->start, tmpinfo->end, purgestart, purgeend)) {
            continue;
        }

        /* If this info block overlaps and we have previous overlap, callout */
        if (overlapvalid &&
            !OVERLAPE(tmpinfo->start, tmpinfo->end, overlapstart, overlapend)) {
            if (overlap) {
                ret = overlap(root, overlapstart, overlapend, odata);
            }
            overlapvalid = 0;
        }

        if (!overlapvalid) {
            overlapstart = __max(tmpinfo->start, purgestart);
            overlapend = __min(tmpinfo->end, purgeend);
        } else {
            overlapend = __max(overlapend, __min(purgeend, tmpinfo->end));
        }
        overlapvalid = 1;

        /* If we start before the purge block we move our start point forward */
        if (tmpinfo->start <= purgestart) {
            purgestart = (tmpinfo->end != ULONGLONG_MAX) ? tmpinfo->end + 1
                                                         : ULONGLONG_MAX;
        }

        /* If we start after the purge block, we should purge the range and
         * continue
         */
        else {
            if (nonoverlap) {
                ret = nonoverlap(root, purgestart,
                                 (tmpinfo->start) ? tmpinfo->start - 1 : 0,
                                 nodata);
            }
            purgestart = (tmpinfo->end != ULONGLONG_MAX) ? tmpinfo->end + 1
                                                         : ULONGLONG_MAX;
        }
    }

    if (overlapvalid && overlap) {
        ret = overlap(root, overlapstart, overlapend, odata);
    }

    if (purgestart <= purgeend && nonoverlap) {
        ret = nonoverlap(root, purgestart, purgeend, nodata);
    }

    return ret;
}
