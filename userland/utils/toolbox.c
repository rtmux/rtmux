/*
 * toolbox - small multi-call utility for early RTMux userland.
 *
 * This is not a full BusyBox clone. It collects tiny commands whose interface
 * is much smaller than a standalone source file, so /bin can grow useful
 * script-building blocks without adding one binary and one build rule per
 * command.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <ctype.h>
#include <crypt.h>
#include <devctl/common.h>
#include <devctl.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/procfs.h>
#include <sys/rtmux-core.h>
#include <sys/states.h>
#include <sys/stat.h>
#include <sys/roottask.h>
#include <sys/sysmsg.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <generated/autoconf.h>

#include "toolbox.h"

#define COPY_BUFSZ 4096
#define TOOLBOX_LINE_MAX 512
#define LOGIN_MAX_TRIES 3
#define LOGIN_BACKOFF_SECONDS 3
#define LOGIN_DEFAULT_PATH "/bin:/sbin:/usr/bin:/usr/sbin"
#define LOGIN_DEFAULT_SHELL "/bin/sh"
#define LOGIN_NAME_MAX 32
#define SHADOW_PATH "/etc/shadow"

static int applet_basename(int argc, char **argv);
static int applet_authtest(int argc, char **argv);
static int applet_cat(int argc, char **argv);
static int applet_clear(int argc, char **argv);
static int applet_cp(int argc, char **argv);
static int applet_date(int argc, char **argv);
static int applet_df(int argc, char **argv);
static int applet_dirname(int argc, char **argv);
static int applet_echo(int argc, char **argv);
static int applet_env(int argc, char **argv);
static int applet_false(int argc, char **argv);
static int applet_find(int argc, char **argv);
static int applet_help(int argc, char **argv);
static int applet_head(int argc, char **argv);
static int applet_id(int argc, char **argv);
static int applet_login(int argc, char **argv);
static int applet_ls(int argc, char **argv);
static int applet_less(int argc, char **argv);
static int applet_more(int argc, char **argv);
static int applet_mv(int argc, char **argv);
static int applet_not(int argc, char **argv);
static int applet_printenv(int argc, char **argv);
static int applet_ps(int argc, char **argv);
static int applet_pwd(int argc, char **argv);
static int applet_rm(int argc, char **argv);
static int applet_seq(int argc, char **argv);
static int applet_setconsole(int argc, char **argv);
static int applet_shutdown(int argc, char **argv);
static int applet_sleep(int argc, char **argv);
static int applet_stat(int argc, char **argv);
static int applet_stty(int argc, char **argv);
static int applet_sync(int argc, char **argv);
static int applet_crash(int argc, char **argv);
static int applet_inject(int argc, char **argv);
static int applet_tail(int argc, char **argv);
static int applet_tee(int argc, char **argv);
static int applet_test(int argc, char **argv);
static int applet_top(int argc, char **argv);
static int applet_touch(int argc, char **argv);
static int applet_true(int argc, char **argv);
static int applet_wc(int argc, char **argv);
static int applet_which(int argc, char **argv);
static int applet_whoami(int argc, char **argv);
static int applet_yes(int argc, char **argv);
static int applet_grep(int argc, char **argv);
static int applet_tr(int argc, char **argv);
static int applet_cut(int argc, char **argv);
static int applet_sort(int argc, char **argv);
static int applet_uniq(int argc, char **argv);
static int applet_kill(int argc, char **argv);
static int applet_xargs(int argc, char **argv);
static int applet_expr(int argc, char **argv);
static int applet_printf(int argc, char **argv);
static int applet_readlink(int argc, char **argv);
/* Network applets backed by the LKL server. */
static int applet_dns(int argc, char **argv);
static int applet_hello(int argc, char **argv);
static int applet_host(int argc, char **argv);
static int applet_httpget(int argc, char **argv);
static int applet_ifconfig(int argc, char **argv);
static int applet_ip(int argc, char **argv);
static int applet_nc(int argc, char **argv);
static int applet_netstat(int argc, char **argv);
static int applet_nslookup(int argc, char **argv);
static int applet_ping(int argc, char **argv);
static int applet_route(int argc, char **argv);
static int applet_wget(int argc, char **argv);

static char **toolbox_environ(void)
{
    return environ;
}

static const struct toolbox_applet applets[] = {
    {"authtest", applet_authtest, "authtest USER"},
    {"basename", applet_basename, "basename PATH"},
    {"cat", applet_cat, "cat [FILE ...]"},
    {"clear", applet_clear, "clear"},
    {"cp", applet_cp, "cp SRC DST"},
    {"crash", applet_crash, "crash"},
    {"cut", applet_cut, "cut -d DELIM -f LIST | -c LIST [FILE ...]"},
    {"date", applet_date, "date"},
    {"df", applet_df, "df PATH ..."},
    {"dirname", applet_dirname, "dirname PATH"},
    {"dmesg", toolbox_dmesg_main, "dmesg [-w] [FILE]"},
    {"echo", applet_echo, "echo [-n] [ARG ...]"},
    {"env", applet_env, "env [NAME=VALUE ...] [COMMAND [ARG ...]]"},
    {"expr", applet_expr, "expr EXPRESSION"},
    {"false", applet_false, "false"},
    {"find", applet_find, "find [PATH ...]"},
    {"getty", toolbox_getty_main, "getty [TTY]"},
    {"grep", applet_grep, "grep [-cilnqv] PATTERN [FILE ...]"},
    {"help", applet_help, "help"},
    {"head", applet_head, "head [-n LINES] [FILE ...]"},
    {"id", applet_id, "id [USER]"},
    {"inject", applet_inject,
     "inject spin-irqoff HART MS | spin-console HART MS | hang-hart HART | "
     "fail-alloc POOL PID | alloc-status"},
    {"kill", applet_kill, "kill [-SIG] PID ..."},
    {"login", applet_login, "login [USER]"},
    {"less", applet_less, "less [FILE ...]"},
    {"ls", applet_ls, "ls [-la] [PATH ...]"},
    {"mount", toolbox_mount_main, "mount [OPTIONS] [SPECIAL NODE]"},
    {"more", applet_more, "more [FILE ...]"},
    {"mv", applet_mv, "mv SRC DST"},
    {"not", applet_not, "not COMMAND [ARG ...]"},
    {"printenv", applet_printenv, "printenv [NAME ...]"},
    {"printf", applet_printf, "printf FORMAT [ARG ...]"},
    {"ps", applet_ps, "ps"},
    {"pwd", applet_pwd, "pwd"},
    {"readlink", applet_readlink, "readlink [-f] FILE ..."},
    {"rm", applet_rm, "rm FILE ..."},
    {"seq", applet_seq, "seq [FIRST [INCREMENT]] LAST"},
    {"setconsole", applet_setconsole, "setconsole DEVICE"},
    {"sh", toolbox_sh_main, "sh [-i] [-c COMMAND] [SCRIPT]"},
    {"shutdown", applet_shutdown, "shutdown [-h | -r] [-n]"},
    {"sleep", applet_sleep, "sleep SECONDS"},
    {"sort", applet_sort, "sort [-bfnru] [FILE ...]"},
    {"stat", applet_stat, "stat FILE ..."},
    {"stty", applet_stty, "stty [-a] [isig | -isig | tostop | -tostop ...]"},
    {"sync", applet_sync, "sync [FILE ...]"},
    {"tail", applet_tail, "tail [-n LINES] [FILE ...]"},
    {"tee", applet_tee, "tee [-a] [FILE ...]"},
    {"test", applet_test, "test EXPR"},
    {"top", applet_top, "top [-n COUNT] [-d SECONDS]"},
    {"touch", applet_touch, "touch FILE ..."},
    {"tr", applet_tr, "tr [-ds] SET1 [SET2]"},
    {"true", applet_true, "true"},
    {"uniq", applet_uniq, "uniq [-cdu] [FILE]"},
    {"vi", toolbox_vi_main, "vi [FILE]"},
    {"wc", applet_wc, "wc [FILE ...]"},
    {"which", applet_which, "which COMMAND ..."},
    {"whoami", applet_whoami, "whoami"},
    {"xargs", applet_xargs, "xargs [-n MAX] [COMMAND [ARG ...]]"},
    {"yes", applet_yes, "yes [STRING]"},
    /* Network applets backed by the LKL server. */
    {"dns", applet_dns, "dns [nameserver A.B.C.D [PORT]]"},
    {"hello", applet_hello, "hello"},
    {"host", applet_host, "host NAME"},
    {"httpget", applet_httpget, "httpget"},
    {"ifconfig", applet_ifconfig, "ifconfig"},
    {"ip", applet_ip, "ip [addr|route|link]"},
    {"nc", applet_nc, "nc [-u] HOST PORT TEXT|--http-get|--probe-owners"},
    {"netstat", applet_netstat, "netstat [-rn|-l]"},
    {"nslookup", applet_nslookup, "nslookup HOST"},
    {"ping", applet_ping, "ping [-4] [-c COUNT] [-W SEC] HOST"},
    {"route", applet_route, "route"},
    {"wget", applet_wget, "wget URL"},
    {NULL, NULL, NULL},
};

const struct toolbox_applet *toolbox_applets(void)
{
    return applets;
}

struct proc_row {
    pid_t pid;
    pid_t ppid;
    unsigned pri;
    unsigned threads;
    uint64_t cpu_ns;
    unsigned tid;
    char state[12];
    char name[64];
};

static const char *progname(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static const char *applet_name(const char *name)
{
    if (name[0] == '-' && name[1] != '\0')
        return name + 1;
    return name;
}

static const struct toolbox_applet *find_applet(const char *name)
{
    const struct toolbox_applet *ap;

    for (ap = applets; ap->name; ap++) {
        if (!strcmp(name, ap->name))
            return ap;
    }
    return NULL;
}

static void usage_all(FILE *out)
{
    const struct toolbox_applet *ap;

    fprintf(out, "usage: APPLET [args...]\n");
    fprintf(out, "available applets:\n");
    for (ap = applets; ap->name; ap++)
        fprintf(out, "  %-10s %s\n", ap->name, ap->usage);
}

static int copy_fd(int in, int out, const char *name)
{
    static char buf[COPY_BUFSZ];
    ssize_t n;

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;

        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t) (n - off));
            if (w <= 0) {
                /* write(2) returning 0 isn't an error in POSIX but it stalls
                 * the copy loop, so surface it as EIO rather than spin.
                 */
                if (w == 0)
                    errno = EIO;
                fprintf(stderr, "write: %s\n", strerror(errno));
                return -1;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
}

static int parse_positive(const char *s, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < 0)
        return -1;
    *out = v;
    return 0;
}

#define THREAD_STATE_NAMES(X)       \
    X(STATE_DEAD, "DEAD")           \
    X(STATE_RUNNING, "RUN")         \
    X(STATE_READY, "READY")         \
    X(STATE_STOPPED, "STOP")        \
    X(STATE_SEND, "SEND")           \
    X(STATE_RECEIVE, "RECV")        \
    X(STATE_REPLY, "REPLY")         \
    X(STATE_STACK, "STACK")         \
    X(STATE_WAITTHREAD, "WAIT")     \
    X(STATE_WAITPAGE, "WAITPG")     \
    X(STATE_SIGSUSPEND, "SIGSUSP")  \
    X(STATE_SIGWAITINFO, "SIGWAIT") \
    X(STATE_NANOSLEEP, "NANOSLP")   \
    X(STATE_MUTEX, "MUTEX")         \
    X(STATE_CONDVAR, "COND")        \
    X(STATE_JOIN, "JOIN")           \
    X(STATE_INTR, "INTR")           \
    X(STATE_SEM, "SEM")             \
    X(STATE_WAITCTX, "WAITCTX")     \
    X(STATE_NET_SEND, "NETSND")     \
    X(STATE_NET_REPLY, "NETRPL")

#define THREAD_STATE_ENTRY(state, name) [state] = name,

static const char *thread_state_name(unsigned state)
{
    static const char *const names[] = {THREAD_STATE_NAMES(THREAD_STATE_ENTRY)};

    if (state < sizeof(names) / sizeof(names[0]) && names[state])
        return names[state];
    return "?";
}

#undef THREAD_STATE_ENTRY
#undef THREAD_STATE_NAMES

static int process_state_rank(unsigned state)
{
    switch (state) {
    case STATE_RUNNING:
        return 0;
    case STATE_READY:
        return 1;
    case STATE_SEND:
    case STATE_REPLY:
        return 2;
    case STATE_RECEIVE:
        return 3;
    case STATE_DEAD:
        return 5;
    default:
        return 4;
    }
}

static void trim_newline(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static int bytes_equal_constant_time(const char *a, const char *b, size_t len)
{
    unsigned char diff = 0;

    for (size_t i = 0; i < len; i++)
        diff |= (unsigned char) ((unsigned char) a[i] ^ (unsigned char) b[i]);
    return diff == 0;
}

static const char *shadow_hash_for(const char *name)
{
    static char hash[TOOLBOX_LINE_MAX];

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    FILE *fp = fopen(SHADOW_PATH, "r");
    char line[TOOLBOX_LINE_MAX];

    if (!fp)
        return NULL;

    while (fgets(line, sizeof(line), fp)) {
        char *user = line;
        char *pass = strchr(user, ':');
        char *end;

        if (!pass || user[0] == '#' || user[0] == '\n')
            continue;
        *pass++ = '\0';
        if (strcmp(user, name) != 0)
            continue;

        end = strchr(pass, ':');
        if (end)
            *end = '\0';
        trim_newline(pass);
        snprintf(hash, sizeof(hash), "%s", pass);
        fclose(fp);
        return hash;
    }

    fclose(fp);
    return NULL;
}

static const char *account_hash(const struct passwd *pw)
{
    if (!pw || !pw->pw_passwd)
        return NULL;
    if (strcmp(pw->pw_passwd, "x") != 0)
        return pw->pw_passwd;
    return shadow_hash_for(pw->pw_name);
}

static int password_hash_equal(const char *attempt, const char *stored)
{
    size_t len;

    if (!attempt || !stored)
        return 0;
    len = strlen(stored);
    if (strlen(attempt) != len)
        return 0;

    /* Password hashes are authentication material; compare equal-length hashes
     * without leaking the first differing byte.
     */
    return bytes_equal_constant_time(attempt, stored, len);
}

static int read_proc_name(pid_t pid, char *buf, size_t len)
{
    struct {
        procfs_debuginfo info;
        char path[PATH_MAX];
    } debug;
    char path[64];
    int fd;
    ssize_t n;

    if (len == 0)
        return -1;
    buf[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%d", (int) pid);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        memset(&debug, 0, sizeof(debug));
        if (devctl(fd, DEVCTL_PROC_MAPDEBUG_BASE, &debug, sizeof(debug),
                   NULL) == EOK &&
            debug.info.path[0] != '\0') {
            snprintf(buf, len, "%s", progname(debug.info.path));
            close(fd);
            return 0;
        }
        close(fd);
    }

    snprintf(path, sizeof(path), "/proc/%d/name", (int) pid);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(buf, len, "?");
        return -1;
    }
    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) {
        snprintf(buf, len, "?");
        return -1;
    }
    buf[n] = '\0';
    trim_newline(buf);
    if (buf[0] == '\0')
        snprintf(buf, len, "?");
    return 0;
}

