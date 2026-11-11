/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/memmsg.h>

struct kcall_args_stack_cont {
    pid_t pid;
    int tid;
    int err;
    struct _thread_attr *attr;
};

/* Resume a thread waiting for a stack to be allocated (PRIV kernel extension
 * body).
 *
 * Looks up the target (pid, tid) in STATE_STACK and either fails it with an
 * errno (marking it for self-kill) or records the allocated stack address /
 * size before readying it. prp->vec_slock guards the lookup and the thp field
 * mutations; ready() takes its own sched_slock, so this lock is dropped before
 * calling it (avoiding a PROCVEC-over-SCHED rank inversion). Parameters:
 *   data : pointer to a struct kcall_args_stack_cont (pid, tid, err, attr)
 */
void kext_stack_cont(void *data)
{
    struct kcall_args_stack_cont *kap = data;
    process_t *prp;
    thread_t *act = get_active_thread(get_cpunum()), *thp;
    int status;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    rcu_read_lock(&tthread_rcu);
    kspinlock_lock(&prp->vec_slock);
    if ((thp = vector_lookup2(&prp->threads, (unsigned) (kap->tid - 1))) ==
            NULL ||
        (thp->state != STATE_STACK)) {
        kspinlock_unlock(&prp->vec_slock);
        rcu_read_unlock(&tthread_rcu);
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = tthread_pin(thp);
    if (status != EOK) {
        kspinlock_unlock(&prp->vec_slock);
        rcu_read_unlock(&tthread_rcu);
        rcu_read_unlock(&process_rcu);
        kcall_err(act, status);
        return;
    }

    if (kap->err) {
        thp->status = (void *) (intptr_t) kap->err;
        atomic_or(&thp->flags, (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));
    } else {
        struct _thread_attr *attr = kap->attr;

        thp->un.lcl.stackaddr = attr->stackaddr;
        thp->un.lcl.stacksize = attr->stacksize;
        if (thp->flags & RTMUX_FLG_THR_WAAA) {
            atomic_or(&thp->flags, RTMUX_FLG_THR_ALLOCED_STACK);
        } else {
            atomic_clr(&thp->flags, RTMUX_FLG_THR_ALLOCED_STACK);
        }
    }
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&tthread_rcu);
    rcu_read_unlock(&process_rcu);
    ready(thp);
    tthread_unpin(thp);
    SETKSTATUS(act, EOK);
}

/* stack_cont wrapper: servers/roottask/proc/kext-wrappers.c */

struct kcall_args_stack_wait_info {
    union sigval value;
    pid_t *ppid;
    int *ptid;
    struct _thread_attr *attr;
};

/* Retrieve a stack-waiting thread's saved attributes (kernel extension body).
 *
 * Decodes the (process-index, tid) pair from the sync value, and if the
 * matching thread is in STATE_STACK, copies out its stack address / size and
 * thread-attr flags. process_rcu covers the process_vector read and the field
 * reads on thp; prp->vec_slock covers the inner thp lookup. Parameters:
 *   data : pointer to a struct kcall_args_stack_wait_info (in: value;
 *          out: ppid, ptid, attr)
 */
void kext_stack_wait_info(void *data)
{
    struct kcall_args_stack_wait_info *kap = data;
    int id;
    process_t *prp;
    thread_t *thp;
    int ret;

    rcu_read_lock(&process_rcu);
    ret = -1;
    if ((id = SYNC_PINDEX(kap->value.sival_int)) < process_vector.nentries &&
        (prp = VECP(prp, &process_vector, id)) &&
        SYNC_PINDEX(kap->value.sival_int) == PINDEX(prp->pid)) {
        kspinlock_lock(&prp->vec_slock);
        if ((thp = vector_lookup2(&prp->threads,
                                  SYNC_TID(kap->value.sival_int))) &&
            (thp->state == STATE_STACK)) {
            struct _thread_attr *attr = kap->attr;

            *kap->ppid = prp->pid;
            *kap->ptid = (int) (thp->tid + 1);
            attr->stackaddr = thp->un.lcl.stackaddr;
            attr->stacksize = thp->un.lcl.stacksize;
            if (thp->flags & RTMUX_FLG_THR_WAAA) {
                attr->flags = (int) (uintptr_t) thp->next.thread;
                attr->guardsize = (unsigned) (uintptr_t) thp->prev.thread;
                attr->prealloc = (unsigned) (uintptr_t) thp->status;
            } else {
                attr->flags = 0;
                attr->prealloc = 0;
                attr->guardsize = ~0U;
            }

            ret = 0;
        }
        kspinlock_unlock(&prp->vec_slock);
    }
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(get_active_thread(get_cpunum()), ret);
}

/* stack_wait_info wrapper: servers/roottask/proc/kext-wrappers.c */
