/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/rtmux-core.h>

#include <devctl/common.h>

#include "bootstrap-manifest.h"
#include "externs.h"
#include "root-process.h"
#include "root-log.h"

static const char default_shell_path[] = "/bin/sh";

/* Close every fd the caller's fdmap marked SPAWN_FDCLOSED, before loader_fd()
 * rebuilds inheritance. This runs early (before open_exe()/elf_load_hook());
 * that is safe today only because the loader's ELF fd is transient and closed
 * before loader_fd(), so it never survives into the child. Ordering trap: any
 * loader step added between here and loader_fd() that opens a *persistent*
 * descriptor could land on one of these just-closed low fd numbers and silently
 * re-populate an fd the caller asked to close — move this preclose adjacent to
 * loader_fd() if that ever happens.
 */
static void loader_preclose_fdmap(struct loader_context *lcp)
{
    int32_t *pfd;

    if (lcp->msg.spawn.i.nfds == 0)
        return;

    pfd = (int32_t *) ((char *) &lcp->msg.spawn.i + sizeof lcp->msg.spawn.i);
    for (unsigned i = 0; i < lcp->msg.spawn.i.nfds; i++) {
        if (pfd[i] == SPAWN_FDCLOSED)
            (void) close((int) i);
    }
}

extern void *_mmap2(void *addr,
                    size_t len,
                    int prot,
                    int flags,
                    int fd,
                    off64_t off,
                    unsigned align,
                    unsigned preload,
                    void **base,
                    size_t *size);

/* set_signal() removed — signal subsystem eliminated */

/* loader_exit — runs when a child died during loading (failed to exec, fault,
 * etc.). Pulls prp->lcp via process_query_info (lcp is a roottask-user-VA
 * pointer that the kernel just copied through, so the deref below is safe) and
 * clears it via the new process_clear_lcp kernel extension. prp pointer is
 * opaque now — caller passes pid_t.
 *
 * Return value: 1 if a spawn request was still waiting and has just been
 * answered with an error (the parent never learns this pid, so nobody will wait
 * for it), else 0.
 */
int loader_exit(ioserv_context_t *ctp, union proc_msg_union *msg, pid_t pid)
{
    struct loader_context *lcp;
    root_proc_info_t qinfo;
    root_process_t *tmp = root_proc(pid);
    int r, replied = 0;

    (void) ctp;
    (void) msg;

    if (process_query_info(pid, &qinfo) != EOK)
        return 0;
    lcp = (struct loader_context *) qinfo.lcp;
    if (!lcp)
        return 0;

    if (lcp->ctx.rcvid != -1) {
        /* siginfo lives in root_process_t now (POSIX policy moved out of the
         * kernel-half process_t).
         */
        switch (tmp->siginfo.si_signo) {
        case SIGCHLD:
            r = tmp->siginfo.si_status;
            break;
        case SIGBUS:
            r = (lcp->ctx.fault_errno != EOK) ? lcp->ctx.fault_errno : ENOMEM;
            break;
        default:
            r = EINTR;
            break;
        }
        msg_error(lcp->ctx.rcvid, r);
        replied = 1;
    }
    loader_context_free(lcp);
    (void) process_clear_lcp(pid);
    return replied;
}

/* Ask the proc service for the key that authorizes this loader thread's dup of
 * one parent fd. The reply entry carries the parent's connection and a key over
 * the exact _IO_DUP bytes; the service refuses anyone but a child's own loader.
 * Parameters:
 *   parent_fd : the fd in the parent to inherit
 *   child_fd  : the fd number it takes in the child
 *   ent       : the entry filled from the reply
 * Return value: EOK, or an errno.
 */
static int loader_dup_key(struct loader_context *lcp,
                          int parent_fd,
                          int child_fd,
                          struct _proc_spawn_fd_info *ent)
{
    proc_spawn_fd_t spawnfd;
    int32_t list = parent_fd;
    iov_t siov[2], riov[2];

    memset(&spawnfd, 0, sizeof spawnfd);
    spawnfd.i.type = _PROC_SPAWN;
    spawnfd.i.subtype = _PROC_SPAWN_FD;
    spawnfd.i.base = child_fd;
    spawnfd.i.ppid = lcp->ctx.ppid;
    spawnfd.i.flags = _PROC_SPAWN_FD_LIST;
    spawnfd.i.nfds = 1;
    SETIOV(siov + 0, &spawnfd.i, sizeof spawnfd.i);
    SETIOV(siov + 1, &list, sizeof list);
    SETIOV(riov + 0, &spawnfd.o, sizeof spawnfd.o);
    SETIOV(riov + 1, ent, sizeof *ent);
    /* ipc-send: roottask-fsio -> roottask-core */
    if (msg_sendv(ROOTTASK_COID, siov, 2, riov, 2) == -1) {
        return errno;
    }
    return spawnfd.o.nfds == 1 ? EOK : EBADF;
}

