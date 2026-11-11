/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROOTTASK_MM_MAP_HEAD_H
#define _ROOTTASK_MM_MAP_HEAD_H

#include <sys/types.h>
#include <stdint.h>

/* struct mm_map_head — the per-as user-VA range tracker. Pulled out of as.h so
 * it can be shared by root-process.h (which holds one by value in
 * root_process_t.map) without dragging in the whole as.h surface (mm_as,
 * mm_map, mm_object_ref, the cpu pgdir struct, …).
 */
struct mm_map; /* defined in as.h */

struct mm_map_head {
    struct mm_map *head;
    struct mm_map *rover;
    volatile unsigned lock;
    uintptr_t start;
    uintptr_t end;
    volatile unsigned walk_gen;
};

#endif /* _ROOTTASK_MM_MAP_HEAD_H */
