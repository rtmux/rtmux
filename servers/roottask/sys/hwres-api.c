/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * High-level resource operations: create, destroy, attach, detach, query, and
 * per-process tracking.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "root-process.h"

#include <hwres.h>

#define CONTAINSE(s1, e1, p) ((s1) <= (p) && (p) <= (e1))
#define OVERLAPE(s1, e1, s2, e2) \
    (CONTAINSE((s1), (e1), (s2)) || CONTAINSE((s2), (e2), (s1)))


/* Determine if this node is a child of the parent */
static int _rsrc_is_child(rsrc_node_t *parent, rsrc_node_t *target)
{
    rsrc_node_t *child;

    if (!parent) {
        return 0;
    }

    if (target == parent) {
        return 1;
    }

    for (child = parent->child; child; child = child->sibling) {
        if (child == target) {
            return 1;
        }
    }

    for (child = parent->child; child; child = child->sibling) {
        if (_rsrc_is_child(child, target) == 0) {
            return 1;
        }
    }

    return 0;
}

/* This function is called when you want to perform some sort of search/removal
 * operation on a set of blocks.
 *
 * hwres_claim_t *req --> The request which has been made from the user pid_t
 * pid --> The process id to set/search on (based on flags) unsigned flags -->
 * Remove the entry or just query Removed entry from pid Removed entry should be
 * assigned to pid int *prio --> Priority of the located block.
 */
#define FLAG_ATTACH_RM 0x1
#define FLAG_ATTACH_RMTOPID 0x2
#define FLAG_ATTACH_RMFROMPID 0x4
#define FLAG_ATTACH_DONTLOOK 0x8
static int _rsrc_block_search_op(hwres_claim_t *rsrc,
                                 pid_t pid,
                                 unsigned flags,
                                 int *priority)
{
    uint64_t alignment, curstart, curend;
    rsrc_root_node_t *root;
    rsrc_node_t *node;
    rsrc_create_t *target;
    hwres_claim_t lastmatch;
    int lastprio, curprio;
    uint8_t curvalid;

    /* Validate the resource entry being attached */
    if (!rsrc || !rsrc->name ||
        ((rsrc->flags & HWRES_FLAG_RANGE) && rsrc->start > rsrc->end)) {
        return EINVAL;
    }

    /* Find the node for this resource */
    if (!(node = rsrc_node_locate(rsrc->name, 0))) {
        return errno;
    }

    /* Find the root for this resource */
    if (!(root = rsrc_root_locate(rsrc->name, 0))) {
        return errno;
    }

    /* Iterate now over all of the entries and if not successfull, then over all
     * of the child entries, looking for the best match that we can offer.
     */
    memset(&lastmatch, 0, sizeof(lastmatch));
    lastprio = INT_MAX;

    if (flags & FLAG_ATTACH_DONTLOOK) {
        lastmatch.start = rsrc->start;
        lastmatch.end = rsrc->end;
        lastprio = 0;
    } else {
        pid_t frompid;

        /* OPTIMIZE: If we are at the head, then the range is 0-ULONGLONG_MAX
         * rather than restricting the range to what is in the create list.
         */
        if ((rsrc->flags & HWRES_FLAG_ALIGN) && rsrc->align) {
            alignment = rsrc->align;
        } else {
            alignment = 1;
        }
        frompid = (flags & FLAG_ATTACH_RMFROMPID) ? pid : 0;

        curvalid = 0;
        curstart = curend = 0;
        for (target = root->created; target && lastprio >= node->priority;
             target = target->next) {
            /* Only look at items which are less than or equal to this node */
            if (node != &root->head_node &&
                !_rsrc_is_child(node, target->node)) {
                continue;
            }
            /* Check the co-alesced creation ranges */
            if (curvalid && target->start && target->start - 1 > curend) {
                if (rsrc->flags & HWRES_FLAG_RANGE) {
                    curstart = __max(rsrc->start, curstart);
                    curend = __min(rsrc->end, curend);
                }

                if (rsrc_find_range(root, rsrc->length, &curstart, &curend,
                                    alignment, frompid, rsrc->flags,
                                    &curprio) != -1) {
                    if (curprio < lastprio ||
                        (rsrc->flags & HWRES_FLAG_TOPDOWN &&
                         curprio <= lastprio)) {
                        lastmatch.start = curstart;
                        lastmatch.end = curend;
                        lastprio = curprio;
                    }
                }
                curvalid = 0;
            }

            if (curvalid) {
                curstart = __min(curstart, target->start);
                curend = __max(curend, target->end);
            } else {
                curstart = target->start;
                curend = target->end;
            }
            curvalid = 1;
        }

        /* One last check on exit if we missed any */
        if (curvalid) {
            if (rsrc->flags & HWRES_FLAG_RANGE) {
                curstart = __max(rsrc->start, curstart);
                curend = __min(rsrc->end, curend);
            }

            if (rsrc_find_range(root, rsrc->length, &curstart, &curend,
                                alignment, frompid, rsrc->flags,
                                &curprio) != -1) {
                if (curprio < lastprio) {
                    lastmatch.start = curstart;
                    lastmatch.end = curend;
                    lastprio = curprio;
                }
            }
        }
        /* We didn't find a matching entry, invalid arguments assumed */
        if (lastprio == INT_MAX) {
            return EINVAL;
        }
    }

    if (flags & FLAG_ATTACH_RMTOPID) { /* Remove and assign to pid */
        (void) rsrc_block_remove_range(root, lastmatch.start, lastmatch.end, 0);
        (void) rsrc_block_add_range_pid(root, lastmatch.start, lastmatch.end,
                                        pid, rsrc->flags & HWRES_FLAG_SHARE);
    } else if (flags & FLAG_ATTACH_RMFROMPID) { /* Remove from the pid */
        (void) rsrc_block_remove_range(root, lastmatch.start, lastmatch.end,
                                       pid);
        if (!(flags & FLAG_ATTACH_RM)) {
            _rsrc_block_limit_op(root, lastmatch.start, lastmatch.end, NULL,
                                 _add_callout, /* Overlap ranges */
                                 NULL, NULL);  /* Non-overlap ranges */
        }
    }

    rsrc->start = lastmatch.start;
    rsrc->end = lastmatch.end;
    if (priority) {
        *priority = lastprio;
    }
    return EOK;
}

