/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/mman.h POSIX Memory Management */

#ifndef __MMAN_H_INCLUDED
#define __MMAN_H_INCLUDED

#include <sys/platform.h>
#include <sys/types.h>

/* Sharing types: Must choose either SHARED, PRIVATE or ANON */
#define MAP_SHARED 0x00000001  /* share changes          */
#define MAP_PRIVATE 0x00000002 /* changes are private    */
#define MAP_ANONYMOUS MAP_ANON
#define MAP_TYPE 0x0000000f /* mask for share type    */

/* Flags to mmap (or-ed in to MAP_SHARED or MAP_PRIVATE)     */
#define MAP_FIXED 0x00000010      /* user assigns address   */
#define MAP_ELF 0x00000020        /* Patch ELF relocations  */
#define MAP_NOSYNCFILE 0x00000040 /* Memory not synced to file */
#define MAP_STACK 0x00001000      /* memory used for stack  */
#define MAP_BELOW 0x00002000      /* allocate below hint    */
#define MAP_NOINIT 0x00004000     /* don't have to initialize the memory */
#define MAP_PHYS 0x00010000       /* map physical memory    */
#define MAP_ANON 0x00080000       /* anonymous memory       */
#define MAP_SYSRAM 0x01000000     /* system ram             */
#define MAP_RESERVMASK 0xf0000000 /* reserved for system use */

/* Page protection flags which may be or-ed together         */
#define PROT_NONE 0x00000000
#define PROT_READ 0x00000100    /* pages can be read      */
#define PROT_WRITE 0x00000200   /* pages can be written   */
#define PROT_EXEC 0x00000400    /* pages can be executed  */
#define PROT_NOCACHE 0x00000800 /* pages are not cached   */
#define PROT_MASK 0x00000f00    /* mask for prot bits     */

/* flags for msync() */
#define MS_ASYNC 0x0000001 /* Flush modified area to storage (Don't wait) */
#define MS_SYNC \
    0x0000002 /* Flush modified area to storage (Return when complete) */
#define MS_INVALIDATE \
    0x0000004 /* Invalidate area (next access will retrieve from storage) */
#define MS_INVALIDATE_ICACHE \
    0x1000000 /* If PROT_EXEC, force execution from cache or storage */
#define MS_CACHE_ONLY 0x2000000 /* Only operate on CPU cache */

/* flags for mlockall() */
#define MCL_CURRENT 0x000000001
#define MCL_FUTURE 0x000000002

#define MAP_FAILED ((void *) -1)

#define NOFD (-1)

#define __PAGESIZE 0x1000UL

__BEGIN_DECLS
extern void *mmap(void *__addr,
                  size_t __len,
                  int __prot,
                  int __flags,
                  int __fd,
                  off_t __off);
extern int mprotect(void *__addr, size_t __len, int __prot);
extern int mlock(__const void *__addr, size_t __len);
extern int mlockall(int __flags);
extern int msync(void *__addr, size_t __len, int __flags);
extern int munmap(void *__addr, size_t __len);
extern int munlock(__const void *__addr, size_t __len);
extern int munlockall(void);

extern int shm_open(__const char *__path, int __oflag, mode_t __mode);
extern int shm_unlink(__const char *__path);

#define POSIX_TYPED_MEM_ALLOCATE 0x0001
#define POSIX_TYPED_MEM_ALLOCATE_CONTIG 0x0002
#define POSIX_TYPED_MEM_MAP_ALLOCATABLE 0x0004
extern int posix_typed_mem_open(__const char *__name, int __oflag, int __tflag);

struct posix_typed_mem_info {
    size_t posix_tmi_length;
    uint32_t __reserved[4];
};
extern int posix_typed_mem_get_info(int __fd,
                                    struct posix_typed_mem_info *__info);

extern int posix_mem_offset(__const void *__addr,
                            size_t __len,
                            off_t *__off,
                            size_t *__contig_len,
                            int *__fd);

__END_DECLS
#endif
