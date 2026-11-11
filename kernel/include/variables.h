/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_VARIABLES_H
#define _KERNEL_VARIABLES_H

#if defined(rdecl) || defined(KERNEL_HDR)
#error Not to be mixed with other kernel/ includes
#endif

#include <stddef.h>
#include <inttypes.h>
#include <arch/cpu-def.h>

#include <sys/fault.h>
#include <sys/debug.h>
#include <macros.h>
#include <kdebug.h>
#include <debug.h>
#include <objects.h>
#include <priv.h>
#include <decls.h>
#include <query.h>
#include "memclass.h"
#include <options.h>

extern soul_t process_souls;
extern vector_t process_vector;
extern proc_hooks_t *proc_hooks;
extern vm_ops_t *vm_ops;
extern unsigned num_processors;
extern int smp_dispatch_enabled;
extern process_t *kernel_threads_prp;

extern unsigned user_boundary_addr;


extern int munmap_flags_default;

#endif
