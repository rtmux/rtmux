/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * mount - mount and report filesystems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/mount.h>
#include <sys/wait.h>

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/ftype.h>
#include <devctl.h>
#include <devctl/common.h>

#include "fstab.h"

static int verbose;

int checkvfsname(const char *, const char **);
const char **makevfslist(char *);

static int catopt(char **, const char *);
static int hasopt(const char *, const char *);
static int mangle(char *, int *, const char ***, int *);
static int mountfs(const char *,
                   const char *,
                   const char *,
                   unsigned,
                   const char *,
                   const char *,
                   int);
static int usage(void);
static int report_mounts(void);
static int spawn_mount_server(const char *, char *const[], int *, pid_t *);
static int lkl_mount_attached(const char *);
static int mount_lkl_ext4(const char *, const char *, unsigned);

#define _PATH_BIN "/bin"
#define _PATH_USRBIN "/usr/bin"
#define _PATH_SBIN "/sbin"

#define BADTYPE(type) (strcmp(type, FSTAB_RW) && strcmp(type, FSTAB_RO))

/* Spawn a filesystem I/O service. A successful server does not exit after
 * mounting; it registers the mount point and then enters its dispatch loop.
 *
 * Returns 0 on success. *found_out distinguishes "helper not present in this
 * directory" (errno == ENOENT) from "helper ran and failed" so the caller can
 * stop probing alternative paths once any helper has been executed.
 */
static int spawn_mount_server(const char *path,
                              char *const argv[],
                              int *found_out,
                              pid_t *pid_out)
{
    pid_t pid;
    int status = 0;

    *found_out = 0;
    status = posix_spawnp(&pid, path, NULL, NULL, argv, NULL);
    if (status != EOK) {
        if (verbose && status != ENOENT)
            fprintf(stderr, "mount: spawn %s failed: %s\n", path,
                    strerror(status));
        errno = status;
        return -1;
    }
    *found_out = 1;
    if (verbose)
        fprintf(stderr, "mount: spawned %s pid=%d\n", path, (int) pid);

    /* The caller polls for an external readiness signal, such as the mountpoint
     * appearing, and reaps the child itself via pid_out. The child stays
     * reapable so that readiness loop can still catch an early exit.
     */
    if (pid_out) {
        *pid_out = pid;
        return 0;
    }

    for (int i = 0; i < 50; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (verbose)
                fprintf(stderr, "mount: %s exited status=0x%x\n", path, status);
            if (WIFEXITED(status))
                return WEXITSTATUS(status) == 0 ? 0 : 1;
            return 1;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&ts, NULL);
    }

    return 0;
}

