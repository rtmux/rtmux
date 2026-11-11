/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/memmsg.h */

#ifndef __MEMMSG_H_INCLUDED
#define __MEMMSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __SYSMSG_H_INCLUDED
#include <sys/sysmsg.h>
#endif

#ifndef __MMAN_H_INCLUDED
#include <sys/mman.h>
#endif

#define VM_COID ROOTTASK_COID

enum {
    _MEM_MAP = _VM_BASE,
    _MEM_CTRL,
    _MEM_INFO,
    _MEM_OFFSET,
    _MEM_DEBUG_INFO,
    _MEM_SWAP,
    _MEM_PEER,
    _MEM_HANDLE,
};

enum {
    _MEM_CTRL_UNMAP,
    _MEM_CTRL_PROTECT,
    _MEM_CTRL_SYNC,
    _MEM_CTRL_LOCKALL,
    _MEM_CTRL_UNLOCKALL,
    _MEM_CTRL_LOCK,
    _MEM_CTRL_UNLOCK,
    _MEM_CTRL_ADVISE
};

enum { _MEM_SWAP_ON, _MEM_SWAP_OFF, _MEM_SWAP_CTRL, _MEM_SWAP_STAT };

enum { _MEM_OFFSET_PHYS, _MEM_OFFSET_FD, _MEM_OFFSET_PT };

/* Message of _MEM_MAP */
struct _mem_map {
    uint16_t type;
    uint16_t zero;
    uint32_t reserved1;
    uint64_t addr;
    uint64_t len;
    uint32_t prot;
    uint32_t flags;
    int32_t fd;
    uint32_t preload;
    uint64_t align;
    off64_t offset;
    uint64_t mem_handle;
    uint32_t mem_rights;
    uint32_t reserved2;
};

struct _mem_map_reply {
    uint64_t real_size;
    uint64_t real_addr;
    uint64_t addr;
};

typedef union {
    struct _mem_map i;
    struct _mem_map_reply o;
} mem_map_t;

enum { _MEM_HANDLE_CREATE = 1, _MEM_HANDLE_CLOSE };

struct _mem_handle {
    uint16_t type;
    uint16_t subtype;
    int32_t fd;
    uint32_t rights;
    uint64_t handle;
};

struct _mem_handle_reply {
    uint64_t handle;
};

typedef union {
    struct _mem_handle i;
    struct _mem_handle_reply o;
} mem_handle_t;


/* Message of _MEM_CTRL */
struct _mem_ctrl {
    uint16_t type;
    uint16_t subtype;
    uint32_t flags;
    uint64_t addr;
    uint64_t len;
};

typedef union {
    struct _mem_ctrl i;
} mem_ctrl_t;


/* Message of _MEM_INFO */
struct _mem_info {
    uint16_t type;
    uint16_t zero;
    int32_t fd;
    uint32_t flags;
    uint32_t reserved;
};

struct _mem_info_reply {
    uint32_t zero[2];
    uint32_t size;
    uint32_t reserved;
    struct posix_typed_mem_info info;
};

typedef union {
    struct _mem_info i;
    struct _mem_info_reply o;
} mem_info_t;

/* Message of _MEM_OFFSET */
struct _mem_offset {
    uint16_t type;
    uint16_t subtype;
    int32_t reserved;
    uint64_t addr;
    uint64_t len;
};

struct _mem_offset_reply {
    uint64_t size;
    off64_t offset;
    int32_t fd;
    uint32_t reserved;
};

typedef union {
    struct _mem_offset i;
    struct _mem_offset_reply o;
} mem_offset_t;

/* Message of _MEM_DEBUG_INFO */
struct _mem_debug_info {
    uint16_t type;
    uint16_t zero;
    uint32_t reserved;
    off64_t offset;
    ino_t ino;
    uintptr_t vaddr;
    size_t size;
    uint32_t flags;
    dev_t dev;
    uintptr_t old_vaddr;
    char path[];
};

typedef union {
    struct _mem_debug_info i;
} mem_debug_info_t;

/* Message of _MEM_SWAP */
struct _mem_swap {
    uint16_t type;
    uint16_t subtype;
};

struct _mem_swap_on {
    uint16_t type;
    uint16_t subtype;
    uint32_t flags;
    uint32_t size;
    uint32_t len;
    uint32_t spare[2];
    /* uint8_t                      name[]; */
};

struct _mem_swap_off {
    uint16_t type;
    uint16_t subtype;
    uint32_t flags;
    uint32_t size;
    uint32_t len;
    uint32_t spare[2];
    /* uint8_t                      name[]; */
};

struct _mem_swap_stat {
    uint16_t type;
    uint16_t subtype;
    uint32_t flags;
    uint32_t len;
    uint32_t spare[5];
};

struct _mem_swap_stat_reply {
    uint16_t type;
    uint16_t subtype;
    uint32_t flags;
    uint64_t total;
    uint64_t inuse;
    uint64_t swapins;
    uint64_t swapouts;
    uint64_t swaprems;
    uint32_t spare[8];
};

typedef union {
    struct _mem_swap swap;
    struct _mem_swap_on swap_on;
    struct _mem_swap_off swap_off;
    struct _mem_swap_stat swap_stat;
    struct _mem_swap_stat_reply swap_stat_reply;
} mem_swap_t;


/* Message of _MEM_PEER */
struct _mem_peer {
    uint16_t type;
    uint16_t peer_msg_len;
    uint32_t pid;
    uint64_t reserved1;
    /* second mem msg follows - keep this structure a multiple of of 64-bits */
};

typedef union {
    struct _mem_peer i;
} mem_peer_t;

#endif