/* Loading is abandoned after fd keys were minted: have the proc service
 * scramble this thread's key before it exits.
 */
static void loader_revoke_key(struct loader_context *lcp)
{
    proc_spawn_fd_t spawnfd;
    iov_t siov;

    memset(&spawnfd, 0, sizeof spawnfd);
    spawnfd.i.type = _PROC_SPAWN;
    spawnfd.i.subtype = _PROC_SPAWN_FD;
    spawnfd.i.ppid = lcp->ctx.ppid;
    spawnfd.i.flags = _PROC_SPAWN_FD_REVOKE;
    SETIOV(&siov, &spawnfd.i, sizeof spawnfd.i);
    /* ipc-send: roottask-fsio -> roottask-core */
    (void) msg_sendv(ROOTTASK_COID, &siov, 1, NULL, 0);
}

/* Get the fd list that needs duplicating, either from the explicit spawn fd
 * array or by walking the parent's inherited descriptors. connect_server_info()
 * supplies each server endpoint, and dup messages ask those servers to attach
 * the child-side fds.
 */
static int loader_fd(struct loader_context *lcp)
{
    struct _server_info info;
    int *pfd, fd2, fd, nfds, i_nfds;
    enum { _FD_SPAWN_LIST, _FD_SPAWN_NOCLOEXC } type;
    io_dup_t msgdup;
    struct _proc_spawn_fd_info ent;
    int parent_fd;

    type = _FD_SPAWN_NOCLOEXC;
    pfd = NULL;

    nfds = i_nfds = lcp->msg.spawn.i.nfds;
    if (nfds != 0) {
        type = _FD_SPAWN_LIST;
        pfd = (int *) ((char *) &lcp->msg.spawn.i + sizeof lcp->msg.spawn.i);
    }

    fd = -1;
    errno = EMFILE;
    parent_fd = -1;

    while (1) {
        switch (type) {
        case _FD_SPAWN_LIST:
            if (++fd >= nfds) {
                if (lcp->msg.spawn.i.parms.flags & SPAWN_FD_SCAN_TAIL) {
                    type = _FD_SPAWN_NOCLOEXC;
                    fd = nfds - 1;
                    nfds = i_nfds = 0;
                    pfd = NULL;
                    continue;
                }
                return EOK;
            }
            CRASHCHECK(!pfd);
            if ((fd2 = *(pfd++)) == SPAWN_FDCLOSED) {
                continue;
            }
            if ((connect_server_info(lcp->ctx.ppid, (uint64_t) fd2, &info) !=
                 fd2) ||
                (info.flags & RTMUX_FLG_COF_DEAD)) {
                return EBADF;
            }
            info.flags = 0;

            /* A remote-node fd cannot be inherited: the roottask fd-info
             * service only answers one local fd per request from the child's
             * own loader, so a remote query never worked.
             */
            if (RTMUX_NODE_CMP(info.nd, RTMUX_LOCAL_NODE) != 0)
                return ENOTSUP;
            parent_fd = fd2;
            break;

        case _FD_SPAWN_NOCLOEXC:
            root_trace("loader_fd: connect_server_info(ppid=%d, fd=%d)...\n",
                       lcp->ctx.ppid, fd + 1);
            if (connect_server_info(lcp->ctx.ppid, (uint64_t) ++fd, &info) ==
                    -1 ||
                ((fd = info.coid) & (int) RTMUX_SIDE_CHANNEL)) {
                root_trace("loader_fd: no more fds (fd=%d), done\n", fd);
                return EOK;
            }
            root_trace(
                "loader_fd: found fd=%d nd=%d pid=%d chid=%d scoid=0x%x "
                "flags=0x%x\n",
                fd, info.nd, info.pid, info.chid, info.scoid, info.flags);
            if (info.flags & RTMUX_FLG_COF_DEAD) {
                continue;
            }
            if (info.flags & RTMUX_FLG_COF_CLOEXEC) {
                continue;
            }
            if (RTMUX_NODE_CMP(info.nd, RTMUX_LOCAL_NODE) != 0)
                return ENOTSUP;
            parent_fd = fd;
            break;

        default:
            crash();
        }

        root_trace(
            "loader_fd: connect_attach(nd=%d, pid=%d, chid=%d, fd=%d, "
            "flags=0x%x)\n",
            info.nd, info.pid, info.chid, fd, info.flags);
        if (connect_attach(info.nd, info.pid, info.chid, (unsigned) fd,
                           info.flags) != fd) {
            root_trace("loader_fd: connect_attach FAILED errno=%d\n", errno);
            return errno;
        }

        if (!((unsigned) info.chid & RTMUX_GLOBAL_CHANNEL)) {
            {
                int err = loader_dup_key(lcp, parent_fd, fd, &ent);

                if (err != EOK)
                    return err;
            }
            spawn_dup_init(&msgdup, lcp->ctx.ppid, ent.srcnd, ent.scoid,
                           ent.coid);
            msgdup.i.key = ent.key;
            root_trace("loader_fd: msg_send DUP fd=%d scoid=0x%x coid=%d\n", fd,
                       info.scoid, info.coid);
            /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
            if (msg_send(fd, &msgdup.i, sizeof msgdup.i, 0, 0) == -1)
                return errno;
        }
    }
}

