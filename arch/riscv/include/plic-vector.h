/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * plic-vector.h - base of the RISC-V PLIC external-interrupt range in the RTMux
 * vector space.
 *
 * A device's public interrupt vector is PLIC_VECTOR_BASE plus its raw PLIC IRQ
 * number. This is a kernel-and-userland ABI value, defined once here so the two
 * sides cannot disagree on the layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ARCH_PLIC_VECTOR_H
#define _ARCH_PLIC_VECTOR_H

#define PLIC_VECTOR_BASE 32

#endif /* _ARCH_PLIC_VECTOR_H */
