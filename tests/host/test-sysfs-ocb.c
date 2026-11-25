/* Drive roottask's /sys OCB pool from several threads at once.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Roottask serves /sys opens and closes on any of its pool threads. Each
 * thread here takes OCBs and gives them back as fast as it can and marks every
 * one it holds as its own; a slot handed to two holders at once, or lost, is a
 * failure. Without the pool's lock two scans that see the same free slot both
 * take it.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "sysfs-ocb.h"

#define THREADS 8
#define ROUNDS 200000
#define HELD 2 /* OCBs each thread holds at once: THREADS * HELD fills the pool */

static atomic_int owner[SYSFS_MAX_OCBS]; /* 1 + the holder's number, or 0 */
static atomic_int shared, lost;
static atomic_int go;
static struct sysfs_ocb *pool_base; /* lowest OCB address seen */

static int slot_of(const struct sysfs_ocb *ocb)
{
    return (int) (ocb - pool_base);
}

static void *worker(void *arg)
{
    int me = (int) (intptr_t) arg;
    struct sysfs_ocb *held[HELD];

    while (!atomic_load(&go))
        ;
    for (int r = 0; r < ROUNDS; r++) {
        for (int h = 0; h < HELD; h++) {
            int expect = 0;

            if (!(held[h] = sysfs_ocb_alloc())) {
                /* every other thread holds at most HELD: one must be free */
                atomic_fetch_add(&lost, 1);
                continue;
            }
            held[h]->idx = me;
            if (!atomic_compare_exchange_strong(&owner[slot_of(held[h])],
                                                &expect, me + 1))
                atomic_fetch_add(&shared, 1);
        }
        for (int h = 0; h < HELD; h++) {
            int expect = me + 1;

            if (!held[h])
                continue;
            if (held[h]->idx != me ||
                !atomic_compare_exchange_strong(&owner[slot_of(held[h])],
                                                &expect, 0))
                atomic_fetch_add(&shared, 1);
            sysfs_ocb_free(held[h]);
        }
    }
    return NULL;
}

int main(void)
{
    struct sysfs_ocb *all[SYSFS_MAX_OCBS];
    pthread_t th[THREADS];
    int n = 0;

    sysfs_ocb_init();
    /* The whole pool can be taken, once, and then nothing more. */
    while (n < SYSFS_MAX_OCBS && (all[n] = sysfs_ocb_alloc()))
        n++;
    assert(n == SYSFS_MAX_OCBS);
    assert(!sysfs_ocb_alloc());
    pool_base = all[0];
    for (int i = 0; i < n; i++) {
        if (all[i] < pool_base)
            pool_base = all[i];
        assert(all[i]->idx == 0 && !all[i]->dyn_buf);
    }
    for (int i = 0; i < n; i++)
        sysfs_ocb_free(all[i]);
    sysfs_ocb_free(NULL);   /* not the pool's: ignored */
    sysfs_ocb_free(all[0]); /* a second free of one OCB changes nothing */

    for (int t = 0; t < THREADS; t++)
        assert(!pthread_create(&th[t], NULL, worker, (void *) (intptr_t) t));
    atomic_store(&go, 1);
    for (int t = 0; t < THREADS; t++)
        assert(!pthread_join(th[t], NULL));
    printf("sysfs-ocb: %d threads x %d rounds: %d shared, %d failed allocs\n",
           THREADS, ROUNDS, atomic_load(&shared), atomic_load(&lost));
    fflush(stdout);
    assert(!atomic_load(&shared) && !atomic_load(&lost));

    /* No slot leaked or doubled: the pool is whole again. */
    for (n = 0; n < SYSFS_MAX_OCBS && (all[n] = sysfs_ocb_alloc()); n++)
        ;
    assert(n == SYSFS_MAX_OCBS && !sysfs_ocb_alloc());
    for (int i = 0; i < n; i++)
        sysfs_ocb_free(all[i]);
    return 0;
}