static int read_proc_status(int fd,
                            const procfs_info *info,
                            procfs_status *status)
{
    procfs_status best;
    int best_rank = 999;
    int lasttid;
    int tid;
    int scanned = 0;
    int have_best = 0;
    int limit = (int) info->num_threads * 2 + 16;

    if (limit < 32)
        limit = 32;
    memset(&best, 0, sizeof(best));
    for (lasttid = tid = 1; scanned < limit; lasttid = ++tid, scanned++) {
        int rank;

        memset(status, 0, sizeof(*status));
        status->tid = tid;
        if (devctl(fd, DEVCTL_PROC_TIDSTATUS, status, sizeof(*status), NULL) !=
            EOK)
            break;
        tid = (int) status->tid;
        if (tid < lasttid)
            break;
        if (status->state != 0 || status->tid_flags != 0 ||
            status->priority != 0 || status->sutime != 0) {
            rank = process_state_rank(status->state);
            if (!have_best || rank < best_rank) {
                best = *status;
                best_rank = rank;
                have_best = 1;
                if (rank == 0)
                    break;
            }
        }
    }
    if (have_best) {
        *status = best;
        return 0;
    }
    return -1;
}

static int read_proc_row(pid_t pid, struct proc_row *row)
{
    char path[64];
    int fd;
    procfs_info info;
    procfs_status status;
    int have_status;

    snprintf(path, sizeof(path), "/proc/%d", (int) pid);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    memset(&info, 0, sizeof(info));
    if (devctl(fd, DEVCTL_PROC_INFO, &info, sizeof(info), NULL) != EOK) {
        close(fd);
        return -1;
    }
    if (info.flags & RTMUX_FLG_PROC_ZOMBIE) {
        close(fd);
        return -1;
    }

    have_status = read_proc_status(fd, &info, &status) == 0;
    if (have_status)
        snprintf(row->state, sizeof(row->state), "%s",
                 thread_state_name(status.state));
    else
        snprintf(row->state, sizeof(row->state), "?");
    close(fd);

    row->pid = info.pid;
    row->ppid = info.parent;
    row->pri = info.priority;
    row->threads = info.num_threads;
    row->cpu_ns = info.utime + info.stime;
    row->tid = have_status ? (unsigned) status.tid : 0;
    read_proc_name(pid, row->name, sizeof(row->name));
    if (!strcmp(row->name, "?")) {
        if (pid == ROOTTASK_PID)
            snprintf(row->name, sizeof(row->name), "roottask");
        else if (info.flags & RTMUX_FLG_PROC_LOADING)
            snprintf(row->name, sizeof(row->name), "(Loading)");
        else
            snprintf(row->name, sizeof(row->name), "[%d]", (int) pid);
    }
    return 0;
}

static int collect_procs(struct proc_row **out, size_t *out_count)
{
    DIR *dp;
    struct dirent *de;
    struct proc_row *rows = NULL;
    size_t count = 0;
    size_t cap = 0;

    /* ipc-send: utils -> roottask-core */
    dp = opendir("/proc");
    if (!dp) {
        fprintf(stderr, "ps: /proc: %s\n", strerror(errno));
        return -1;
    }
    while ((de = readdir(dp))) {
        struct proc_row row;
        char *end;
        long pid;

        if (!isdigit((unsigned char) de->d_name[0]))
            continue;
        pid = strtol(de->d_name, &end, 10);
        if (*end != '\0' || pid <= 0)
            continue;
        if (read_proc_row((pid_t) pid, &row) != 0)
            continue;
        if (count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            struct proc_row *new_rows =
                realloc(rows, new_cap * sizeof(*new_rows));
            if (!new_rows) {
                free(rows);
                closedir(dp);
                errno = ENOMEM;
                return -1;
            }
            rows = new_rows;
            cap = new_cap;
        }
        rows[count++] = row;
    }
    closedir(dp);
    *out = rows;
    *out_count = count;
    return 0;
}

static int cmp_proc_cpu(const void *a, const void *b)
{
    const struct proc_row *pa = a;
    const struct proc_row *pb = b;

    if (pa->cpu_ns < pb->cpu_ns)
        return 1;
    if (pa->cpu_ns > pb->cpu_ns)
        return -1;
    return (int) pa->pid - (int) pb->pid;
}

static void print_ps_rows(struct proc_row *rows, size_t count)
{
    printf("%5s %5s %3s %3s %-8s %8s %s\n", "PID", "PPID", "PRI", "THR",
           "STATE", "CPUms", "COMMAND");
    for (size_t i = 0; i < count; i++) {
        printf("%5d %5d %3u %3u %-8s %8llu %s\n", (int) rows[i].pid,
               (int) rows[i].ppid, rows[i].pri, rows[i].threads, rows[i].state,
               (unsigned long long) (rows[i].cpu_ns / 1000000ULL),
               rows[i].name);
    }
}

static void mode_string(mode_t mode, char out[11])
{
    out[0] = S_ISDIR(mode)    ? 'd'
             : S_ISLNK(mode)  ? 'l'
             : S_ISCHR(mode)  ? 'c'
             : S_ISBLK(mode)  ? 'b'
             : S_ISFIFO(mode) ? 'p'
             : S_ISSOCK(mode) ? 's'
                              : '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void ls_print_entry(const char *name,
                           const char *path,
                           struct stat *st,
                           int long_mode)
{
    char target[PATH_MAX];
    int target_len = -1;

    if (S_ISLNK(st->st_mode) && path) {
        target_len = (int) readlink(path, target, sizeof(target) - 1);
        if (target_len >= 0)
            target[target_len] = '\0';
    }
    if (!long_mode) {
        if (target_len >= 0)
            printf("%s -> %s\n", name, target);
        else
            printf("%s\n", name);
        return;
    }

    {
        char mode[11];
        mode_string(st->st_mode, mode);
        if (target_len >= 0)
            printf("%s %2d %5d %5d %8ld %s -> %s\n", mode, (int) st->st_nlink,
                   (int) st->st_uid, (int) st->st_gid, (long) st->st_size, name,
                   target);
        else
            printf("%s %2d %5d %5d %8ld %s\n", mode, (int) st->st_nlink,
                   (int) st->st_uid, (int) st->st_gid, (long) st->st_size,
                   name);
    }
}

static int ls_one(const char *path, int all_mode, int long_mode)
{
    struct stat st;
    DIR *dp;
    struct dirent *de;
    int rc = 0;

    if (lstat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        ls_print_entry(progname(path), path, &st, long_mode);
        return 0;
    }

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    dp = opendir(path);
    if (!dp) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    while ((de = readdir(dp))) {
        char child[PATH_MAX];

        if (!all_mode && de->d_name[0] == '.')
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (lstat(child, &st) < 0) {
            fprintf(stderr, "ls: %s: %s\n", child, strerror(errno));
            rc = 1;
            continue;
        }
        ls_print_entry(de->d_name, child, &st, long_mode);
    }
    closedir(dp);
    return rc;
}

/* Cap on directory-recursion depth. Each frame carries a PATH_MAX buffer, so an
 * unbounded walk of a deep (or crafted) tree overflows the stack; 64 is far
 * deeper than any realistic layout. Tuning bound; revisit if a real tree needs
 * it.
 */
#define FIND_MAX_DEPTH 64

static int find_one(const char *path, int depth)
{
    struct stat st;
    DIR *dp;
    struct dirent *de;
    int rc = 0;

    if (lstat(path, &st) < 0) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        return 1;
    }
    puts(path);
    if (!S_ISDIR(st.st_mode))
        return 0;
    if (depth >= FIND_MAX_DEPTH) {
        fprintf(stderr, "find: %s: max directory depth (%d) exceeded\n", path,
                FIND_MAX_DEPTH);
        return 1;
    }

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    dp = opendir(path);
    if (!dp) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        return 1;
    }
    while ((de = readdir(dp))) {
        char child[PATH_MAX];

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        rc |= find_one(child, depth + 1);
    }
    closedir(dp);
    return rc;
}

static int applet_find(int argc, char **argv)
{
    int rc = 0;

    if (argc == 1)
        return find_one(".", 0);
    for (int i = 1; i < argc; i++)
        rc |= find_one(argv[i], 0);
    return rc;
}

static int applet_basename(int argc, char **argv)
{
    const char *name;

    if (argc != 2) {
        fprintf(stderr, "usage: basename PATH\n");
        return 1;
    }
    name = progname(argv[1]);
    printf("%s\n", *name != '\0' ? name : "/");
    return 0;
}

static char *read_password(const char *prompt)
{
    static char password[128];
    bool opened_tty = false, have_tio = false;
    /* ipc-send: utils -> roottask-core */
    int tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);

    if (tty_fd >= 0) {
        opened_tty = true;
    } else {
        tty_fd = STDIN_FILENO;
    }

    struct termios saved, raw;
    if (tcgetattr(tty_fd, &saved) == 0) {
        have_tio = true;
        raw = saved;
        raw.c_lflag &= (tcflag_t) ~(ECHO | ECHONL);
        raw.c_lflag |= ICANON;
        (void) tcsetattr(tty_fd, TCSANOW, &raw);
    }

    FILE *out = opened_tty ? NULL : stderr;
    if (prompt && *prompt) {
        if (out) {
            fputs(prompt, out);
        } else {
            (void) !write(tty_fd, prompt, strlen(prompt));
        }
    }
    if (out)
        fflush(out);

    size_t len = 0;
    while (len + 1 < sizeof(password)) {
        char ch;
        ssize_t n = read(tty_fd, &ch, 1);

        if (n <= 0)
            break;
        if (ch == '\n' || ch == '\r')
            break;
        password[len++] = ch;
    }
    password[len] = '\0';

    if (have_tio)
        (void) tcsetattr(tty_fd, TCSANOW, &saved);
    if (out) {
        fputc('\n', out);
        fflush(out);
    } else {
        (void) !write(tty_fd, "\n", 1);
    }
    if (opened_tty)
        close(tty_fd);

    return password;
}

static int applet_authtest(int argc, char **argv)
{
    struct passwd *pw;
    const char *stored;
    char *guess;
    char *attempt;

    if (argc != 2) {
        fprintf(stderr, "usage: authtest USER\n");
        return 2;
    }
    pw = getpwnam(argv[1]);
    if (!pw) {
        fprintf(stderr, "authtest: %s: not in /etc/passwd\n", argv[1]);
        return 1;
    }
    printf("user:    %s\n", pw->pw_name);
    printf("uid/gid: %u/%u\n", (unsigned) pw->pw_uid, (unsigned) pw->pw_gid);
    printf("home:    %s\n", pw->pw_dir);
    printf("shell:   %s\n", pw->pw_shell);

    stored = account_hash(pw);

    if (!stored) {
        fprintf(stderr, "authtest: %s: shadow entry missing\n", argv[1]);
        return 1;
    }
    if (stored[0] == '\0') {
        puts("(empty password slot - accepting any input)");
        return 0;
    }
    guess = read_password("Password: ");
    attempt = crypt(guess, stored);
    memset(guess, 0, strlen(guess));
    if (!attempt) {
        fprintf(stderr, "authtest: crypt failed\n");
        return 1;
    }
    if (password_hash_equal(attempt, stored)) {
        puts("OK - password matches.");
        return 0;
    }
    puts("FAIL - password does not match.");
    return 1;
}

static void login_prompt_name(char *buf, size_t bufsz)
{
    fputs("login: ", stdout);
    fflush(stdout);
    if (!fgets(buf, (int) bufsz, stdin))
        exit(0);
    trim_newline(buf);
}

static char *login_argv0(const char *shell)
{
    const char *base = progname(shell);
    size_t len = strlen(base);
    char *out = malloc(len + 2);

    if (!out)
        return NULL;
    out[0] = '-';
    memcpy(out + 1, base, len + 1);
    return out;
}

