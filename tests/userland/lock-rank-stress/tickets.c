/* SPDX-License-Identifier: Apache-2.0 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "ticket-lock.h"
#ifndef TICKET_HOST_TEST
#include <sys/rtmux-core.h>
#endif

#define WORKERS 8
#define ROUNDS 10000
static intrspin_t hot;
static atomic_int start, failed;
static unsigned grants;
static uint64_t max_wait;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + (uint64_t) ts.tv_nsec;
}

static void *contend(void *arg)
{
#ifndef TICKET_HOST_TEST
    unsigned mask = 1u << (unsigned)(uintptr_t)arg;
    if (thread_ctl(RTMUX_TCTL_RUNMASK, (void *)(uintptr_t)mask) == -1)
        atomic_store(&failed, 1);
#else
    (void)arg;
#endif
    while (!atomic_load(&start))
        sched_yield();
    for (unsigned i = 0; i < ROUNDS && !atomic_load(&failed); i++) {
        uint64_t before = now_ns();
        unsigned ticket = intrspin_ticket(&hot);
        uint64_t wait = now_ns() - before;
        if (wait > max_wait)
            max_wait = wait;
        if (grants == 0) {
            /* Require a real queue. Every other worker has reserved a ticket
             * before the first releases. */
            while ((__atomic_load_n(&hot.value, __ATOMIC_RELAXED) >> 16) != WORKERS) {
                if (now_ns() - before > 10000000000ULL) {
                    atomic_store(&failed, 1);
                    break;
                }
            }
        }
        if (ticket != (unsigned short)grants || !intrspin_is_locked(&hot))
            atomic_store(&failed, 1);
        grants++;
        intrspin_release(&hot);
    }
    return NULL;
}

int ticket_stress(void)
{
    pthread_t threads[WORKERS];
    unsigned created = 0;
    for (; created < WORKERS; created++) {
        if (pthread_create(&threads[created], NULL, contend,
                           (void *)(uintptr_t)created)) {
            atomic_store(&failed, 1);
            break;
        }
    }
    atomic_store(&start, 1);
    for (unsigned i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    printf("ticket-lock: %u grants, longest wait %llu ns, released %s\n",
           grants, (unsigned long long)max_wait,
           intrspin_is_locked(&hot) ? "locked" : "unlocked");
    return atomic_load(&failed) || grants != WORKERS * ROUNDS ||
           intrspin_is_locked(&hot);
}

#ifdef TICKET_HOST_TEST
int main(void)
{
    return ticket_stress();
}
#endif
