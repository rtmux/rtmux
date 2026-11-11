/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PCI_HWRES_CLIENT_H_INCLUDED
#define __PCI_HWRES_CLIENT_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../roottask/include/hwres-types.h"

int hwres_create(hwres_range_t *list, unsigned count);
int hwres_claim(hwres_claim_t *list, unsigned count);
int hwres_claim_for_pid(hwres_claim_t *list, unsigned count, pid_t pid);
int hwres_release(hwres_claim_t *list, unsigned count);
int hwres_release_for_pid(hwres_claim_t *list, unsigned count, pid_t pid);
int hwres_claim_range(hwres_claim_t *claim,
                      uint64_t base,
                      size_t size,
                      uint32_t type);
void hwres_release_range(hwres_claim_t *claim);
int hwres_query(hwres_range_t *list, int listcnt, int start, uint32_t type);

#endif
