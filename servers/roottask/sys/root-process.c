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

    /* Wipe stale process state; the proc-mux lock belongs to the slot. */
    pthread_mutex_lock(&slot_lock);
    root_process_clear(tmp);
    tmp->pid = pid;
    tmp->parent_pid = parent_pid;
    pthread_mutex_unlock(&slot_lock);
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

    /* Idempotent: callers (proc_exit) sometimes hit the destroy path twice on a
     * swap-during-exit sequence. Only clear if the slot still claims this pid;
     * stale entries belong to whoever took over the slot.
     */
    pthread_mutex_lock(&slot_lock);
    if (tmp->pid == pid) {
        free(tmp->debug_name); /* tolerates NULL */
        if (tmp->session && atomic_sub_value(&tmp->session->links, 1) == 1) {
            /* The last member is gone: release the controlling terminal
             * descriptor, if one was bound, and the session itself.
             */
            if (tmp->session->fd >= 0)
                (void) close(tmp->session->fd);
            free(tmp->session);
        }
        memcap_process_fini(tmp);
        root_process_clear(tmp);
    }
    pthread_mutex_unlock(&slot_lock);
}
