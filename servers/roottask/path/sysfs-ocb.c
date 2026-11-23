/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * sysfs-ocb - the fixed pool of /sys open-file state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <string.h>
#include "root-mutex.h"
#include "sysfs-ocb.h"

static struct sysfs_ocb ocb_pool[SYSFS_MAX_OCBS];
static int ocb_inuse[SYSFS_MAX_OCBS];

/* Guards ocb_inuse: opens and closes run on different pool threads, and two
 * opens that both saw a slot free would share one OCB and free its buffer
 * twice. Held only for the scan and the release.
 */
static pthread_mutex_t ocb_lock = PTHREAD_MUTEX_INITIALIZER;

void sysfs_ocb_init(void)
{
    pool_canary_init(ocb_pool, SYSFS_MAX_OCBS);
}

struct sysfs_ocb *sysfs_ocb_alloc(void)
{
    struct sysfs_ocb *ocb = NULL;

    root_mutex_lock(&ocb_lock);
    for (int i = 0; i < SYSFS_MAX_OCBS; i++) {
        if (!ocb_inuse[i]) {
            pool_canary_check(&ocb_pool[i], "sysfs_ocb", i);
            ocb_inuse[i] = 1;
            ocb = &ocb_pool[i];
            break;
        }
    }
    pthread_mutex_unlock(&ocb_lock);
    if (ocb) { /* the slot is this caller's alone from here */
        memset(ocb, 0, sizeof(*ocb));
        pool_canary_arm(ocb);
    }
    return ocb;
}

void sysfs_ocb_free(struct sysfs_ocb *ocb)
{
    uintptr_t off = (uintptr_t) ocb - (uintptr_t) ocb_pool;
    int i = (int) (off / sizeof(*ocb));

    if (off >= sizeof(ocb_pool) || off % sizeof(*ocb))
        return;
    pool_canary_check(ocb, "sysfs_ocb", i);
    root_mutex_lock(&ocb_lock);
    ocb_inuse[i] = 0;
    pthread_mutex_unlock(&ocb_lock);
}
