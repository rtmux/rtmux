/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* riscv/cpu.h */

#ifndef __RISCV_CPU_H_INCLUDED
#define __RISCV_CPU_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __RISCV_CONTEXT_H_INCLUDED
#include <arch/context.h>
#endif

#define RISCV_STRINGNAME "riscv"

/* Exception vector offsets */
#define RISCV_EXC_FOO 0

#endif /* __RISCV_CPU_H_INCLUDED */
