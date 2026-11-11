/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include "root-process.h"
#include <sys/path-registry.h>

pthread_mutex_t path_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Allocate a node entry, initializing it to zero with the name stuffed. This
 * should only be called by namespace lookup and during namespace
 * initialization.
 */
node_t *path_node_alloc(const char *name, unsigned len)
{
    node_t *nop;

    if ((nop = _smalloc(offsetof(node_t, name) + len + 1))) {
        memset(nop, 0x00, offsetof(node_t, name));
        if ((nop->len = (uint16_t) len)) {
            (void) memccpy(nop->name, name, '\0', len + 1);
        }
        nop->name[len] = '\0';
    }
    return nop;
}

/* This call is used to indicate that we are "reading" something associated with
 * the node. No external calls (ie messages, anything which could result in a
 * recursive call back into a node accessing call) can be made between the
 * access and the complete calls.
 */
void path_node_access(node_t *node)
{
    pthread_mutex_lock(&path_mutex);
}

/* This call is used to indicate that we are finished doing something associated
 * with the node.
 */
void path_node_complete(node_t *node)
{
    pthread_mutex_unlock(&path_mutex);
}


/* Caller must hold path_mutex via path_node_lookup (held throughout) or
 * path_node_access / path_node_complete around the call.
 *
 * Returns nonzero if any object attached to nop bypasses the dentry cache.
 * Shared between the in-helper sticky tracking and the path_resolve_path
 * per-component walk so a future NAMESPACE_FLAG_* bit lands in one place.
 */
int path_node_is_uncacheable(const node_t *nop)
{
    const path_object_t *o;

    for (o = nop->object; o; o = o->hdr.next) {
        if (o->hdr.flags & NAMESPACE_FLAG_UNCACHEABLE)
            return 1;
    }
    return 0;
}

/* This call is used to indicate that we're making a copy of a pointer to the
 * node.
 */
node_t *path_node_clone(node_t *node)
{
    pthread_mutex_lock(&path_mutex);
    CRASHCHECK(node->links == 0);

    ++node->links;
    pthread_mutex_unlock(&path_mutex);
    return node;
}

/* This function will find the matching node for a path. It ignores multiple
 * slashes. If "object" is passed in, the node will be created if it doesn't
 * exist. It will also be linked to the current objects on that node.
 */
node_t *path_node_lookup(register node_t *nop,
                         register const char *path,
                         unsigned flags,
                         const char **result)
{
    register node_t *n;
    register node_t *lastnode;
    register const char *lasttail;
    int uncacheable;

    /* Pick a sensible default */
    if (!nop) {
        nop = root_proc(ROOTTASK_PID)->root;
    }

    /* Grab the mutex here */
    pthread_mutex_lock(&path_mutex);
    lastnode = NULL;
    lasttail = NULL;
    uncacheable = 0;
    while (*path) {
        unsigned len;

        /* Skip any multiple slashes */
        for (len = 0; path[len] && path[len] != '/'; len++) {
            /* nothing to do */
        }

        /* Search current level for a match */
        for (n = nop->child; n; n = n->sibling) {
            if (len == n->len && !memcmp(path, n->name, len)) {
                break;
            }
        }


        if (!n) {
            /* No match found */
            if (flags & PATH_LOOKUP_CREATE) {
                /* If we are linking create a node */
                if ((n = path_node_alloc(path, len))) {
                    /* Link it to the parent */
                    n->sibling = nop->child;
                    n->parent = nop;
                    nop->child = n;
                } else {
                    /* If no memory clean up */
                    nop->links++;
                    pthread_mutex_unlock(&path_mutex);
                    path_node_detach(nop);
                    pthread_mutex_lock(&path_mutex);
                    nop = 0;
                    break;
                }
            } else if ((flags & PATH_LOOKUP_NOSERVER) || !result) {
                /* If a result was asked for, then return node, and the
                 * remaining path
                 */
                nop = 0;
                break;
            } else {
                break;
            }
        }

        /* If we don't want any empties, and this is an empty node, then hope
         * that there is a fallback node that we can connect with in the
         * lastnode.
         */
        if ((flags & PATH_LOOKUP_NOEMPTIES) && !n->object &&
            n->child_objects == 0) {
            nop = 0;
            break;
        }

        /* Found matching node */
        nop = n;

        /* Uncacheable is sticky across the path walk: once any ancestor marks
         * the lookup as bypassing the cache, every remaining component for this
         * call counts under cache_uncacheable_bypasses rather than
         * cache_misses. That matches the semantic of an uncacheable directory
         * tainting anything below it.
         */
        if (!uncacheable && path_node_is_uncacheable(nop))
            uncacheable = 1;
        path += len;

        /* Skip slash if present */
        if (*path) {
            path++;
        }

        /* Do not recurse the children */
        if (flags & PATH_LOOKUP_PRUNE) {
            break;
        }

        /* Break out if node has non-server objects */
        if (flags & PATH_LOOKUP_NOSERVER) {
            path_object_t *object;

            if ((object = nop->object)) {
                /* We should have an option for shortest/longest pathname
                 * matches shortest match would break at this point, longest one
                 * would continue
                 */
                if (object->hdr.type != OBJECT_NONE &&
                    object->hdr.type != OBJECT_SERVER) {
                    lasttail = path;
                    lastnode = nop;
                }
            }
        }
    }

    /* If we don't have any objects (ie the node was autocreated), then fall
     * back and use the last node that we think was valid
     */
    if ((flags & PATH_LOOKUP_NOAUTO) && nop && !nop->object) {
        nop = 0;
        /* Fall through to the next test */
    }

    /* If we don't have a node, and there is a last node then use it */
    if (!nop && lastnode) {
        nop = lastnode;
        path = lasttail;
    }

    /* Store the remaining path if requested */
    if (result) {
        *result = path;
    }

    /* Increment link count */
    if (nop && (flags & PATH_LOOKUP_ATTACH)) {
        nop->links++;
    }

    /* Free the mutex */
    pthread_mutex_unlock(&path_mutex);

    /* Only external pathname resolution should move the cache counters;
     * internal admin / attach / bootstrap lookups would otherwise inflate the
     * miss count and make namespace statistics meaningless.
     */
    if (flags & PATH_LOOKUP_STATS)
        namespace_stats_lookup(uncacheable);
    return nop;
}


/* This will lower the link count on the node. If the links are zero, it will
 * remove the node, and all parents with a link count of zero.
 */
void path_node_detach_flags(node_t *nop, unsigned flags)
{
    /* Protect the node list */
    pthread_mutex_lock(&path_mutex);

    nop->flags &= (uint16_t) ~flags;
    if (nop->links == 0 || --nop->links == 0) {
        CRASHCHECK(nop->object);

        /* Remove the node */
        while (nop->links == 0 && !nop->child) {
            node_t *p, **pp;

            CRASHCHECK(!nop->parent);
            /* Unlink from parent or siblings */
            for (pp = &nop->parent->child; (p = *pp); pp = &p->sibling) {
                if (p == nop) {
                    *pp = p->sibling;
                    break;
                }
            }
            CRASHCHECK(!p);

            /* Rember the parent */
            p = nop->parent;

            _sfree(nop, offsetof(node_t, name) + nop->len + 1);

            /* Loop to see if parent also has no children or links */
            nop = p;
        }
    }
    pthread_mutex_unlock(&path_mutex);
}


/* Detach a node, lowering its link count (with no flags to clear).
 *
 * Parameters:
 *   nop : node to detach
 */
void path_node_detach(node_t *nop)
{
    path_node_detach_flags(nop, 0);
}
