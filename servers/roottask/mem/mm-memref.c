/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include "as.h"
#include "root-process.h"



/* static soul_t or_souls INITSOUL(-1, struct mm_object_ref, 32, 8, ~0);
 * SMP_SPINVAR(static, ref_spin); static struct mm_object_ref *ref_free_list;
 */


void memref_walk_restart(struct mm_map_head *mh)
{
    atomic_add(&mh->walk_gen, 1);
}


int memref_add(struct mm_map *mm,
               path_object_t *obp,
               pid_t pid,
               address_t *adp,
               int fd)
{
    struct mm_object_ref **owner;
    struct mm_object_ref * or ;
    struct mm_map *next;

    VERIFY_OBJ_LOCK(obp);

    /* RUSH3: cache last N results for faster operation? RUSH3: keep all the
     * same 'adp' values grouped together so we can bail RUSH3: from the list
     * faster? Maybe vis-versa and group 'fd' together?
     */
    owner = &obp->mem.mm.refs;
    for (;;) {
        or = *owner;
        if (!or)
            break;
        if ((or->adp == adp) && (or->fd == fd))
            goto found_it;
        owner = &or->next;
    }

    /* RUSH3: object allocator is slightly slower than straight _smalloc RUSH3:
     * keep our own free list? - that's slower as well for some reason.
     */
    /* or = proc_object_alloc(&or_souls); INTR_LOCK(&ref_spin); or =
     * ref_free_list; if(or) { ref_free_list = or->next;
     *      INTR_UNLOCK(&ref_spin);
     * } else {
     *      INTR_UNLOCK(&ref_spin);
     */
    or = _smalloc(sizeof(*or));
    if (!or)
        return ENOMEM;
    /*  } */
    or->next = *owner;
    *owner = or ;
    or->fd = fd;
    or->adp = adp;
    or->pid = pid;
    or->obp = obp;
    or->first_ref = NULL;

found_it:
    mm->obj_ref = or ;
    next = or->first_ref;
    mm->ref.next = next;
    if (next) {
        next->ref.owner = &mm->ref.next;
    }
    mm->ref.owner = &or->first_ref;
    or->first_ref = mm;
    /* Walk-gen is in root_proc(pid)->map.walk_gen now. */
    {
        extern void memref_walk_restart(struct mm_map_head * mh);
        struct mm_map_head *mh = (mm->obj_ref && mm->obj_ref->pid)
                                     ? &(root_proc(mm->obj_ref->pid))->map
                                     : NULL;
        if (mh)
            memref_walk_restart(mh);
    }
    return EOK;
}


void memref_del(struct mm_map *del_mm)
{
    struct mm_map *mm;
    struct mm_object_ref * or ;

    /* Don't have to worry about the reference list changing on us - we should
     * always have the as locked in this function.
     */
    or = del_mm->obj_ref;
    if (or) {
        mm = del_mm->ref.next;
        *del_mm->ref.owner = mm;
        if (mm) {
            mm->ref.owner = del_mm->ref.owner;
        }

        if (!or->first_ref) {
            struct mm_object_ref **owner;
            struct mm_object_ref *chk;
            path_object_t *obp;

            obp = or->obp;
            VERIFY_OBJ_LOCK(obp);

            /* RUSH3: cache last N results for faster operation? reorder list?
             * RUSH3: double link list?
             */
            owner = &obp->mem.mm.refs;
            for (;;) {
                chk = *owner;
                CRASHCHECK(!chk);
                if (chk == or)
                    break;
                owner = &chk->next;
            }
            *owner = chk->next;
            del_mm->obj_ref = NULL;
            /*          proc_object_free(&or_souls, or); */
            _sfree(or, sizeof(*or));

            /*          INTR_LOCK(&ref_spin);
             *          or->next = ref_free_list;
             *          ref_free_list = or;
             *          INTR_UNLOCK(&ref_spin);
             */
        }
    }
}


/* This function has to be able to traverse the reference list without locking
 * the address space(s) - the object is locked however.
 */
int memref_walk(path_object_t *obp,
                int (*func)(struct mm_object_ref *, struct mm_map *, void *),
                void *d)
{
    struct mm_object_ref * or ;
    struct mm_map *mm;
    int r;
    struct mm_map_head *mh;
    unsigned wg;

    VERIFY_OBJ_LOCK(obp);
    r = EOK;
    for (or = obp->mem.mm.refs; or ; or = or->next) {
        mh = (or->pid != 0) ? &(root_proc(or->pid))->map : NULL;
        if (!mh)
            continue;
        mm = NULL;
        wg = mh->walk_gen - 1; /* force a restart */
        for (;;) {
            pthread_mutex_lock(&map_mutex);
            if (mm)
                mm->inuse = 0;
            if (mh->walk_gen != wg) {
                wg = mh->walk_gen;
                mm = or->first_ref;
            } else {
                CRASHCHECK(!mm);
                mm = mm->ref.next;
            }
            if (mm)
                mm->inuse = 1;
            pthread_mutex_unlock(&map_mutex);
            if (!mm)
                break;
            CRASHCHECK(mm->obj_ref != or);
            r = func(or, mm, d);
            if (r != EOK) {
                mm->inuse = 0;
                if (r < 0)
                    r = EOK;
                return r;
            }
        }
    }
    return EOK;
}
