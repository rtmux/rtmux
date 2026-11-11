/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

void rdecl kcall_err(thread_t *thp, int err)
{
    if (TYPE_MASK(thp->type) == TYPE_VTHREAD) {
        return;
    }

    /* Last error wins. The last error winning is used when the condvar code
     * calls the mutex code so the condvar code can override the error.
     */
    SETKSTATUS(thp, -err);

    /* Check for multiple errors in one kernel call. */
    if (err != EOK && (thp->flags & RTMUX_FLG_THR_KCALL_ERR_SET) == 0) {
        atomic_or((volatile unsigned *) &thp->flags,
                  RTMUX_FLG_THR_KCALL_ERR_SET);
        thp->restart = NULL;
        timeout_stop(thp);
        SETKIP(thp, KIP(thp) + KCALL_ERR_SKIPAHEAD);
    }
}


bool rdecl cred_is_root(thread_t *thp)
{
    if (thp->process->cred->info.euid == 0) {
        return true;
    }

    kcall_err(thp, EPERM);
    return false;
}


bool rdecl cred_may_access(thread_t *srcthp, process_t *dstprp)
{
    credential_t *src = srcthp->process->cred;
    credential_t *dst = dstprp->cred;

    if (src->info.euid == 0 || src->info.ruid == dst->info.ruid ||
        src->info.ruid == dst->info.suid || src->info.euid == dst->info.ruid ||
        src->info.euid == dst->info.suid) {
        return true;
    }

    kcall_err(srcthp, EPERM);
    return false;
}

int rdecl ksched_ok(thread_t *thp, int policy, const struct sched_param *param)
{
    if (param->sched_priority < 0 || param->sched_priority >= NUM_PRI)
        return EINVAL;
    if (param->sched_priority == 0 &&
        ((thp->process->flags & RTMUX_FLG_PROC_LOADING) != 0 ||
         thp->process->priv_caps == 0))
        return EINVAL;
    if (!(thp->process->priv_caps & RTMUX_PRIV_CAP_SCHED) &&
        param->sched_priority >= kopt.priv_prio)
        return EPERM;
    if (policy < RTMUX_SCHED_NOCHANGE || policy > SCHED_SPORADIC)
        return EINVAL;
    if (policy != SCHED_SPORADIC)
        return EOK;
    if (param->ss_max_repl < 1 || param->ss_max_repl > SS_REPL_MAX)
        return EINVAL;
    if (param->ss_low_priority <= 0 ||
        param->ss_low_priority > param->sched_priority)
        return EINVAL;
    if (timespec2nsec(&param->ss_repl_period) <
        timespec2nsec(&param->ss_init_budget))
        return EINVAL;
    return EOK;
}

int kcall_attach(unsigned callnum, ksys_call_t *func)
{
    if (callnum >= __KSYS_BAD)
        return -1;

    ksys_call_table[callnum] = func;

    return 0;
}

/* Can be set to one by the kernel debugger so we can continue on from a crash()
 * message
 */
int kdebug_walkout;

void rdecl(crash)(const char *file, int line)
{
    interrupt_disable();
    kprintf("Crash[%d] at %s line %d.\n", get_cpunum(), file, line);
    for (;;) {
        interrupt_disable();
        debug_kdbreak();
        if (!privateptr->debug_call) {
            system_reboot(1);
        }
        if (kdebug_walkout)
            break;
    }
}


/* Routines to free any unrequired memory when the physical allocator can't
 * satisfy an allocation
 */

struct purger {
    struct purger *next;
    unsigned prio;
    int (*purger)(size_t);
};
static struct purger *purge_list;
SMP_SPINVAR(static, purge_slock);

int purger_invoke(size_t amount)
{
    struct purger *purge;

    purge = purge_list;
    for (;;) {
        if (!purge)
            return 0;
        if (purge->purger(amount))
            return 1;
        purge = purge->next;
    }
}


