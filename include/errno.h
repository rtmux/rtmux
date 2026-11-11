/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* errno.h Error numbers */

#ifndef _ERRNO_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define _ERRNO_H_INCLUDED

#ifndef _ERRNO_H_DECLARED
#define _ERRNO_H_DECLARED

__BEGIN_DECLS
extern int errno;
#ifndef errno
int *__get_errno_ptr(void) __attribute__((__const__));
#define errno (*__get_errno_ptr())
#endif

__END_DECLS
/* Symbolic names of the error return conditions */
#define EOK 0           /* No error                                 */
#define EPERM 1         /* Not owner                                */
#define ENOENT 2        /* No such file or directory                */
#define ESRCH 3         /* No such process                          */
#define EINTR 4         /* Interrupted system call                  */
#define EIO 5           /* I/O error                                */
#define ENXIO 6         /* No such device or address                */
#define E2BIG 7         /* Arg list too big                         */
#define ENOEXEC 8       /* Exec format error                        */
#define EBADF 9         /* Bad file number                          */
#define ECHILD 10       /* No child processes                       */
#define EAGAIN 11       /* Resource unavailable, try again          */
#define ENOMEM 12       /* Not enough space                         */
#define EACCES 13       /* Permission denied                        */
#define EFAULT 14       /* Bad address                              */
#define ENOTBLK 15      /* Block device required                    */
#define EBUSY 16        /* Device or resource busy                  */
#define EEXIST 17       /* File exists                              */
#define EXDEV 18        /* Cross-device link                        */
#define ENODEV 19       /* No such device                           */
#define ENOTDIR 20      /* Not a directory                          */
#define EISDIR 21       /* Is a directory                           */
#define EINVAL 22       /* Invalid argument                         */
#define ENFILE 23       /* File table overflow                      */
#define EMFILE 24       /* Too many open files                      */
#define ENOTTY 25       /* Inappropriate I/O control operation      */
#define ETXTBSY 26      /* Text file busy                           */
#define EFBIG 27        /* File too large                           */
#define ENOSPC 28       /* No space left on device                  */
#define ESPIPE 29       /* Illegal seek                             */
#define EROFS 30        /* Read-only file system                    */
#define EMLINK 31       /* Too many links                           */
#define EPIPE 32        /* Broken pipe                              */
#define EDOM 33         /* Math argument out of domain of function  */
#define ERANGE 34       /* Result too large                         */
#define ENOMSG 35       /* No message of desired type               */
#define EIDRM 36        /* Identifier removed                       */
#define EL3HLT 39       /* Level 3 halted                           */
#define EL2HLT 44       /* Level 2 halted                           */
#define EDEADLK 45      /* Deadlock avoided                         */
#define ENOLCK 46       /* No locks available in system             */
#define ECANCELED 47    /* Operation canceled (1003.1b-1993)        */
#define ENOTSUP 48      /* Not supported (1003.1b-1993)             */
#define EDQUOT 49       /* Disc quota exceded                       */
#define EOWNERDEAD 58   /* Owner died holding lock                  */
#define EREMOTE 66      /* The object is remote                     */
#define ENOLINK 67      /* The link has been severed                */
#define EPROTO 71       /* Protocol error                           */
#define EBADMSG 77      /* Bad message (1003.1b-1993)               */
#define ENAMETOOLONG 78 /* Name too long                            */
#define EOVERFLOW 79    /* Value too large to be stored in data type */
/* Shared library errors */
#define ELIBACC 83 /* Can't access shared library              */
#define ELIBBAD 84 /* Accessing a corrupted shared library     */
#define EILSEQ 88  /* Illegal byte sequence.                   */

#define ENOSYS 89      /* Unknown system call                      */
#define ELOOP 90       /* Too many symbolic link or prefix loops   */
#define ERESTART 91    /* Restartable system call                  */
#define ENOTEMPTY 93   /* Directory not empty                      */
#define EOPNOTSUPP 103 /* Operation not supported                  */
#define ESTALE 122     /* Potentially recoverable i/o error        */
/* Sockets non-blocking and interrupt i/o */
/*        EAGAIN           35        */ /* Resource temporarily unavailable */
/*        EWOULDBLOCK      35        */ /* Operation would block */
#define EWOULDBLOCK EAGAIN              /* Operation would block */
#define EINPROGRESS 236                 /* Operation now in progress */
#define EALREADY 237                    /* Operation already in progress */
/* ipc/network software -- argument errors */
#define ENOTSOCK 238        /* Socket operation on non-socket */
#define EDESTADDRREQ 239    /* Destination address required */
#define EMSGSIZE 240        /* Inappropriate message buffer length */
#define EPROTOTYPE 241      /* Protocol wrong type for socket */
#define ENOPROTOOPT 242     /* Protocol not available */
#define EPROTONOSUPPORT 243 /* Protocol not supported */
#define ESOCKTNOSUPPORT 244 /* Socket type not supported */
#define EPFNOSUPPORT 246    /* Protocol family not supported */
#define EAFNOSUPPORT 247  /* Address family not supported by protocol family */
#define EADDRINUSE 248    /* Address already in use */
#define EADDRNOTAVAIL 249 /* Can't assign requested address */
/* ipc/network software -- operational errors */
#define ENETDOWN 250                /* Network is down */
#define ENETUNREACH 251             /* Network is unreachable */
#define ENETRESET 252               /* Network dropped connection on reset */
#define ECONNABORTED 253            /* Software caused connection abort */
#define ECONNRESET 254              /* Connection reset by peer */
#define ENOBUFS 255                 /* No buffer space available */
#define EISCONN 256                 /* Socket is already connected */
#define ENOTCONN 257                /* Socket is not connected */
#define ESHUTDOWN 258               /* Can't send after socket shutdown */
#define ETOOMANYREFS 259            /* Too many references: can't splice */
#define ETIMEDOUT 260               /* Connection timed out */
#define ECONNREFUSED 261            /* Connection refused */
/*      ELOOP          62        */ /* Too many levels of symbolic links */
/*      ENAMETOOLONG   63        */ /* File name too long */
#define EHOSTDOWN 264               /* Host is down */
#define EHOSTUNREACH 265            /* No route to host */
/* Network File System */
/*      ESTALE           70     */ /* Stale NFS file handle */
/*      EREMOTE          71     */ /* Too many levels of remote in path */
#define EBADRPC 272                /* RPC struct is bad */
#define ERPCMISMATCH 273           /* RPC version wrong */
#define EPROGUNAVAIL 274           /* RPC prog. not avail */
#define EPROGMISMATCH 275          /* Program version wrong */
#define EPROCUNAVAIL 276           /* Bad procedure for program */
#endif

#endif