static int applet_login(int argc, char **argv)
{
    char name[LOGIN_NAME_MAX + 1] = "";
    struct passwd *pw = NULL;
    const char *hash = NULL;
    char *guess;
    int tries = 0;
    int home_ok;
    const char *shell;
    char *shell_argv0;
    pid_t pid;
    int status;

    if (argc > 2) {
        fprintf(stderr, "usage: login [USER]\n");
        return 1;
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    if (argc == 2) {
        strncpy(name, argv[1], sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    for (;;) {
        if (name[0] == '\0')
            login_prompt_name(name, sizeof(name));

        pw = getpwnam(name);
        hash = account_hash(pw);
        if (pw && hash && hash[0] == '\0')
            break;

        guess = read_password("Password: ");
        if (pw && hash && hash[0] != '\0') {
            char *enc = crypt(guess, hash);

            if (password_hash_equal(enc, hash)) {
                memset(guess, 0, strlen(guess));
                break;
            }
        }
        memset(guess, 0, strlen(guess));

        if (++tries >= LOGIN_MAX_TRIES) {
            fputs("Maximum number of tries exceeded.\n", stderr);
            return 1;
        }
        fputs("Login incorrect\n", stdout);
        sleep(LOGIN_BACKOFF_SECONDS);
        name[0] = '\0';
    }

    home_ok = chdir(pw->pw_dir) == 0;
    if (!home_ok)
        chdir("/");

    if (setgid(pw->pw_gid) < 0) {
        perror("setgid");
        return 1;
    }
    if (setuid(pw->pw_uid) < 0) {
        perror("setuid");
        return 1;
    }

    shell = pw->pw_shell[0] != '\0' ? pw->pw_shell : LOGIN_DEFAULT_SHELL;
    setenv("HOME", pw->pw_dir, 1);
    setenv("PWD", home_ok ? pw->pw_dir : "/", 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", LOGIN_DEFAULT_PATH, 0);

    shell_argv0 = login_argv0(shell);
    if (!shell_argv0)
        return 1;
    {
        char *spawn_argv[] = {
            [0] = shell_argv0,
            [1] = NULL,
        };
        int err;

        err = posix_spawn(&pid, shell, NULL, NULL, spawn_argv, NULL);
        if (err != EOK) {
            fprintf(stderr, "%s: %s\n", shell, strerror(err));
            free(shell_argv0);
            return 1;
        }
    }
    free(shell_argv0);

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int stream_files(const char *applet, int argc, char **argv)
{
    int rc = 0;

    if (argc == 1)
        return copy_fd(STDIN_FILENO, STDOUT_FILENO, "stdin") != 0;

    for (int i = 1; i < argc; i++) {
        int fd;

        if (!strcmp(argv[i], "-")) {
            if (copy_fd(STDIN_FILENO, STDOUT_FILENO, "stdin") != 0)
                rc = 1;
            continue;
        }

        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "%s: %s: %s\n", applet, argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (copy_fd(fd, STDOUT_FILENO, argv[i]) != 0)
            rc = 1;
        close(fd);
    }
    return rc;
}

static int applet_cat(int argc, char **argv)
{
    return stream_files("cat", argc, argv);
}

static int print_file(const char *applet, const char *path);

static int applet_hello(int argc, char **argv)
{
    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: hello\n");
        return 1;
    }
    puts("LKL-fsd reuses Linux ext4 inside a contained user-mode server.");
    puts("Mount it with:");
    puts("  mount -t lkl-ext4 /dev/nvme0n1p1 /fs/root");
    puts("");
    puts("/fs/root/hello.txt:");
    return print_file("hello", "/fs/root/hello.txt");
}

static int print_file(const char *applet, const char *path)
{
    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    int fd = open(path, O_RDONLY);
    int rc;

    if (fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", applet, path, strerror(errno));
        if (errno == ENOENT) {
            if (strncmp(path, "/net/", 5) == 0)
                fprintf(stderr, "%s: start /sbin/lkl-netd --serve-net first\n",
                        applet);
        }
        return 1;
    }
    rc = copy_fd(fd, STDOUT_FILENO, path) != 0;
    close(fd);
    return rc;
}

#define TOOLBOX_NETD_SLOTS 4

static int net_clone_id(const char *proto, char *id, size_t idsz);
static void net_close_id(const char *proto, const char *id);
static int copy_http_response_fd(const char *applet,
                                 int fd,
                                 bool require_content_length);
static int dns_lookup_a(const char *name, char *addr, size_t addrsz);

static int applet_dns(int argc, char **argv)
{
    char line[64];
    int fd;
    size_t len;

    if (argc == 1)
        return print_file("dns", "/net/dns");
    if ((argc != 3 && argc != 4) || strcmp(argv[1], "nameserver") != 0) {
        fprintf(stderr, "usage: dns [nameserver A.B.C.D [PORT]]\n");
        return 1;
    }
    if (snprintf(line, sizeof(line),
                 argc == 4 ? "nameserver %s %s\n" : "nameserver %s\n", argv[2],
                 argc == 4 ? argv[3] : "") >= (int) sizeof(line)) {
        fprintf(stderr, "dns: nameserver too long\n");
        return 1;
    }
    /* ipc-send: utils -> roottask-core,lkl-netd */
    fd = open("/net/dns", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "dns: /net/dns: %s\n", strerror(errno));
        return 1;
    }
    len = strlen(line);
    if (write(fd, line, len) != (ssize_t) len) {
        fprintf(stderr, "dns: write: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int fetch_http(const char *applet,
                      const char *host,
                      const char *port,
                      const char *path,
                      bool require_content_length)
{
    char ctl[80];
    char req[256];
    char id[8];
    char file[32];
    int ctl_fd;
    int data_fd;
    int rc = 1;
    size_t len;

    if (net_clone_id("tcp", id, sizeof(id)) != 0)
        return 1;
    if (snprintf(ctl, sizeof(ctl), "connect %s %s\n", host, port) >=
        (int) sizeof(ctl)) {
        fprintf(stderr, "%s: address too long\n", applet);
        goto out;
    }
    snprintf(file, sizeof(file), "/net/tcp/%s/ctl", id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    ctl_fd = open(file, O_WRONLY);
    if (ctl_fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", applet, file, strerror(errno));
        goto out;
    }
    len = strlen(ctl);
    if (write(ctl_fd, ctl, len) != (ssize_t) len) {
        fprintf(stderr, "%s: connect: %s\n", applet, strerror(errno));
        close(ctl_fd);
        goto out;
    }
    close(ctl_fd);
    snprintf(file, sizeof(file), "/net/tcp/%s/data", id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    data_fd = open(file, O_RDWR);
    if (data_fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", applet, file, strerror(errno));
        goto out;
    }
    if (snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path,
                 host) >= (int) sizeof(req)) {
        fprintf(stderr, "%s: request too long\n", applet);
        close(data_fd);
        goto out;
    }
    len = strlen(req);
    if (write(data_fd, req, len) != (ssize_t) len) {
        fprintf(stderr, "%s: request: %s\n", applet, strerror(errno));
        close(data_fd);
        goto out;
    }
    rc = copy_http_response_fd(applet, data_fd, require_content_length);
    close(data_fd);
out:
    net_close_id("tcp", id);
    return rc;
}

static int write_all(const char *applet, const char *buf, size_t len)
{
    for (size_t off = 0; off < len;) {
        ssize_t n = write(STDOUT_FILENO, buf + off, len - off);

        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            fprintf(stderr, "%s: stdout: %s\n", applet, strerror(errno));
            return 1;
        }
        off += (size_t) n;
    }
    return 0;
}

static char *http_header_value(char *header, char *end, const char *name)
{
    size_t name_len = strlen(name);

    for (char *line = header; line < end;) {
        char *next = memchr(line, '\n', (size_t) (end - line));
        char *line_end = next ? next : end;

        if (line_end > line && line_end[-1] == '\r')
            line_end--;
        if ((size_t) (line_end - line) >= name_len &&
            strncasecmp(line, name, name_len) == 0)
            return line + name_len;
        line = next ? next + 1 : end;
    }
    return NULL;
}

/* Read an HTTP/1.0 response and copy it (headers included, matching the old
 * dumb copy) to stdout. Honor Content-Length when present; otherwise the body
 * is EOF-delimited, so a clean EOF is success. Headers are bounded; the body
 * streams without a size cap.
 */
static int copy_http_response_fd(const char *applet,
                                 int fd,
                                 bool require_content_length)
{
    char header[8192 + 1];
    char buf[8192];
    size_t hlen = 0;
    size_t body_off = 0;
    long content_len = -1;
    long body_seen;

    while (body_off == 0 && hlen < sizeof(header) - 1) {
        ssize_t n = read(fd, header + hlen, sizeof(header) - 1 - hlen);
        char *end;

        if (n < 0) {
            fprintf(stderr, "%s: response: %s\n", applet, strerror(errno));
            return 1;
        }
        if (n == 0)
            break;
        hlen += (size_t) n;
        header[hlen] = '\0';
        end = strstr(header, "\r\n\r\n");
        if (!end)
            end = strstr(header, "\n\n");
        if (end) {
            body_off = (size_t) (end - header) + (end[0] == '\r' ? 4U : 2U);
            char *cl = http_header_value(header, end, "Content-Length:");

            if (cl && cl < end) {
                char *ep;

                while (*cl == ' ' || *cl == '\t')
                    cl++;
                errno = 0;
                content_len = strtol(cl, &ep, 10);
                if (errno != 0 || ep == cl || content_len < 0)
                    content_len = -1;
            }
        }
    }
    if (body_off == 0) {
        fprintf(stderr, "%s: incomplete HTTP response\n", applet);
        return 1;
    }
    if (require_content_length && content_len < 0) {
        fprintf(stderr, "%s: missing Content-Length\n", applet);
        return 1;
    }

    /* Emit headers, then the body capped to Content-Length when present, so
     * trailing/pipelined bytes past the declared length are not copied out.
     */
    if (write_all(applet, header, body_off))
        return 1;

    size_t have = hlen - body_off;

    if (content_len >= 0 && have > (size_t) content_len)
        have = (size_t) content_len;
    if (have > 0 && write_all(applet, header + body_off, have))
        return 1;
    body_seen = (long) have;

    while (content_len < 0 || body_seen < content_len) {
        size_t want = sizeof(buf);
        ssize_t n;

        if (content_len >= 0 && (size_t) (content_len - body_seen) < want)
            want = (size_t) (content_len - body_seen);
        n = read(fd, buf, want);

        if (n < 0) {
            fprintf(stderr, "%s: response: %s\n", applet, strerror(errno));
            return 1;
        }
        if (n == 0) {
            if (content_len >= 0 && body_seen < content_len) {
                fprintf(stderr, "%s: incomplete HTTP response\n", applet);
                return 1;
            }
            break; /* clean EOF: EOF-delimited body or exact Content-Length */
        }
        if (write_all(applet, buf, (size_t) n))
            return 1;
        body_seen += n;
    }
    return 0;
}

static int parse_http_url(const char *url,
                          char *host,
                          size_t hostsz,
                          char *port,
                          size_t portsz,
                          char *path,
                          size_t pathsz)
{
    const char *p = url;
    const char *slash;
    const char *colon;
    size_t hostlen;
    size_t portlen;

    if (strncmp(p, "http://", 7))
        return -1;
    p += 7;
    slash = strchr(p, '/');
    if (!slash)
        slash = p + strlen(p);
    colon = memchr(p, ':', (size_t) (slash - p));
    hostlen = (size_t) ((colon ? colon : slash) - p);
    if (hostlen == 0 || hostlen >= hostsz)
        return -1;
    memcpy(host, p, hostlen);
    host[hostlen] = '\0';
    if (colon) {
        portlen = (size_t) (slash - colon - 1);
        if (portlen == 0 || portlen >= portsz)
            return -1;
        memcpy(port, colon + 1, portlen);
        port[portlen] = '\0';
    } else {
        snprintf(port, portsz, "80");
    }
    if (*slash == '\0')
        snprintf(path, pathsz, "/");
    else if (strlen(slash) >= pathsz)
        return -1;
    else
        snprintf(path, pathsz, "%s", slash);
    return 0;
}

static int applet_httpget(int argc, char **argv)
{
    bool require_content_length = false;

    if (argc == 2 && !strcmp(argv[1], "--require-content-length")) {
        require_content_length = true;
    } else if (argc != 1) {
        fprintf(stderr, "usage: httpget [--require-content-length]\n");
        return 1;
    }
    return fetch_http("httpget", "10.0.2.2", "8000", "/",
                      require_content_length);
}

static int applet_wget(int argc, char **argv)
{
    char host[32];
    char addr[32];
    char port[8];
    char path[128];
    bool require_content_length = false;

    if (argc == 3 && !strcmp(argv[2], "--require-content-length")) {
        require_content_length = true;
    } else if (argc != 2) {
        fprintf(stderr, "usage: wget URL [--require-content-length]\n");
        return 1;
    }
    if (parse_http_url(argv[1], host, sizeof(host), port, sizeof(port), path,
                       sizeof(path)) != 0) {
        fprintf(stderr, "wget: only http://HOST[:PORT]/PATH is supported\n");
        return 1;
    }
    for (const char *p = host; *p != '\0'; p++) {
        if ((*p < '0' || *p > '9') && *p != '.') {
            if (dns_lookup_a(host, addr, sizeof(addr)) != 0)
                return 1;
            return fetch_http("wget", addr, port, path, require_content_length);
        }
    }
    return fetch_http("wget", host, port, path, require_content_length);
}

static void net_close_id(const char *proto, const char *id)
{
    char path[32];
    int fd;

    snprintf(path, sizeof(path), "/net/%s/%s/ctl", proto, id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    (void) write(fd, "close\n", 6);
    close(fd);
}

static int net_clone_id(const char *proto, char *id, size_t idsz)
{
    char path[32];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/net/%s/clone", proto);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "nc: %s: %s\n", path, strerror(errno));
        return -1;
    }
    n = read(fd, id, idsz - 1);
    close(fd);
    if (n <= 0)
        return -1;
    id[n] = '\0';
    trim_newline(id);
    return id[0] != '\0' && !strchr(id, '/') ? 0 : -1;
}

static int wait_child_seconds(pid_t pid, int *status, unsigned seconds)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
    unsigned ticks = seconds * 10U;

    while (ticks-- > 0) {
        pid_t got = waitpid(pid, status, WNOHANG);

        if (got == pid)
            return 0;
        if (got == -1 && errno != EINTR)
            return -1;
        (void) nanosleep(&ts, NULL);
    }
    (void) kill(pid, SIGKILL);
    for (unsigned i = 0; i < 10U; i++) {
        pid_t got = waitpid(pid, status, WNOHANG);

        if (got == pid)
            break;
        if (got == -1 && errno != EINTR)
            break;
        (void) nanosleep(&ts, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int net_probe_clones(const char *proto)
{
    char a[8] = "";
    char b[8] = "";
    int rc = 0;

    if (net_clone_id(proto, a, sizeof(a)) != 0 ||
        net_clone_id(proto, b, sizeof(b)) != 0 || !strcmp(a, b)) {
        fprintf(stderr, "nc: %s clone probe failed\n", proto);
        rc = 1;
    }
    net_close_id(proto, a);
    net_close_id(proto, b);
    return rc;
}

static int net_probe_owner(const char *proto)
{
    char id[8] = "";
    char *spawn_argv[] = {"nc", "--try-owner", (char *) proto, id, NULL};
    pid_t pid;
    int status;
    int err;

    if (net_clone_id(proto, id, sizeof(id)) != 0)
        return 1;
    err = posix_spawn(&pid, "/bin/nc", NULL, NULL, spawn_argv, NULL);
    if (err != EOK) {
        fprintf(stderr, "nc: spawn owner probe: %s\n", strerror(err));
        net_close_id(proto, id);
        return 1;
    }
    if (wait_child_seconds(pid, &status, 3) != 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "nc: %s owner probe failed\n", proto);
        net_close_id(proto, id);
        return 1;
    }
    net_close_id(proto, id);
    return 0;
}

static int net_try_owner(const char *proto, const char *id)
{
    char path[32];
    int fd;

    snprintf(path, sizeof(path), "/net/%s/%s/ctl", proto, id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_WRONLY);
    if (fd != -1) {
        close(fd);
        return 1;
    }
    return errno == EACCES ? 0 : 1;
}

static int net_leak_clone(const char *proto)
{
    char id[8];

    return net_clone_id(proto, id, sizeof(id)) == 0 ? 0 : 1;
}

static int net_probe_stale_clone(const char *proto)
{
    char *spawn_argv[] = {"nc", "--leak-clone", (char *) proto, NULL};
    char ids[TOOLBOX_NETD_SLOTS][8];
    pid_t pid;
    int status;
    int err;
    int rc = 0;

    err = posix_spawn(&pid, "/bin/nc", NULL, NULL, spawn_argv, NULL);
    if (err != EOK) {
        fprintf(stderr, "nc: spawn stale probe: %s\n", strerror(err));
        return 1;
    }
    if (wait_child_seconds(pid, &status, 3) != 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "nc: %s stale clone child timed out\n", proto);
        return 1;
    }
    for (int i = 0; i < TOOLBOX_NETD_SLOTS; i++)
        ids[i][0] = '\0';
    for (int i = 0; i < TOOLBOX_NETD_SLOTS; i++) {
        if (net_clone_id(proto, ids[i], sizeof(ids[i])) != 0) {
            fprintf(stderr, "nc: %s stale clone probe failed\n", proto);
            rc = 1;
            break;
        }
    }
    for (int i = 0; i < TOOLBOX_NETD_SLOTS; i++)
        net_close_id(proto, ids[i]);
    return rc;
}

static int net_udp_exchange(const char *host,
                            const char *port,
                            const void *tx,
                            size_t txlen,
                            void *rx,
                            size_t rxsz)
{
    char id[8];
    char path[32];
    char ctl[80];
    int fd;
    ssize_t n;

    if (net_clone_id("udp", id, sizeof(id)) != 0)
        return -1;
    snprintf(path, sizeof(path), "/net/udp/%s/ctl", id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_WRONLY);
    if (fd < 0)
        goto fail;
    if (snprintf(ctl, sizeof(ctl), "connect %s %s\n", host, port) >=
        (int) sizeof(ctl)) {
        close(fd);
        goto fail;
    }
    if (write(fd, ctl, strlen(ctl)) != (ssize_t) strlen(ctl)) {
        close(fd);
        goto fail;
    }
    close(fd);
    snprintf(path, sizeof(path), "/net/udp/%s/data", id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_RDWR);
    if (fd < 0)
        goto fail;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (write(fd, tx, txlen) != (ssize_t) txlen) {
            close(fd);
            goto fail;
        }
        n = read(fd, rx, rxsz);
        if (n > 0) {
            close(fd);
            net_close_id("udp", id);
            return (int) n;
        }
    }
    close(fd);
    net_close_id("udp", id);
    return -1;
fail:
    net_close_id("udp", id);
    return -1;
}

static int dns_name(unsigned char *buf, size_t bufsz, const char *name)
{
    const char *p = name;
    unsigned char *out = buf;

    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t) (dot - p) : strlen(p);

        if (len == 0 || len > 63 || (size_t) (out - buf) + len + 2 > bufsz)
            return -1;
        *out++ = (unsigned char) len;
        memcpy(out, p, len);
        out += len;
        if (!dot)
            break;
        p = dot + 1;
    }
    if ((size_t) (out - buf) + 1 > bufsz)
        return -1;
    *out++ = 0;
    return (int) (out - buf);
}

static unsigned dns_u16(const unsigned char *p)
{
    return ((unsigned) p[0] << 8) | p[1];
}

static int read_dns_server(char *host, size_t hostsz, char *port, size_t portsz)
{
    char line[80];
    /* ipc-send: utils -> roottask-core,lkl-netd */
    int fd = open("/net/dns", O_RDONLY);
    ssize_t n;
    char *p;
    char *sp;

    if (fd < 0)
        return -1;
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    line[n] = '\0';
    if (strncmp(line, "nameserver ", 11))
        return -1;
    p = line + 11;
    sp = strchr(p, ' ');
    if (!sp)
        return -1;
    *sp++ = '\0';
    trim_newline(sp);
    snprintf(host, hostsz, "%s", p);
    snprintf(port, portsz, "%s", sp);
    return 0;
}

static int dns_lookup_a(const char *name, char *addr, size_t addrsz)
{
    unsigned char q[256] = {
        [0] = 0x12,
        [1] = 0x34,
        [2] = 0x01,
        [5] = 0x01,
    };
    unsigned char r[512];
    char host[32];
    char port[8];
    int qn;
    int rn;
    size_t off;
    unsigned an;

    if (read_dns_server(host, sizeof(host), port, sizeof(port)) != 0) {
        fprintf(stderr, "nslookup: /net/dns unavailable\n");
        return -1;
    }
    qn = dns_name(q + 12, sizeof(q) - 16, name);
    if (qn < 0) {
        fprintf(stderr, "nslookup: bad name\n");
        return -1;
    }
    off = 12 + (size_t) qn;
    q[off + 1] = 1;
    q[off + 3] = 1;
    rn = net_udp_exchange(host, port, q, off + 4, r, sizeof(r));
    if (rn < 12 || r[0] != 0x12 || r[1] != 0x34) {
        fprintf(stderr, "nslookup: no response\n");
        return -1;
    }
    an = dns_u16(r + 6);
    off += 4;
    for (unsigned i = 0; i < an && off + 12 <= (size_t) rn; i++) {
        unsigned type;
        unsigned len;

        if ((r[off] & 0xc0) == 0xc0)
            off += 2;
        else {
            while (off < (size_t) rn && r[off] != 0)
                off += (size_t) r[off] + 1;
            off++;
        }
        if (off + 10 > (size_t) rn)
            break;
        type = dns_u16(r + off);
        len = dns_u16(r + off + 8);
        off += 10;
        if (off + len > (size_t) rn)
            break;
        if (type == 1 && len == 4) {
            snprintf(addr, addrsz, "%u.%u.%u.%u", r[off], r[off + 1],
                     r[off + 2], r[off + 3]);
            return 0;
        }
        off += len;
    }
    fprintf(stderr, "nslookup: no A record\n");
    return -1;
}

static int applet_nslookup(int argc, char **argv)
{
    char addr[32];

    if (argc == 4 && !strcmp(argv[1], "--expect")) {
        if (dns_lookup_a(argv[2], addr, sizeof(addr)) != 0)
            return 1;
        printf("%s %s\n", argv[2], addr);
        if (strcmp(addr, argv[3]) != 0) {
            fprintf(stderr, "nslookup: expected %s, got %s\n", argv[3], addr);
            return 1;
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: nslookup [--expect] HOST [ADDR]\n");
        return 1;
    }
    if (dns_lookup_a(argv[1], addr, sizeof(addr)) != 0)
        return 1;
    printf("%s %s\n", argv[1], addr);
    return 0;
}

static int applet_host(int argc, char **argv)
{
    char addr[32];

    if (argc == 4 && !strcmp(argv[1], "--expect")) {
        if (dns_lookup_a(argv[2], addr, sizeof(addr)) != 0)
            return 1;
        printf("%s has address %s\n", argv[2], addr);
        if (strcmp(addr, argv[3]) != 0) {
            fprintf(stderr, "host: expected %s, got %s\n", argv[3], addr);
            return 1;
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: host [--expect] NAME [ADDR]\n");
        return 1;
    }
    if (dns_lookup_a(argv[1], addr, sizeof(addr)) != 0)
        return 1;
    printf("%s has address %s\n", argv[1], addr);
    return 0;
}

static int ping_once(const char *host,
                     long timeout,
                     long seq,
                     int expect_fail,
                     int expect_revoke)
{
    char line[48];
    char addr[32];
    const char *target = host;
    char reply[80];
    int fd;
    size_t len;
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    long elapsed_us;
    long bytes;
    long ttl;
    char from[32];
    ssize_t n;
    int rc;

    if (!expect_fail && !expect_revoke) {
        for (const char *p = host; *p != '\0'; p++) {
            if ((*p < '0' || *p > '9') && *p != '.') {
                if (dns_lookup_a(host, addr, sizeof(addr)) != 0)
                    return 1;
                target = addr;
                break;
            }
        }
    }
    if (snprintf(line, sizeof(line), "%s %ld\n", target, timeout) >=
        (int) sizeof(line)) {
        fprintf(stderr, "ping: host too long\n");
        return 1;
    }
    /* ipc-send: utils -> roottask-core,lkl-netd */
    fd = open("/net/ping", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ping: /net/ping: %s\n", strerror(errno));
        if (errno == ENOENT)
            fputs("ping: start /sbin/lkl-netd --serve-net first\n", stderr);
        return 1;
    }
    len = strlen(line);
    (void) __clock_time(CLOCK_MONOTONIC, NULL, &start_ns);
    if (write(fd, line, len) != (ssize_t) len) {
        if (expect_revoke) {
            close(fd);
            return 0;
        }
        if (expect_fail && errno == EIO) {
            close(fd);
            return 0;
        }
        if (errno == EIO)
            fprintf(stderr, "ping: %s: no response\n", host);
        else
            fprintf(stderr, "ping: %s: %s\n", host, strerror(errno));
        close(fd);
        return 1;
    }
    if (expect_fail) {
        fprintf(stderr, "ping: %s unexpectedly succeeded\n", host);
        close(fd);
        return 1;
    }
    if (expect_revoke) {
        fprintf(stderr, "ping: %s did not revoke\n", host);
        close(fd);
        return 1;
    }
    n = read(fd, reply, sizeof(reply) - 1);
    (void) __clock_time(CLOCK_MONOTONIC, NULL, &end_ns);
    if (n <= 0) {
        if (n == 0)
            errno = EIO;
        fprintf(stderr, "ping: %s: %s\n", host, strerror(errno));
        close(fd);
        return 1;
    }
    reply[n] = '\0';
    char *p = reply;
    char *parse_end;

    errno = 0;
    bytes = strtol(p, &parse_end, 10);
    if (errno != 0 || parse_end == p || *parse_end != ' ') {
        fprintf(stderr, "ping: %s: bad reply\n", host);
        close(fd);
        return 1;
    }
    p = parse_end + 1;
    errno = 0;
    ttl = strtol(p, &parse_end, 10);
    if (errno != 0 || parse_end == p || *parse_end != ' ') {
        fprintf(stderr, "ping: %s: bad reply\n", host);
        close(fd);
        return 1;
    }
    p = parse_end + 1;
    trim_newline(p);
    if (strlen(p) >= sizeof(from)) {
        fprintf(stderr, "ping: %s: bad reply\n", host);
        close(fd);
        return 1;
    }
    snprintf(from, sizeof(from), "%s", p);
    elapsed_us = end_ns >= start_ns ? (long) ((end_ns - start_ns) / 1000U) : 0;
    printf("%ld bytes from %s: icmp_seq=%ld ttl=%ld time=%ld.%03ld ms\n", bytes,
           from, seq, ttl, elapsed_us / 1000L, elapsed_us % 1000L);
    rc = 0;
    close(fd);
    return rc;
}

static int applet_ping(int argc, char **argv)
{
    long count = 3;
    long timeout = 3;
    int expect_fail = 0;
    int expect_revoke = 0;
    int arg = 1;
    int rc = 0;

    if (argc == 3 && !strcmp(argv[1], "--expect-fail")) {
        expect_fail = 1;
        arg = 2;
    } else if (argc == 3 && !strcmp(argv[1], "--expect-revoke")) {
        expect_revoke = 1;
        arg = 2;
    } else {
        while (arg < argc && argv[arg][0] == '-') {
            if (!strcmp(argv[arg], "-4")) {
                arg++;
            } else if (!strcmp(argv[arg], "-c")) {
                if (++arg >= argc || parse_positive(argv[arg], &count) != 0 ||
                    count == 0) {
                    fprintf(stderr,
                            "usage: ping [-4] [-c COUNT] [-W SEC] HOST\n");
                    return 1;
                }
                arg++;
            } else if (strncmp(argv[arg], "-c", 2) == 0 &&
                       argv[arg][2] != '\0') {
                if (parse_positive(argv[arg] + 2, &count) != 0 || count == 0) {
                    fprintf(stderr,
                            "usage: ping [-4] [-c COUNT] [-W SEC] HOST\n");
                    return 1;
                }
                arg++;
            } else if (!strcmp(argv[arg], "-W")) {
                if (++arg >= argc || parse_positive(argv[arg], &timeout) != 0 ||
                    timeout == 0 || timeout > 30) {
                    fprintf(stderr,
                            "usage: ping [-4] [-c COUNT] [-W SEC] HOST\n");
                    return 1;
                }
                arg++;
            } else if (strncmp(argv[arg], "-W", 2) == 0 &&
                       argv[arg][2] != '\0') {
                if (parse_positive(argv[arg] + 2, &timeout) != 0 ||
                    timeout == 0 || timeout > 30) {
                    fprintf(stderr,
                            "usage: ping [-4] [-c COUNT] [-W SEC] HOST\n");
                    return 1;
                }
                arg++;
            } else {
                fprintf(stderr, "usage: ping [-4] [-c COUNT] [-W SEC] HOST\n");
                return 1;
            }
        }
    }
    if (argc != arg + 1) {
        fprintf(stderr, "usage: ping [-4] [-c COUNT] [-W SEC] HOST\n");
        return 1;
    }
    if (!expect_fail && !expect_revoke)
        printf("PING %s (%s): 56 data bytes\n", argv[arg], argv[arg]);
    for (long seq = 0; seq < count; seq++) {
        rc = ping_once(argv[arg], timeout, seq, expect_fail, expect_revoke);
        if (rc != 0 || expect_fail || expect_revoke)
            break;
        if (seq + 1 < count)
            sleep(1);
    }
    return rc;
}

static int decode_escape_text(const char *src, char *dst, size_t dstsz)
{
    size_t out = 0;

    while (*src != '\0') {
        char c = *src++;

        if (out == dstsz)
            return -1;
        if (c == '\\') {
            c = *src++;
            switch (c) {
            case '0':
                c = '\0';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '\\':
                c = '\\';
                break;
            case '\0':
                return -1;
            default:
                break;
            }
        }
        dst[out++] = c;
    }
    return (int) out;
}

static int applet_nc(int argc, char **argv)
{
    char ctl[80];
    char id[8];
    char out[512];
    char rx[512];
    char expect[512];
    const char *expect_text = NULL;
    const char *proto = "tcp";
    int arg = 1;
    int udp = 0;
    int ctl_fd;
    int data_fd;
    int len = 0;
    int rc = 1;
    ssize_t n = -1;

    if (argc == 2 && !strcmp(argv[1], "--probe-clones"))
        return net_probe_clones("tcp") || net_probe_clones("udp");
    if (argc == 2 && !strcmp(argv[1], "--probe-owners"))
        return net_probe_owner("tcp") || net_probe_owner("udp");
    if (argc == 2 && !strcmp(argv[1], "--probe-stale-clones"))
        return net_probe_stale_clone("tcp") || net_probe_stale_clone("udp");
    if (argc == 3 && !strcmp(argv[1], "--leak-clone"))
        return net_leak_clone(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--try-owner"))
        return net_try_owner(argv[2], argv[3]);

    if (argc > 1 && !strcmp(argv[1], "-u")) {
        proto = "udp";
        arg = 2;
        udp = 1;
    }
    if (!udp && argc >= arg + 3 && !strcmp(argv[arg + 2], "--http-get")) {
        bool require_content_length = false;

        if (argc == arg + 4 &&
            !strcmp(argv[arg + 3], "--require-content-length"))
            require_content_length = true;
        else if (argc != arg + 3) {
            fprintf(
                stderr,
                "usage: nc HOST PORT --http-get [--require-content-length]\n");
            return 1;
        }
        return fetch_http("nc", argv[arg], argv[arg + 1], "/",
                          require_content_length);
    }
    if (udp && argc == arg + 5 && !strcmp(argv[arg + 3], "--expect")) {
        expect_text = argv[arg + 4];
    } else if (argc != arg + 3) {
        fprintf(stderr,
                "usage: nc [-u] HOST PORT TEXT [--expect TEXT]|--http-get "
                "[--require-content-length]|--probe-owners\n");
        return 1;
    }
    if (net_clone_id(proto, id, sizeof(id)) != 0)
        return 1;

    /* Past the clone every exit goes through done_id: so the netd connection
     * slot (only NETD_SLOTS exist) is always released, not leaked on error.
     */
    if (snprintf(ctl, sizeof(ctl), "connect %s %s\n", argv[arg],
                 argv[arg + 1]) >= (int) sizeof(ctl)) {
        fprintf(stderr, "nc: address too long\n");
        goto done_id;
    }
    snprintf(out, sizeof(out), "/net/%s/%s/ctl", proto, id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    ctl_fd = open(out, O_WRONLY);
    if (ctl_fd < 0) {
        fprintf(stderr, "nc: %s: %s\n", out, strerror(errno));
        goto done_id;
    }
    if (write(ctl_fd, ctl, strlen(ctl)) != (ssize_t) strlen(ctl)) {
        fprintf(stderr, "nc: connect: %s\n", strerror(errno));
        close(ctl_fd);
        goto done_id;
    }
    close(ctl_fd);
    snprintf(out, sizeof(out), "/net/%s/%s/data", proto, id);

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    data_fd = open(out, O_RDWR);
    if (data_fd < 0) {
        fprintf(stderr, "nc: %s: %s\n", out, strerror(errno));
        goto done_id;
    }
    len = decode_escape_text(argv[arg + 2], out, sizeof(out));
    if (len < 0) {
        fprintf(stderr, "nc: text too long or malformed escape\n");
        goto done_fd;
    }
    if ((size_t) len >= sizeof(out)) {
        fprintf(stderr, "nc: text too long\n");
        goto done_fd;
    }
    if (write(data_fd, out, (size_t) len) != len) {
        fprintf(stderr, "nc: write: %s\n", strerror(errno));
        goto done_fd;
    }
    if (udp) {
        n = read(data_fd, rx, sizeof(rx));
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            fprintf(stderr, "nc: read: %s\n", strerror(errno));
            goto done_fd;
        }
        if (write(STDOUT_FILENO, rx, (size_t) n) != n) {
            fprintf(stderr, "nc: stdout: %s\n", strerror(errno));
            goto done_fd;
        }
        if (expect_text) {
            int expect_len =
                decode_escape_text(expect_text, expect, sizeof(expect));

            if (expect_len < 0 || n != expect_len ||
                memcmp(rx, expect, (size_t) expect_len) != 0) {
                fprintf(stderr, "nc: unexpected UDP payload\n");
                goto done_fd;
            }
        }
        rc = 0;
        goto done_fd;
    }
    snprintf(ctl, sizeof(ctl), "/net/%s/%s/data", proto, id);
    n = copy_fd(data_fd, STDOUT_FILENO, ctl);
    rc = n != 0;
done_fd:
    close(data_fd);
done_id:
    net_close_id(proto, id);
    return rc;
}

static int applet_ifconfig(int argc, char **argv)
{
    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: ifconfig\n");
        return 1;
    }
    return print_file("ifconfig", "/net/ipifc");
}

static int applet_ip(int argc, char **argv)
{
    if (argc == 1 || !strcmp(argv[1], "addr") || !strcmp(argv[1], "link")) {
        if (argc > 2) {
            fprintf(stderr, "usage: ip [addr|route|link]\n");
            return 1;
        }
        return print_file("ip", "/net/ipifc");
    }
    if (!strcmp(argv[1], "route")) {
        if (argc > 2) {
            fprintf(stderr, "usage: ip [addr|route|link]\n");
            return 1;
        }
        return print_file("ip", "/net/route");
    }
    fprintf(stderr, "usage: ip [addr|route|link]\n");
    return 1;
}

static int applet_route(int argc, char **argv)
{
    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: route\n");
        return 1;
    }
    return print_file("route", "/net/route");
}

static int netstat_routes(bool numeric)
{
    char buf[128];
    char gwstr[16];
    char netstr[16];
    unsigned gw[4];
    const char *p;
    int fd;
    int n;

    /* ipc-send: utils -> roottask-core,lkl-netd */
    fd = open("/net/route", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "netstat: /net/route: %s\n", strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr,
                    "netstat: start /sbin/lkl-netd --serve-net first\n");
        return 1;
    }
    n = (int) read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
        n = 0;
    buf[n] = '\0';

    puts("Kernel IP routing table");
    puts(
        "Destination     Gateway         Genmask         "
        "Flags   MSS Window  irtt Iface");

    /* lkl-netd publishes a single "default A.B.C.D" line. Without it there is
     * nothing to show, matching BusyBox's empty routing table. Parse the four
     * octets by hand — this libc has no scanf family.
     */
    if (strncmp(buf, "default ", 8))
        return 0;
    p = buf + 8;
    for (int i = 0; i < 4; i++) {
        char *end;
        unsigned long v = strtoul(p, &end, 10);

        if (end == p || v > 255)
            return 0;
        gw[i] = (unsigned) v;
        p = end;
        if (i < 3) {
            if (*p != '.')
                return 0;
            p++;
        }
    }

    snprintf(gwstr, sizeof(gwstr), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);

    /* lkl-netd hands out a /24 slirp subnet, so mask the gateway to derive the
     * on-link network. Read /net/ipifc if the prefix ever varies.
     */
    snprintf(netstr, sizeof(netstr), "%u.%u.%u.0", gw[0], gw[1], gw[2]);

    printf("%-16s%-16s%-16s%-6s%5d %-6d%6d %s\n",
           numeric ? "0.0.0.0" : "default", gwstr, "0.0.0.0", "UG", 0, 0, 0,
           "eth0");
    printf("%-16s%-16s%-16s%-6s%5d %-6d%6d %s\n", netstr,
           numeric ? "0.0.0.0" : "*", "255.255.255.0", "U", 0, 0, 0, "eth0");
    return 0;
}

static int applet_netstat(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-r")))
        return netstat_routes(false);
    if (argc == 2 && (!strcmp(argv[1], "-rn") || !strcmp(argv[1], "-n")))
        return netstat_routes(true);
    if (argc == 2 && !strcmp(argv[1], "-l")) {
        puts("Proto Local Address");
        puts("tcp   /net/tcp/clone");
        puts("udp   /net/udp/clone");
        return 0;
    }
    fprintf(stderr, "usage: netstat [-rn|-l]\n");
    return 1;
}


static int applet_clear(int argc, char **argv)
{
    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: clear\n");
        return 1;
    }
    fputs("\033[H\033[J", stdout);
    return 0;
}

static int applet_cp(int argc, char **argv)
{
    struct stat src_st;
    struct stat dst_st;
    int in;
    int out;
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: cp SRC DST\n");
        return 1;
    }

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    in = open(argv[1], O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (fstat(in, &src_st) < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        close(in);
        return 1;
    }
    if (stat(argv[2], &dst_st) == 0) {
        if (src_st.st_dev == dst_st.st_dev && src_st.st_ino == dst_st.st_ino) {
            fprintf(stderr, "cp: '%s' and '%s' are the same file\n", argv[1],
                    argv[2]);
            close(in);
            return 1;
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "cp: %s: %s\n", argv[2], strerror(errno));
        close(in);
        return 1;
    }

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[2], strerror(errno));
        close(in);
        return 1;
    }
    rc = copy_fd(in, out, argv[1]) != 0;
    close(out);
    close(in);
    return rc;
}

