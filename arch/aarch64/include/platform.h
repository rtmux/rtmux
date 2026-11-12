/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_PLATFORM_H
#define RTMUX_AARCH64_PLATFORM_H

/* AArch64 little-endian LP64 ABI. */
#if (!defined(__aarch64__) && !defined(RTMUX_HOST_TEST)) || \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ || __SIZEOF_POINTER__ != 8
#error This platform requires little-endian AArch64
#endif
#define _FILE_OFFSET_BITS 64
#define _PADDR_BITS 64
#define ARCH_MACHINE_STRING "AArch64 Raspberry Pi 3B"
#define __JMPBUFSIZE 48
typedef unsigned long __jmpbufalign;
int cpuid(void);
#define packedstruct struct __attribute__((packed))

#endif
