/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_INTR_CONTROLLER_H
#define _RISCV_INTR_CONTROLLER_H

void plicinit(void);
void plicinithart(void);
int plic_claim(void);
void plic_complete(int);

#endif
