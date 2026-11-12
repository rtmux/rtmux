/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * machine/tls.h for RTMux ldso (RISC-V)
 *
 * RISC-V uses TLS Variant I (TCB before TLS data, tp points past TCB).
 */
#ifndef _MACHINE_TLS_H
#define _MACHINE_TLS_H

#include <stdint.h>

struct pthread;

struct tcb {
    uintptr_t *tcb_dtv;
    struct pthread *tcb_thread;
};

#define TLS_TCB_SIZE sizeof(struct tcb)
#define TLS_DTV_OFFSET 0x800
#define TLS_TCB_ALIGN 16
#define TLS_TP_OFFSET 0

/* RTMux: do NOT write to the tp register. The kernel sets tp to the libc
 * _thread_local_storage pointer; overwriting it (as FreeBSD ldso does via "addi
 * tp, tcb, TLS_TCB_SIZE") corrupts errno and crashes the process.
 *
 * Instead, store the ELF TCB pointer in a global variable (_ldso_tcb_ptr).
 * ldso's __tls_get_addr() reads it back via _tcb_get() and works correctly.
 */
extern struct tcb *_ldso_tcb_ptr;

static __inline void _tcb_set(struct tcb *tcb)
{
    _ldso_tcb_ptr = tcb;
}

static __inline struct tcb *_tcb_get(void)
{
    return _ldso_tcb_ptr;
}

#endif