static int applet_less(int argc, char **argv)
{
    return stream_files("less", argc, argv);
}

static int applet_more(int argc, char **argv)
{
    return stream_files("more", argc, argv);
}

static int applet_mv(int argc, char **argv)
{
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: mv SRC DST\n");
        return 1;
    }
    rc = applet_cp(argc, argv);
    if (rc != 0)
        return rc;
    if (unlink(argv[1]) != 0) {
        fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

static int applet_date(int argc, char **argv)
{
    time_t now;
    char buf[64];

    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: date\n");
        return 1;
    }
    now = time(NULL);
    if (strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S UTC %Y", gmtime(&now)) ==
        0)
        return 1;
    puts(buf);
    return 0;
}

static unsigned long long df_kbytes(unsigned long long blocks,
                                    unsigned long frsize)
{
    return (blocks * (unsigned long long) frsize) / 1024ULL;
}

static int applet_df(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: df PATH ...\n");
        return 1;
    }
    puts("Filesystem 1K-blocks Used Available Mounted-on");
    for (int i = 1; i < argc; i++) {
        struct statvfs stv;
        unsigned long frsize;
        unsigned long long blocks;
        unsigned long long free_blocks;

        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0) {
            fprintf(stderr, "df: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        int st = devctl(fd, DEVCTL_STATVFS, &stv, sizeof(stv), NULL);

        if (st != EOK) {
            /* devctl returns the error code directly; errno is untouched. */
            fprintf(stderr, "df: %s: %s\n", argv[i], strerror(st));
            close(fd);
            rc = 1;
            continue;
        }
        close(fd);
        frsize = stv.f_frsize != 0 ? stv.f_frsize : stv.f_bsize;
        blocks = (unsigned long long) stv.f_blocks;
        free_blocks = (unsigned long long) stv.f_bfree;
        /* A buggy server can report f_bfree > f_blocks; don't underflow. */
        printf(
            "%-10s %9llu %4llu %9llu %s\n",
            stv.f_basetype[0] != '\0' ? stv.f_basetype : "fs",
            df_kbytes(blocks, frsize),
            df_kbytes(blocks > free_blocks ? blocks - free_blocks : 0, frsize),
            df_kbytes((unsigned long long) stv.f_bavail, frsize), argv[i]);
    }
    return rc;
}