/* Test whether a path names a regular (or named) file the caller may execute.
 * Parameters:
 *   path : the pathname to check
 * Return value: 0 if executable; -1 with errno set otherwise.
 */
static int is_exec(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
        return -1;

    if (!S_ISREG(st.st_mode) && !S_ISNAM(st.st_mode)) {
        errno = EACCES;
        return -1;
    }

    if (access(path, X_OK) == -1)
        return -1;

    return 0;
}

/* Search a colon-separated path list for an executable named "name".
 * Parameters:
 *   name    : the file name to find
 *   path    : colon-separated list of directories to search
 *   buffer  : output buffer for the resolved full path
 *   bufsize : size of buffer
 * Return value: 0 with buffer filled on success; -1 with errno set
 *               (ENOENT, ENAMETOOLONG) otherwise.
 */
static int searchpath(const char *name,
                      const char *path,
                      char *buffer,
                      size_t bufsize)
{
    char *b;
    ssize_t n;
    int trailing;


    if (!name || *name == '\0' || !path || *path == '\0') {
        errno = ENOENT;
    } else {
#ifndef NDEBUG
        /* assert proper args */
        if ((!buffer) || (bufsize < strlen(name)))
            crash();
#endif

        bufsize -= strlen(name);
        do {
            for (n = (ssize_t) bufsize, *(b = buffer) = 0;
                 *path && *path != ':'; n--, path++) {
                if (n > 0) {
                    *b++ = *path;
                }
            }
            if (n > 0) {
                if (*buffer && b[-1] != '/') {
                    *b++ = '/';
                }
                STRLCPY(b, name, bufsize - (size_t) (b - buffer));
                if (is_exec(buffer) != -1) {
                    return 0;
                }
            } else {
                errno = ENAMETOOLONG;
                break;
            }
            trailing = (*path != ':') ? 0 : !*++path;
        } while (*path || trailing);
    }
    *buffer = 0;
    return -1;
}

/* Build the new process's initial stack contents: argv, envp, and the aux
 * vector.
 *
 * Lays the argument and environment strings, the AT_EXEFILE entry, and the
 * argv/envp pointer arrays out at the top of the child's stack, fetching the
 * argument bytes via a _PROC_SPAWN_ARGS message if they did not fit inline.
 * When an interpreter is involved, its name (and optional argument) replace
 * argv[0] appropriately. Updates lcp->ctx.start.start_sp to the final stack
 * pointer. Parameters:
 *   lcp         : loader context for the child
 *   p           : pointer to the inline argv/envp string block
 *   exefile     : the executable name to record as AT_EXEFILE
 *   interp_name : interpreter name, or NULL if none
 *   interp_arg  : interpreter argument, or NULL if none
 */
