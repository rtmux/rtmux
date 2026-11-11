/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* stat.h File stat structure and definitions */
#ifndef __STAT_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define __STAT_H_INCLUDED

#ifndef __STAT_H_DECLARED
#define __STAT_H_DECLARED

#include <sys/types.h>

struct stat {
    ino_t st_ino; /* File serial number.                  */
    off_t st_size;
    dev_t st_dev;  /* ID of device containing file.        */
    dev_t st_rdev; /* Device ID, for inode that is device  */
    uid_t st_uid;
    gid_t st_gid;
    time_t st_mtime; /* Time of last data modification       */
    time_t st_atime; /* Time last accessed                   */
    time_t st_ctime; /* Time of last status change           */
    mode_t st_mode;  /* see below                            */
    nlink_t st_nlink;
    blksize_t st_blocksize; /* Size of a block used by st_nblocks   */
    int32_t st_nblocks;     /* Number of blocks st_blocksize blocks */
    blksize_t st_blksize;   /* Prefered I/O block size for object   */
    blkcnt_t st_blocks;     /* Number of 512 byte blocks            */
};

#define _S_IFMT 0xF000   /*  Type of file                    */
#define _S_IFIFO 0x1000  /*  FIFO                            */
#define _S_IFCHR 0x2000  /*  Character special               */
#define _S_IFDIR 0x4000  /*  Directory                       */
#define _S_IFNAM 0x5000  /*  Special named file              */
#define _S_IFBLK 0x6000  /*  Block special                   */
#define _S_IFREG 0x8000  /*  Regular                         */
#define _S_IFLNK 0xA000  /*  Symbolic link                   */
#define _S_IFSOCK 0xC000 /*  Socket                          */

#define S_ISFIFO(m) \
    (((m) & _S_IFMT) == _S_IFIFO) /* Test for FIFO.             */
#define S_ISCHR(m) \
    (((m) & _S_IFMT) == _S_IFCHR) /* Test for char special file. */
#define S_ISDIR(m) \
    (((m) & _S_IFMT) == _S_IFDIR) /* Test for directory file.   */
#define S_ISBLK(m) \
    (((m) & _S_IFMT) == _S_IFBLK) /* Test for block specl file. */
#define S_ISREG(m) \
    (((m) & _S_IFMT) == _S_IFREG) /* Test for regular file.     */

#define S_ISLNK(m) \
    (((m) & _S_IFMT) == _S_IFLNK) /* Test for symbolic link.    */
#define S_ISNAM(m) \
    (((m) & _S_IFMT) == _S_IFNAM) /* Test for special named file */
#define S_ISSOCK(m) \
    (((m) & _S_IFMT) == _S_IFSOCK) /* Test for socket.           */


#define S_TYPEISMQ(buf) (S_ISNAM((buf)->st_mode) && ((buf)->st_rdev == _S_INMQ))
#define S_TYPEISSEM(buf) \
    (S_ISNAM((buf)->st_mode) && ((buf)->st_rdev == _S_INSEM))
#define S_TYPEISSHM(buf) \
    (S_ISNAM((buf)->st_mode) && ((buf)->st_rdev == _S_INSHD))


/* For special named files (IFNAM), the subtype is encoded in st_rdev. The
 * subtypes are:
 */
#define _S_INSEM 00001 /*  semaphore subtype               */
#define _S_INSHD 00002 /*  shared data subtype             */
#define _S_INMQ 00003  /*  message queue subtype           */

/* Common filetype macros */
#define S_IFMT _S_IFMT     /*  Type of file                    */
#define S_IFIFO _S_IFIFO   /*  FIFO                            */
#define S_IFCHR _S_IFCHR   /*  Character special               */
#define S_IFDIR _S_IFDIR   /*  Directory                       */
#define S_IFNAM _S_IFNAM   /*  Special named file              */
#define S_IFBLK _S_IFBLK   /*  Block special                   */
#define S_IFREG _S_IFREG   /*  Regular                         */
#define S_IFLNK _S_IFLNK   /*  Symbolic link                   */
#define S_IFSOCK _S_IFSOCK /*  Socket                          */

#define S_ISUID 004000 /* set user id on execution         */
#define S_ISGID 002000 /* set group id on execution        */
#define S_ISVTX 001000 /* sticky bit                       */

/* Owner permissions */
#define S_IRWXU 000700 /*  Read, write, execute/search     */
#define S_IRUSR 000400 /*  Read permission                 */
#define S_IWUSR 000200 /*  Write permission                */
#define S_IXUSR 000100 /*  Execute/search permission       */

/* Group permissions */
#define S_IRWXG 000070 /*  Read, write, execute/search     */
#define S_IRGRP 000040 /*  Read permission                 */
#define S_IWGRP 000020 /*  Write permission                */
#define S_IXGRP 000010 /*  Execute/search permission       */

/* Other permissions */
#define S_IRWXO 000007 /*  Read, write, execute/search     */
#define S_IROTH 000004 /*  Read permission                 */
#define S_IWOTH 000002 /*  Write permission                */
#define S_IXOTH 000001 /*  Execute/search permission       */

__BEGIN_DECLS
/* POSIX 1003.1 Prototypes. */
extern int stat(const char *__path, struct stat *__buf);
extern int lstat(const char *__path, struct stat *__buf);
extern int fstat(int __fildes, struct stat *__buf);
extern int fstatat(int __dirfd,
                   const char *__path,
                   struct stat *__buf,
                   int __flag);

extern mode_t umask(mode_t __cmask);

__END_DECLS
#endif

#endif