static int applet_dirname(int argc, char **argv)
{
    char *slash;

    if (argc != 2) {
        fprintf(stderr, "usage: dirname PATH\n");
        return 1;
    }
    slash = strrchr(argv[1], '/');
    if (!slash) {
        puts(".");
        return 0;
    }
    while (slash > argv[1] && slash[-1] == '/')
        slash--;
    if (slash == argv[1]) {
        puts("/");
        return 0;
    }
    *slash = '\0';
    puts(argv[1]);
    return 0;
}

static int applet_echo(int argc, char **argv)
{
    int newline = 1;
    int first = 1;

    if (argc > 1 && !strcmp(argv[1], "-n")) {
        newline = 0;
        first = 2;
    }
    for (int i = first; i < argc; i++)
        printf("%s%s", i == first ? "" : " ", argv[i]);
    if (newline)
        putchar('\n');
    return 0;
}

static int applet_env(int argc, char **argv)
{
    int arg = 1;

    while (arg < argc && strchr(argv[arg], '=')) {
        char *tmp = strdup(argv[arg]);
        char *eq;

        if (!tmp)
            return 1;
        eq = strchr(tmp, '=');
        *eq++ = '\0';
        if (setenv(tmp, eq, 1) != 0) {
            fprintf(stderr, "env: %s: %s\n", tmp, strerror(errno));
            free(tmp);
            return 1;
        }
        free(tmp);
        arg++;
    }
    if (arg < argc) {
        pid_t pid;
        int status;
        int err;

        err = posix_spawnp(&pid, argv[arg], NULL, NULL, &argv[arg], NULL);
        if (err != EOK) {
            fprintf(stderr, "env: %s: %s\n", argv[arg], strerror(err));
            return 127;
        }
        if (waitpid(pid, &status, 0) < 0)
            return 1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    for (char **ep = toolbox_environ(); ep && *ep; ep++)
        puts(*ep);
    return 0;
}

static int applet_not(int argc, char **argv)
{
    pid_t pid;
    int status;
    int err;

    if (argc < 2) {
        fprintf(stderr, "usage: not COMMAND [ARG ...]\n");
        return 1;
    }
    err = posix_spawnp(&pid, argv[1], NULL, NULL, &argv[1], NULL);
    if (err != EOK) {
        fprintf(stderr, "not: %s: %s\n", argv[1], strerror(err));
        return 1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "not: waitpid: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;
    return 0;
}

static int applet_false(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    return 1;
}

static int applet_help(int argc, char **argv)
{
    const struct toolbox_applet *ap;

    if (argc > 2) {
        fprintf(stderr, "usage: help [APPLET]\n");
        return 1;
    }
    if (argc == 2) {
        ap = find_applet(argv[1]);
        if (!ap) {
            fprintf(stderr, "help: unknown applet '%s'\n", argv[1]);
            return 1;
        }
        printf("usage: %s\n", ap->usage);
        return 0;
    }
    usage_all(stdout);
    return 0;
}

static int applet_head(int argc, char **argv)
{
    long lines = 10;
    int arg = 1;
    int rc = 0;

    if (argc > 2 && !strcmp(argv[1], "-n")) {
        if (parse_positive(argv[2], &lines) != 0) {
            fprintf(stderr, "head: invalid line count '%s'\n", argv[2]);
            return 1;
        }
        arg = 3;
    }
    if (arg == argc)
        argv[--arg] = "-";

    for (int i = arg; i < argc; i++) {
        FILE *fp;
        char buf[TOOLBOX_LINE_MAX];
        long n = 0;

        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        fp = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        while (n < lines && fgets(buf, sizeof(buf), fp)) {
            fputs(buf, stdout);
            n++;
        }
        if (fp != stdin)
            fclose(fp);
    }
    return rc;
}

static int applet_ls(int argc, char **argv)
{
    int all_mode = 0;
    int long_mode = 0;
    int arg = 1;
    int rc = 0;

    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1] != '\0'; arg++) {
        const char *p = argv[arg] + 1;
        while (*p != '\0') {
            if (*p == 'a')
                all_mode = 1;
            else if (*p == 'l')
                long_mode = 1;
            else {
                fprintf(stderr, "usage: ls [-la] [PATH ...]\n");
                return 1;
            }
            p++;
        }
    }
    if (arg == argc)
        return ls_one(".", all_mode, long_mode);
    for (int i = arg; i < argc; i++)
        rc |= ls_one(argv[i], all_mode, long_mode);
    return rc;
}

static int applet_stat(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: stat FILE ...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        char mode[11];

        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        mode_string(st.st_mode, mode);
        printf("%s: size=%lld mode=%s inode=%llu links=%llu\n", argv[i],
               (long long) st.st_size, mode, (unsigned long long) st.st_ino,
               (unsigned long long) st.st_nlink);
    }
    return rc;
}

static int applet_test(int argc, char **argv)
{
    if (argc == 2)
        return argv[1][0] == '\0';
    if (argc == 3 && !strcmp(argv[1], "-n"))
        return argv[2][0] == '\0';
    if (argc == 4 && !strcmp(argv[2], "="))
        return strcmp(argv[1], argv[3]) != 0;
    if (argc == 4 && !strcmp(argv[2], "!="))
        return !strcmp(argv[1], argv[3]);
    return 1;
}

static int applet_sync(int argc, char **argv)
{
    int rc = 0;

    if (argc == 1) {
        sync();
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0) {
            fprintf(stderr, "sync: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (fsync(fd) != 0 || fdatasync(fd) != 0) {
            fprintf(stderr, "sync: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
        close(fd);
    }
    return rc;
}

static int applet_id(int argc, char **argv)
{
    struct passwd *pw = NULL;
    uid_t uid;
    gid_t gid;

    if (argc > 2) {
        fprintf(stderr, "usage: id [USER]\n");
        return 1;
    }
    if (argc == 2) {
        pw = getpwnam(argv[1]);
        if (!pw) {
            fprintf(stderr, "id: %s: no such user\n", argv[1]);
            return 1;
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
    } else {
        uid = getuid();
        gid = getgid();
        pw = getpwuid(uid);
    }
    printf("uid=%u", (unsigned) uid);
    if (pw)
        printf("(%s)", pw->pw_name);
    printf(" gid=%u\n", (unsigned) gid);
    return 0;
}

static int applet_ps(int argc, char **argv)
{
    struct proc_row *rows = NULL;
    size_t count = 0;

    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: ps\n");
        return 1;
    }
    if (collect_procs(&rows, &count) != 0) {
        fprintf(stderr, "ps: %s\n", strerror(errno));
        return 1;
    }
    print_ps_rows(rows, count);
    free(rows);
    return 0;
}

static int applet_printenv(int argc, char **argv)
{
    int rc = 0;

    if (argc == 1) {
        for (char **ep = toolbox_environ(); ep && *ep; ep++)
            puts(*ep);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);

        if (v) {
            puts(v);
        } else {
            rc = 1;
        }
    }
    return rc;
}

static int applet_pwd(int argc, char **argv)
{
    char *cwd;

    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: pwd\n");
        return 1;
    }
    cwd = getcwd(NULL, 0);
    if (!cwd) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(cwd);
    free(cwd);
    return 0;
}