static void loader_args(struct loader_context *lcp,
                        const char *p,
                        const char *exefile,
                        char *interp_name,
                        char *interp_arg)
{
    char *aux_exefile;
    const char **arg, *orig_exefile;
    char *strings;
    int naux;
    auxv_t *auxv;
    size_t exelen;
    size_t arg_bytes;
    int ninterp = 0;
    size_t interplen = 0, interp_name_len = 0, interp_arg_len = 0;

    int skip_arg0 = 0;

    p += strlen(p) + 1;

    orig_exefile = NULL;
    if (interp_name) {
        orig_exefile = exefile;
        exefile = interp_name;
        interplen = interp_name_len = strlen(interp_name) + 1;
        interplen += strlen(orig_exefile) + 1;
        ninterp = 2;
    }

    if (interp_arg) {
        interp_arg_len = strlen(interp_arg) + 1;
        interplen += interp_arg_len;
        ninterp++;
    }

    arg_bytes = lcp->msg.spawn.i.nbytes;
    arg_bytes += interplen;

#ifdef STACK_GROWS_UP
#error STACK_GROWS_UP not supported
#endif
    /* strings is also top of stack */
    exelen = strlen(exefile) + 1;
    aux_exefile = (char *) lcp->ctx.start.start_sp - exelen;
    strcpy(aux_exefile, exefile);
    if (exefile[0] != '/') {
        int cwd_len;

        /* lcp->ctx.pid is the child's pid, set by proc_spawn before the loader
         * runs. ctx.process is the kernel-VA process_t pointer — opaque to
         * U-mode roottask (heap PTE_U=0).
         */
        root_process_t *tmp = root_proc(lcp->ctx.pid);

        /* not a full path, tack the CWD at the front */
        cwd_len = path_node_fullpath(tmp->cwd, NULL, 0);
        if (cwd_len > 0) {
            int real_len;

            aux_exefile -= cwd_len;
            real_len = path_node_fullpath(tmp->cwd, aux_exefile, cwd_len);
            if (real_len > 0) {
                if (real_len < cwd_len) {
                    /* The first path_node_fullpath() can overestimate the
                     * length of the CWD during path normalization. If that
                     * happens, slide things up.
                     */
                    memmove(aux_exefile + 1, aux_exefile, (size_t) real_len);
                    ++aux_exefile;
                }
                /* Add a path separator between the CWD and exefile */
                aux_exefile[real_len - 1] = '/';
            }
        }
    }
    strings = aux_exefile - arg_bytes;

    if (interp_name) {
        strcpy(strings, interp_name);
        if (interp_arg) {
            strcpy(strings + interp_name_len, interp_arg);
        }
        CRASHCHECK(!orig_exefile);
        strcpy(strings + interp_name_len + interp_arg_len, orig_exefile);
    }
    /* can only modify "p" and "nbytes" */
    if (lcp->ctx.msgsize - (p - lcp->msg.filler) > lcp->msg.spawn.i.nbytes) {
        memcpy(strings + interplen, p, lcp->msg.spawn.i.nbytes);
    } else {
        proc_spawn_args_t args;

        args.i.type = _PROC_SPAWN;
        args.i.subtype = _PROC_SPAWN_ARGS;
        args.i.nbytes = (uint32_t) lcp->msg.spawn.i.nbytes;
        args.i.offset = (uintptr_t) p - (uintptr_t) &lcp->msg;
        args.i.zero = 0;
        /* ipc-send: roottask-fsio -> roottask-core */
        if (msg_send(ROOTTASK_COID, &args.i, sizeof args.i, strings + interplen,
                     args.i.nbytes) == -1) {
            _exit(errno);
        }
    }

    /* count the size of the aux vector */
    for (naux = 1, auxv = lcp->ctx.start.aux; auxv->a_type != AT_NULL;
         auxv++, naux++) {
        /* nothing to do */
    }
    auxv[1] = auxv[0];
    auxv->a_type = AT_EXEFILE;
    auxv->a_un.a_ptr = aux_exefile;
    naux++;

    /* find start of args, must include argc and 2 terminating null pointers */
    arg = (const char **) ((((uintptr_t) strings & ~(sizeof *arg - 1)) -
                            ((unsigned) ninterp + lcp->msg.spawn.i.nargv +
                             lcp->msg.spawn.i.narge +
                             (unsigned) naux *
                                 (sizeof *lcp->ctx.start.aux / sizeof *arg) +
                             3) *
                                sizeof *arg) &
                           ~(STACK_ALIGNMENT - 1));
    lcp->ctx.start.start_sp = (uintptr_t) arg;
    if ((*(unsigned long *) arg = lcp->msg.spawn.i.nargv) == 0) {
        do {
            root_log("loader EINVAL @ %s:%d\n", __FILE__, __LINE__);
            _exit(EINVAL);
        } while (0);
    }

    /* account for the interpreter and its arg if they are present */
    if (ninterp) {
        *(unsigned long *) arg +=
            (unsigned) (ninterp - 1); /* -1 is because we skip argv[0] */
    }
    arg++;

    for (p = strings;;) {
        *arg++ = p;

        if (lcp->msg.spawn.i.nargv) {
            if (ninterp) {
                ninterp--;
                if (ninterp == 0) {
                    /* we have to skip argv[0] because it isn't the correct
                     * argv[0]. the correct argv[0] (i.e. with a full path) is
                     * stored in orig_exefile and is accounted for by "ninterp".
                     */
                    skip_arg0 = 1;
                    lcp->msg.spawn.i.nargv--;
                }
            } else {
                lcp->msg.spawn.i.nargv--;
            }

            if (lcp->msg.spawn.i.nargv == 0) {
                *arg++ = 0;
            }
        }

        if (lcp->msg.spawn.i.nargv == 0) {
            if (lcp->msg.spawn.i.narge == 0) {
                *arg++ = 0;
                break;
            } else {
                lcp->msg.spawn.i.narge--;
            }
        }

    redo:
        while (*p++ != '\0') {
            if (p > aux_exefile) {
                do {
                    root_log("loader EINVAL @ %s:%d\n", __FILE__, __LINE__);
                    _exit(EINVAL);
                } while (0);
            }
        }
        if ((char *) arg > strings) {
            do {
                root_log("loader EINVAL @ %s:%d\n", __FILE__, __LINE__);
                _exit(EINVAL);
            } while (0);
        }
        if (skip_arg0) { /* this makes us skip over two strings */
            skip_arg0 = 0;
            goto redo;
        }
    }
    memcpy(arg, lcp->ctx.start.aux,
           sizeof *lcp->ctx.start.aux * (unsigned) naux);
}

