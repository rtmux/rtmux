/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * exec — replace the image of the calling process.
 *
 * execv() sends an ordinary spawn flagged SPAWN_EXEC. The spawn runs as for any
 * other: a child process, the loader shell, is created, and its loader thread
 * maps the new image into the shell's address space, builds the stack, and
 * sends _PROC_SPAWN_EXEC where a spawn would send _PROC_SPAWN_DONE. Two threads
 * are now held in a reply by roottask: the loader thread (message of this
 * handler) and the thread that called exec (lcp->ctx.rcvid, the spawn request).
 * proc_exec_swap() finishes the job:
 *
 *   1. every other thread of the caller is destroyed (kernel EXEC_BEGIN);
 *   2. what the old image owned is released: event registrations, timers,
 *      channels, close-on-exec descriptors, hardware resources, its address
 *      space; the pid, the other descriptors and the process's place in its
 *      group, session and parent stay;
 *   3. the shell's address space and its roottask-side map state are handed to
 *      the caller (EXEC_QUIESCE, EXEC_COMMIT, adopt_address_space);
 *   4. the calling thread is replied to and starts the new image; the shell,
 *      now without an address space, is retired like a child that died while
 *      loading.
 *
 * Up to the first step a failure is an ordinary failed exec: the caller is
 * answered with the error and nothing of it has changed. From the first step on
 * the old image is being taken apart, so a failure kills the process.
 */

#include "externs.h"
#include <sys/procmsg.h>
#include <private/root-proc-info.h>
#include "private.h"
#include "root-process.h"
#include "as.h"

extern sync_t map_mutex;
void memref_walk_restart(struct mm_map_head *mh);

/* Sleep a millisecond between the retries of a kernel step that is waiting for
 * a thread to leave (the same pacing as the other teardown calls).
 */
static void exec_nap(void)
{
    static const struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};

    (void) nanosleep(&ts, NULL);
}

/* One step of the swap in the kernel, retried while it asks for it (EAGAIN: a
 * thread that must leave first is held by something that needs this hart).
 * Returns EOK or an errno.
 */
static int exec_step(unsigned op,
                     pid_t ppid,
                     int lrcvid,
                     int crcvid,
                     pid_t shell,
                     uint64_t caps)
{
    unsigned tries = 0;

    while (process_exec(op, ppid, lrcvid, crcvid, shell, caps) == -1) {
        if (errno != EAGAIN)
            return errno;
        if (++tries % 1000 == 0)
            root_warn("exec: pid %d: step %u still waiting after %u tries\n",
                      (int) ppid, op, tries);
        exec_nap();
    }
    return EOK;
}

/* Retire the shell: it dies like a child that failed while loading, so the
 * ordinary termination path (events, descriptors, its loader context, the
 * roottask slot) cleans up after it. It is handed to roottask first, so that
 * its death is never reported to the process that execed.
 */
static void exec_retire_shell(pid_t shell_pid)
{
    process_t *prp;
    int started;

    if (!(prp = root_proc_lock(shell_pid)))
        return;
    process_reparent(shell_pid);
    started = proc_exit_begin_locked(shell_pid, 0, 0);
    root_proc_unlock(shell_pid);
    if (started)
        proc_exit_finish(shell_pid);
}

/* The exec is given up after the old image was touched: the process dies, as if
 * killed. Its caller thread is among those destroyed.
 */
static void exec_kill_process(pid_t pid)
{
    process_t *prp;
    int started;

    if (!(prp = root_proc_lock(pid)))
        return;
    started = proc_exit_begin_locked(pid, SIGKILL, 1);
    root_proc_unlock(pid);
    if (started)
        proc_exit_finish(pid);
}

/* Close the descriptors of the old image that are marked close-on-exec. */
static void exec_close_cloexec(process_t *prp, pid_t pid)
{
    struct _server_info sinfo;
    int base = 0;

    while (connect_server_info(pid, (uint64_t) base, &sinfo) != -1 &&
           !((unsigned) sinfo.coid & RTMUX_SIDE_CHANNEL)) {
        base = sinfo.coid + 1;
        if (sinfo.flags & RTMUX_FLG_COF_CLOEXEC)
            (void) process_connect_detach(prp, sinfo.coid);
    }
}

