/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* unistd.h */
#ifndef _UNISTD_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define _UNISTD_H_INCLUDED

#ifndef _UNISTD_H_DECLARED
#define _UNISTD_H_DECLARED

#ifndef _CONFNAME_H_INCLUDED
#include <confname.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

/* Symbolic constants for the access() function */

#define R_OK 4 /*  Test for read permission    */
#define W_OK 2 /*  Test for write permission   */
#define X_OK 1 /*  Test for execute permission */
#define F_OK 0 /*  Test for existence of file  */

/* Symbolic constants for the lseek() function */

#ifndef _SEEKPOS_DEFINED_ /* If not already defined, define them  */
#define SEEK_SET 0        /* Seek relative to the start of file   */
#define SEEK_CUR 1        /* Seek relative to current position    */
#define SEEK_END 2        /* Seek relative to the end of the file */
#define _SEEKPOS_DEFINED_
#endif

/* Symbolic constants for stream I/O */

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Compile-time Symbolic Constants for Portability Specifications */

#undef _POSIX_VERSION
#undef _POSIX2_VERSION
#define _POSIX_VERSION 200112L
#define _POSIX2_VERSION 200112L

#undef _POSIX_JOB_CONTROL
#define _POSIX_SAVED_IDS 1

#undef _POSIX_ASYNCHRONOUS_IO /* no aio.h / aio_* implementation */
#define _POSIX_FSYNC _POSIX_VERSION
#define _POSIX_MAPPED_FILES _POSIX_VERSION
#define _POSIX_MEMLOCK _POSIX_VERSION
#define _POSIX_MEMLOCK_RANGE _POSIX_VERSION
#define _POSIX_MEMORY_PROTECTION _POSIX_VERSION
#define _POSIX_MESSAGE_PASSING _POSIX_VERSION
#undef _POSIX_PRIORITIZED_IO /* aio prioritization; no aio implementation */
#define _POSIX_PRIORITY_SCHEDULING _POSIX_VERSION
#undef _POSIX_REALTIME_SIGNALS
#define _POSIX_SEMAPHORES _POSIX_VERSION
#define _POSIX_SHARED_MEMORY_OBJECTS _POSIX_VERSION
#define _POSIX_SYNCHRONIZED_IO _POSIX_VERSION
#define _POSIX_TIMERS _POSIX_VERSION

#define _POSIX_THREADS _POSIX_VERSION
#define _POSIX_THREAD_PRIO_INHERIT _POSIX_VERSION
#define _POSIX_THREAD_PRIO_PROTECT _POSIX_VERSION
#define _POSIX_THREAD_PRIORITY_SCHEDULING _POSIX_VERSION
#define _POSIX_THREAD_ATTR_STACKADDR _POSIX_VERSION
#define _POSIX_THREAD_SAFE_FUNCTIONS _POSIX_VERSION
#define _POSIX_THREAD_PROCESS_SHARED _POSIX_VERSION
#define _POSIX_THREAD_ATTR_STACKSIZE _POSIX_VERSION

/* POSIX 1003.1d D14 */
#undef _POSIX_ADVISORY_INFO
#undef _POSIX_CPUTIME
#define _POSIX_SPAWN _POSIX_VERSION
#define _POSIX_SPORADIC_SERVER _POSIX_VERSION
#define _POSIX_THREAD_CPUTIME _POSIX_VERSION
#define _POSIX_TIMEOUTS _POSIX_VERSION
#define _POSIX_THREAD_SPORADIC_SERVER _POSIX_VERSION

/* POSIX 1003.1j D10 */
#define _POSIX_BARRIERS _POSIX_VERSION
#define _POSIX_CLOCK_SELECTION _POSIX_VERSION
#define _POSIX_MONOTONIC_CLOCK _POSIX_VERSION
#define _POSIX_READER_WRITER_LOCKS _POSIX_VERSION
#define _POSIX_SPIN_LOCKS _POSIX_VERSION
#define _POSIX_TYPED_MEMORY_OBJECTS _POSIX_VERSION

/* POSIX 1003.1q-2000 */
#undef _POSIX_TRACE_EVENT_FILTER
#undef _POSIX_TRACE
#undef _POSIX_TRACE_INHERIT
#undef _POSIX_TRACE_LOG

#undef _POSIX_IPV6
#undef _POSIX_RAW_SOCKETS

/* POSIX 1003.1-2001 states job control is mandatory */
#define _POSIX_JOB_CONTROL 1

/* POSIX.1-2001 lists regex (regcomp/regexec/regerror/regfree) and the shell
 * command interface (system/popen/pclose) as mandatory, but RTMux ships
 * neither: there is no regex engine, and system/popen historically assume
 * fork(), which RTMux does not provide (it uses posix_spawn). Advertise both as
 * unsupported (-1) so the macro matches reality -- sysconf() already returns -1
 * for each: the _SC_REGEXP/_SC_SHELL names exist (confname.h) but neither
 * sysconf table carries a value row for them. RTMux therefore conforms to a
 * documented POSIX.1-2001 subset here. Restore to _POSIX_VERSION once a regex
 * engine lands, or once system()/popen() are implemented over posix_spawn.
 */
#define _POSIX_REGEXP -1
#define _POSIX_SHELL -1

#define _POSIX2_C_VERSION _POSIX2_VERSION
#define _POSIX2_C_BIND _POSIX2_VERSION
#define _POSIX2_CHAR_TERM _POSIX2_VERSION
#define _POSIX2_LOCALEDEF _POSIX2_VERSION
#define _POSIX2_UPE _POSIX2_VERSION
#define _POSIX2_SW_DEV _POSIX2_VERSION

