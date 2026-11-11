/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/statvfs.h - VFS Filesystem information structure. */

#ifndef __STATVFS_H_INCLUDED
#define __STATVFS_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _SYS_FILETYPES_H
#include <sys/_filetypes.h>
#endif

struct statvfs {
    unsigned long f_bsize;  /* file system block size */
    unsigned long f_frsize; /* fundamental filesystem block size */
    fsblkcnt_t f_blocks;    /* total number of blocks on file system in units of
                               f_frsize
                               */
    fsblkcnt_t f_bfree;     /* total number of free blocks */
    fsblkcnt_t f_bavail;    /* number of free blocks available to non-privileged
                               process
                               */
    fsfilcnt_t f_files;     /* total number of file serial numbers */
    fsfilcnt_t f_ffree;     /* total number of free file serial numbers */
    fsfilcnt_t f_favail;    /* number of file serial numbers available to
                               non-privileged process
                               */
    unsigned long f_fsid;   /* file system id */
    char f_basetype[16];    /* null terminated name of target file system */
    unsigned long f_flag;   /* bit mask of f_flag values */
    unsigned long f_namemax;    /* maximum filename length */
    unsigned long f_filler[21]; /* padding */
};

#define ST_RDONLY 0x01U /* rw read only */
#define ST_NOEXEC 0x02U /* rw can't exec from filesystem */
#define ST_NOSUID 0x04U /* rw don't honor setuid bits on fs */

__BEGIN_DECLS
extern int statvfs(const char *__path, struct statvfs *__buf);
extern int fstatvfs(int __fildes, struct statvfs *__buf);

__END_DECLS
#endif