/* Release what the old image owned that is not the process's own: the same
 * pieces proc_terminate frees at the end of a process, less the descriptors,
 * the connections, the roottask slot and everything that names the process
 * rather than its image. The caller holds the process lock during teardown.
 */
static void exec_release_old_image(process_t *prp, pid_t pid)
{
    unsigned i = -1U;

    proc_event_destroy(pid);
    (void) ProcessDestroyChancons(pid, TYPE_CHANNEL);
    while (query_object(_QUERY_PROCESS, (unsigned) pid, _QUERY_PROCESS_TIMERS,
                        ++i, &i, 0, 0)) {
        process_timer_destroy(prp, (int) i);
    }
    exec_close_cloexec(prp, pid);
    roottask_sysconf_destroy(pid);
    hwres_destroy_process(pid);
}

/* Unmap the old image and shut its address space down, as the end of a process
 * does. The process keeps a thread, which has no address space until the
 * commit.
 *
 * Returns EOK, or an errno when the process has gone.
 */
static int exec_drop_old_address_space(process_t *prp, pid_t pid)
{
    root_proc_info_t qinfo;

    if (process_query_info(pid, &qinfo) != EOK)
        return ESRCH;
    if (!qinfo.has_memory)
        return EOK;

    if (proc_wlock_adp(pid) != -1) {
        proc_lock_owner_mark(pid);
        root_vm_ops.munmap(pid, prp, 0, ~(size_t) 0, 0);
        as_mdestroy_pid(pid);
        proc_unlock_adp(pid);
    }
    proc_wlock_adp(ROOTTASK_PID);
    while (process_shutdown(prp, 1) == -1 && errno == EAGAIN)
        ;
    proc_unlock_adp(ROOTTASK_PID);
    return EOK;
}

/* Give the shell's address-space state to the process: the map list, the
 * anonymous backing object, the accounting and the page-table handle, and with
 * them each memory reference that names the shell, which must name the process
 * from now on. Called with the address-space locks of both held; the shell is
 * left with an empty map, as a process whose address space was freed.
 */
static void exec_adopt_address_space(pid_t pid, pid_t shell)
{
    root_process_t *dst = root_proc(pid);
    root_process_t *src = root_proc(shell);
    struct mm_map *mm;

    pthread_mutex_lock(&map_mutex);
    dst->map = src->map;
    dst->map.lock = 0;
    dst->rlimit = src->rlimit;
    dst->anon = src->anon;
    dst->tmap_base = src->tmap_base;
    dst->tmap_size = src->tmap_size;
    dst->adp = src->adp;
    for (mm = dst->map.head; mm; mm = mm->next) {
        if (mm->obj_ref) {
            mm->obj_ref->pid = pid;
            mm->obj_ref->adp = dst->adp;
        }
    }
    memset(&src->map, 0, sizeof src->map);
    memset(&src->rlimit, 0, sizeof src->rlimit);
    src->anon = NULL;
    src->tmap_base = 0;
    src->tmap_size = 0;
    src->adp = NULL;
    pthread_mutex_unlock(&map_mutex);

    /* A walk of an object's references in progress starts again, on the
     * addresses they have now.
     */
    memref_walk_restart(&dst->map);
    memref_walk_restart(&src->map);

    /* The path the image was loaded from names the process from now on. */
    free(dst->debug_name);
    dst->debug_name = src->debug_name;
    src->debug_name = NULL;
}

/* exec's last step for a process that survived to it.
 *
 * Returns EOK, or an errno for the caller to kill the process with.
 */
