/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

struct kcall_args_cred_dirty {
    pid_t pid;
};

/* Mark a process's credentials as dirty (PRIV kernel extension body).
 *
 * Looks up the process by pid and bumps its credential generation. cred_slock
 * guards the cred-generation bump. Parameters:
 *   data : pointer to a struct kcall_args_cred_dirty (carries the pid)
 */
void kext_cred_dirty(void *data)
{
    struct kcall_args_cred_dirty *kap = data;
    process_t *prp;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(get_active_thread(get_cpunum()), ESRCH);
        return;
    }
    kspinlock_lock(&cred_slock);
    cred_dirty(prp);
    kspinlock_unlock(&cred_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(get_active_thread(get_cpunum()), 0);
}

struct kcall_args_cred_get {
    pid_t pid;
    struct _cred_info *cip;
};

/* Snapshot a process's credential info (PRIV kernel extension body).
 *
 * Looks up the process by pid and copies its _cred_info into the
 * caller-supplied buffer. cred_slock guards the cred->info snapshot.
 * Parameters:
 *   data : pointer to a struct kcall_args_cred_get (pid + destination buffer)
 */
void kext_cred_get(void *data)
{
    struct kcall_args_cred_get *kap = data;
    process_t *prp;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(get_active_thread(get_cpunum()), ESRCH);
        return;
    }
    kspinlock_lock(&cred_slock);
    *kap->cip = prp->cred->info;
    kspinlock_unlock(&cred_slock);
    rcu_read_unlock(&process_rcu);

    SETKSTATUS(get_active_thread(get_cpunum()), 0);
}

/* cred_get_call wrapper: servers/roottask/proc/kext-wrappers.c */


struct kcall_args_cred_set {
    pid_t pid;
    struct _cred_info *cip;
};


/* Replace a process's credentials (PRIV kernel extension body).
 *
 * Looks up the process by pid, marks its credentials dirty, and installs the
 * supplied _cred_info. cred_slock covers cred_dirty + cred_set (cred_set walks
 * credential_list internally). Parameters:
 *   data : pointer to a struct kcall_args_cred_set (pid + source _cred_info)
 */
void kext_cred_set(void *data)
{
    struct kcall_args_cred_set *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    int status;
    process_t *prp;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    kspinlock_lock(&cred_slock);
    cred_dirty(prp);
    status = cred_set(&prp->cred, kap->cip);
    kspinlock_unlock(&cred_slock);
    process_unpin(prp);
    SETKSTATUS(act, status);
}

/* cred_set_call wrapper: servers/roottask/proc/kext-wrappers.c */
