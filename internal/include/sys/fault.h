/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/fault.h - private fault identifiers for procfs/debug state. */
#ifndef __FAULT_H_INCLUDED
#define __FAULT_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

typedef struct {
    long bits[2];
} fltset_t;

#define FLTILL 1    /* Illegal instruction */
#define FLTPRIV 2   /* Privileged instruction */
#define FLTBPT 3    /* Breakpoint instruction */
#define FLTTRACE 4  /* Trace trap (single-step) */
#define FLTACCESS 5 /* Memory access (e.g. alignment) */
#define FLTBOUNDS 6 /* Memory bounds (invalid address) */
#define FLTIOVF 7   /* Integer overflow */
#define FLTIZDIV 8  /* Integer zero divide */
#define FLTFPE 9    /* Floating-point exception */
#define FLTSTACK 10 /* Irrecoverable stack fault */
#define FLTPAGE 11  /* Recoverable page fault (no associated sig) */

/* First CPU specific fault number */
#define _FIRST_CPU_FAULT 32

#include <arch/fault.h>

#endif