int purger_register(int (*purger)(size_t), unsigned prio)
{
    struct purger *new;
    struct purger **owner;
    struct purger *chk;

    /* Something to note. If we ever start allowing unregistering purgers (why,
     * I don't know, but play along), there's going to be a problem freeing the
     * struct purger entries. This function might be called from either the
     * kernel or proc service, so the _smalloc might come from either heap. The
     * _sfree path has to free it from the matching heap. back to the right
     * place. My take: do a _Ring0() call and always allocate from the kernel
     * heap. It'd also let you get rid of the INTR_[UN]LOCK below when inserting
     * into the list.
     */
    new = _smalloc(sizeof(*new));
    if (!new)
        return ENOMEM;
    new->prio = prio;
    new->purger = purger;
    owner = &purge_list;
    for (;;) {
        chk = *owner;
        if (!chk)
            break;
        if (chk->prio > prio)
            break;
        owner = &chk->next;
    }
    if (pcpu_array[boot_cpunum].pc_alive) {
        INTR_LOCK(&purge_slock);
    }
    new->next = *owner;
    *owner = new;
    if (pcpu_array[boot_cpunum].pc_alive) {
        INTR_UNLOCK(&purge_slock);
    }
    return EOK;
}


/* The pril (PRIority List) data structure is a modified doubly linked list. The
 * next pointers are the usual. The prev pointer field is as per usual *except*
 * for the first element on the list of a particular priority (known as a HEAD
 * entry). In that case, what's normally the prev field instead points at the
 * last element on the list of the same priority (the TAIL entry). In this
 * particular implementation, HEAD entries are marked with FLAG_HEAD in the
 * 'type' field, though we could actually store the information in the bottom
 * bit of the 'prev' field if we didn't have another field to put it in. TAIL
 * entries have FLAG_TAIL set in the the 'type' field - we don't really need
 * that bit since a TAIL entry is one with no next entry or a next entry at a
 * different priority, but it simplifies the code in pril_rem() to maintain the
 * information as a bit. In the diagram below, the number in the center of the
 * boxes is the priority of the entry, the lines at the top are the next field
 * and the prev field are the bottom lines. The "H" and "T" on top of the boxes
 * indicate the HEAD and TAIL flags.
 *
 *    H                   T      H T      H          T
 * ### ---> ### ---> ### ---> ### ---> ### ---> ###
 *    #9#      #9#      #9#      #7#      #6#      #6#
 * +- ### <--- ### <--- ###   +- ###   +- ### <--- ###
 * |                    ^     |  ^     |           ^
 * +--------------------+     +--+     +-----------+
 */

#define FLAG_TAIL (TYPE_FLAG_FIRST << 1)
#define FLAG_HEAD (TYPE_FLAG_FIRST << 2)
#define FLAG_UPDATER (TYPE_FLAG_FIRST << 3)

/* The notification and signal queues have the ability to say that they want
 * something inserted at the front, no matter what their priority is :-(. We
 * handle that by setting the TYPE_FLAG_FIRST and treating that as an extra
 * priority bit
 */
#define PRIL_PRIORITY(p) \
    ((p)->priority + (((p)->type & TYPE_FLAG_FIRST) ? NUM_PRI : 0))

static struct pril_update *update_list;

#if defined(CONFIG_DEBUG_PRIL)
static void pril_dump(const char *why, pril_head_t *ph, pril_t *bad)
{
    pril_t *p;
    int i;

    kprintf("\n*** PRIL CORRUPTION: %s ***\n", why);
    kprintf("  ph=%p ph->data=%p bad=%p\n", ph, ph->data, bad);
    if (bad)
        kprintf("  bad: type=0x%x prio=%u next=%p prev=%p\n", bad->type,
                bad->priority, bad->next.pril, bad->prev.pril);
    kprintf("  list walk:\n");
    for (p = ph->data, i = 0; p && i < 16; p = p->next.pril, ++i) {
        unsigned tmask = p->type & 0x0f; /* TYPE_TYPE_MASK */
        const char *tn = (tmask == 0)   ? "THREAD"
                         : (tmask == 1) ? "NOTIFICATION"
                         : (tmask == 3) ? "VTHREAD"
                         : (tmask == 6) ? "SYNCEVENT"
                                        : "??";
        kprintf("    [%d] %p %s type=0x%x prio=%u next=%p prev=%p", i, p, tn,
                p->type, p->priority, p->next.pril, p->prev.pril);
        if (tmask == 0) {
            /* Best-effort: if type tag is 0 but it *claims* to be a thread,
             * pid/tid are at stable offsets.
             */
            thread_t *thp = (thread_t *) p;
            kprintf(" tid=%u pid=%u state=%u", thp->tid + 1,
                    thp->process ? thp->process->pid : 0, thp->state);
        }
        kprintf("\n");
    }
    if (p)
        kprintf("    ...(truncated)\n");
}

