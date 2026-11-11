/* SPDX-License-Identifier: Apache-2.0 */

#ifndef _ASMMACRO_H
#define _ASMMACRO_H

#define ENTRY(name)        \
    .globl name;           \
    .type name, @function; \
    name:

#endif
