/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Persistent namespace object accounting.
 *
 * Counts durable namespace/object registrations by creator pid and object
 * class. The hot path is path lookup; this code only runs on object
 * create/destroy, so a single mutex is the right tool; earlier lock-free
 * attempts raced between concurrent claim/release on the same pid slot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "path-object.h"
#include "path.h"
#include "root-process.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#define NAMESPACE_ACCOUNT_TYPES (OBJECT_MEM_TYPED + 1)
#define NAMESPACE_ACCOUNT_PID_RECORDS ROOT_PROCESS_MAX
#define NAMESPACE_ACCOUNT_PID_OBJECT_LIMIT 64

struct account_counter {
    unsigned long long current;
    unsigned long long highwater;
};

struct pid_counter {
    pid_t pid;
    struct account_counter objects;
};

static pthread_mutex_t accounting_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct account_counter total_objects;
static struct account_counter type_objects[NAMESPACE_ACCOUNT_TYPES];
static struct pid_counter pid_objects[NAMESPACE_ACCOUNT_PID_RECORDS];

static const char *const type_names[NAMESPACE_ACCOUNT_TYPES] = {
    [OBJECT_NONE] = "none",         [OBJECT_SERVER] = "server",
    [OBJECT_NAME] = "name",         [OBJECT_PROC_SYMLINK] = "proc-symlink",
    [OBJECT_MEM_ANON] = "mem-anon", [OBJECT_MEM_SHARED] = "mem-shared",
    [OBJECT_MEM_FD] = "mem-fd",     [OBJECT_MEM_TYPED] = "mem-typed",
};

/* Caller holds accounting_mutex. */
static void counter_inc(struct account_counter *counter)
{
    counter->current++;
    if (counter->current > counter->highwater)
        counter->highwater = counter->current;
}

/* Caller holds accounting_mutex. */
static void counter_dec(struct account_counter *counter)
{
    if (counter->current > 0)
        counter->current--;
}

/* Caller holds accounting_mutex.
 *
 * Returns the slot for pid, claiming an empty slot if necessary. NULL if the
 * table is full.
 */
static struct pid_counter *pid_counter_get(pid_t pid)
{
    struct pid_counter *empty = NULL;

    for (int i = 0; i < NAMESPACE_ACCOUNT_PID_RECORDS; i++) {
        if (pid_objects[i].pid == pid)
            return &pid_objects[i];
        if (pid_objects[i].pid == 0 && !empty)
            empty = &pid_objects[i];
    }

    if (!empty)
        return NULL;
    empty->pid = pid;
    empty->objects.current = 0;
    empty->objects.highwater = 0;
    return empty;
}

/* Caller holds accounting_mutex. */
static struct pid_counter *pid_counter_find(pid_t pid)
{
    for (int i = 0; i < NAMESPACE_ACCOUNT_PID_RECORDS; i++) {
        if (pid_objects[i].pid == pid)
            return &pid_objects[i];
    }
    return NULL;
}

int namespace_account_object_admit(pid_t creator_pid, int type)
{
    struct pid_counter *pc;
    int status = EOK;

    if (type < 0 || type >= NAMESPACE_ACCOUNT_TYPES)
        return EINVAL;
    if (creator_pid == ROOTTASK_PID)
        return EOK;

    pthread_mutex_lock(&accounting_mutex);
    pc = pid_counter_find(creator_pid);
    if (pc && pc->objects.current >= NAMESPACE_ACCOUNT_PID_OBJECT_LIMIT) {
        status = EDQUOT;
    }
    pthread_mutex_unlock(&accounting_mutex);
    return status;
}

void namespace_account_object_create(pid_t creator_pid, int type)
{
    struct pid_counter *pc;

    pthread_mutex_lock(&accounting_mutex);
    counter_inc(&total_objects);
    if (type >= 0 && type < NAMESPACE_ACCOUNT_TYPES)
        counter_inc(&type_objects[type]);

    pc = pid_counter_get(creator_pid);
    if (pc)
        counter_inc(&pc->objects);
    pthread_mutex_unlock(&accounting_mutex);
}

void namespace_account_object_destroy(pid_t creator_pid, int type)
{
    struct pid_counter *pc;

    pthread_mutex_lock(&accounting_mutex);
    counter_dec(&total_objects);
    if (type >= 0 && type < NAMESPACE_ACCOUNT_TYPES)
        counter_dec(&type_objects[type]);

    pc = pid_counter_find(creator_pid);
    if (pc) {
        counter_dec(&pc->objects);
        if (pc->objects.current == 0) {
            pc->objects.highwater = 0;
            pc->pid = 0;
        }
    }
    pthread_mutex_unlock(&accounting_mutex);
}

static size_t append(char *buf, size_t cap, size_t used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (used >= cap)
        return used;

    va_start(ap, fmt);
    n = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);

    if (n < 0)
        return used;
    if ((size_t) n >= cap - used)
        return cap - 1;
    return used + (size_t) n;
}

size_t namespace_accounting_snapshot(char *buf, size_t cap)
{
    size_t used = 0;

    if (cap == 0)
        return 0;

    pthread_mutex_lock(&accounting_mutex);
    used = append(buf, cap, used, "total_current_objects %llu\n",
                  total_objects.current);
    used = append(buf, cap, used, "total_highwater_objects %llu\n",
                  total_objects.highwater);
    used = append(buf, cap, used, "per_pid_object_limit %u\n",
                  NAMESPACE_ACCOUNT_PID_OBJECT_LIMIT);

    for (int i = 0; i < NAMESPACE_ACCOUNT_TYPES; i++) {
        const char *name = type_names[i] ? type_names[i] : "unknown";
        used = append(buf, cap, used, "type %s current %llu highwater %llu\n",
                      name, type_objects[i].current, type_objects[i].highwater);
    }

    for (int i = 0; i < NAMESPACE_ACCOUNT_PID_RECORDS; i++) {
        unsigned long long current = pid_objects[i].objects.current;
        unsigned long long highwater = pid_objects[i].objects.highwater;
        pid_t pid = pid_objects[i].pid;

        if (current == 0 && highwater == 0)
            continue;
        used = append(buf, cap, used, "pid %d current %llu highwater %llu\n",
                      (int) pid, current, highwater);
    }
    pthread_mutex_unlock(&accounting_mutex);

    if (used >= cap)
        used = cap - 1;
    buf[used] = '\0';
    return used;
}