/* Parse a "#!" interpreter line from the head of a file. Parameters:
 *   fd     : open file descriptor to read from
 *   pname  : out — set to the interpreter name (points into buf)
 *   arg    : out — set to the optional interpreter argument, or NULL
 *   buf    : caller buffer the first line is read into
 *   buflen : size of buf
 * Return value: EOK if a valid "#!" line was parsed; ENOEXEC if the
 *               file does not begin with a usable "#!" line.
 */
static int try_hash_bang(int fd,
                         char **pname,
                         char **arg,
                         char *buf,
                         int buflen)
{
    ssize_t numread;
    char *ptr = buf;

    *pname = NULL; /* initialize these two */
    *arg = NULL;

    numread = proc_read(fd, buf, buflen, 0);
    if (numread <= 3) { /* need at least #! characters */
        return ENOEXEC;
    }

    if (ptr[0] != '#' || ptr[1] != '!') {
        return ENOEXEC;
    }
    ptr += 2;

    /* skip whitespace after the #! */
    while (ptr < (buf + numread) && (*ptr == ' ' || *ptr == '\t')) {
        ptr++;
    }

    /* If this condition is true it means the first line was just "#!\n". The
     * spec says this is not valid. We could be nice and interpret it to be the
     * same as "#!/bin/sh" but we won't do that for now.
     */
    if (ptr >= (buf + numread) || *ptr == '\n') {
        return ENOEXEC;
    }

    /* interpreter name starts here */
    *pname = ptr;

    /* find the end of the interpeter name */
    while (ptr < (buf + numread) && *ptr != ' ' && *ptr != '\t' &&
           *ptr != '\n') {
        ptr++;
    }

    if (ptr >= (buf + numread)) { /* hmmm, interpreter name was too long... */
        return ENOEXEC;
    }

    if (*ptr == '\n') { /* The interpreter path ends at the newline. */
        *ptr = '\0';
        return EOK;
    }

    *ptr++ = '\0'; /* Terminate the interpreter path and parse its arguments. */

    /* skip whitespace after the #! */
    while (ptr < (buf + numread) && (*ptr == ' ' || *ptr == '\t')) {
        ptr++;
    }

    if (ptr >= (buf + numread) || *ptr == '\n') { /* there was nothing else */
        return EOK;
    }

    *arg = ptr;
    /* find the end of the interpeter name */
    while (ptr < (buf + numread) && *ptr != '\n') {
        ptr++;
    }

    if (ptr >= (buf + numread)) {
        ptr--;
    }
    if (*ptr == '\n' || *ptr == ' ' || *ptr == '\t') {
        ptr--;
        while (*ptr == ' ' || *ptr == '\t') {
            ptr--;
        }
        ptr++;
    }

    *ptr = '\0'; /* null terminate the arg */

    return EOK;
}

/* Open an executable for loading and stat it.
 *
 * Tries the real uid/gid first, falling back to the effective ids, and clears
 * the setuid/setgid bits if the containing filesystem is mounted nosuid. On any
 * error it _exit()s the loader thread with errno. Parameters:
 *   name  : pathname of the executable
 *   statp : out — stat of the opened file
 * Return value: the open file descriptor (never returns on failure).
 */
static int open_exe(const char *name, struct stat *statp)
{
    int fd;
    unsigned flags;

    /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
    fd = open(name, O_RDONLY);
    if ((fd == -1) || (fstat(fd, statp) == -1)) {
        if (errno == EBUSY) {
            errno = ETXTBSY;
        }
        _exit(errno);
    }

    /* Honour the MOUNT_NOSUID qualifier by knocking down these bits (affects
     * local/internal processing only). If filesystem does not support this
     * probe, assume that MOUNT_NOSUID is disabled.
     */
    if ((statp->st_mode & (S_ISUID | S_ISGID)) &&
        _devctl(fd, DEVCTL_GET_MOUNT_FLAGS, &flags, sizeof(flags), 0) == EOK &&
        (flags & _MOUNT_NOSUID)) {
        statp->st_mode &= ~(mode_t) (S_ISUID | S_ISGID);
    }
    return fd;
}

#define BOOT_IMAGE_HASH_FNV_OFFSET 1469598103934665603ULL
#define BOOT_IMAGE_HASH_FNV_PRIME 1099511628211ULL

static int boot_image_hash_fd(int fd, uint64_t *hashp)
{
    unsigned char buf[512];
    uint64_t hash = BOOT_IMAGE_HASH_FNV_OFFSET;
    off_t save;

    save = lseek(fd, 0, SEEK_CUR);
    if (save == (off_t) -1)
        return errno;
    if (lseek(fd, 0, SEEK_SET) == (off_t) -1)
        return errno;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0)
            return errno;
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            hash ^= buf[i];
            hash *= BOOT_IMAGE_HASH_FNV_PRIME;
        }
    }
    if (lseek(fd, save, SEEK_SET) == (off_t) -1)
        return errno;
    *hashp = hash;
    return EOK;
}