void pril_validate(pril_head_t *ph, pril_t *present, pril_t *not_present)
{
    pril_t **owner;
    pril_t *p;
    pril_t *n;

    owner = &ph->data;
    for (;;) {
        p = *owner;
        if (!p)
            break;
        if (p == not_present) {
            pril_dump("expected-absent entry is present", ph, p);
            crash();
        }
        if (p == present) {
            present = NULL;
        }
        if (p->type & FLAG_HEAD) {
            n = p->prev.prio_tail;
            if (PRIL_PRIORITY(p) != PRIL_PRIORITY(n)) {
                pril_dump("HEAD prev.prio_tail priority mismatch", ph, p);
                crash();
            }
        } else {
            if (p->prev.pril != owner) {
                pril_dump("non-HEAD prev.pril != owner", ph, p);
                crash();
            }
        }
        n = p->next.pril;
        if (p->type & FLAG_TAIL) {
            if (n && PRIL_PRIORITY(p) <= PRIL_PRIORITY(n)) {
                pril_dump("TAIL next has >= priority", ph, p);
                crash();
            }
        } else {
            /* Not-TAIL must have a successor at same priority. */
            if (!n) {
                pril_dump("non-TAIL with NULL next", ph, p);
                crash();
            }
            if (PRIL_PRIORITY(p) != PRIL_PRIORITY(n)) {
                pril_dump("non-TAIL priority mismatch with next", ph, p);
                crash();
            }
        }
        owner = &p->next.pril;
    }

    /* There was either no verification entry to check, or it was on the list if
     * !present.
     */
    if (present) {
        pril_dump("expected-present entry is missing", ph, present);
        crash();
    }
}
#else
#define pril_validate(h, p, np)
#endif


#if !defined(pril_first)
void *rdecl pril_first(pril_head_t *ph)
{
    return ph->data;
}
#endif


#if !defined(pril_next)
void *rdecl pril_next(void *p)
{
    return ((pril_t *) p)->next.pril;
}
#endif


void *rdecl pril_next_prio(void *d)
{
    pril_t *p = d;

    CRASHCHECK(!(p->type & FLAG_HEAD));
    return p->prev.prio_tail->next.pril;
}


void *rdecl pril_find_insertion(pril_head_t *ph, unsigned priority)
{
    pril_t *head;
    pril_t *tail;
    pril_t **owner;
    unsigned n_prio;
    unsigned h_prio;

    pril_validate(ph, NULL, NULL);
    n_prio = (uint8_t) priority;
    if (priority & _NOTIFICATION_PRIO_BOOST)
        n_prio += NUM_PRI;
    owner = &ph->data;
    for (;;) {
        head = *owner;
        if (!head)
            break;
        h_prio = PRIL_PRIORITY(head);
        if (h_prio < n_prio)
            break;
        tail = head->prev.prio_tail;
        if (h_prio == n_prio) {
            return (priority & _NOTIFICATION_PRIO_HEAD) ? head : tail;
        }
        owner = &tail->next.pril;
    }
    return NULL;
}