static int applet_rm(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: rm FILE ...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) != 0) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int applet_seq(int argc, char **argv)
{
    long first = 1;
    long inc = 1;
    long last;
    char *end;

    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: seq [FIRST [INCREMENT]] LAST\n");
        return 1;
    }
    errno = 0; /* catch out-of-range operands from any strtol below */
    if (argc == 2) {
        last = strtol(argv[1], &end, 10);
    } else if (argc == 3) {
        first = strtol(argv[1], &end, 10);
        if (*end != '\0')
            goto bad;
        last = strtol(argv[2], &end, 10);
    } else {
        first = strtol(argv[1], &end, 10);
        if (*end != '\0')
            goto bad;
        inc = strtol(argv[2], &end, 10);
        if (*end != '\0' || inc == 0)
            goto bad;
        last = strtol(argv[3], &end, 10);
    }
    if (*end != '\0' || errno != 0)
        goto bad;
    for (long v = first;;) {
        if (inc > 0 ? v > last : v < last)
            break;
        printf("%ld\n", v);
        long next;
        if (__builtin_add_overflow(v, inc, &next))
            break; /* stop before signed overflow wraps the counter */
        v = next;
    }
    return 0;

bad:
    fprintf(stderr, "seq: invalid number\n");
    return 1;
}

static int applet_shutdown(int argc, char **argv)
{
    sys_cmd_t msg;
    int reboot = 0;
    int nosync = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h"))
            reboot = 0;
        else if (!strcmp(argv[i], "-r"))
            reboot = 1;
        else if (!strcmp(argv[i], "-n"))
            nosync = 1;
        else {
            fprintf(stderr, "usage: shutdown [-h | -r] [-n]\n");
            return 1;
        }
    }

    memset(&msg, 0, sizeof(msg));
    msg.i.type = _SYS_CMD;
    msg.i.cmd = reboot ? _SYS_CMD_REBOOT : _SYS_CMD_SHUTDOWN;
    if (nosync)
        msg.i.flags = _SYS_CMD_FLAG_NOSYNC;
    printf("%s...\n", reboot ? "Rebooting" : "Shutting down");
    /* ipc-send: utils -> roottask-core */
    if (msg_sendnc(ROOTTASK_COID, &msg.i, sizeof(msg.i), NULL, 0) == -1) {
        fprintf(stderr, "shutdown: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int applet_sleep(int argc, char **argv)
{
    char *end;
    unsigned long seconds;

    if (argc != 2) {
        fprintf(stderr, "usage: sleep SECONDS\n");
        return 1;
    }
    errno = 0;
    seconds = strtoul(argv[1], &end, 10);
    if (errno != 0 || *end != '\0') {
        fprintf(stderr, "sleep: invalid seconds '%s'\n", argv[1]);
        return 1;
    }
    while (seconds != 0)
        seconds = sleep((unsigned) seconds);
    return 0;
}

/* Print a control character the way stty does: ^C, or <undef> when disabled. */
static void stty_print_cc(const char *name, cc_t c)
{
    if (c == _POSIX_VDISABLE)
        printf("%s = <undef>; ", name);
    else if (c < ' ')
        printf("%s = ^%c; ", name, c + '@');
    else if (c == 0x7f)
        printf("%s = ^?; ", name);
    else
        printf("%s = %c; ", name, c);
}

/* stty — show or change the terminal on standard input. Only the settings that
 * decide job control are changeable: isig (the INTR/QUIT/SUSP characters raise
 * signals) and tostop (a background write stops with SIGTTOU).
 */
static int applet_stty(int argc, char **argv)
{
    struct termios t;
    int changed = 0, i;

    if (tcgetattr(STDIN_FILENO, &t) != 0) {
        fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
        return 1;
    }
    for (i = 1; i < argc; i++) {
        tcflag_t bit;
        int on = argv[i][0] != '-';
        const char *name = on ? argv[i] : argv[i] + 1;

        if (!strcmp(argv[i], "-a"))
            continue; /* the settings are shown whenever none is changed */
        if (!strcmp(name, "isig")) {
            bit = ISIG;
        } else if (!strcmp(name, "tostop")) {
            bit = TOSTOP;
        } else {
            fprintf(stderr, "stty: invalid argument '%s'\n", argv[i]);
            return 1;
        }
        changed = 1;
        if (on)
            t.c_lflag |= bit;
        else
            t.c_lflag &= ~bit;
    }
    if (!changed) {
        stty_print_cc("intr", t.c_cc[VINTR]);
        stty_print_cc("quit", t.c_cc[VQUIT]);
        stty_print_cc("susp", t.c_cc[VSUSP]);
        stty_print_cc("eof", t.c_cc[VEOF]);
        printf("\n%cisig %ctostop %cicanon %cecho\n",
               t.c_lflag & ISIG ? ' ' : '-', t.c_lflag & TOSTOP ? ' ' : '-',
               t.c_lflag & ICANON ? ' ' : '-', t.c_lflag & ECHO ? ' ' : '-');
    }
    if (changed && tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) {
        fprintf(stderr, "stty: standard input: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int applet_setconsole(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: setconsole DEVICE\n");
        return 1;
    }
    if (roottask_console_set(argv[1]) == -1) {
        fprintf(stderr, "setconsole: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

static int applet_crash(int argc, char **argv)
{
    volatile int *p = NULL;

    if (argc != 1) {
        fprintf(stderr, "usage: crash\n");
        return 1;
    }
    fprintf(stderr, "crash: dereferencing NULL...\n");
    fflush(stderr);
    *p = 0;
    return 1;
}

/* inject — drive the debug fault-injection private call. Always compiled (the
 * toolbox object must not change with the kernel config) but installed as
 * /bin/inject, with its own boot-manifest grant, only when the kernel is built
 * with CONFIG_DEBUG_INJECT; against any other kernel the call is unbound and
 * this prints ENOSYS.
 */
#include <sys/rtmux-core.h>
#include "../../kernel/include/private/priv-calls.h"
#include "../../kernel/include/private/inject.h"
#include "../../kernel/include/private/kstats.h"

static int inject_call(unsigned op, unsigned long a0, unsigned long a1)
{
    struct kcall_args_debug_inject args = {.op = op, .arg0 = a0, .arg1 = a1};
    long rc = syscall_priv(__KSYS_PRIV_DEBUG_INJECT, &args);

    if (rc != 0) {
        fprintf(stderr, "inject: call refused: %s\n",
                strerror((int) (rc < 0 ? -rc : rc)));
        return -1;
    }
    return args.ret;
}

static int applet_inject(int argc, char **argv)
{
    static const char *const pools[] = {
#define KSTAT_SOUL_NAME(name, var) [KSTAT_SOUL_##name] = #name,
        KSTAT_SOUL_LIST(KSTAT_SOUL_NAME)
#undef KSTAT_SOUL_NAME
    };
    int rc;

    if (argc == 4 && !strcmp(argv[1], "spin-irqoff")) {
        rc = inject_call(INJECT_OP_SPIN_IRQOFF, strtoul(argv[2], NULL, 0),
                         strtoul(argv[3], NULL, 0));
    } else if (argc == 4 && !strcmp(argv[1], "spin-console")) {
        rc = inject_call(INJECT_OP_SPIN_CONSOLE, strtoul(argv[2], NULL, 0),
                         strtoul(argv[3], NULL, 0));
    } else if (argc == 3 && !strcmp(argv[1], "hang-hart")) {
        rc = inject_call(INJECT_OP_HANG_HART, strtoul(argv[2], NULL, 0), 0);
    } else if (argc == 4 && !strcmp(argv[1], "fail-alloc")) {
        unsigned id;

        for (id = 0; id < KSTAT_SOUL_COUNT; id++)
            if (!strcmp(argv[2], pools[id]))
                break;
        if (id == KSTAT_SOUL_COUNT) {
            fprintf(stderr, "inject: unknown pool %s\n", argv[2]);
            return 2;
        }
        rc = inject_call(INJECT_OP_FAIL_NEXT_ALLOC, id,
                         strtoul(argv[3], NULL, 0));
    } else if (argc == 2 && !strcmp(argv[1], "alloc-status")) {
        rc = inject_call(INJECT_OP_ALLOC_STATUS, 0, 0);
        if (rc >= 0)
            printf("alloc-consumed=%d\n", rc);
        return rc < 0 ? 1 : 0;
    } else {
        fprintf(stderr,
                "usage: inject spin-irqoff HART MS | spin-console HART MS | "
                "hang-hart HART | fail-alloc POOL PID | alloc-status\n");
        return 2;
    }
    return rc == 0 ? 0 : 1;
}

static int applet_tail(int argc, char **argv)
{
    long lines = 10;
    int arg = 1;
    int rc = 0;

    if (argc > 2 && !strcmp(argv[1], "-n")) {
        if (parse_positive(argv[2], &lines) != 0) {
            fprintf(stderr, "tail: invalid line count '%s'\n", argv[2]);
            return 1;
        }
        arg = 3;
    }
    if (arg == argc)
        argv[--arg] = "-";

    for (int i = arg; i < argc; i++) {
        FILE *fp;
        char **ring;
        long pos = 0;
        long seen = 0;
        char buf[TOOLBOX_LINE_MAX];

        ring = calloc((size_t) (lines == 0 ? 1 : lines), sizeof(*ring));
        if (!ring) {
            fprintf(stderr, "tail: no memory\n");
            return 1;
        }

        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        fp = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(errno));
            free(ring);
            rc = 1;
            continue;
        }
        while (lines > 0 && fgets(buf, sizeof(buf), fp)) {
            free(ring[pos]);
            ring[pos] = strdup(buf);
            if (!ring[pos]) {
                fprintf(stderr, "tail: no memory\n");
                rc = 1;
                break;
            }
            pos = (pos + 1) % lines;
            seen++;
        }
        if (lines > 0) {
            long out = seen < lines ? seen : lines;
            long start = seen < lines ? 0 : pos;
            for (long j = 0; j < out; j++) {
                char *s = ring[(start + j) % lines];
                if (s)
                    fputs(s, stdout);
            }
        }
        for (long j = 0; j < (lines == 0 ? 1 : lines); j++)
            free(ring[j]);
        free(ring);
        if (fp != stdin)
            fclose(fp);
    }
    return rc;
}

static int applet_tee(int argc, char **argv)
{
    FILE **fps;
    int append = 0;
    int arg = 1;
    int files;
    int rc = 0;
    char buf[COPY_BUFSZ];
    size_t n;

    if (argc > 1 && !strcmp(argv[1], "-a")) {
        append = 1;
        arg = 2;
    }
    files = argc - arg;
    fps = calloc((size_t) (files > 0 ? files : 1), sizeof(*fps));
    if (!fps)
        return 1;
    for (int i = 0; i < files; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        fps[i] = fopen(argv[arg + i], append ? "a" : "w");
        if (!fps[i]) {
            fprintf(stderr, "tee: %s: %s\n", argv[arg + i], strerror(errno));
            rc = 1;
        }
    }
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        if (fwrite(buf, 1, n, stdout) != n)
            rc = 1;
        for (int i = 0; i < files; i++) {
            if (fps[i] && fwrite(buf, 1, n, fps[i]) != n)
                rc = 1;
        }
    }
    if (ferror(stdin))
        rc = 1;
    for (int i = 0; i < files; i++) {
        if (fps[i] && fclose(fps[i]) != 0)
            rc = 1;
    }
    free(fps);
    return rc;
}

struct top_input_state {
    int active;
    int fd_flags;
    struct termios termios;
};

static void top_input_enter(struct top_input_state *state)
{
    struct termios raw;

    memset(state, 0, sizeof(*state));
    if (!isatty(STDIN_FILENO))
        return;
    state->fd_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (state->fd_flags < 0)
        return;
    if (tcgetattr(STDIN_FILENO, &state->termios) != 0)
        return;
    raw = state->termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return;
    if (fcntl(STDIN_FILENO, F_SETFL, state->fd_flags | O_NONBLOCK) != 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &state->termios);
        return;
    }
    state->active = 1;
}

static void top_input_leave(struct top_input_state *state)
{
    if (!state->active)
        return;
    tcsetattr(STDIN_FILENO, TCSANOW, &state->termios);
    fcntl(STDIN_FILENO, F_SETFL, state->fd_flags);
    state->active = 0;
}

static int top_wait_or_quit(long delay_sec)
{
    long ticks = delay_sec > 0 ? delay_sec * 10 : 1;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};

    for (long i = 0; i < ticks; i++) {
        char c;

        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 'Q')
                return 1;
        }
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int applet_top(int argc, char **argv)
{
    long count = -1;
    long delay_sec = 1;
    struct top_input_state input;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            if (parse_positive(argv[++i], &count) != 0 || count == 0) {
                fprintf(stderr, "top: invalid count\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            if (parse_positive(argv[++i], &delay_sec) != 0) {
                fprintf(stderr, "top: invalid delay\n");
                return 1;
            }
        } else {
            fprintf(stderr, "usage: top [-n COUNT] [-d SECONDS]\n");
            return 1;
        }
    }

    top_input_enter(&input);
    for (long iter = 0; count < 0 || iter < count; iter++) {
        struct proc_row *rows = NULL;
        size_t n = 0;

        if (collect_procs(&rows, &n) != 0) {
            top_input_leave(&input);
            fprintf(stderr, "top: %s\n", strerror(errno));
            return 1;
        }
        qsort(rows, n, sizeof(*rows), cmp_proc_cpu);
        printf("\033[H\033[J");
        printf("top: processes=%u delay=%lds", (unsigned) n, delay_sec);
        if (count > 0)
            printf(" iteration=%ld/%ld", iter + 1, count);
        putchar('\n');
        print_ps_rows(rows, n);
        fflush(stdout);
        free(rows);
        if ((count < 0 || iter + 1 < count) && top_wait_or_quit(delay_sec))
            break;
    }
    top_input_leave(&input);
    return 0;
}

static int applet_touch(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: touch FILE ...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}

static int applet_true(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    return 0;
}

static int wc_one(FILE *fp, const char *name, int print_name)
{
    unsigned long lines = 0;
    unsigned long words = 0;
    unsigned long bytes = 0;
    int in_word = 0;
    int c;

    while ((c = fgetc(fp)) != EOF) {
        bytes++;
        if (c == '\n')
            lines++;
        if (isspace((unsigned char) c)) {
            in_word = 0;
        } else if (!in_word) {
            words++;
            in_word = 1;
        }
    }
    printf("%8lu %8lu %8lu", lines, words, bytes);
    if (print_name)
        printf(" %s", name);
    putchar('\n');
    return 0;
}

static int applet_wc(int argc, char **argv)
{
    int rc = 0;

    if (argc == 1)
        return wc_one(stdin, "-", 0);
    for (int i = 1; i < argc; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        wc_one(fp, argv[i], 1);
        fclose(fp);
    }
    return rc;
}

static int which_one(const char *name)
{
    char *path;
    char *save;
    char *dir;

    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            puts(name);
            return 0;
        }
        return 1;
    }
    {
        const char *env_path = getenv("PATH");

        path = strdup(env_path ? env_path : "/bin:/sbin");
    }
    if (!path)
        return 1;
    for (dir = strtok_r(path, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];

        snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", name);
        if (access(full, X_OK) == 0) {
            puts(full);
            free(path);
            return 0;
        }
    }
    free(path);
    return 1;
}