static int mount_lkl_ext4(const char *spec, const char *name, unsigned flags)
{
    char *const argv[] = {"lkl-fsd",
                          "--serve-ext4",
                          (char *) spec,
                          (char *) name,
                          (flags & _MOUNT_READONLY) ? "-r" : NULL,
                          NULL};
    int found;
    int rc;
    pid_t pid;

    if (!spec || !name) {
        fprintf(stderr, "mount: lkl-ext4 requires image and mountpoint\n");
        return 1;
    }
    if (lkl_mount_attached(name))
        return 0;
    rc = spawn_mount_server("/sbin/lkl-fsd", argv, &found, &pid);
    if (rc != 0 || !found)
        return rc != 0 ? rc : 1;

    /* lkl-fsd attaches the mountpoint, then rtmux_detach(EXIT_SUCCESS) unblocks
     * our waitpid; any pre-attach failure exits non-zero first. Booting an LKL
     * Linux kernel plus mounting ext4 takes a few seconds, more so when another
     * LKL server competes for CPU. Poll up to 5s for the mount to appear, but
     * bail out immediately if the server dies first.
     */
    for (int i = 0; i < 50; i++) {
        pid_t w = waitpid(pid, NULL, WNOHANG);

        if (lkl_mount_attached(name))
            return 0;
        if (w == pid) {
            fprintf(stderr, "mount: lkl-fsd failed to mount %s\n", name);
            return 1;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "mount: %s did not appear\n", name);
    return 1;
}

static int lkl_mount_attached(const char *name)
{
    struct statvfs stv;

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    int fd = open(name, O_RDONLY);
    int ok = 0;

    if (fd == -1)
        return 0;
    ok = devctl(fd, DEVCTL_STATVFS, &stv, sizeof(stv), NULL) == EOK &&
         !strcmp(stv.f_basetype, "ext4");
    close(fd);
    return ok;
}

int toolbox_mount_main(int argc, char **argv)
{
    const char *mntfromname, *mntonname, *vfstype;
    const char **vfslist;
    char *options, *extoptions;
    struct fstab *fs;
    int all, rval, ch;
    unsigned init_flags;

    rval = 0;
    options = NULL;
    while (argc) {
        vfslist = NULL;
        mntfromname = mntonname = vfstype = extoptions = NULL;
        all = verbose = 0;
        init_flags = 0;
        while ((ch = getopt(argc, argv, "T:t:o:aurwv")) != -1) {
            switch (ch) {
            case 'a':
                all = 1;
                break;
            case 'u':
                init_flags |= _MOUNT_REMOUNT;
                break;
            case 'r':
                init_flags |= _MOUNT_READONLY;
                break;
            case 'w':
                init_flags &= ~_MOUNT_READONLY;
                break;
            case 'o':
                if (catopt(&options, optarg) != 0) {
                    fprintf(stderr, "mount: out of memory\n");
                    free(options);
                    return 1;
                }
                break;
            case 'T':
                init_flags |= _MFLAG_OCB;
                vfslist = makevfslist(optarg);
                vfstype = optarg;
                break;
            case 't':
                init_flags &= ~_MFLAG_OCB;
                vfslist = makevfslist(optarg);
                vfstype = optarg;
                break;
            case 'v':
                verbose++;
                break;
            case '?':
            default:
                return usage();
            }
        }
        argc -= optind;
        argv += optind;

        /* First non-option is the device name */
        if (argc > 0) {
            mntfromname = argv[0];
            argv++;
            argc--;
        }
        /* Next non-option is the mount point */
        if (argc > 0 && *argv[0] != '-') {
            mntonname = argv[0];
            argv++;
            argc--;
        }

        if (verbose) {
            fprintf(stderr, "mount: from [%s] on [%s] type [%s]\n",
                    mntfromname ? mntfromname : "NULL",
                    mntonname ? mntonname : "NULL", vfstype ? vfstype : "NULL");
        }

        if (init_flags & _MOUNT_REMOUNT && !mntfromname) {
            fprintf(stderr, "mount: remount requires a mount point\n");
            free(options);
            return 1;
        }

        /* No arguments: report mounts or mount from fstab */
        if (!mntfromname && !mntonname) {
            if (all || vfstype) {
                while ((fs = getfsent())) {
                    if (BADTYPE(fs->fs_type) ||
                        checkvfsname(fs->fs_vfstype, vfslist) ||
                        hasopt(fs->fs_mntops, "noauto"))
                        continue;
                    if (vfstype && strcmp(vfstype, fs->fs_vfstype) != 0)
                        continue;
                    if (mountfs(fs->fs_vfstype, fs->fs_spec, fs->fs_file,
                                init_flags, options, fs->fs_mntops, 0)) {
                        free(options);
                        return 1;
                    }
                    rval++;
                }
                if (rval == 0) {
                    if (vfstype)
                        fprintf(stderr, "mount: no fstab entries for type %s\n",
                                vfstype);
                    else
                        fprintf(stderr, "mount: no fstab entries\n");
                    free(options);
                    return 1;
                }
            } else {
                report_mounts();
            }
            free(options);
            return 0;
        }
        /* Device specified but no mount point */
        else if (mntfromname && !mntonname) {
            if (init_flags & _MOUNT_REMOUNT) {
                fs = NULL;
            } else {
                fs = getfsfile(mntfromname);
                if (!fs)
                    fs = getfsspec(mntfromname);
                if (fs && BADTYPE(fs->fs_type))
                    fs = NULL;
            }

            if (fs) {
                vfstype = fs->fs_vfstype;
                mntfromname = fs->fs_spec;
                mntonname = fs->fs_file;
                extoptions = fs->fs_mntops;
                init_flags = (init_flags & ~fs->init_mask) | fs->init_flags;
            } else if (init_flags & _MOUNT_REMOUNT) {
                mntonname = mntfromname;
                mntfromname = NULL;
            } else {
                mntonname = "/";
                init_flags |= _MOUNT_IMPLIED;
            }
        }

        if (!vfstype) {
            if (init_flags & (_MOUNT_ENUMERATE | _MOUNT_REMOUNT)) {
                vfstype = "";
            } else {
                /* Default to romfs */
                vfstype = "romfs";
            }
        }

        rval = mountfs(vfstype, mntfromname, mntonname, init_flags, options,
                       extoptions, 0);
        if (rval != 0)
            break;

        optind = 0;
    }

    free(options);
    return rval;
}

static int hasopt(const char *mntopts, const char *option)
{
    int negative, found;
    char *opt, *optbuf;

    if (option[0] == 'n' && option[1] == 'o') {
        negative = 1;
        option += 2;
    } else {
        negative = 0;
    }
    optbuf = strdup(mntopts);
    if (!optbuf) /* on OOM, passing NULL to strtok would resume stale state */
        return 0;
    found = 0;
    for (opt = optbuf; (opt = strtok(opt, ",")); opt = NULL) {
        if (opt[0] == 'n' && opt[1] == 'o') {
            if (!strcasecmp(opt + 2, option))
                found = negative;
        } else if (!strcasecmp(opt, option)) {
            found = !negative;
        }
    }
    free(optbuf);
    return found;
}

/* Attempt to mount a filesystem by spawning mount_<type>, fs-<type>, or <type>.
 * Searches /bin, /usr/bin, /sbin and current directory.
 */
static int mountfs(const char *vfstype,
                   const char *spec,
                   const char *name,
                   unsigned flags,
                   const char *options,
                   const char *mntopts,
                   int skipmounted)
{
    static const char *const edirs[] = {_PATH_SBIN, _PATH_BIN, _PATH_USRBIN,
                                        ".", NULL};
    const char **argv;
    const char *const *edir;
    /* Try the common helper names first, then a direct server name. */
    char execname[PATH_MAX + 1];
    char mount_base[PATH_MAX], fs_base[PATH_MAX];
    int argc, i, maxargc;
    char *optbuf;

    (void) skipmounted;

    optbuf = NULL;
    argv = NULL;
    if (!strcmp(vfstype, "lkl-ext4"))
        return mount_lkl_ext4(spec, name, flags);

    if ((mntopts && catopt(&optbuf, mntopts) != 0) ||
        (options && catopt(&optbuf, options) != 0) ||
        (!mntopts && !options && catopt(&optbuf, "rw") != 0))
        goto enomem;

    if (((flags & _MOUNT_ENUMERATE) && catopt(&optbuf, "enumerate") != 0) ||
        ((flags & _MOUNT_REMOUNT) && catopt(&optbuf, "remount") != 0) ||
        ((flags & _MOUNT_IMPLIED) && catopt(&optbuf, "implied") != 0) ||
        ((flags & _MOUNT_READONLY) && catopt(&optbuf, "ro") != 0) ||
        ((flags & _MFLAG_OCB) && catopt(&optbuf, "nostat") != 0))
        goto enomem;

    maxargc = 64;
    argv = malloc(sizeof(char *) * (size_t) maxargc);
    if (!argv)
        goto enomem;

    argc = 0;
    snprintf(mount_base, sizeof mount_base, "mount_%s", vfstype);
    snprintf(fs_base, sizeof fs_base, "fs-%s", vfstype);
    argv[argc++] = mount_base; /* placeholder, updated per attempt */
    if (optbuf && mangle(optbuf, &argc, &argv, &maxargc) != 0)
        goto enomem;
    if (spec)
        argv[argc++] = spec;
    argv[argc++] = name;
    argv[argc] = NULL;

    if (verbose > 1) {
        fprintf(stderr, "mount: exec:");
        for (i = 0; i < argc; i++)
            fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }

    /* If any helper actually runs (found=1), do not try alternatives. The
     * helper had its say and the mount either succeeded or failed for real.
     */
    const char *const bases[] = {
        [0] = mount_base,
        [1] = fs_base,
        [2] = vfstype,
    };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        for (edir = edirs; *edir; edir++) {
            int found = 0;

            snprintf(execname, sizeof execname, "%s/%s", *edir, bases[b]);
            argv[0] = bases[b];
            i = spawn_mount_server(execname, (char *const *) argv, &found,
                                   NULL);
            if (i == 0) {
                free(argv);
                free(optbuf);
                return 0;
            }
            if (found) {
                if (verbose)
                    fprintf(stderr, "mount: %s: helper failed\n", execname);
                fprintf(stderr,
                        "mount: can't %smount %s (type %s): helper exited "
                        "non-zero\n",
                        (flags & _MOUNT_REMOUNT) ? "re" : "",
                        name ? name : "(null)", *vfstype ? vfstype : "default");
                free(argv);
                free(optbuf);
                return 1;
            }
        }
    }

    fprintf(stderr, "mount: can't %smount %s (type %s): %s\n",
            (flags & _MOUNT_REMOUNT) ? "re" : "", name ? name : "(null)",
            *vfstype ? vfstype : "default", strerror(errno));

    free(argv);
    free(optbuf);
    return 1;

enomem:
    fprintf(stderr, "mount: out of memory\n");
    free(argv);
    free(optbuf);
    return 1;
}

