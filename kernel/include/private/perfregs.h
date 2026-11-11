/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Kernel-private performance-register debug flags.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PRIVATE_PERFREGS_H
#define PRIVATE_PERFREGS_H

#define PERFREGS_ENABLED_FLAG 0x80000000
#define PERFREGS_SAVED_FLAG 0x40000000

#define PERFREGS_CPUID_SHIFT 16
#define PERFREGS_PARTID_SHIFT 0

#define PERFREGS_MAKEID(cpu, part) \
    ((cpu) << PERFREGS_CPUID_SHIFT) | ((part) << PERFREGS_PARTID_SHIFT)

#endif