static int applet_which(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: which COMMAND ...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (which_one(argv[i]) != 0)
            rc = 1;
    }
    return rc;
}

static int applet_whoami(int argc, char **argv)
{
    struct passwd *pw;

    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: whoami\n");
        return 1;
    }
    pw = getpwuid(geteuid());
    if (!pw) {
        fprintf(stderr, "whoami: unknown uid %u\n", (unsigned) geteuid());
        return 1;
    }
    puts(pw->pw_name);
    return 0;
}

static int applet_yes(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "y";

    while (1) {
        for (int i = 1; i < argc; i++)
            printf("%s%s", i == 1 ? "" : " ", argv[i]);
        if (argc == 1)
            fputs(s, stdout);
        putchar('\n');
    }
    return 0;
}

/* Fixed-string match with ^/$ anchors, not full BRE. Covers the common shell
 * cases; wire in a regex engine if scripts need char classes.
 */
static int grep_cmp(const char *a, const char *b, size_t n, bool ci)
{
    return ci ? strncasecmp(a, b, n) : strncmp(a, b, n);
}

static bool grep_line(const char *line,
                      const char *pat,
                      size_t patlen,
                      bool ci,
                      bool as,
                      bool ae)
{
    size_t linelen = strlen(line);

    if (as && ae)
        return linelen == patlen && grep_cmp(line, pat, patlen, ci) == 0;
    if (as)
        return linelen >= patlen && grep_cmp(line, pat, patlen, ci) == 0;
    if (ae)
        return linelen >= patlen &&
               grep_cmp(line + (linelen - patlen), pat, patlen, ci) == 0;
    if (patlen == 0)
        return true;
    for (size_t i = 0; i + patlen <= linelen; i++) {
        if (grep_cmp(line + i, pat, patlen, ci) == 0)
            return true;
    }
    return false;
}

static int applet_grep(int argc, char **argv)
{
    bool ci = false, inv = false, count = false, listf = false, quiet = false,
         num = false;
    bool as = false, ae = false, matched = false;
    int arg = 1, err = 0, nfiles;
    const char *pat;
    size_t patlen;

    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        for (const char *p = argv[arg] + 1; *p; p++) {
            switch (*p) {
            case 'i':
                ci = true;
                break;
            case 'v':
                inv = true;
                break;
            case 'c':
                count = true;
                break;
            case 'l':
                listf = true;
                break;
            case 'q':
                quiet = true;
                break;
            case 'n':
                num = true;
                break;
            default:
                fprintf(stderr, "usage: grep [-cilnqv] PATTERN [FILE ...]\n");
                return 2;
            }
        }
    }
    if (arg >= argc) {
        fprintf(stderr, "usage: grep [-cilnqv] PATTERN [FILE ...]\n");
        return 2;
    }
    pat = argv[arg++];
    if (pat[0] == '^') {
        as = true;
        pat++;
    }
    patlen = strlen(pat);
    if (patlen > 0 && pat[patlen - 1] == '$') {
        ae = true;
        patlen--;
    }
    if (arg == argc)
        argv[--arg] = "-";
    nfiles = argc - arg;

    for (int i = arg; i < argc; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        FILE *fp = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        char buf[TOOLBOX_LINE_MAX];
        long nmatch = 0, lineno = 0;

        if (!fp) {
            fprintf(stderr, "grep: %s: %s\n", argv[i], strerror(errno));
            err = 1;
            continue;
        }
        while (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);
            bool hit;

            lineno++;
            if (len && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            hit = grep_line(buf, pat, patlen, ci, as, ae);
            if (hit == inv)
                continue;
            matched = true;
            nmatch++;
            if (quiet) {
                if (fp != stdin)
                    fclose(fp);
                return 0;
            }
            if (listf)
                break;
            if (!count) {
                if (nfiles > 1)
                    printf("%s:", argv[i]);
                if (num)
                    printf("%ld:", lineno);
                puts(buf);
            }
        }
        if (fp != stdin)
            fclose(fp);
        if (listf) {
            if (nmatch)
                puts(argv[i]);
        } else if (count) {
            if (nfiles > 1)
                printf("%s:", argv[i]);
            printf("%ld\n", nmatch);
        }
    }
    return err ? 2 : (matched ? 0 : 1);
}

/* Supports ranges (a-z) and \n \t \r \\ escapes; no [:class:] or equiv sets. */
static size_t tr_set(const char *s, unsigned char *out, size_t outsz)
{
    unsigned char raw[256];
    size_t rn = 0, n = 0;

    for (const char *p = s; *p && rn < sizeof(raw);) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':
                raw[rn++] = '\n';
                break;
            case 't':
                raw[rn++] = '\t';
                break;
            case 'r':
                raw[rn++] = '\r';
                break;
            default:
                raw[rn++] = (unsigned char) *p;
                break;
            }
            p++;
        } else {
            raw[rn++] = (unsigned char) *p++;
        }
    }
    for (size_t i = 0; i < rn && n < outsz;) {
        if (i + 2 < rn && raw[i + 1] == '-' && raw[i + 2] >= raw[i]) {
            for (int c = raw[i]; c <= (int) raw[i + 2] && n < outsz; c++)
                out[n++] = (unsigned char) c;
            i += 3;
        } else {
            out[n++] = raw[i++];
        }
    }
    return n;
}

static int applet_tr(int argc, char **argv)
{
    bool del = false, squeeze = false;
    int arg = 1, c, last = -1;
    unsigned char s1[256], s2[256];
    size_t n1, n2;

    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        for (const char *p = argv[arg] + 1; *p; p++) {
            if (*p == 'd')
                del = true;
            else if (*p == 's')
                squeeze = true;
            else {
                fprintf(stderr, "usage: tr [-ds] SET1 [SET2]\n");
                return 1;
            }
        }
    }
    if (arg >= argc) {
        fprintf(stderr, "usage: tr [-ds] SET1 [SET2]\n");
        return 1;
    }
    n1 = tr_set(argv[arg++], s1, sizeof(s1));
    n2 = (arg < argc) ? tr_set(argv[arg++], s2, sizeof(s2)) : 0;

    while ((c = getchar()) != EOF) {
        unsigned char uc = (unsigned char) c;
        bool in1 = false;
        size_t idx;

        for (idx = 0; idx < n1; idx++) {
            if (s1[idx] == uc) {
                in1 = true;
                break;
            }
        }
        if (del) {
            if (in1)
                continue;
        } else if (in1 && n2 > 0) {
            uc = s2[idx < n2 ? idx : n2 - 1];
        }
        if (squeeze) {
            const unsigned char *sq = n2 ? s2 : s1;
            size_t sn = n2 ? n2 : n1;
            bool insq = false;

            for (size_t k = 0; k < sn; k++) {
                if (sq[k] == uc) {
                    insq = true;
                    break;
                }
            }
            if (insq && (int) uc == last)
                continue;
        }
        putchar(uc);
        last = (int) uc;
    }
    return 0;
}

/* cut */
static bool cut_in_list(const char *list, long n)
{
    const char *p = list;

    while (*p) {
        long a = 1, b;
        char *end;

        if (isdigit((unsigned char) *p)) {
            a = strtol(p, &end, 10);
            p = end;
        }
        if (*p == '-') {
            p++;
            if (isdigit((unsigned char) *p)) {
                b = strtol(p, &end, 10);
                p = end;
            } else {
                b = LONG_MAX;
            }
        } else {
            b = a;
        }
        if (n >= a && n <= b)
            return true;
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
    return false;
}

static int applet_cut(int argc, char **argv)
{
    char delim = '\t';
    const char *list = NULL;
    bool chars = false;
    int arg = 1, rc = 0;

    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        const char *o = argv[arg];

        if (!strcmp(o, "-d")) {
            if (++arg >= argc)
                goto usage;
            delim = argv[arg][0];
        } else if (!strncmp(o, "-d", 2)) {
            delim = o[2];
        } else if (!strcmp(o, "-f") || !strcmp(o, "-c")) {
            chars = o[1] == 'c';
            if (++arg >= argc)
                goto usage;
            list = argv[arg];
        } else if (!strncmp(o, "-f", 2) || !strncmp(o, "-c", 2)) {
            chars = o[1] == 'c';
            list = o + 2;
        } else {
            goto usage;
        }
    }
    if (!list)
        goto usage;
    if (arg == argc)
        argv[--arg] = "-";

    for (int i = arg; i < argc; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        FILE *fp = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        char buf[TOOLBOX_LINE_MAX];

        if (!fp) {
            fprintf(stderr, "cut: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        while (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);

            if (len && buf[len - 1] == '\n')
                buf[--len] = '\0';
            if (chars) {
                for (size_t k = 0; k < len; k++) {
                    if (cut_in_list(list, (long) k + 1))
                        putchar(buf[k]);
                }
            } else if (!strchr(buf, delim)) {
                fputs(buf, stdout);
            } else {
                const char *start = buf;
                long field = 1;
                bool first = true;

                for (char *q = buf;; q++) {
                    if (*q == delim || *q == '\0') {
                        if (cut_in_list(list, field)) {
                            if (!first)
                                putchar(delim);
                            fwrite(start, 1, (size_t) (q - start), stdout);
                            first = false;
                        }
                        field++;
                        start = q + 1;
                        if (*q == '\0')
                            break;
                    }
                }
            }
            putchar('\n');
        }
        if (fp != stdin)
            fclose(fp);
    }
    return rc;
usage:
    fprintf(stderr, "usage: cut -d DELIM -f LIST | -c LIST [FILE ...]\n");
    return 1;
}

/* sort */
static bool sort_numeric, sort_fold, sort_ignb;

static int sort_cmp(const void *pa, const void *pb)
{
    const char *a = *(const char *const *) pa;
    const char *b = *(const char *const *) pb;

    if (sort_ignb) {
        while (isspace((unsigned char) *a))
            a++;
        while (isspace((unsigned char) *b))
            b++;
    }
    if (sort_numeric) {
        long x = strtol(a, NULL, 10), y = strtol(b, NULL, 10);

        return x < y ? -1 : (x > y ? 1 : 0);
    }
    return sort_fold ? strcasecmp(a, b) : strcmp(a, b);
}

static int applet_sort(int argc, char **argv)
{
    bool rev = false, uniq = false;
    int arg = 1, rc = 0;
    char **lines = NULL;
    size_t nlines = 0, cap = 0;

    sort_numeric = sort_fold = sort_ignb = false;
    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        for (const char *p = argv[arg] + 1; *p; p++) {
            switch (*p) {
            case 'n':
                sort_numeric = true;
                break;
            case 'r':
                rev = true;
                break;
            case 'u':
                uniq = true;
                break;
            case 'f':
                sort_fold = true;
                break;
            case 'b':
                sort_ignb = true;
                break;
            default:
                fprintf(stderr, "usage: sort [-bfnru] [FILE ...]\n");
                return 1;
            }
        }
    }
    if (arg == argc)
        argv[--arg] = "-";
    for (int i = arg; i < argc; i++) {
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
        FILE *fp = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
        char buf[TOOLBOX_LINE_MAX];

        if (!fp) {
            fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        while (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);

            if (len && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            if (nlines == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                char **tmp = realloc(lines, ncap * sizeof(*lines));

                if (!tmp) {
                    fprintf(stderr, "sort: out of memory\n");
                    rc = 1;
                    if (fp != stdin)
                        fclose(fp);
                    goto done;
                }
                lines = tmp;
                cap = ncap;
            }
            lines[nlines] = strdup(buf);
            if (!lines[nlines]) {
                fprintf(stderr, "sort: out of memory\n");
                rc = 1;
                if (fp != stdin)
                    fclose(fp);
                goto done;
            }
            nlines++;
        }
        if (fp != stdin)
            fclose(fp);
    }
    qsort(lines, nlines, sizeof(*lines), sort_cmp);
    for (size_t i = 0; i < nlines; i++) {
        size_t j = rev ? (nlines - 1 - i) : i;

        if (uniq && i > 0) {
            size_t pj = rev ? (nlines - i) : (i - 1);

            if (sort_cmp(&lines[j], &lines[pj]) == 0)
                continue;
        }
        puts(lines[j]);
    }
done:
    for (size_t i = 0; i < nlines; i++)
        free(lines[i]);
    free(lines);
    return rc;
}

/* uniq */
static void uniq_emit(const char *line, long count, bool c, bool d, bool u)
{
    if (d && count < 2)
        return;
    if (u && count > 1)
        return;
    if (c)
        printf("%7ld %s\n", count, line);
    else
        puts(line);
}

static int applet_uniq(int argc, char **argv)
{
    bool c = false, d = false, u = false, have_prev = false;
    int arg = 1;
    long count = 0;
    FILE *fp;
    char cur[TOOLBOX_LINE_MAX], prev[TOOLBOX_LINE_MAX];

    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        for (const char *p = argv[arg] + 1; *p; p++) {
            switch (*p) {
            case 'c':
                c = true;
                break;
            case 'd':
                d = true;
                break;
            case 'u':
                u = true;
                break;
            default:
                fprintf(stderr, "usage: uniq [-cdu] [FILE]\n");
                return 1;
            }
        }
    }

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fp = (arg >= argc || !strcmp(argv[arg], "-")) ? stdin
                                                  : fopen(argv[arg], "r");
    if (!fp) {
        fprintf(stderr, "uniq: %s: %s\n", argv[arg], strerror(errno));
        return 1;
    }
    while (fgets(cur, sizeof(cur), fp)) {
        size_t len = strlen(cur);

        if (len && cur[len - 1] == '\n')
            cur[len - 1] = '\0';
        if (have_prev && !strcmp(cur, prev)) {
            count++;
            continue;
        }
        if (have_prev)
            uniq_emit(prev, count, c, d, u);
        strcpy(prev, cur);
        count = 1;
        have_prev = true;
    }
    if (have_prev)
        uniq_emit(prev, count, c, d, u);
    if (fp != stdin)
        fclose(fp);
    return 0;
}

