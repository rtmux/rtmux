/*
 * lock-rank-stress — userland mutex stress that intentionally tries to generate
 * a cycle (A->B and B->A acquisitions across workers) so a CONFIG_LOCK_DEBUG
 * kernel can flag the ordering violation if one ever leaks into a system call
 * path.
 *
 * The test itself is deadlock-prone if both workers schedule at exactly the
 * wrong moment. We use pthread_mutex_trylock so a userland deadlock never
 * wedges the boot; the goal is to exercise the kernel's lock-rank graph, not to
 * prove userland is deadlock-free.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rtmux-core.h>

#define DEFAULT_ITERATIONS 1000
#define AB_WORKERS 2
#define BA_WORKERS 2
#define WORKERS (AB_WORKERS + BA_WORKERS)

static pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;
static atomic_int failed;
static atomic_int completed;

struct worker_arg {
    int iterations;
    int reverse; /* 0 = lock A then B, 1 = lock B then A */
};

static int try_acquire(pthread_mutex_t *first, pthread_mutex_t *second)
{
    int rc = pthread_mutex_trylock(first);

    if (rc == EBUSY) {
        (void) __sched_yield();
        return 1;
    }
    if (rc != EOK)
        return -1;
    /* Trylock the second mutex. If it is held by an opposing-order worker we
     * bail and retry — that's the cycle the kernel lock-rank checker is meant
     * to notice if it manifests across kernel locks too.
     */
    if (pthread_mutex_trylock(second) != EOK) {
        (void) pthread_mutex_unlock(first);
        (void) __sched_yield();
        return 1;
    }
    if (pthread_mutex_unlock(second) != EOK ||
        pthread_mutex_unlock(first) != EOK)
        return -1;
    return 0;
}

static void *worker(void *raw)
{
    struct worker_arg *arg = raw;
    pthread_mutex_t *first = arg->reverse ? &b : &a;
    pthread_mutex_t *second = arg->reverse ? &a : &b;

    for (int i = 0; i < arg->iterations; i++) {
        int rc = try_acquire(first, second);
        if (rc < 0) {
            atomic_store_explicit(&failed, 1, memory_order_release);
            atomic_fetch_add_explicit(&completed, 1, memory_order_release);
            return NULL;
        }
        if ((i & 31) == 0)
            (void) __sched_yield();
    }
    atomic_fetch_add_explicit(&completed, 1, memory_order_release);
    return NULL;
}

int main(int argc, char **argv)
{
    int iterations = DEFAULT_ITERATIONS;
    struct worker_arg args[WORKERS];
    pthread_t tids[WORKERS];

    if (argc > 1)
        iterations = atoi(argv[1]);
    if (iterations <= 0) {
        fprintf(stderr, "usage: lock-rank-stress [iterations]\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < WORKERS; i++) {
        args[i].iterations = iterations;
        args[i].reverse = i >= AB_WORKERS;
        int rc = pthread_create(&tids[i], NULL, worker, &args[i]);
        if (rc != EOK) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return EXIT_FAILURE;
        }
    }
    while (atomic_load_explicit(&completed, memory_order_acquire) < WORKERS)
        (void) __sched_yield();
    if (atomic_load_explicit(&failed, memory_order_acquire)) {
        fprintf(stderr, "lock-rank-stress: worker failed\n");
        return EXIT_FAILURE;
    }
    printf("lock-rank-stress: AB_workers=%d BA_workers=%d iterations=%d ok\n",
           AB_WORKERS, BA_WORKERS, iterations);
    return EXIT_SUCCESS;
}