/*** INTERFACE FUNCTIONS ***/
/* Create a resource range so it can later be allocated.
 *
 * Adds a create block (and the matching free data blocks) for the named
 * resource range, owned by pid unless HWRES_FLAG_NOREMOVE makes it
 * system-owned. Parameters:
 *   rsrc : the resource range (name, start, end, flags)
 *   pid  : the creating process
 * Return value: EOK on success, or an errno (EINVAL, ENOMEM, ...).
 */
int _hwres_create(hwres_range_t *rsrc, pid_t pid)
{
    rsrc_root_node_t *root;
    rsrc_node_t *node;
    rsrc_create_t *create;

    /* Validate the resource entry being attached */
    if (!rsrc || !rsrc->name || rsrc->start > rsrc->end) {
        return EINVAL;
    }
    pid = (rsrc->flags & HWRES_FLAG_NOREMOVE) ? 0 : pid;

    /* Find the node for this resource */
    if (!(node = rsrc_node_locate(rsrc->name, 1))) {
        return errno;
    }

    /* Find the root for this resource */
    if (!(root = rsrc_root_locate(rsrc->name, 0))) {
        return errno;
    }

    /* Create a new resource allocation entry */
    if (!(create = rsrc_create_allocate())) {
        return errno;
    }
    create->start = rsrc->start;
    create->end = rsrc->end;
    create->flags = (uint16_t) rsrc->flags;
    create->creator = pid;
    create->node = node;

    /* Insert the data blocks before we insert the create block */
    _rsrc_block_limit_op(root, create->start, create->end, NULL,
                         NULL,                /* Overlap ranges */
                         NULL, _add_callout); /* Non-overlap ranges */

    /* Insert the create block into the list ordered by start fields */
    rsrc_insert_create(&root->created, create);

    return EOK;
}

/* Destroy (or shrink/split) a previously created resource range. Parameters:
 *   rsrc : the resource range to remove
 *   pid  : the owning process whose create blocks are affected
 * Return value: EOK on success, or an errno (EINVAL, ENOMEM, ...).
 */
