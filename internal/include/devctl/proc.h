/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DEVCTL_PROC_H_INCLUDED
#define __DEVCTL_PROC_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _DEVCTL_H_INCLUDED
#include <devctl.h>
#endif

__BEGIN_DECLS

typedef struct devctl_mem_object {
    off64_t offset;
    uint64_t size;
    int32_t flags;
    uint32_t special;
    uint32_t reserved[2];
} devctl_mem_object;

enum {
    MEMOBJ_INFO_TYPE_NONE = 0,
    MEMOBJ_INFO_TYPE_DEV_MEM = 1,
    MEMOBJ_INFO_TYPE_SHARED = 2,
    MEMOBJ_INFO_TYPE_TYPED = 3,
};

enum {
    MEMOBJ_INFO_CACHE_DEFAULT = 0,
    MEMOBJ_INFO_CACHE_SELECTABLE = 1,
    MEMOBJ_INFO_CACHE_DEVICE_NOCACHE = 2,
};

#define MEMOBJ_INFO_FLAG_FILE_BACKED 0x00000001u
#define MEMOBJ_INFO_FLAG_PHYSICAL 0x00000002u
#define MEMOBJ_INFO_FLAG_PRIVILEGED 0x00000004u
#define MEMOBJ_INFO_FLAG_TYPED 0x00000008u
#define MEMOBJ_INFO_FLAG_ALLOCATE 0x00000010u
#define MEMOBJ_INFO_FLAG_ALLOCATE_CONTIG 0x00000020u
#define MEMOBJ_INFO_FLAG_MAP_ALLOCATABLE 0x00000040u
#define MEMOBJ_INFO_FLAG_RESOURCEDB 0x00000080u

typedef struct devctl_mem_object_info {
    uint32_t version;
    uint32_t type;
    uint32_t flags;
    uint32_t cache_policy;
    off64_t offset;
    uint64_t size;
    uint32_t tflags;
    uint32_t reserved[5];
} devctl_mem_object_info;


#define __PROC_SUBCMD_PROCFS 0
#define __PROC_SUBCMD_MEMOBJ 100
#define __PROC_SUBCMD_USER 200


/* Change attributes of a memory object associated with a file descriptor. The
 * payload is a devctl_mem_object. This is for internal VM service use; callers
 * should reach it through the POSIX memory APIs.
 */
#define DEVCTL_MEM_OBJECT_SET \
    __DIOT(DEVCTL_CLASS_PROC, __PROC_SUBCMD_MEMOBJ + 0, devctl_mem_object)
#define DEVCTL_MEM_OBJECT_INFO \
    __DIOF(DEVCTL_CLASS_PROC, __PROC_SUBCMD_MEMOBJ + 1, devctl_mem_object_info)

__END_DECLS
#endif