static int catopt(char **sp, const char *o)
{
    char *s;

    s = *sp;
    if (s) {
        size_t i = strlen(s);
        size_t j = i + 1 + strlen(o) + 1;
        char *ns = realloc(s, j);
        if (!ns)
            return -1;
        s = ns;
        snprintf(s + i, j - i, ",%s", o);
    } else {
        s = strdup(o);
        if (!s)
            return -1;
    }
    *sp = s;
    return 0;
}

static int mangle(char *options, int *argcp, const char ***argvp, int *maxargcp)
{
    char *p, *saveptr = NULL;
    int argc = *argcp;
    const char **argv = *argvp;
    int maxargc = *maxargcp;

    for (p = strtok_r(options, ",", &saveptr); p;
         p = strtok_r(NULL, ",", &saveptr)) {
        if (argc >= maxargc - 4) {
            const char **na;
            int new_max = maxargc << 1;

            na = realloc(argv, (size_t) new_max * sizeof(char *));
            if (!na) {
                *argcp = argc;
                *argvp = argv;
                *maxargcp = maxargc;
                return -1;
            }
            argv = na;
            maxargc = new_max;
        }
        if (*p != '\0') {
            if (*p == '-') {
                argv[argc++] = p;
                p = strchr(p, '=');
                if (p) {
                    *p = '\0';
                    argv[argc++] = p + 1;
                }
            } else if (strcmp(p, "rw") != 0) {
                argv[argc++] = "-o";
                argv[argc++] = p;
            }
        }
    }

    *argcp = argc;
    *argvp = argv;
    *maxargcp = maxargc;
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: mount [-aruvw] [-t type | -T type] [-o options]"
            " [device] [mountpoint]\n"
            "       mount                       (list mounted filesystems)\n"
            "\n"
            "Options:\n"
            "  -a            mount every fstab entry (filter by -t if given)\n"
            "  -r            mount read-only\n"
            "  -w            mount read/write\n"
            "  -u            remount an already-mounted filesystem (update)\n"
            "  -v            verbose\n"
            "  -t type       send only to the server that owns 'device'\n"
            "  -T type       'device' is a hint for the server rather than a"
            " real device\n"
            "  -o options    comma-separated server-specific options\n");
    return 1;
}

