/* Exercise generation reuse before the old process's finalizer runs.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

/* Supply the slot's external dependencies while exercising its production
 * ownership and locking code. Two slots suffice to force a generation reuse.
 */
#define _ROOTTASK_EXTERNS_H
#define _ROOTTASK_ROOT_PROCESS_H
#define ROOT_PROCESS_MAX 2
#define ROOTTASK_PID 1
#define EOK 0
#define crash() abort()
typedef struct {
    unsigned links;
    pid_t leader, pgrp;
    int fd;
} session_t;
typedef struct {
    pid_t pid, parent_pid;
    void *lock;
    char *debug_name;
    session_t *session;
    pid_t pgrp;
} root_process_t;
#define root_proc(pid) (&root_process[(pid) % ROOT_PROCESS_MAX])
#define atomic_add_value(p, n) __atomic_fetch_add(p, n, __ATOMIC_RELAXED)
static unsigned reaps, frees;
static void proc_pgrp_reap(pid_t pid)
{
    assert(pid > 0);
    ++reaps;
}
static void memcap_process_fini(root_process_t *p)
{
    assert(p->pid);
    ++frees;
}
static int proc_pgrp_boot(pid_t pid)
{
    (void) pid;
    return EOK;
}
static void proc_session_unref(session_t *sep);
#include "../../servers/roottask/sys/root-process.c"

static void proc_session_unref(session_t *sep)
{
    assert(pthread_mutex_trylock(&slot_lock) == 0);
    assert(sep->links > 0);
    --sep->links;
    pthread_mutex_unlock(&slot_lock);
}

int main(void)
{
    session_t old = {.links = 1}, next = {.links = 1};
    root_process_init(2, 1);
    root_proc(2)->session = &old;
    root_proc(2)->lock = &old;
    root_proc(2)->debug_name = malloc(8);
    root_process_init(4, 3);
    assert(old.links == 0 && reaps == 1 && frees == 1);
    assert(root_proc(4)->lock == &old && !root_proc(4)->session);
    root_proc(4)->session = &next;
    root_process_fini(2);
    assert(root_proc(4)->pid == 4 && root_proc(4)->parent_pid == 3);
    assert(next.links == 1);
    root_process_fini(4);
    root_process_fini(4);
    assert(next.links == 0 && reaps == 2 && frees == 2);
    puts("process-slot-test: PASS");
    return 0;
}
