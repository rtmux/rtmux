/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PATH_OBJECT_H
#define PATH_OBJECT_H

#include <path-node.h>

enum {
    OBJECT_NONE,
    OBJECT_SERVER,
    OBJECT_NAME,
    OBJECT_PROC_SYMLINK,
    OBJECT_MEM_ANON,
    OBJECT_MEM_SHARED,
    OBJECT_MEM_FD,
    OBJECT_MEM_TYPED,
};

/* Max size of a path object is limited by the uint8_t we use to keep track of
 * it's length in object_header.len
 */
#define PATH_MAX_OBJECT_LEN UINT8_MAX

typedef union path_object path_object_t;

struct object_header {
    path_object_t *next;
    node_t *node;
    uint8_t type;
    uint8_t len;
    uint16_t flags;
    pid_t creator;
    unsigned refs;
};

struct server_object {
    struct object_header hdr;
    uint32_t nd;
    pid_t pid;
    int chid;
    unsigned handle;
    unsigned file_type;
};

struct symlink_object {
    struct object_header hdr;
    int len;
    char name[1];
};

/* This structure is used by both the path and memory subsystems of proc. I'd
 * really like to split it into two, but I can't see a clean way of doing that
 * :-(.
 */
struct mm_object {
    struct object_header hdr;

    struct path_info {
        time_t mtime;
        mode_t mode; /* Permissions should all live elsewhere */
        uid_t uid;
        gid_t gid;
    } pm;

    struct vm_stuff {
        struct proc_mux_lock *mux;
        struct pa_quantum *pmem;

        /* Walk-resume hint: the predecessor pa_quantum visited by the last
         * memobj_pmem_walk / memobj_pmem_del* / memobj_pmem_add call. NULL
         * means "start at head". Pre-U-mode-roottask this was "pa_quantum **"
         * (a pointer to the .next slot of the predecessor, typed for direct
         * *cache = newval mutation). That was a kernel-VA pointer for any
         * non-head position; U-mode roottask cannot deref it. Now we hold the
         * predecessor pq* directly and use pa_pq_inuse_next() to advance.
         */
        struct pa_quantum *pmem_cache;
        struct mm_object_ref *refs;

        /* RUSH2: Another restriction list for where to allocate COR memory
         * from?
         */
        struct pa_restrict *restriction;
        off64_t size;
        unsigned flags;
    } mm;
};

struct mm_object_anon {
    struct mm_object mem;
    /* RUSH2: Stuff to track available offsets... */
};

struct mm_object_shared {
    struct mm_object mem;
    unsigned special;
    uintptr_t vaddr;
};

struct mm_object_fd {
    struct mm_object mem;
    int fd;
    time_t ftime;
    ino_t ino;
    dev_t dev;
    char *name;
};

struct mm_object_typed {
    struct mm_object_shared shmem;
    char *name;
};

typedef union path_object {
    struct object_header hdr;
    struct server_object server;
    struct symlink_object symlink;
    struct mm_object mem;
    struct mm_object_anon anmem;
    struct mm_object_shared shmem;
    struct mm_object_fd fdmem;
    struct mm_object_typed tymem;
} path_object_t;


/* Page mapping status bits returned by roottask mapinfo. */
#define MM_PG_MODIFIED 0x00100000U
#define MM_PG_REFERENCED 0x00200000U
#define MM_PG_HWMAPPED 0x00400000U
#define MM_PG_MASK 0x00f00000U

/* These bits do not correspond to any of the prot or flags bits that a user is
 * allowed to set in the mmap message (they're read only bits that come back
 * from mapinfo). The path service flips them on in the input message structure
 * to pass metadata back to VM.
 */
#define IMAP_MASK (MM_PG_MASK | MAP_SYSRAM)
#define IMAP_OCB_RDONLY \
    0x00100000 /* OCB is read-only, mprotect() can't turn on PROT_WRITE */
#define IMAP_TYMEM_ALLOCATE 0x00200000 /* POSIX_TYPED_MEM_ALLOCATE */
#define IMAP_TYMEM_ALLOCATE_CONTIG \
    0x00400000 /* POSIX_TYPED_MEM_ALLOCATE_CONTIG */
#define IMAP_TYMEM_MAP_ALLOCATABLE \
    0x00800000                 /* POSIX_TYPED_MEM_MAP_ALLOCATABLE */
#define IMAP_GLOBAL MAP_SYSRAM /* creating a global vaddr */

#endif
