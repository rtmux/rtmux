/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"

static struct proc_event_entry {
    struct proc_event_entry *next;
    struct proc_event_entry **prev;
    int rcvid;
    unsigned flags;
    struct sigevent event;
} *event_list;

static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;

/* proc_event_del() deletes the event associated with <handle>
 *
 * If this is the last handle stored in the per-process events vector (now in
 * root_proc(prp->pid)->events) then the vector is freed.
 *
 * This function assumes that the process is already locked.
 */
static int proc_event_del(pid_t pid, int handle)
{
    struct proc_event_entry *p;
    vector_t *events;
    root_process_t *tmp = root_proc(pid);

    if ((p = vector_lookup(tmp->events, (unsigned) handle))) {
        vector_rem(tmp->events, (unsigned) handle);

        pthread_mutex_lock(&event_mutex);
        LINK2_REM(p);
        pthread_mutex_unlock(&event_mutex);

        _sfree(p, sizeof *p);

        if (tmp->events->nentries == tmp->events->nfree) {
            events = tmp->events;
            tmp->events = NULL;
            vector_free(events);
            _sfree(events, sizeof(vector_t));
        }
        return EOK;
    }
    return ESRCH;
}

/* Destroy all proc service events associated with this process
 *
 * Assumes that the process is locked.
 */
void proc_event_destroy(pid_t pid)
{
    int handle;
    root_process_t *tmp = root_proc(pid);

    for (handle = 0; tmp->events; handle++) {
        (void) proc_event_del(pid, handle);
    }
}

/* Deliver the sigevent associated with the event that matches <flags> */
void proc_event_trigger(unsigned flags)
{
    struct proc_event_entry *p;

    pthread_mutex_lock(&event_mutex);
    for (p = event_list; p; p = p->next) {
        if (p->flags & flags) {
            (void) msg_deliver_event(p->rcvid, &p->event);
        }
    }
    pthread_mutex_unlock(&event_mutex);
}

/* Handle the _PROC_EVENT type. */
int proc_event(ioserv_context_t *ctp, proc_event_t *msg)
{
    struct proc_event_entry *p;
    proc_event_del_t *dmsg;

    switch (msg->i.subtype) {
    case _PROC_EVENT_NOTIFY_ADD:
        if (!root_proc_lock(ctp->info.pid)) {
            return ESRCH;
        }

        {
            root_process_t *tmp = root_proc(ctp->info.pid);

            if (!tmp->events) {
                tmp->events = _scalloc(sizeof(vector_t));
                if (!tmp->events) {
                    return proc_error(ENOMEM, ctp->info.pid);
                }
            }

            p = _scalloc(sizeof *p);
            if (!p) {
                return proc_error(ENOMEM, ctp->info.pid);
            }

            int handle = vector_add(tmp->events, p, 1);
            if (handle == -1) {
                _sfree(p, sizeof(*p));
                return proc_error(ENOMEM, ctp->info.pid);
            }

            /* Lock the mutex to prevent proc_event_trigger() from delivering
             * the event before all the necessary fields have been updated.
             */
            pthread_mutex_lock(&event_mutex);
            p->rcvid = ctp->rcvid;
            p->flags = msg->i.flags;
            p->event = msg->i.event;

            LINK2_BEG(event_list, p, struct proc_event_entry);
            pthread_mutex_unlock(&event_mutex);

            IOSERV_STATUS(ctp, handle);
        }

        return proc_error(EOK, ctp->info.pid);

    case _PROC_EVENT_NOTIFY_DEL:

        dmsg = (proc_event_del_t *) msg;

        if (!root_proc_lock(ctp->info.pid)) {
            return ESRCH;
        }

        return proc_error(proc_event_del(ctp->info.pid, dmsg->i.id),
                          ctp->info.pid);

        /* NOTREACHED */
        break;

    case _PROC_EVENT_TRIGGER:
        if (!proc_isaccess(0, &ctp->info)) {
            if (msg->i.flags & PROC_EVENT_PRIVILEGED) {
                return EPERM;
            }
        }
        proc_event_trigger(msg->i.flags);
        return EOK;

    default:
        break;
    }
    return ENOSYS;
}
