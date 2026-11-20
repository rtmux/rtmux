/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Backing storage for root_process[] — the roottask-side per-process state
 * array that holds fields previously parked in the kernel-half process_t (root,
 * cwd, events, wap, debug_waiting).
 *
 * See servers/roottask/include/root-process.h for the rationale.
 */

#include <stddef.h>
#include <string.h>
#include <stdlib.h> /* free() — for tmp->debug_name on fini */
#include "externs.h"
#include "root-process.h"

/* BSS-resident: 4096 * sizeof(root_process_t) ≈ 192 KiB of zeros at the end of
 * roottask's data segment. Cheap and avoids any allocator dependency for the
 * per-pid slot path.
 */
root_process_t root_process[ROOT_PROCESS_MAX];

/* root_process_clear zeroes everything except `lock` by clearing the two byte
 * ranges around it; this assert keeps that reasoning honest if the struct is
 * reordered so `lock` is no longer the single slot-persistent pointer member.
 */
_Static_assert(offsetof(root_process_t, parent_pid) +
                       sizeof(((root_process_t *) 0)->parent_pid) ==
                   offsetof(root_process_t, lock),
               "unexpected padding before root_process_t.lock");

/* Serializes slot init against slot fini. The kernel frees a pid's index when
 * it destroys the process, which is before roottask calls root_process_fini for
 * it, so a new process can be created at the same index (pid + 4096) while the
 * old generation's fini is still running. Without this lock that fini, having
 * already seen its own tag, clears the slot out from under the new process's
 * init and leaves a live process whose roottask tag is 0.
 */
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;

static void root_process_clear(root_process_t *tmp)
{
    size_t tail = offsetof(root_process_t, lock) + sizeof(tmp->lock);

    memset(tmp, 0, offsetof(root_process_t, lock));
    memset((char *) tmp + tail, 0, sizeof(*tmp) - tail);
}

/* Claim and initialize the root_process[] slot for a pid.
 *
 * Clears any stale state left by a previous generation and records the pid and
 * its parent. Parameters:
 *   pid        : the process id whose slot to initialize
 *   parent_pid : the parent's pid (0 for roottask itself)
 */
void root_process_init(pid_t pid, pid_t parent_pid)
{
    root_process_t *tmp = root_proc(pid);

    /* Wipe stale process state; the proc-mux lock belongs to the slot. A
     * previous generation whose fini has not run yet leaves its group first, or
     * its count would never end.
     */
    pthread_mutex_lock(&slot_lock);
    if (tmp->pid)
        proc_pgrp_reap(tmp->pid);
    root_process_clear(tmp);
    tmp->pid = pid;
    tmp->parent_pid = parent_pid;
    tmp->pgrp = pid;
    pthread_mutex_unlock(&slot_lock);
}

/* The process group of p. A slot starts in its own group (root_process_init),
 * so the pid fallback only covers a slot that was never initialized.
 */
pid_t root_process_pgrp(const root_process_t *p)
{
    return p->pgrp ? p->pgrp : p->pid;
}

/* The session id of p: its session's leader, or the system session's. */
pid_t root_session_leader(const root_process_t *p)
{
    return p->session ? p->session->leader : ROOTTASK_PID;
}

/* Take one link on the session of process pid, and report its session id, in
 * one hold of slot_lock: root_process_fini clears the slot and takes its
 * session away under the same lock, so a parent that is being torn down cannot
 * have its session freed between the read and the link. A process that no
 * longer owns its slot, or has no session, is in the system session: NULL, and
 * *sid is roottask's; its group is then its own pid. The caller drops the link
 * with proc_session_unref.
 */
session_t *root_session_link_of(pid_t pid, pid_t *sid, pid_t *pgid)
{
    root_process_t *p = root_proc(pid);
    session_t *sep = NULL;

    *sid = ROOTTASK_PID;
    *pgid = pid;
    pthread_mutex_lock(&slot_lock);
    if (p->pid == pid) {
        /* The group and the session of one owner of the slot, read together. */
        *pgid = root_process_pgrp(p);
        if (p->session) {
            sep = p->session;
            *sid = sep->leader;
            (void) atomic_add_value(&sep->links, 1);
        }
    }
    pthread_mutex_unlock(&slot_lock);
    return sep;
}

/* The system session. It is static, so that creating it cannot fail and no
 * process is ever without a session object. One link is roottask's own; the
 * other is never dropped, so that an unbalanced release cannot free a static
 * object. Roottask never exits, so it is never freed.
 */
static session_t boot_session;

/* Make pid the leader of the system session and of its own group, with no
 * controlling terminal; its children inherit both. Boot does this for roottask,
 * so that the session exists before any process is created; the leader's
 * SLEADER flag is not set, because roottask never calls setsid or setpgid.
 */
void root_process_boot_session(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);

    boot_session.links = 2; /* roottask's, and one that is never dropped */
    boot_session.leader = pid;
    boot_session.pgrp = boot_session.fd = -1;
    pthread_mutex_lock(&slot_lock);
    tmp->session = &boot_session;
    pthread_mutex_unlock(&slot_lock);
    if (proc_pgrp_boot(pid) != EOK)
        crash();
}

/* Release the root_process[] slot for a pid.
 *
 * Idempotent: only clears the slot if it still claims this pid, so a double
 * destroy (or a slot already reused) is harmless. Parameters:
 *   pid : the process id whose slot to release
 */
void root_process_fini(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    session_t *sep = NULL;

    /* Idempotent: callers (proc_exit) sometimes hit the destroy path twice on a
     * swap-during-exit sequence. Only clear if the slot still claims this pid;
     * stale entries belong to whoever took over the slot.
     */
    pthread_mutex_lock(&slot_lock);
    if (tmp->pid == pid) {
        free(tmp->debug_name); /* tolerates NULL */
        proc_pgrp_reap(pid); /* a no-op for a child its waiter already reaped */
        sep = tmp->session;  /* its link is dropped below, outside the lock */
        memcap_process_fini(tmp);
        root_process_clear(tmp);
    }
    pthread_mutex_unlock(&slot_lock);
    if (sep)
        proc_session_unref(sep);
}