#define LOADER_EXIT(code) _exit(code)

/* Loader thread for posix_spawn(): turn a spawn request into a running process
 * image.
 *
 * Resolves the executable (honouring SPAWN_SEARCH_PATH, setsid, setpgid,
 * runmask, and uid/gid handling), loads it with elf_load_hook (falling back to
 * a "#!" interpreter or the default shell on ENOEXEC), maps and initializes the
 * stack, builds argv/envp/aux via loader_args, duplicates inherited fds,
 * publishes the entry point and stack pointer to the kernel, and finally
 * replies _PROC_SPAWN_DONE / _PROC_SPAWN_EXEC to the proc service. Any failure
 * _exit()s the loader thread with an errno that the parent's spawn observes.
 * Parameters:
 *   parm : the loader_context describing the spawn
 * Return value: NULL (never reached — the thread always _exit()s).
 */
void *loader_load(void *parm)
{
    struct loader_context *lcp = parm;
    const char *p;
    int fd;
    int status;
    uid_t uid;
    gid_t gid;
    struct stat statl;
    char buffer[_POSIX_PATH_MAX + 1];
    void *base;
    size_t size;
    char *interp_name = NULL, *interp_arg = NULL;
    char linebuf[128]; /* for the '#!' test */
    int needs_secure;
    int resetids;
    unsigned prealloc;

    buffer[0] = '\0';
    lcp->ctx.tid = pthread_self();
    resetids = (lcp->msg.spawn.i.parms.flags & POSIX_SPAWN_RESETIDS) != 0;

    /* SignalProcmask removed — signal subsystem eliminated */

    switch (lcp->msg.spawn.i.parms.flags & SPAWN_ALIGN_MASK) {
    case SPAWN_ALIGN_DEFAULT:
        status = 0;
        break;
    case SPAWN_ALIGN_FAULT:
        status = 1;
        break;
    case SPAWN_ALIGN_NOFAULT:
        status = -1;
        break;
    default:
        LOADER_EXIT(EINVAL);
    }
    (void) thread_ctl(RTMUX_TCTL_ALIGN_FAULT, &status);

    /* SPAWN_SETSIGDEF / SPAWN_SETSIGIGN ignored — signals eliminated */

    if ((fd = connect_attach(0, ROOTTASK_PID, ROOTTASK_CHID, ROOTTASK_COID,
                             0)) == -1) {
        LOADER_EXIT(errno);
    } else if (fd != ROOTTASK_COID) {
        LOADER_EXIT(EL3HLT);
    }

    if (lcp->msg.spawn.i.parms.flags & SPAWN_SETSID) {
        if (setsid() == -1) {
            LOADER_EXIT(errno);
        }
    }

    if (lcp->msg.spawn.i.parms.flags & SPAWN_EXPLICIT_CPU) {
        unsigned runmask = lcp->msg.spawn.i.parms.runmask;
        struct {
            struct _thread_runmask hdr;
            unsigned mask[2];
        } thread_runmask = {
            .hdr = {.size = 1U},
        };

        /* Verify specified runmask is valid */
        if ((runmask & LEGAL_CPU_BITMASK) == 0) {
            LOADER_EXIT(EINVAL);
        }
        thread_runmask.mask[0] = runmask;
        thread_runmask.mask[1] = runmask;
        if (thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT,
                       &thread_runmask) == -1) {
            LOADER_EXIT(errno);
        }
    }

    if (lcp->msg.spawn.i.parms.flags & SPAWN_SETGROUP) {
        if (setpgid(getpid(), lcp->msg.spawn.i.parms.pgroup) == -1) {
            LOADER_EXIT(errno);
        }
    }
    loader_preclose_fdmap(lcp);

    /* proc_spawn set saved_user_id to 0, so this will work... */
    uid = getuid();
    gid = getgid();
    (void) setreuid(geteuid(), 0);
    (void) setregid(getegid(), getgid());

    p = (const char *) &lcp->msg.spawn.i +
        (sizeof lcp->msg.spawn.i + lcp->msg.spawn.i.nfds * sizeof(int32_t));
    if (lcp->msg.spawn.i.parms.flags & SPAWN_SEARCH_PATH) {
        if (searchpath(p + lcp->msg.spawn.i.searchlen, p, buffer,
                       sizeof buffer) == -1) {
            LOADER_EXIT(errno);
        }
        p += lcp->msg.spawn.i.searchlen;
    } else {
        if (is_exec(p) == -1) {
            LOADER_EXIT(errno);
        } else if (!memccpy(buffer, p, '\0', sizeof(buffer))) {
            LOADER_EXIT(ENAMETOOLONG);
        }
    }

    fd = open_exe(buffer, &statl);

    /* Provenance for the bootstrap capability grant: a manifest grant is
     * honored only for an image that physically lives on the read-only boot
     * cpio. Two facts identify it, and both come from the actually-opened fd
     * rather than the caller's namespace, so chroot or bind-mount path tricks
     * cannot forge them. First, the fd's serving process must be roottask
     * itself: cpiofs is a roottask-internal path service, whereas any mounted
     * filesystem, including a root-mounted one carrying an imposter, is a
     * separate server with a different pid that the kernel tracks and the
     * server cannot spoof. Second, among roottask's own services the backing
     * device (statl.st_rdev) must equal the cpio device number, which selects
     * cpiofs over roottask's other devices (devmem, sysfs). An external server
     * can lie about st_rdev but cannot masquerade as roottask's pid, and a
     * roottask-internal service reports its device honestly, so the pair is not
     * forgeable by an unprivileged caller or a root-mounted imposter service.
     * cpiofs_boot_devno is zero until the cpio is registered, which fails the
     * grant closed. The grant itself is bound below, on the direct-ELF path, to
     * the resolved path.
     */
    struct _server_info exec_srv;
    dev_t boot_devno = cpiofs_boot_devno();
    bool exec_on_boot =
        boot_devno != 0 && statl.st_rdev == boot_devno &&
        connect_server_info(0, (uint64_t) fd, &exec_srv) == fd &&
        exec_srv.pid == ROOTTASK_PID;
    uint64_t exec_hash = 0;
    lcp->ctx.grant_caps = 0;
    lcp->ctx.grant_system = 0;

    if (exec_on_boot) {
        status = boot_image_hash_fd(fd, &exec_hash);
        if (status != EOK)
            LOADER_EXIT(status);
        status = bootstrap_cap_manifest_grant_for(buffer, exec_hash, NULL);
        if (status != EOK)
            LOADER_EXIT(status);
    }

    lcp->ctx.start.aux[0].a_type = AT_NULL;
    lcp->ctx.start.aux[0].a_un.a_val = 0;

    /* Secure mode compares the image owner with the spawner's real ids. The
     * thread's own real ids were replaced above by setreuid(geteuid(), 0), so
     * use the values captured before that call.
     */
    needs_secure = 0;
    if ((statl.st_mode & S_ISUID) && (statl.st_uid != uid)) {
        needs_secure = 1;
    }
    if ((statl.st_mode & S_ISGID) && (statl.st_gid != gid)) {
        needs_secure = 1;
    }
    if (needs_secure) {
        lcp->ctx.start.aux[0].a_type = AT_LIBPATH;
        lcp->ctx.start.aux[1].a_type = AT_RTMUX_SECURE;
        lcp->ctx.start.aux[1].a_un.a_val = 1;
        lcp->ctx.start.aux[2].a_type = AT_NULL;
        lcp->ctx.start.aux[2].a_un.a_val = 0;
    }
    if ((status = elf_load_hook(fd, buffer, &lcp->ctx.start, &statl,
                                &lcp->msg.spawn.i.parms)) == -1) {
        /* fd is not an ELF executable - see if we should exec interpreter */
        status = try_hash_bang(fd, &interp_name, &interp_arg, linebuf,
                               sizeof(linebuf));
        if (status == ENOEXEC &&
            lcp->msg.spawn.i.parms.flags & SPAWN_CHECK_SCRIPT) {
            /* POSIX 1003.1-2001 requires exec[vl]p to execute a shell as the
             * interpreter for ENOEXEC errors.
             */
            strcpy(linebuf, default_shell_path);
            interp_name = linebuf;
            interp_arg = 0;
            status = EOK;
        }
        close(fd);

        if (status == EOK) {
            if (is_exec(interp_name) == -1) {
                LOADER_EXIT(errno);
            }
            fd = open_exe(interp_name, &statl);
            status = elf_load_hook(fd, interp_name, &lcp->ctx.start, &statl,
                                   &lcp->msg.spawn.i.parms);
        }

        if (status == -1) {
            status = ENOEXEC;
        }
    } else if (exec_on_boot) {
        /* The direct ELF load of the resolved path succeeded; bind the manifest
         * grant to that boot-image path. The script or interpreter fallback
         * above runs a generic interpreter, not the named server, so an image
         * that reaches U-mode through it gets no grant.
         */
        status = bootstrap_cap_manifest_grant_for(buffer, exec_hash,
                                                  &lcp->ctx.grant_caps);
        if (status != EOK) {
            close(fd);
            LOADER_EXIT(status);
        }
        lcp->ctx.grant_system = bootstrap_cap_manifest_listed(buffer);
    }
    if (status != EOK) {
        close(fd);
        LOADER_EXIT(status);
    }
    close(fd);

    if (lcp->msg.spawn.i.parms.flags & SPAWN_SETSTACKMAX) {
        lcp->ctx.start.stacksize = lcp->msg.spawn.i.parms.stack_max;
    }

    if (lcp->ctx.start.stacksize < lcp->ctx.start.stackalloc) {
        lcp->ctx.start.stacksize = lcp->ctx.start.stackalloc;
    }
    if (lcp->ctx.flags & PROC_LF_LAZYSTACK) {
        prealloc = 0;
    } else {
        prealloc = (unsigned) lcp->ctx.start.stackalloc;
    }
    if (_mmap2((void *) lcp->ctx.start.stackaddr,
               lcp->ctx.start.stacksize + root_vm_ops.pagesize,
               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_STACK, NOFD,
               root_vm_ops.pagesize, __PAGESIZE, prealloc, &base,
               &size) == MAP_FAILED) {
        LOADER_EXIT(ENOMEM);
    }

    /* Reserve a 16-byte guard at the very top of the stack mapping.
     *
     * Without this, libc functions that take a pointer to their own caller
     * frame (compiler-emitted prologue: "addi s0, sp, N", "sd s0, (N-16)(sp)")
     * end up with "s0" exactly equal to (base + size) — the first VA of
     * whatever follows the stack mapping in the address space. In the logger
     * service's case that's libc.so text at 0x38000000 (mapped R-X) and any
     * later "sd reg, off(s0)" SIGSEGVs on the read-only text.
     *
     * Asking thread_tls to lay TLS / argv / envp / auxv out below "base + size
     * - 16" instead of "base + size" shifts the whole stack initial-content one
     * slot down; the resulting start_sp is far below the guarded top, but the
     * running process's highest possible "s0" value still sits 16 bytes inside
     * writable memory.
     */
    if ((lcp->ctx.start.start_sp = (uintptr_t) thread_tls(
             0, (uintptr_t) base, size, root_vm_ops.pagesize,
             (uintptr_t) base + size - 16)) == (uintptr_t) -1) {
        LOADER_EXIT(errno == EFAULT ? ENOMEM : errno);
    }

    loader_args(lcp, p, buffer, interp_name, interp_arg);

    if (!(lcp->ctx.flags & SPAWN_EXEC)) {
        status = loader_fd(lcp);
        if (status != EOK) {
            loader_revoke_key(lcp);
            LOADER_EXIT(status);
        }
    }

    (void) setregid(getegid(), (statl.st_mode & S_ISGID)
                                   ? statl.st_gid
                                   : (resetids ? gid : getgid()));
    (void) setreuid(uid, (statl.st_mode & S_ISUID)
                             ? statl.st_uid
                             : (resetids ? uid : getuid()));

    if (!(lcp->ctx.flags & SPAWN_EXEC)) {
        if ((lcp->msg.spawn.i.parms.flags & SPAWN_TCSETPGROUP) &&
            isatty(STDIN_FILENO) > 0) {
            if ((lcp->msg.spawn.i.parms.flags & SPAWN_SETSID) &&
                tcsetsid(STDIN_FILENO, getpgrp()) == -1) {
                root_log(
                    "loader: pid %d: cannot bind controlling terminal "
                    "(errno %d)\n",
                    lcp->ctx.pid, errno);
            }
            if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1) {
                root_log(
                    "loader: pid %d: cannot set foreground group "
                    "(errno %d)\n",
                    lcp->ctx.pid, errno);
            }
        }
    }

    /* Pre-publish initial_ip and initial_sp on the child's prp so
     * kext_process_startup can read them. The kernel heap is PTE_U=0, so U-mode
     * roottask cannot write prp directly; this goes through
     * __KSYS_PRIV_PROC_SET_SPAWN, which also applies the SPAWN_EXEC /
     * SPAWN_NOZOMBIE inheritance rules. lcp->ctx.flags only holds 32-bit
     * SPAWN_* values; narrow safely at the boundary into the kcall_args
     * struct's unsigned field.
     */
    unsigned spawn_flags = (unsigned) lcp->ctx.flags;
    (void) process_set_spawn(lcp->ctx.pid, lcp->ctx.start.start_ip,
                             lcp->ctx.start.start_sp, &spawn_flags);
    lcp->ctx.flags = spawn_flags;

    lcp->msg.spawn_done.i.type = _PROC_SPAWN;
    lcp->msg.spawn_done.i.subtype =
        (lcp->ctx.flags & SPAWN_EXEC) ? _PROC_SPAWN_EXEC : _PROC_SPAWN_DONE;
    SETIOV(lcp->ctx.iov + 0, &lcp->msg.spawn_done.i,
           sizeof lcp->msg.spawn_done.i);
    /* ipc-send: roottask-fsio -> roottask-core */
    msg_sendv(ROOTTASK_COID, lcp->ctx.iov, 1, 0, 0);
    LOADER_EXIT(errno);
    return 0;
}