/* Report mounted filesystems by walking /proc/mount.
 *
 * /proc/mount entries are named: nid,pid,chid,handle,ftype We walk the tree and
 * report directory-type entries. Parse a /proc/mount entry name:
 * "nd,pid,chid,handle,ftype"
 * Returns the number of fields parsed (5 on success).
 */
static int parse_mount_entry(const char *name,
                             int *nd,
                             int *pid,
                             int *chid,
                             int *handle,
                             int *ftype)
{
    int *const fields[] = {
        [0] = nd, [1] = pid, [2] = chid, [3] = handle, [4] = ftype,
    };
    const char *p = name;
    int i;

    for (i = 0; i < 5; i++) {
        char *end;
        long val;
        errno = 0;
        val = strtol(p, &end, 10);
        if (end == p)
            return i;
        if (errno == ERANGE || val < INT_MIN || val > INT_MAX)
            return i;
        *fields[i] = (int) val;
        if (i < 4) {
            if (*end != ',')
                return i + 1;
            p = end + 1;
        }
    }
    return 5;
}

static int ftype_is_user_mount(int ftype)
{
    return ftype == _FTYPE_ANY || ftype == _FTYPE_MOUNT ||
           ftype == _FTYPE_NAME || ftype == _FTYPE_FILE;
}

static int report_mount_dir(const char *proc_path,
                            const char *mount_path,
                            int *shown)
{
    DIR *dp;
    struct dirent *dent;
    int nd, pid, chid, handle, ftype;

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    dp = opendir(proc_path);
    if (!dp) {
        if (errno != ENOENT && errno != ENOTDIR)
            fprintf(stderr, "mount: %s: %s\n", proc_path, strerror(errno));
        return 1;
    }

    while ((dent = readdir(dp))) {
        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
            continue;
        if (parse_mount_entry(dent->d_name, &nd, &pid, &chid, &handle,
                              &ftype) == 5) {
            if (verbose) {
                printf("%s: nd %d pid %d chid %d handle %d ftype %d\n",
                       mount_path, nd, pid, chid, handle, ftype);
                (*shown)++;
            } else if (ftype_is_user_mount(ftype)) {
                printf("pid %d on %s\n", pid, mount_path);
                (*shown)++;
            }
        } else {
            char child_proc[PATH_MAX];
            char child_mount[PATH_MAX];

            snprintf(child_proc, sizeof(child_proc), "%s/%s", proc_path,
                     dent->d_name);
            if (!strcmp(mount_path, "/"))
                snprintf(child_mount, sizeof(child_mount), "/%s", dent->d_name);
            else
                snprintf(child_mount, sizeof(child_mount), "%s/%s", mount_path,
                         dent->d_name);
            report_mount_dir(child_proc, child_mount, shown);
        }
    }
    closedir(dp);
    return 0;
}

static int report_mounts(void)
{
    int shown = 0;
    int rc;

    rc = report_mount_dir("/proc/mount", "/", &shown);
    if (shown == 0 && rc == 0)
        puts("mount: no mounted filesystems reported");
    return rc;
}