int _hwres_destroy(hwres_range_t *rsrc, pid_t pid)
{
    rsrc_root_node_t *root;
    rsrc_node_t *node;
    rsrc_create_t **prev, *target;

    /* Validate the resource entry being attached */
    if (!rsrc || !rsrc->name || rsrc->start > rsrc->end) {
        return EINVAL;
    }
    pid = (rsrc->flags & HWRES_FLAG_NOREMOVE) ? 0 : pid;

    /* Find the node for this resource */
    if (!(node = rsrc_node_locate(rsrc->name, 0))) {
        return errno;
    }

    /* Find the root for this resource */
    if (!(root = rsrc_root_locate(rsrc->name, 0))) {
        return errno;
    }

    /* Remove/Split/Modify the create block to erase this range. Then remove the
     * active resources from the range. Makes it multipass but this is a low
     * runner function.
     */
    prev = &root->created;
    while ((target = *prev) && target->start <= rsrc->end) {
        if (target->node != node || target->creator != pid ||
            !OVERLAPE(target->start, target->end, rsrc->start, rsrc->end)) {
            prev = &(*prev)->next;
            continue;
        }
        /* Remove entire block, split the block or shrink block */
        if (rsrc->start <= target->start && target->end <= rsrc->end) {
            *prev = target->next;
            rsrc_create_free(target);
        } else if (target->start < rsrc->start && rsrc->end < target->end) {
            if (!(target = rsrc_create_allocate())) {
                return ENOMEM;
            }
            /* Duplicate this block and insert w/ adjusted end point */
            memcpy(target, *prev, sizeof(*target));
            target->start = rsrc->end + 1;
            (*prev)->end = rsrc->start - 1;

            target->next = NULL;
            rsrc_insert_create(&root->created, target);
            prev = &(*prev)->next;
        } else if (rsrc->start <= target->start) {
            target->start = rsrc->end + 1;
            *prev = target->next;

            target->next = NULL;
            rsrc_insert_create(&root->created, target);
            /* Don't adjust prev, look at it again */
        } else { /* rsrc->end => target->end */
            target->end = rsrc->start - 1;
            prev = &target->next;
        }
    }

    /* Remove the data blocks after we removed the create block */
    _rsrc_block_limit_op(root, rsrc->start, rsrc->end, NULL,
                         NULL,                   /* Overlap ranges */
                         NULL, _remove_callout); /* Non-overlap ranges */

    return EOK;
}

/* Find, Remove and Attach all work to find a resource based on the set of
 * criteria which the user has specified.
 */
int _hwres_find(hwres_claim_t *rsrc, pid_t pid, int *priority)
{
    return _rsrc_block_search_op(rsrc, pid, 0, priority);
}

/* Remove a matching range from the database: detach it from pid (when pid is
 * negative) or claim it for pid (when pid is non-negative). Parameters:
 *   rsrc     : the request describing the range to remove
 *   pid      : negative to remove-from-pid, non-negative to remove-to-pid
 *   priority : out — priority of the located block (may be NULL)
 * Return value: EOK on success, or an errno.
 */
int _hwres_remove(hwres_claim_t *rsrc, pid_t pid, int *priority)
{
    if (pid < 0)
        return _rsrc_block_search_op(
            rsrc, -pid, FLAG_ATTACH_RMFROMPID | FLAG_ATTACH_DONTLOOK, priority);

    return _rsrc_block_search_op(
        rsrc, pid, FLAG_ATTACH_RMTOPID | FLAG_ATTACH_DONTLOOK, priority);
}

/* Find a matching free range and allocate it to pid. Parameters:
 *   rsrc : the request describing the desired range
 *   pid  : the process to assign the range to
 * Return value: EOK on success (rsrc updated with start/end), or errno.
 */
int _hwres_attach(hwres_claim_t *rsrc, pid_t pid)
{
    return _rsrc_block_search_op(rsrc, pid, FLAG_ATTACH_RMTOPID, 0);
}

/* Return a value to the system. This must be a value that is valid for the
 * process, otherwise this will error. This function can't be used to return all
 * values to the system.
 */
int _hwres_detach(hwres_claim_t *rsrc, pid_t pid)
{
    return _rsrc_block_search_op(rsrc, pid, FLAG_ATTACH_RMFROMPID, 0);
}

/* Remove all resources associated with this pid. First we remove all of the
 * resources that have been created and not allocated, then we remove all of the
 * resources that this particular process has had allocated.
 */
int _hwres_pid_clear(pid_t pid)
{
    process_t *prp;
    root_process_t *tmp = root_proc(pid);
    rsrc_root_node_t *root;
    rsrc_create_t **create, *ctarget;
    rsrc_block_t **blockp, *btarget;

    if (!(prp = root_proc_lock(pid))) {
        return 0;
    }
    if (!tmp->hwres_state || !tmp->hwres_state->haversrc) {
        root_proc_unlock(pid);
        return 0;
    }
    root_proc_unlock(pid);

    for (root = g_rsrc_root; root; root = root->next_root) {
        create = &root->created;
        while ((ctarget = *create)) {
            if (ctarget->creator == pid) {
                *create = ctarget->next;

                _rsrc_block_limit_op(root, ctarget->start, ctarget->end, NULL,
                                     NULL, /* Overlap ranges */
                                     NULL,
                                     _remove_callout); /* Non-overlap ranges */

                rsrc_create_free(ctarget);
            } else {
                create = &ctarget->next;
            }
        }

        blockp = &root->active;
        while ((btarget = *blockp)) {
            if (btarget->owner == pid) {
                if ((*blockp = btarget->next)) {
                    (*blockp)->prev = btarget->prev;
                }

                _rsrc_block_limit_op(root, btarget->start, btarget->end, NULL,
                                     _add_callout, /* Overlap ranges */
                                     NULL, NULL);  /* Non-overlap ranges */
                rsrc_block_free(btarget);
            } else {
                blockp = &btarget->next;
            }
        }
    }

    return 0;
}

