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
#include "root-log.h"

/* Snapshot the distinct mount endpoints in the namespace for the sync sweep.
 *
 * Walks the node tree under path_mutex and records every OBJECT_SERVER mount
 * (NAMESPACE_FLAG_DIR: a server that resolves longer paths, _FTYPE_ANY: not a
 * typed service such as the FIFO directory) once per (nd, pid, chid), skipping
 * roottask's own. The label is the first path seen for the endpoint. Nothing is
 * sent here: the caller messages the endpoints after the lock is dropped,
 * because a server may call back into roottask. Parameters:
 *   out : array receiving the targets
 *   max : capacity of out
 * A mount past max is not stored; the walk goes on and the omission is logged
 * once after the lock is dropped, with the first path seen and a count.
 * Return value: the number of targets stored.
 */
size_t namespace_sync_snapshot(struct sync_target *out, size_t max)
{
    node_t *root = root_proc(ROOTTASK_PID)->root;
    node_t *n = root;
    size_t count = 0, skipped = 0;
    char first[SYNC_LABEL_MAX];

    first[0] = '\0';
    pthread_mutex_lock(&path_mutex);
    while (n) {
        path_object_t *o;

        for (o = n->object; o; o = o->hdr.next) {
            size_t i;

            if (o->hdr.type != OBJECT_SERVER ||
                !(o->hdr.flags & NAMESPACE_FLAG_DIR) ||
                o->server.file_type != _FTYPE_ANY ||
                o->server.pid == ROOTTASK_PID)
                continue;
            for (i = 0; i < count; i++) {
                if (out[i].nd == o->server.nd && out[i].pid == o->server.pid &&
                    out[i].chid == o->server.chid)
                    break;
            }
            if (i < count)
                continue;
            if (count == max) {
                /* Not recorded, so a skipped endpoint seen again counts again.
                 */
                if (!skipped++ &&
                    path_node_fullpath(n, first, sizeof first) <= 1)
                    strcpy(first, "/");
                continue;
            }
            out[count].nd = o->server.nd;
            out[count].pid = o->server.pid;
            out[count].chid = o->server.chid;
            if (path_node_fullpath(n, out[count].label, SYNC_LABEL_MAX) <= 1)
                strcpy(out[count].label, "/");
            count++;
        }

        /* Preorder step without recursion: child, else next sibling, else the
         * nearest ancestor's next sibling.
         */
        if (n->child) {
            n = n->child;
        } else {
            while (n != root && !n->sibling)
                n = n->parent;
            if (n == root)
                break;
            n = n->sibling;
        }
    }
    pthread_mutex_unlock(&path_mutex);
    if (skipped)
        root_log(
            "sync: %d mount(s) past the %d-target limit not flushed, first "
            "%s\n",
            (int) skipped, (int) max, first);
    return count;
}
