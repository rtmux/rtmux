/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MEMCLASS_H_
#define _MEMCLASS_H_

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#include <limits.h>
#include <stdint.h>

/* memsize_t
 *
 * This type is used to represent all size information for the memory class and
 * memory partitioning systems. The API defines this type to be 64 bits
 * regardless of the processor on which code may be executing. Internally, if
 * the processor does not support 64 bit types, and any values exchanged between
 * an application and the OS cannot be represented, a EOVERFLOW error will be
 * returned
 */
#ifndef _MEMSIZE_T_
#define _MEMSIZE_T_ uint64_t
typedef _MEMSIZE_T_ memsize_t;
#endif

/* memsize_t_INFINITY
 *
 * When creating a partition, the caller may wish to leave the maximum value
 * unspecified so that any limits will be governed by partitions higher up in
 * the hierarchy. This value should be used to represent such a choice.
 */
#define memsize_t_INFINITY ((_MEMSIZE_T_) ULLONG_MAX)

/* memclass_id_t
 *
 * Memory class identifier This identifier is used to specify the class of
 * memory for which the configuration attributes and/or information sizes refer
 * to.
 */
typedef uint32_t memclass_id_t;
#define memclass_id_t_INVALID ((memclass_id_t) 0xFFFFFFFF)

/* memclass_limits_t
 *
 * This structure defines specific limits as related to the memory class and its
 * associated allocator. The values are considered read-only and are provided to
 * allow an application to make optimal decisions when interacting with the
 * allocator for the memory class.
 *
 * For example, the allocation sub-structure indicates the smallest and largest
 * allocatable unit supported by the allocator for the class.
 */
typedef struct memclass_limits_s {
    struct {
        struct {
            _MEMSIZE_T_ min; /* minimum size of an allocation unit */
            _MEMSIZE_T_ max; /* maximum size of an allocation unit */
        } size;
    } alloc;

} memclass_limits_t;

/* memclass_attr_t
 *
 * This structure defines the attributes of a memory class
 */
typedef struct memclass_attr_s {
    _MEMSIZE_T_ size;         /* the amount of available memory */
    memclass_limits_t limits; /* limits as defined above */
} memclass_attr_t;

/* MEMCLASS_DEFAULT_ATTR
 *
 * Default static memory class attribute initializer
 */
#define MEMCLASS_DEFAULT_ATTR                                \
    {                                                        \
        .size = 0,                                           \
        .limits = {.alloc = {.size = {.min = 0, .max = 0}}}, \
    }

/* memclass_sizeinfo_t
 *
 * This structure defines the retrievable size information provided by the
 * allocator for the memory class. Memory class size information consists of the
 * amount of reserved memory (memory that has been set aside for partitions
 * created with a non-zero minimum value) and unreserved memory (memory which is
 * freely available to all partitions of the memory class in a discretionary
 * fashion. The sum of all fields will always be equivalent to the size
 * attribute defined in memclass_attr_t.
 */
typedef struct memclass_sizeinfo_s {
    struct {
        _MEMSIZE_T_ free;
        _MEMSIZE_T_ used;
    } reserved;
    struct {
        _MEMSIZE_T_ free;
        _MEMSIZE_T_ used;
    } unreserved;
} memclass_sizeinfo_t;

/* memclass_info_t
 *
 * This structure defines the information that may be retrieved from the
 * allocator for a memory class.
 */
typedef struct memclass_info_s {
    memclass_id_t id;
    uint32_t reserved;
    memclass_attr_t attr;
    memclass_sizeinfo_t size;
} memclass_info_t;

#endif /* _MEMCLASS_H_ */