void rdecl pril_add(pril_head_t *ph, void *d)
{
    pril_t *new = d;
    pril_t *head;
    pril_t *tail;
    pril_t **owner;
    unsigned n_prio;
    unsigned h_prio;

    pril_validate(ph, NULL, new);
    n_prio = PRIL_PRIORITY(new);
    owner = &ph->data;
    for (;;) {
        head = *owner;
        if (!head)
            goto new_level;
        h_prio = PRIL_PRIORITY(head);
        if (h_prio < n_prio)
            goto new_level;
        tail = head->prev.prio_tail;
        if (h_prio == n_prio)
            break;
        owner = &tail->next.pril;
    }

    /* add to current level */
    head->prev.prio_tail = new;
    new->next.pril = tail->next.pril;
    new->type = (new->type &(uint8_t) ~FLAG_HEAD) | FLAG_TAIL;
    tail->type &= (uint8_t) ~FLAG_TAIL;
    tail->next.pril = new;
    new->prev.pril = &tail->next.pril;
    pril_validate(ph, new, NULL);
    return;

new_level:
    /* create new level */
    new->next.pril = head;
    new->type |= FLAG_HEAD | FLAG_TAIL;
    new->prev.prio_tail = new;
    *owner = new;
    pril_validate(ph, new, NULL);
    return;
}


void rdecl pril_rem(pril_head_t *ph, void *d)
{
    pril_t *p = d;
    pril_t *next;
    pril_t *t;
    pril_t *head;
    pril_t *tail;
    pril_t **owner;
    unsigned f;
    unsigned p_prio;

    pril_validate(ph, p, NULL);
    next = p->next.pril;
    f = p->type;
    if (f & (FLAG_HEAD | FLAG_TAIL)) {
        p_prio = PRIL_PRIORITY(p);
        owner = &ph->data;
        for (;;) {
            head = *owner;
            if (PRIL_PRIORITY(head) == p_prio)
                break;
            tail = head->prev.prio_tail;
            owner = &tail->next.pril;
        }
        if (!(f & FLAG_TAIL)) {
            /* just removing a head */
            *owner = next;
            next->prev.prio_tail = p->prev.prio_tail;
            next->type |= FLAG_HEAD;
        } else if (!(f & FLAG_HEAD)) {
            /* just removing a tail */
            owner = p->prev.pril;
            *owner = next;
            t = (pril_t *) ((uintptr_t) owner - offsetof(pril_t, next.pril));
            head->prev.prio_tail = t;
            t->type |= FLAG_TAIL;
        } else {
            /* totally removing a level */
            *owner = next;
        }
        p->type &= (uint8_t) ~(FLAG_HEAD | FLAG_TAIL);
    } else {
        owner = p->prev.pril;
        next->prev.pril = owner;
        *owner = next;
    }
    pril_validate(ph, NULL, p);
    if (f & FLAG_UPDATER) {
        struct pril_update *upp;

        /* We're deleting an entry that somebody is using - update to the next
         * one
         */
        for (upp = update_list; upp; upp = upp->next) {
            if (upp->pril == p) {
                upp->pril = next;
                next->type |= FLAG_UPDATER;
            }
        }
    }
}


void rdecl pril_update_register(pril_head_t *ph, struct pril_update *upp)
{
    struct pril_update *prev;
    struct pril_update *chk;
    pril_t *pril;

    pril = upp->pril;
    prev = NULL;
    chk = update_list;
    for (;;) {
        if (!chk)
            break;

        /* We're keeping the list sorted so that we can keep all updaters
         * working on the same pril together. That way it's easier for the
         * pril_update_unregister routine to know when to turn off FLAG_UPDATER.
         */
        if ((uintptr_t) chk->pril >= (uintptr_t) pril)
            break;
        prev = chk;
        chk = chk->next;
    }
    upp->next = chk;
    upp->prev = prev;
    if (chk) {
        chk->prev = upp;
    }
    if (prev) {
        prev->next = upp;
    } else {
        update_list = upp;
    }
    if (pril) {
        pril->type |= FLAG_UPDATER;
    }
}


void rdecl pril_update_unregister(pril_head_t *ph, struct pril_update *upp)
{
    struct pril_update *next;
    struct pril_update *prev;
    pril_t *pril;
    int more_updaters = 0;

    pril = upp->pril;
    next = upp->next;
    prev = upp->prev;

    if (next) {
        next->prev = prev;
        if (next->pril == pril)
            more_updaters = 1;
    }
    if (prev) {
        prev->next = next;
        if (prev->pril == pril)
            more_updaters = 1;
    } else {
        update_list = next;
    }
    if (pril && !more_updaters) {
        pril->type &= (uint8_t) ~FLAG_UPDATER;
    }
}