/* kill */
static int kill_signo(const char *name)
{
    static const struct {
        const char *n;
        int s;
    } sigs[] = {
        {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
        {"ILL", SIGILL},   {"ABRT", SIGABRT}, {"FPE", SIGFPE},
        {"KILL", SIGKILL}, {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE},
        {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"USR1", SIGUSR1},
        {"USR2", SIGUSR2}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT},
        {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {NULL, 0},
    };

    if (isdigit((unsigned char) name[0]))
        return atoi(name);
    if (!strncasecmp(name, "SIG", 3))
        name += 3;
    for (int i = 0; sigs[i].n; i++) {
        if (!strcasecmp(name, sigs[i].n))
            return sigs[i].s;
    }
    return -1;
}

static int applet_kill(int argc, char **argv)
{
    int sig = SIGTERM, arg = 1, rc = 0;

    if (arg < argc && argv[arg][0] == '-' && argv[arg][1]) {
        sig = kill_signo(argv[arg] + 1);
        if (sig < 0) {
            fprintf(stderr, "kill: unknown signal '%s'\n", argv[arg] + 1);
            return 1;
        }
        arg++;
    }
    if (arg >= argc) {
        fprintf(stderr, "usage: kill [-SIG] PID ...\n");
        return 1;
    }
    for (; arg < argc; arg++) {
        char *end;
        long pid;

        errno = 0;
        pid = strtol(argv[arg], &end, 10);
        if (end == argv[arg] || *end != '\0' || errno != 0 || pid < INT_MIN ||
            pid > INT_MAX) {
            fprintf(stderr, "kill: invalid pid '%s'\n", argv[arg]);
            rc = 1;
            continue;
        }
        if (kill((pid_t) pid, sig) != 0) {
            fprintf(stderr, "kill: (%ld): %s\n", pid, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* xargs */
static int xargs_run(char **av)
{
    pid_t pid;
    int status;
    int err = posix_spawnp(&pid, av[0], NULL, NULL, av, environ);

    if (err != 0) {
        fprintf(stderr, "xargs: %s: %s\n", av[0], strerror(err));
        return 1;
    }
    if (waitpid(pid, &status, 0) < 0)
        return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void xargs_free_pending(char **av, int base, int n)
{
    for (int k = base; k < n; k++)
        free(av[k]);
}

static int xargs_flush(char **av, int base, int *np)
{
    int n = *np;
    int rc = 0;

    if (n == base)
        return 0;

    av[n] = NULL;
    if (xargs_run(av) != 0)
        rc = 1;
    xargs_free_pending(av, base, n);
    *np = base;
    return rc;
}

static int applet_xargs(int argc, char **argv)
{
    enum { XARGS_ARGV_CAP = 250 };
    long maxn = 0;
    int arg = 1, rc = 0, base = 0, n;
    char *cmd_argv[256];
    char tok[TOOLBOX_LINE_MAX];
    size_t tl = 0;
    bool eof = false;
    bool fatal = false;
    bool saw_token = false;

    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        if (!strcmp(argv[arg], "-n")) {
            if (++arg >= argc || parse_positive(argv[arg], &maxn) != 0 ||
                maxn == 0) {
                fprintf(stderr, "xargs: bad -n count\n");
                return 1;
            }
        } else {
            fprintf(stderr, "usage: xargs [-n MAX] [COMMAND [ARG ...]]\n");
            return 1;
        }
    }
    if (arg < argc) {
        for (; arg < argc && base < 200; arg++)
            cmd_argv[base++] = argv[arg];
        if (arg < argc) {
            fprintf(stderr, "xargs: too many command arguments\n");
            return 1;
        }
    } else {
        cmd_argv[base++] = (char *) "echo";
    }

    n = base;
    while (!eof) {
        int c = getchar();

        if (c == EOF)
            eof = true;
        if (c == EOF || c == ' ' || c == '\t' || c == '\n') {
            bool full;

            if (tl == 0)
                continue;
            if (n >= XARGS_ARGV_CAP) {
                if (xargs_flush(cmd_argv, base, &n) != 0)
                    rc = 1;
            }

            tok[tl] = '\0';
            cmd_argv[n] = strdup(tok);
            if (!cmd_argv[n]) {
                xargs_free_pending(cmd_argv, base, n);
                n = base;
                rc = 1;
                fatal = true;
                break;
            }
            n++;
            saw_token = true;
            tl = 0;
            full = (maxn && (long) (n - base) >= maxn) || n >= XARGS_ARGV_CAP;
            if (full || (eof && n > base)) {
                if (xargs_flush(cmd_argv, base, &n) != 0)
                    rc = 1;
            }
        } else if (tl + 1 < sizeof(tok)) {
            tok[tl++] = (char) c;
        } else {
            xargs_free_pending(cmd_argv, base, n);
            n = base;
            fprintf(stderr, "xargs: argument too long\n");
            rc = 1;
            fatal = true;
            break;
        }
    }
    if (!fatal) {
        if (n > base && xargs_flush(cmd_argv, base, &n) != 0)
            rc = 1;
        else if (!saw_token && xargs_run(cmd_argv) != 0)
            rc = 1;
    }
    return rc;
}

/* expr Recursive-descent over argv tokens: | & (= != < <= > >=) (+ -) (* / %)
 * (). No string ops (length/substr/index) or `:` regex match. Intermediate
 * result strings are never freed — the applet exits right after evaluating. Cap
 * on '(' nesting in `expr`. Tuning bound (see SH_ARITH_MAX_DEPTH): far above
 * any real expression, small worst-case stack. Revisit if a real expr needs
 * deeper nesting.
 */
#define EXPR_MAX_DEPTH 64

struct expr_ctx {
    char **tok;
    int n;
    int pos;
    bool err;
    int depth; /* current '(' nesting, capped at EXPR_MAX_DEPTH */
};

static char *expr_num(long v)
{
    char b[32];

    snprintf(b, sizeof(b), "%ld", v);
    return strdup(b);
}

static long expr_toint(const char *s, bool *ok)
{
    char *end;
    long v;

    if (!s) { /* an upstream strdup failed (OOM); treat as non-numeric */
        *ok = false;
        return 0;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    *ok = (*s != '\0' && *end == '\0' && errno == 0);
    return v;
}

static bool expr_truthy(const char *s)
{
    bool ok;
    long v;

    if (!s || *s == '\0')
        return false;
    v = expr_toint(s, &ok);
    return ok ? v != 0 : true;
}

static char *expr_cur(struct expr_ctx *c)
{
    return c->pos < c->n ? c->tok[c->pos] : NULL;
}

static bool expr_is(struct expr_ctx *c, const char *op)
{
    char *t = expr_cur(c);

    return t && !strcmp(t, op);
}

static char *expr_or(struct expr_ctx *c);

static char *expr_prim(struct expr_ctx *c)
{
    if (expr_is(c, "(")) {
        char *v;

        c->pos++;
        if (c->depth >= EXPR_MAX_DEPTH) {
            c->err = true; /* refuse to recurse deeper */
            return strdup("");
        }
        c->depth++;
        v = expr_or(c);
        c->depth--;
        if (expr_is(c, ")"))
            c->pos++;
        else
            c->err = true;
        return v;
    }
    if (c->pos < c->n)
        return strdup(c->tok[c->pos++]);
    c->err = true;
    return strdup("");
}

static char *expr_mul(struct expr_ctx *c)
{
    char *l = expr_prim(c);

    while (expr_is(c, "*") || expr_is(c, "/") || expr_is(c, "%")) {
        char op = c->tok[c->pos][0];
        char *r;
        bool ok1, ok2;
        long a, b;

        c->pos++;
        r = expr_prim(c);
        a = expr_toint(l, &ok1);
        b = expr_toint(r, &ok2);
        if (!ok1 || !ok2) {
            c->err = true;
            return expr_num(0);
        }
        if (op != '*' && b == 0) {
            fprintf(stderr, "expr: division by zero\n");
            c->err = true;
            return expr_num(0);
        }
        {
            long res;

            if (op == '*') {
                if (__builtin_mul_overflow(a, b, &res)) {
                    fprintf(stderr, "expr: integer overflow\n");
                    c->err = true;
                    return expr_num(0);
                }
            } else if (a == LONG_MIN && b == -1) {
                /* a / -1 overflows; a %% -1 is 0 — compute neither (UB). */
                if (op == '/') {
                    fprintf(stderr, "expr: integer overflow\n");
                    c->err = true;
                    return expr_num(0);
                }
                res = 0;
            } else {
                res = op == '/' ? a / b : a % b;
            }
            l = expr_num(res);
        }
    }
    return l;
}

static char *expr_add(struct expr_ctx *c)
{
    char *l = expr_mul(c);

    while (expr_is(c, "+") || expr_is(c, "-")) {
        char op = c->tok[c->pos][0];
        char *r;
        bool ok1, ok2;
        long a, b;

        c->pos++;
        r = expr_mul(c);
        a = expr_toint(l, &ok1);
        b = expr_toint(r, &ok2);
        if (!ok1 || !ok2) {
            c->err = true;
            return expr_num(0);
        }
        {
            long res;

            if (op == '+' ? __builtin_add_overflow(a, b, &res)
                          : __builtin_sub_overflow(a, b, &res)) {
                fprintf(stderr, "expr: integer overflow\n");
                c->err = true;
                return expr_num(0);
            }
            l = expr_num(res);
        }
    }
    return l;
}

static char *expr_relop(struct expr_ctx *c)
{
    char *l = expr_add(c);

    while (expr_is(c, "=") || expr_is(c, "!=") || expr_is(c, "<") ||
           expr_is(c, "<=") || expr_is(c, ">") || expr_is(c, ">=")) {
        char op[3];
        char *r;
        bool ok1, ok2, res;
        long a, b;
        int cmp;

        snprintf(op, sizeof(op), "%s", c->tok[c->pos]);
        c->pos++;
        r = expr_add(c);
        a = expr_toint(l, &ok1);
        b = expr_toint(r, &ok2);
        if (ok1 && ok2)
            cmp = a < b ? -1 : (a > b ? 1 : 0);
        else if (!l || !r) { /* an upstream strdup failed (OOM) */
            c->err = true;
            cmp = 0;
        } else {
            int s = strcmp(l, r);

            cmp = s < 0 ? -1 : (s > 0 ? 1 : 0);
        }
        if (!strcmp(op, "="))
            res = cmp == 0;
        else if (!strcmp(op, "!="))
            res = cmp != 0;
        else if (!strcmp(op, "<"))
            res = cmp < 0;
        else if (!strcmp(op, "<="))
            res = cmp <= 0;
        else if (!strcmp(op, ">"))
            res = cmp > 0;
        else
            res = cmp >= 0;
        l = expr_num(res ? 1 : 0);
    }
    return l;
}

static char *expr_and(struct expr_ctx *c)
{
    char *l = expr_relop(c);

    while (expr_is(c, "&")) {
        char *r;

        c->pos++;
        r = expr_relop(c);
        if (!expr_truthy(l) || !expr_truthy(r))
            l = expr_num(0);
    }
    return l;
}

static char *expr_or(struct expr_ctx *c)
{
    char *l = expr_and(c);

    while (expr_is(c, "|")) {
        char *r;

        c->pos++;
        r = expr_and(c);
        if (!expr_truthy(l))
            l = r;
    }
    return l;
}

static int applet_expr(int argc, char **argv)
{
    struct expr_ctx c = {argv + 1, argc - 1, 0, false, 0};
    char *r;

    if (argc < 2) {
        fprintf(stderr, "usage: expr EXPRESSION\n");
        return 2;
    }
    r = expr_or(&c);
    if (c.err || c.pos != c.n) {
        fprintf(stderr, "expr: syntax error\n");
        return 2;
    }
    puts(r);
    return expr_truthy(r) ? 0 : 1;
}

/* printf */
static int applet_printf(int argc, char **argv)
{
    const char *fmt;
    int argi;

    if (argc < 2) {
        fprintf(stderr, "usage: printf FORMAT [ARG ...]\n");
        return 1;
    }
    fmt = argv[1];
    argi = 2;

    do {
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n':
                    putchar('\n');
                    break;
                case 't':
                    putchar('\t');
                    break;
                case 'r':
                    putchar('\r');
                    break;
                case '\\':
                    putchar('\\');
                    break;
                case 'a':
                    putchar('\a');
                    break;
                case 'b':
                    putchar('\b');
                    break;
                case 'f':
                    putchar('\f');
                    break;
                case 'v':
                    putchar('\v');
                    break;
                case '\0':
                    putchar('\\');
                    p--;
                    break;
                default:
                    putchar('\\');
                    putchar(*p);
                    break;
                }
            } else if (*p == '%') {
                char spec[32];
                size_t sl = 0;
                char conv;
                const char *a;

                spec[sl++] = '%';
                p++;
                if (*p == '%') {
                    putchar('%');
                    continue;
                }
                while (*p && strchr("-+ #0", *p) && sl < sizeof(spec) - 3)
                    spec[sl++] = *p++;
                while (isdigit((unsigned char) *p) && sl < sizeof(spec) - 3)
                    spec[sl++] = *p++;
                if (*p == '.') {
                    if (sl < sizeof(spec) - 3)
                        spec[sl++] = '.';
                    p++;
                    while (isdigit((unsigned char) *p)) {
                        if (sl < sizeof(spec) - 3)
                            spec[sl++] = *p;
                        p++;
                    }
                }
                conv = *p;

                /* Only fetch an argument for conversions that consume one, so
                 * an unsupported or bare specifier does not desync the stream.
                 */
                switch (conv) {
                case 'd':
                case 'i':
                    spec[sl++] = 'l';
                    spec[sl++] = 'd';
                    spec[sl] = '\0';
                    printf(spec,
                           strtol(argi < argc ? argv[argi++] : "0", NULL, 0));
                    break;
                case 'u':
                case 'x':
                case 'X':
                case 'o':
                    spec[sl++] = 'l';
                    spec[sl++] = conv;
                    spec[sl] = '\0';
                    printf(spec,
                           strtoul(argi < argc ? argv[argi++] : "0", NULL, 0));
                    break;
                case 'c':
                    a = (argi < argc) ? argv[argi++] : "";

                    /* A missing or empty %c argument prints nothing rather than
                     * a stray NUL byte, matching BusyBox/GNU printf.
                     */
                    if (a[0] != '\0') {
                        spec[sl++] = 'c';
                        spec[sl] = '\0';
                        printf(spec, a[0]);
                    }
                    break;
                case 's':
                    spec[sl++] = 's';
                    spec[sl] = '\0';
                    printf(spec, argi < argc ? argv[argi++] : "");
                    break;
                case '\0':
                    putchar('%');
                    p--;
                    break;
                default:
                    putchar('%');
                    putchar(conv);
                    break;
                }
            } else {
                putchar(*p);
            }
        }
    } while (argi < argc && argi > 2);
    return 0;
}

/* readlink */
static int applet_readlink(int argc, char **argv)
{
    int arg = 1, rc = 0;

    /* -f is accepted for script compatibility but not canonicalized — the libc
     * has no realpath() and the flat FS has essentially no symlinks.
     */
    if (arg < argc && !strcmp(argv[arg], "-f"))
        arg++;
    if (arg >= argc) {
        fprintf(stderr, "usage: readlink [-f] FILE ...\n");
        return 1;
    }
    for (; arg < argc; arg++) {
        char buf[PATH_MAX];
        ssize_t n = readlink(argv[arg], buf, sizeof(buf) - 1);

        if (n < 0) {
            fprintf(stderr, "readlink: %s: %s\n", argv[arg], strerror(errno));
            rc = 1;
            continue;
        }
        buf[n] = '\0';
        puts(buf);
    }
    return rc;
}

int main(int argc, char **argv)
{
    const char *name;
    const struct toolbox_applet *ap;

    if (argc < 1 || !argv[0]) {
        usage_all(stderr);
        return 1;
    }

    name = applet_name(progname(argv[0]));
    if (!strcmp(name, "toolbox")) {
        if (argc < 2) {
            usage_all(stderr);
            return 1;
        }
        name = applet_name(argv[1]);
        argc--;
        argv++;
    }

    ap = find_applet(name);
    if (!ap) {
        fprintf(stderr, "toolbox: unknown applet '%s'\n", name);
        usage_all(stderr);
        return 1;
    }
    optind = 1;
    return ap->main(argc, argv);
}
