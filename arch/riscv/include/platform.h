/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \file riscv/platform.h
 *
 * \note Platform definitions for RISC-V.
 */

#ifndef _RISCV_PLATFORM_H
#define _RISCV_PLATFORM_H

#define _FILE_OFFSET_BITS 64
#define _PADDR_BITS 64

#define __JMPBUFSIZE 32
typedef unsigned __jmpbufalign;

int cpuid(void);

#define ARCH_MACHINE_STRING "RISC-V virt"

#define packedstruct struct __attribute__((packed))

#endif
