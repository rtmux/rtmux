/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_SPINLOCK_H
#define _SYS_SPINLOCK_H

/* Spinlock structure */
typedef struct intrspin {
    volatile unsigned value;
    /* For debugging */
    char *name; /* Name of lock. */
    /* struct cpu *cpu; The CPU holding the lock */
} intrspin_t;

#endif
