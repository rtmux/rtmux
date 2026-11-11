/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* cpu-pgtable.h — Sv39 page table management interface
 *
 * License: Apache 2.0
 */

#ifndef _RISCV_CPU_PGTABLE_H
#define _RISCV_CPU_PGTABLE_H

#include <paging.h>

/* Allocate a fresh root page table (zeroed 4KB page) */
pagetable_t cpu_pgdir_create(void);

/* Map VA → PA with given PTE flags in the page table */
int cpu_map_pages(pagetable_t pgdir,
                  uintptr_t va,
                  paddr_t pa,
                  size_t size,
                  uint64_t pte_flags);

/* Free a page table tree (walk and free all levels) */
void cpu_pgdir_free(pagetable_t pgdir);

/* Look up a VA in a page table, return PA (or 0 if unmapped) */
paddr_t cpu_va2pa(pagetable_t pgdir, uintptr_t va);

#endif /* _RISCV_CPU_PGTABLE_H */
