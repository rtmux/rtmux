/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if !defined(CPU_PRT_SYSPAGE_RTN)

#include <startup.h>
#include <startup-api.h>

#define CONFIG_PSP_STARTUP 1
#define PSP_SYSPAGE lsp.syspage.p
#define PSP_SPRINTF ksprintf
#define PSP_VERBOSE(lvl) (kopt.loglevel > (lvl))

#define PSP_NATIVE_ENDIAN16(v) (v)
#define PSP_NATIVE_ENDIAN32(v) (v)
#define PSP_NATIVE_ENDIAN64(v) (v)
#define PSP_NATIVE_ENDIANPTR(v) (v)

#endif