/* Mark the process as having accessed a resource */
int _hwres_pid_mark(pid_t pid)
{
    process_t *prp;
    root_process_t *tmp = root_proc(pid);
    hwres_process_state_t *pidrsrc;

    if (!(prp = root_proc_lock(pid))) {
        return -1;
    }

    if (!(pidrsrc = tmp->hwres_state)) {
        pidrsrc = tmp->hwres_state = _scalloc(sizeof(*pidrsrc));
    }

    pidrsrc->haversrc = 1;
    root_proc_unlock(pid);
    return 0;
}

/* Things we want to query for: Created Resources (start,end,owner,priority)
 * (any level) Free Resources (start,end,flags) (top level) Allcated Resources
 * (start,end,owner,flags) (top level)
 *
 * Return the start,end,flags & pid of resources which are: pid == 0 --> All
 * free resources pid == -1 --> All used resources pid == -2 --> All resources
 * pid == pid--> All resources used by pid
 *
 * Starting at the 0 based index defined as start
 *
 * If newname is non NULL, then space is carved away from the input buffer and
 * the name is returned in the pointer.
 *
 * TODO: Put this support into the proc filesystem.
 *
 * TODO: Additionally return information on created blocks rather than the
 * allocated blocks if the FLAG_QUERY_CREAT is set.
 */

#define FLAG_QUERY_NEXT 0x1  /* Get the next root element */
#define FLAG_QUERY_COUNT 0x2 /* Return the count of the objects */
#define FLAG_QUERY_CREATE \
    0x4 /* Look at the create rather than the active blocks */
int _hwres_query(const char *name,
                 pid_t pid,
                 unsigned flags,
                 int start,
                 hwres_range_t *array,
                 uint32_t nbytes,
                 char **newname)
{
    rsrc_root_node_t *root;
    rsrc_block_t *blockp;
    int i, total;

    root = rsrc_root_locate((name) ? name : NULL, 0);
    if (root && name && flags & FLAG_QUERY_NEXT) {
        root = root->next_root;
    }

    if (!root) { /* This could be viewed as an error and not a 0 count */
        return 0;
    }

    if (flags & FLAG_QUERY_CREATE) {
        errno = ENOTSUP;
        return -1;
    }

    if (newname) {
        *newname = NULL;
    }

    for (total = 0, blockp = root->active; blockp; blockp = blockp->next) {
        if (pid == -2 || blockp->owner == pid || (pid == -1 && blockp->owner)) {
            total++;
        }
    }

    if (flags & FLAG_QUERY_COUNT || total == 0) {
        if (newname && array && nbytes > 0) {
            *newname = (char *) array;
            STRLCPY(*newname, root->head_node.name, nbytes);
        }
        return total;
    }

    if (total <= start || !array || !nbytes) {
        errno = EINVAL;
        return -1;
    }

    /* Stuff the name at the end of the data array if possible/wanted */
    if (newname) {
        if (nbytes < (strlen(root->head_node.name) + 1)) {
            errno = EINVAL;
            return -1;
        }
        nbytes -= (uint32_t) (strlen(root->head_node.name) + 1);
    }

    /* Determine the number of elements that we should copy */
    int avail = (int) (nbytes / sizeof(hwres_range_t));
    if (total - start < avail)
        total = total - start;
    else
        total = avail;

    for (i = 0, blockp = root->active; blockp && i < (start + total);
         blockp = blockp->next) {
        if (pid == -2 || blockp->owner == pid || (pid == -1 && blockp->owner)) {
            if (i++ < start) {
                continue;
            }
            array[(i - start) - 1].start = blockp->start;
            array[(i - start) - 1].end = blockp->end;
            array[(i - start) - 1].flags = blockp->flags;
            *((pid_t *) (&array[(i - start) - 1].name)) = blockp->owner;
        }
    }

    if (newname) {
        *newname = (char *) &array[i - start];
        STRLCPY(*newname, root->head_node.name,
                nbytes - ((size_t) &array[i - start] - (size_t) array));
    }

    return total;
}