static int exec_install(struct loader_context *lcp,
                        pid_t pid,
                        pid_t shell,
                        int lrcvid)
{
    int rc;

    /* As in the shutdown of an address space: roottask's own address space is
     * held steady while harts are taken off another's.
     */
    proc_wlock_adp(ROOTTASK_PID);
    rc = exec_step(EXEC_QUIESCE, pid, lrcvid, lcp->ctx.rcvid, shell, 0);
    proc_unlock_adp(ROOTTASK_PID);
    if (rc != EOK)
        return rc;

    /* The loader decided on the new image's authority (the bootstrap manifest
     * grant of the image it opened), as for any spawn.
     */
    root_proc(pid)->system = lcp->ctx.grant_system != 0;

    (void) proc_wlock_adp(pid);
    (void) proc_wlock_adp(shell);
    rc = exec_step(EXEC_COMMIT, pid, lrcvid, lcp->ctx.rcvid, shell,
                   lcp->ctx.grant_caps);
    if (rc == EOK) {
        exec_adopt_address_space(pid, shell);

        /* What the old image told terminal servers about SIGTTIN and SIGTTOU is
         * its own; the new image reports its own state when it starts, from the
         * signals it inherits.
         */
        root_proc(pid)->tty_hold = 0;
    }
    proc_unlock_adp(shell);
    proc_unlock_adp(pid);
    return rc;
}

/* proc_exec_swap — the new image is loaded: make it the caller's.
 *
 * Parameters:
 *   ctp : the loader thread's _PROC_SPAWN_EXEC message
 *   lcp : the loader context of the shell
 * Return value: IOSERV_NOREPLY. Every thread this answers is answered here: the
 * caller by the reply that starts the new image (or the error that says why
 * not), the loader thread never, as it is destroyed with the shell.
 */
int proc_exec_swap(ioserv_context_t *ctp, struct loader_context *lcp)
{
    pid_t shell = ctp->info.pid;
    pid_t pid = lcp->ctx.ppid;
    int lrcvid = ctp->rcvid;
    int crcvid = lcp->ctx.rcvid;
    process_t *prp;
    int rc;

    /* The first step takes every other thread down; it fails, leaving the
     * caller as it was, when the caller is not where it must be.
     */
    rc = exec_step(EXEC_BEGIN, pid, lrcvid, crcvid, 0, 0);
    if (rc != EOK) {
        lcp->ctx.rcvid = -1;
        exec_retire_shell(shell);
        msg_error(crcvid, rc);
        return IOSERV_NOREPLY;
    }

    /* BEGIN has made the change irreversible. Serialize the exec mark with
     * setpgid before the kernel makes the new image runnable.
     */
    proc_pgrp_lock();
    root_proc(pid)->execed = 1;
    proc_pgrp_unlock();

    /* From here the old image is being taken apart. The process stays locked
     * for it, as it is for the end of any process, so that a kill or a wait
     * that comes meanwhile sees the process before or after the swap.
     */
    if (!(prp = root_proc_lock(pid))) {
        exec_retire_shell(shell);
        return IOSERV_NOREPLY;
    }

    /* The process may have begun to end between the first step and the lock (a
     * SIGKILL through signal_one, or a fault): its threads are then being
     * destroyed, so there is no image left to replace.
     */
    {
        root_proc_info_t qinfo;

        if (root_proc(pid)->exit_begun ||
            process_query_info(pid, &qinfo) != EOK ||
            (qinfo.flags &
             (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL))) {
            root_proc_unlock(pid);
            lcp->ctx.rcvid = -1;
            exec_retire_shell(shell);
            return IOSERV_NOREPLY;
        }
    }
    exec_release_old_image(prp, pid);
    if ((rc = exec_drop_old_address_space(prp, pid)) == EOK)
        rc = exec_install(lcp, pid, shell, lrcvid);
    root_proc_unlock(pid);
    if (rc != EOK) {
        root_warn("exec: pid %d: cannot start the new image: %s\n", (int) pid,
                  strerror(rc));
        lcp->ctx.rcvid = -1;
        exec_kill_process(pid);
        exec_retire_shell(shell);
        return IOSERV_NOREPLY;
    }

    /* The shell goes first, so that it is no child of the caller by the time
     * the caller can wait for children; then the thread that called exec runs
     * the new image.
     */
    lcp->ctx.rcvid = -1;
    exec_retire_shell(shell);
    msg_error(crcvid, EOK);
    return IOSERV_NOREPLY;
}