/* POSIX 1003.1d Draft 8 */
#define _POSIX_DEVICE_CONTROL 1
#define _POSIX_DEVCTL_DIRECTION 1
#define _POSIX_INTERRUPT_CONTROL 1
/* POSIX 1003.1j Draft 5 */
#undef _POSIX_PROCESS_SPIN_LOCKS
#undef _POSIX_THREAD_SPIN_LOCKS
#undef _POSIX_TYPED_MEMORY_ACCESS_MGMT
#undef _POSIX_SYNCHRONIZED_CLOCK
#define _POSIX_THREAD_ASYNC_ABORT 1

/* Execution-time Symbolic Constants for Portability Specifications */

#define _POSIX_NO_TRUNC 1 /* pathname components > NAME_MAX */
#define _POSIX_VDISABLE 0 /* terminal special chars can be disabled */

#define _POSIX_ASYNC_IO 1
#define _POSIX_PRIO_IO 1
#define _POSIX_SYNC_IO 1

/* Trim the option macros above to the active build profile. Generated from
 * scripts/posix_profile.POSIX_PROFILES; the default `full` profile trims
 * nothing, so this is a no-op unless a smaller profile is selected via the
 * Kconfig POSIX_PROFILE choice.
 */
#include <sys/posix-profile-trim.h>

__BEGIN_DECLS
extern int optind; /*  index of current option being scanned */

extern char *optarg; /*  points to optional argument */

extern int opterr; /*  print|don't print error message */

extern int optopt; /*  offending letter when error detected */

#ifndef SETIOV
#define SETIOV(_iov, _addr, _len) \
    ((_iov)->iov_base = (void *) (_addr), (_iov)->iov_len = (size_t) (_len))
#endif
#ifndef GETIOVBASE
#define GETIOVBASE(_iov) ((_iov)->iov_base)
#endif
#ifndef GETIOVLEN
#define GETIOVLEN(_iov) ((_iov)->iov_len)
#endif

/* POSIX 1003.1 Prototypes */

extern int access(const char *__path, int __mode);
extern int faccessat(int __fd, const char *__path, int __mode, int __flag);
extern int chdir(const char *__path);
extern int close(int __fildes);
extern size_t confstr(int, char *, size_t);
extern int dup(int __fildes);
extern int dup2(int __fildes, int __fildes2);
extern int execl(const char *__path, const char *__arg0, ...);
extern int execle(const char *__path, const char *__arg0, ...);
extern int execlp(const char *__file, const char *__arg0, ...);
extern int execv(const char *__path, char *const __argv[]);
extern int execve(const char *__path,
                  char *const __argv[],
                  char *const __envp[]);
extern int execvp(const char *__file, char *const __argv[]);
extern void _exit(int __status) __attribute__((__noreturn__));
extern long fpathconf(int __fildes, int __name);
extern long pathconf(const char *__path, int __name);
extern char *getcwd(char *__buf, size_t __size);
extern gid_t getegid(void);
extern uid_t geteuid(void);
extern gid_t getgid(void);
extern int getgroups(int __gidsetsize, gid_t __grouplist[]);
extern pid_t getpgrp(void);
extern pid_t getpid(void);
extern pid_t getppid(void);
extern uid_t getuid(void);
extern int issetugid(void);
extern char *getlogin(void);
extern int getlogin_r(char *__name, size_t __namesize);
extern int isatty(int __fildes);
extern char *ttyname(int __fildes);
extern int ttyname_r(int __fildes, char *__name, size_t __namesize);
extern off_t lseek(int __fildes, off_t __offset, int __whence);
extern int pause(void);
extern int pipe(int __fildes[2]);
extern ssize_t pread(int __filedes,
                     void *__buff,
                     size_t __nbytes,
                     off_t __offset);
extern ssize_t pwrite(int __filedes,
                      const void *__buff,
                      size_t __nbytes,
                      off_t __offset);
extern ssize_t read(int __fildes, void *__buffer, size_t __len);
extern ssize_t readlink(const char *__path, char *__buf, size_t __bufsiz);
extern ssize_t readlinkat(int __dirfd,
                          const char *__path,
                          char *__buf,
                          size_t __bufsiz);
extern int setgid(gid_t __newgroup);
extern int setpgid(pid_t __pid, pid_t __pgroupid);
extern pid_t setsid(void);
extern int setuid(uid_t __newuserid);
extern unsigned int sleep(unsigned int __seconds);
extern long sysconf(int __name);
extern pid_t tcgetpgrp(int __fildes);
extern int tcsetpgrp(int __fildes, pid_t __pgrp_id);
extern int unlink(const char *__path);
extern int unlinkat(int __fd, const char *__path, int __flag);
extern ssize_t write(int __fildes, const void *__buf, size_t __len);

extern int fdatasync(int __fildes);
extern int fsync(int __fildes);
extern void sync(void);

extern int chroot(const char *__path);
extern char *crypt(const char *, const char *);
extern int setregid(gid_t __readgroupid, gid_t __effectivegroupid);
extern int setreuid(uid_t __readuserid, uid_t __effectiveuserid);

extern int getopt(int __argc, char *const __argv[], const char *__optstring);

extern int setegid(gid_t __newegroup);
extern int seteuid(uid_t __newuserid);

extern char **environ; /*  pointer to environment table        */

__END_DECLS
#endif

#endif
