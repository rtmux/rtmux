/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* fcntl.h File control options used by open */

#ifndef _FCNTL_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define _FCNTL_H_INCLUDED

#ifndef _FCNTL_H_DECLARED
#define _FCNTL_H_DECLARED

#ifndef _UNISTD_H_INCLUDED
#include <unistd.h>
#endif

#ifndef __STAT_H_INCLUDED
#include <sys/stat.h>
#endif

/* Flag values accessible to both open() and fcntl() (The first three can only
 * be set by open)
 */

/* File access modes. */

#define O_RDONLY 000000 /*  Read-only mode  */
#define O_WRONLY 000001 /*  Write-only mode */
#define O_RDWR 000002   /*  Read-Write mode */

/* Mask for file access modes. */

#define O_ACCMODE 000003

/* File status flags used for open() and fcntl(). */

#define O_NONBLOCK 000200 /*  Non-blocking I/O                */
#define O_APPEND 000010   /*  Append (writes guaranteed at the end)   */

#define O_DSYNC 000020 /*  Data integrity synch    */
#define O_RSYNC 000100 /*  Data integrity synch    */
#define O_SYNC 000040  /*  File integrity synch    */

/* oflag values for open() */

#define O_CREAT 000400  /*  Opens with file create      */
#define O_TRUNC 001000  /*  Open with truncation        */
#define O_EXCL 002000   /*  Exclusive open          */
#define O_NOCTTY 004000 /*  Don't assign a controlling terminal */

#define O_CLOEXEC 020000 /*  File descriptor will be close-on-exec */
#define O_REALIDS 040000 /*  Use real uid/gid for permision checking */

/* Mask of flags that change be set with fcntl() */
#define O_SETFLAG (O_NONBLOCK | O_APPEND | O_DSYNC | O_RSYNC | O_SYNC)

/* fcntl() requests */

#define F_DUPFD 0         /*  Duplicate file descriptor   */
#define F_GETFD 1         /*  Get file descriptor flags   */
#define F_SETFD 2         /*  Set file descriptor flags   */
#define F_GETFL 3         /*  Get file status flags   */
#define F_SETFL 4         /*  Set file status flags   */
#define F_DUPFD_CLOEXEC 5 /* Duplicate fd, close-on-exec set */

#define F_SETLK 106   /*  Set record locking info */
#define F_SETLKW 107  /*  Set record locking info;    */
#define F_ALLOCSP 110 /*              */
#define F_FREESP 111  /*              */
#define F_GETLK 114   /*  Get record locking info */

/* File descriptor flags used for fcntl() */

#define FD_CLOEXEC 0x01 /*  Close on exec       */

/* fd-relative pathname operations. RTMux libc currently supports AT_FDCWD and
 * absolute paths; fd-relative lookup is namespace follow-up work.
 */
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x0001
#define AT_EACCESS 0x0002 /* faccessat: check using effective ids */
#define AT_REMOVEDIR                                         \
    0x0004 /* unlinkat: remove a directory instead of a file \
            */

/* l_type values for record locking with fcntl() */

#define F_RDLCK 1 /*  Shared or read lock     */
#define F_WRLCK 2 /*  Exclusive or write lock */
#define F_UNLCK 3 /*  Unlock          */

/* flock structure. */

typedef struct flock {
    int16_t l_type;
    int16_t l_whence;
    int32_t l_zero1;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
    uint32_t l_sysid; /* node descriptor */
} flock_t;

__BEGIN_DECLS
/* POSIX 1003.1 Prototypes. */
extern int open(const char *__path, int __oflag, ...);
extern int openat(int __dirfd, const char *__path, int __oflag, ...);
extern int creat(const char *__path, const mode_t __mode);
extern int fcntl(int __fildes, int __cmd, ...);

__END_DECLS
#endif

#endif
