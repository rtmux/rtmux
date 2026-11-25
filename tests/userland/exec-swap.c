/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* exec() starts the new image in the calling process.
 *
 * Each case spawns a child of this binary that sets some state up and execs.
 * What the parent can observe from outside is the child's pid and its wait
 * status; what only the new image can say it reports on its stdout (a pipe the
 * parent reads), from "--exec-report":
 *
 * - /bin/true and /bin/false run, and the status the parent collects is theirs;
 * - the new image has the pid the parent spawned, and the same parent;
 * - a descriptor without close-on-exec is open in it, one with is closed, and
 *   the standard streams are the ones the child had;
 * - a signal ignored before the exec is still ignored, one that was caught is
 *   back to its default, and the blocked set is kept;
 * - the working directory and the umask are kept, argv and the envp passed to
 *   execve arrive as given;
 * - a failed exec returns -1 with errno set and the process carries on;
 * - the call from a second thread, or with another thread waiting, replaces
 *   the whole process, and so does one while another thread keeps creating
 *   threads;
 * - the new image can start a thread of its own;
 * - an exec given up in its first step (debug-inject kernels) leaves the
 *   caller able to start threads and exec;
 * - setpgid on a child that has exec'd is refused with EACCES, and on one that
 *   has only been spawned it works.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "outcome.h"
#include "test.h"

#define EXEC_FD_KEPT 7
#define EXEC_FD_CLOSED 8
#define EXEC_UMASK 027
#define EXEC_UMASK_OCTAL 27 /* the digits the report prints with %o */
#define EXEC_ENV "EXEC_TEST=swapped"
#define EXEC_ARG "exec-arg-1"
#define EXEC_DIR "/bin"
#define EXEC_CHAIN_LEN "20" /* execs in a chain, one image after another */
#define EXEC_CONCURRENT 6   /* children exec'ing at the same time */

static void exec_usr2_handler(int signo)
{
    (void) signo;
}

/* The state the child leaves behind for the new image to find. */
static int exec_child_setup(void)
{
    struct sigaction sa;
    sigset_t set;
    int fd = open("/dev/null", O_RDONLY);

    if (fd < 0)
        return -1;
    if (dup2(fd, EXEC_FD_KEPT) != EXEC_FD_KEPT ||
        dup2(fd, EXEC_FD_CLOSED) != EXEC_FD_CLOSED)
        return -1;
    (void) close(fd);
    if (fcntl(EXEC_FD_CLOSED, F_SETFD, FD_CLOEXEC) != 0)
        return -1;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        return -1;
    sa.sa_handler = exec_usr2_handler;
    if (sigaction(SIGUSR2, &sa, NULL) != 0)
        return -1;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
        return -1;

    (void) umask(EXEC_UMASK);
    return chdir(EXEC_DIR);
}

static void *exec_from_thread(void *arg)
{
    char *argv[] = {"syscall_testing", "--exec-report", EXEC_ARG, NULL};
    char *envp[] = {EXEC_ENV, NULL};

    (void) arg;
    (void) execve("/bin/syscall_testing", argv, envp);
    _exit(97);
}

static void *exec_idle_thread(void *arg)
{
    (void) arg;
    for (;;)
        sleep(1);
    return NULL;
}

static void *exec_nop_thread(void *arg)
{
    return arg;
}

/* Starts and joins threads for as long as it lives: the exec of the first
 * thread takes this one down while it is creating them, and refuses a thread
 * created in the meantime.
 */
static atomic_int exec_created; /* threads the creator has started and joined */

static void *exec_creator_thread(void *arg)
{
    pthread_t th;

    (void) arg;
    for (;;) {
        if (pthread_create(&th, NULL, exec_nop_thread, NULL) == 0) {
            (void) pthread_join(th, NULL);
            atomic_fetch_add(&exec_created, 1);
        }
    }
    return NULL;
}

/* The mode of the child: set up, then exec according to what is asked. */
int exec_swap_child(const char *what)
{
    char *report[] = {"syscall_testing", "--exec-report", EXEC_ARG, NULL};
    char *envp[] = {EXEC_ENV, NULL};
    pthread_t th;

    if (!strcmp(what, "true")) {
        (void) execl("/bin/true", "true", (char *) NULL);
        return 97;
    }
    if (!strcmp(what, "false")) {
        (void) execl("/bin/false", "false", (char *) NULL);
        return 97;
    }
    if (!strcmp(what, "report")) {
        if (exec_child_setup() != 0)
            return 96;
        (void) execve("/bin/syscall_testing", report, envp);
        return 97;
    }
    if (!strcmp(what, "path")) {
        /* execvp finds the program on PATH. */
        char *argv[] = {"true", NULL};

        (void) setenv("PATH", "/bin", 1);
        (void) execvp("true", argv);
        return 97;
    }
    if (!strcmp(what, "from-thread")) {
        /* The thread that calls exec is not the first one. */
        if (exec_child_setup() != 0)
            return 96;
        if (pthread_create(&th, NULL, exec_from_thread, NULL) != 0)
            return 95;
        for (;;)
            sleep(1);
    }
    if (!strcmp(what, "with-thread")) {
        /* The first thread calls exec while another waits. */
        if (exec_child_setup() != 0)
            return 96;
        if (pthread_create(&th, NULL, exec_idle_thread, NULL) != 0)
            return 95;
        sleep_ms(50);
        (void) execve("/bin/syscall_testing", report, envp);
        return 97;
    }
    if (!strcmp(what, "with-creator")) {
        /* The first thread calls exec while another creates threads. */
        if (exec_child_setup() != 0)
            return 96;
        if (pthread_create(&th, NULL, exec_creator_thread, NULL) != 0)
            return 95;
        /* Exec only once the creator is creating threads, for 5 s at most. */
        for (int i = 0; atomic_load(&exec_created) < 2; i++) {
            if (i == 5000)
                return 94;
            sleep_ms(1);
        }
        (void) execve("/bin/syscall_testing", report, envp);
        return 97;
    }
    if (!strcmp(what, "fail")) {
        /* A failed exec changes nothing: the descriptors, the process and its
         * threads are still there, and the next exec works. Says what it saw
         * and then replaces itself by the report.
         */
        int e1, e2;

        errno = 0;
        (void) execl("/nonexistent/exec-target", "x", (char *) NULL);
        e1 = errno;
        errno = 0;
        (void) execl("/bin", "x", (char *) NULL);
        e2 = errno;
        printf("fail1=%d fail2=%d pid=%d\n", e1, e2, (int) getpid());
        fflush(stdout);
        (void) execve("/bin/syscall_testing", report, envp);
        return 97;
    }
    if (!strcmp(what, "abandoned")) {
        /* An exec given up in its first step (debug-inject: the loader shell
         * is gone at the step's second call) fails, and this thread, all that
         * is left of the process, can still start a thread and exec. Says
         * what it saw and then replaces itself by the report.
         */
        char pidarg[16];
        int e, made;

        if (pthread_create(&th, NULL, exec_idle_thread, NULL) != 0)
            return 95;
        snprintf(pidarg, sizeof pidarg, "%d", (int) getpid());
        if (inject_run("abandon-exec", pidarg, NULL) != 0)
            return 93;
        errno = 0;
        (void) execve("/bin/syscall_testing", report, envp);
        e = errno;
        made = pthread_create(&th, NULL, exec_nop_thread, NULL);
        if (made == 0)
            (void) pthread_join(th, NULL);
        printf("abandoned=%d create=%d\n", e, made);
        fflush(stdout);
        (void) execve("/bin/syscall_testing", report, envp);
        return 97;
    }
    if (!strcmp(what, "chain")) {
        (void) exec_swap_chain(EXEC_CHAIN_LEN);
        return 97;
    }
    if (!strcmp(what, "hold")) {
        char *argv[] = {"syscall_testing", "--getsid-hold", NULL};

        (void) execv("/bin/syscall_testing", argv);
        return 97;
    }
    return 98;
}

/* The mode of an image in a chain of execs: exec again until the count runs
 * out, then say where it ended up.
 */
int exec_swap_chain(const char *count)
{
    int n = atoi(count);
    char next[16];
    char *argv[] = {"syscall_testing", "--exec-chain", next, NULL};

    if (n > 0) {
        snprintf(next, sizeof next, "%d", n - 1);
        (void) execv("/bin/syscall_testing", argv);
        return 97;
    }
    printf("pid=%d chain=done\n", (int) getpid());
    fflush(stdout);
    return 0;
}

/* The mode of the new image: describe the process on stdout. */
int exec_swap_report(const char *arg)
{
    struct sigaction sa;
    sigset_t set;
    char cwd[64];
    mode_t um;
    const char *env = getenv("EXEC_TEST");
    int usr1 = -1, usr2 = -1, hup = -1, thread = 0;
    pthread_t th;
    void *rv = NULL;

    if (sigaction(SIGUSR1, NULL, &sa) == 0)
        usr1 = sa.sa_handler == SIG_IGN ? 1 : sa.sa_handler == SIG_DFL ? 0 : 2;
    if (sigaction(SIGUSR2, NULL, &sa) == 0)
        usr2 = sa.sa_handler == SIG_IGN ? 1 : sa.sa_handler == SIG_DFL ? 0 : 2;
    if (sigprocmask(SIG_BLOCK, NULL, &set) == 0)
        hup = sigismember(&set, SIGHUP);
    if (!getcwd(cwd, sizeof cwd))
        cwd[0] = '?', cwd[1] = '\0';
    um = umask(0);
    (void) umask(um);
    if (pthread_create(&th, NULL, exec_nop_thread, &thread) == 0 &&
        pthread_join(th, &rv) == 0 && rv == &thread)
        thread = 1;

    printf(
        "pid=%d ppid=%d kept=%d closed=%d usr1=%d usr2=%d hup=%d "
        "umask=%o thread=%d cwd=%s arg=%s env=%s\n",
        (int) getpid(), (int) getppid(), fcntl(EXEC_FD_KEPT, F_GETFD) >= 0,
        fcntl(EXEC_FD_CLOSED, F_GETFD) >= 0, usr1, usr2, hup, (unsigned) um,
        thread, cwd, arg, env ? env : "(none)");
    fflush(stdout);
    return 0;
}

struct exec_run {
    pid_t pid;
    int status;
    int rfd;
    char out[512];
};

/* Spawn the child with its stdout on a pipe. Returns 0, or -1 if it could not
 * be run.
 */
static int exec_start(const char *what, struct exec_run *r)
{
    char *argv[] = {"syscall_testing", "--exec-child", (char *) what, NULL};
    posix_spawn_file_actions_t fa;
    int p[2];
    int rc;

    memset(r, 0, sizeof *r);
    r->rfd = -1;
    if (pipe(p) != 0)
        return -1;
    (void) fcntl(p[0], F_SETFD, FD_CLOEXEC);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_adddup2(&fa, p[1], STDOUT_FILENO);
    (void) posix_spawn_file_actions_addclose(&fa, p[1]);
    rc = posix_spawn(&r->pid, "/bin/syscall_testing", &fa, NULL, argv, NULL);
    (void) posix_spawn_file_actions_destroy(&fa);
    (void) close(p[1]);
    if (rc != 0) {
        (void) close(p[0]);
        return -1;
    }
    r->rfd = p[0];
    return 0;
}

/* Collect the output and the wait status of a child exec_start() began.
 * Returns 0, or -1 if the child could not be collected.
 */
static int exec_finish(struct exec_run *r)
{
    size_t n = 0;
    ssize_t got;

    while (n < sizeof r->out - 1 &&
           (got = read(r->rfd, r->out + n, sizeof r->out - 1 - n)) > 0)
        n += (size_t) got;
    r->out[n] = '\0';
    (void) close(r->rfd);
    r->rfd = -1;
    if (waitpid(r->pid, &r->status, 0) != r->pid)
        return -1;
    return 0;
}

static int exec_run(const char *what, struct exec_run *r)
{
    return exec_start(what, r) == 0 ? exec_finish(r) : -1;
}

/* The integer after the last "key=" in out (a field of the line the new image
 * wrote, whatever the child printed before the exec), or -1.
 */
static long exec_field(const char *out, const char *key)
{
    const char *p;
    long val = -1;
    size_t klen = strlen(key);

    for (p = out; (p = strstr(p, key)); p++) {
        if ((p == out || p[-1] == ' ' || p[-1] == '\n') && p[klen] == '=')
            val = strtol(p + klen + 1, NULL, 10);
    }
    return val;
}

static int exec_has(const char *out, const char *text)
{
    return strstr(out, text) != NULL;
}

static void exec_status(const char *what, int want)
{
    struct exec_run r;
    char name[64], detail[128];

    snprintf(name, sizeof name, "exec %s: status", what);
    if (exec_run(what, &r) != 0) {
        test_fail(name, "the child could not be run");
        return;
    }
    snprintf(detail, sizeof detail, "want exit %d, got status %#x", want,
             r.status);
    check(WIFEXITED(r.status) && WEXITSTATUS(r.status) == want, name, detail);
}

/* The new image of the "report" cases: its own report line, against what the
 * child left behind.
 */
static void exec_check_report(const char *what, const struct exec_run *r)
{
    char name[96];

    snprintf(name, sizeof name, "exec %s: the new image ran, status 0", what);
    check(WIFEXITED(r->status) && WEXITSTATUS(r->status) == 0 &&
              exec_has(r->out, "pid="),
          name, "the new image must run and exit 0");
    snprintf(name, sizeof name, "exec %s: same pid", what);
    check(exec_field(r->out, "pid") == (long) r->pid, name,
          "exec keeps the pid");
    snprintf(name, sizeof name, "exec %s: same parent", what);
    check(exec_field(r->out, "ppid") == (long) getpid(), name,
          "exec keeps the parent");
    snprintf(name, sizeof name, "exec %s: descriptor without CLOEXEC kept",
             what);
    check(exec_field(r->out, "kept") == 1, name,
          "a descriptor without close-on-exec stays open");
    snprintf(name, sizeof name, "exec %s: descriptor with CLOEXEC closed",
             what);
    check(exec_field(r->out, "closed") == 0, name,
          "a close-on-exec descriptor is closed");
    snprintf(name, sizeof name, "exec %s: ignored signal stays ignored", what);
    check(exec_field(r->out, "usr1") == 1, name, "SIG_IGN survives exec");
    snprintf(name, sizeof name, "exec %s: caught signal back to default", what);
    check(exec_field(r->out, "usr2") == 0, name,
          "a handler does not survive exec");
    snprintf(name, sizeof name, "exec %s: blocked signal stays blocked", what);
    check(exec_field(r->out, "hup") == 1, name,
          "the blocked set survives exec");
    snprintf(name, sizeof name, "exec %s: umask and cwd kept", what);
    check(exec_field(r->out, "umask") == EXEC_UMASK_OCTAL &&
              exec_has(r->out, "cwd=" EXEC_DIR " "),
          name, "umask and working directory survive exec");
    snprintf(name, sizeof name, "exec %s: the new image starts a thread",
             what);
    check(exec_field(r->out, "thread") == 1, name,
          "pthread_create works in the new image");
    snprintf(name, sizeof name, "exec %s: argv and envp as passed", what);
    check(exec_has(r->out, "arg=" EXEC_ARG " ") &&
              exec_has(r->out, "env=swapped"),
          name, "the new image gets the argv and envp given to execve");
}

static void exec_report_case(const char *what)
{
    struct exec_run r;
    char name[96];

    if (exec_run(what, &r) != 0) {
        snprintf(name, sizeof name, "exec %s", what);
        test_fail(name, "the child could not be run");
        return;
    }
    exec_check_report(what, &r);
}

static void exec_failure_case(void)
{
    struct exec_run r;

    if (exec_run("fail", &r) != 0) {
        test_fail("exec failure", "the child could not be run");
        return;
    }
    check(exec_field(r.out, "fail1") == ENOENT,
          "exec of a missing file fails with ENOENT",
          "a failed exec returns -1 with the error");
    check(exec_field(r.out, "fail2") == EACCES ||
              exec_field(r.out, "fail2") == ENOEXEC,
          "exec of a directory fails",
          "a directory is not an executable image");
    check(exec_field(r.out, "pid") == (long) r.pid,
          "exec after failed execs: same pid",
          "the process carries on and execs");
    check(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0,
          "exec after failed execs: new image exits 0",
          "the new image runs after the failed attempts");
}

/* An exec given up in its first kernel step, after that step has marked the
 * process and destroyed the caller's other threads, leaves the caller able to
 * start threads and to exec: the mark that refuses new threads while an exec
 * is under way must go with the exec. Needs a debug-inject kernel.
 */
static void exec_abandoned_case(void)
{
    struct exec_run r;
    char detail[96];

    if (inject_run("alloc-status", NULL, NULL) != 0) {
        test_skip("exec abandoned", "fault injection unavailable");
        return;
    }
    if (exec_run("abandoned", &r) != 0) {
        test_fail("exec abandoned", "the child could not be run");
        return;
    }
    snprintf(detail, sizeof detail, "abandoned=%ld, status %#x",
             exec_field(r.out, "abandoned"), r.status);
    check(exec_field(r.out, "abandoned") == ESRCH,
          "exec abandoned in its first step fails with ESRCH", detail);
    snprintf(detail, sizeof detail, "pthread_create returned %ld",
             exec_field(r.out, "create"));
    check(exec_field(r.out, "create") == 0,
          "exec abandoned: the caller can still start a thread", detail);
    check(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0 &&
              exec_field(r.out, "pid") == (long) r.pid &&
              exec_field(r.out, "thread") == 1,
          "exec abandoned: the next exec runs the new image",
          "the caller execs again and the new image starts a thread");
}

/* --exec-abandon: the debug-inject stage. */
void test_exec_abandon(void)
{
    exec_abandoned_case();
}

/* A chain of execs ends in the process it started in, with the same pid. */
static void exec_chain_case(void)
{
    struct exec_run r;

    if (exec_run("chain", &r) != 0) {
        test_fail("exec chain", "the child could not be run");
        return;
    }
    check(exec_has(r.out, "chain=done") &&
              exec_field(r.out, "pid") == (long) r.pid,
          "a chain of execs ends in the same process",
          "every exec keeps the pid, and the last image runs");
    check(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0,
          "a chain of execs exits 0", "the last image's status is collected");
}

/* Several children exec at once, so that the swaps overlap on the harts. */
static void exec_concurrent_case(void)
{
    struct exec_run r[EXEC_CONCURRENT];
    int started = 0, good = 0;

    for (int i = 0; i < EXEC_CONCURRENT; i++) {
        if (exec_start("report", &r[i]) != 0)
            break;
        started++;
    }
    for (int i = 0; i < started; i++) {
        if (exec_finish(&r[i]) == 0 && WIFEXITED(r[i].status) &&
            WEXITSTATUS(r[i].status) == 0 &&
            exec_field(r[i].out, "pid") == (long) r[i].pid &&
            exec_field(r[i].out, "usr1") == 1)
            good++;
    }
    check(started == EXEC_CONCURRENT && good == started,
          "concurrent execs all start their new image",
          "each child keeps its own pid and runs its new image to exit 0");
}

/* A pipe pair for a child that holds until stdin is closed. */
static pid_t exec_hold_spawn(const char *what, int *hold, int *rfd)
{
    char *argv[] = {"syscall_testing", "--exec-child", (char *) what, NULL};
    posix_spawn_file_actions_t fa;
    int in[2], out[2];
    pid_t pid = (pid_t) -1;
    int rc;

    if (pipe(in) != 0)
        return -1;
    if (pipe(out) != 0) {
        (void) close(in[0]);
        (void) close(in[1]);
        return -1;
    }
    (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(out[0], F_SETFD, FD_CLOEXEC);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
    (void) posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    (void) posix_spawn_file_actions_addclose(&fa, in[0]);
    (void) posix_spawn_file_actions_addclose(&fa, out[1]);
    rc = posix_spawn(&pid, "/bin/syscall_testing", &fa, NULL, argv, NULL);
    (void) posix_spawn_file_actions_destroy(&fa);
    (void) close(in[0]);
    (void) close(out[1]);
    if (rc != 0) {
        (void) close(in[1]);
        (void) close(out[0]);
        return -1;
    }
    *hold = in[1];
    *rfd = out[0];
    return pid;
}

/* setpgid on a child that has exec'd is EACCES; on one that has only been
 * spawned, it works. The exec'd child says it is running from its new image.
 */
static void exec_setpgid_case(void)
{
    int h_exec = -1, r_exec = -1, h_plain = -1, r_plain = -1;
    pid_t c = exec_hold_spawn("hold", &h_exec, &r_exec);
    char *argv[] = {"syscall_testing", "--getsid-hold", NULL};
    char b = 0;
    int rc, err, st;
    pid_t p = (pid_t) -1;
    posix_spawn_file_actions_t fa;
    int in[2], out[2];

    check(c != (pid_t) -1, "exec setpgid: spawn the child that execs",
          "the case needs a child that execs");
    if (c == (pid_t) -1)
        return;
    check(read(r_exec, &b, 1) == 1 && b == 'r',
          "exec setpgid: the new image is running",
          "the exec'd child must start its new image");

    errno = 0;
    rc = setpgid(c, c);
    err = errno;
    check(rc == -1 && err == EACCES, "setpgid after the child's exec",
          "a child that has exec'd must be refused with EACCES");

    /* A child that has only been spawned stays movable. */
    if (pipe(in) == 0 && pipe(out) == 0) {
        (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
        (void) fcntl(out[0], F_SETFD, FD_CLOEXEC);
        (void) posix_spawn_file_actions_init(&fa);
        (void) posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
        (void) posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
        (void) posix_spawn_file_actions_addclose(&fa, in[0]);
        (void) posix_spawn_file_actions_addclose(&fa, out[1]);
        rc = posix_spawn(&p, "/bin/syscall_testing", &fa, NULL, argv, NULL);
        (void) posix_spawn_file_actions_destroy(&fa);
        (void) close(in[0]);
        (void) close(out[1]);
        h_plain = in[1];
        r_plain = out[0];
        if (rc == 0 && read(r_plain, &b, 1) == 1) {
            check(setpgid(p, p) == 0 && getpgid(p) == p,
                  "setpgid of a spawned child that has not exec'd",
                  "a child that has only been spawned stays movable");
        } else {
            test_fail("setpgid of a spawned child that has not exec'd",
                      "the plain child did not run");
        }
    }

    (void) close(h_exec);
    (void) waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "exec setpgid: the exec'd child exits 0 when its stdin closes",
          "the new image reads the descriptor it inherited");
    (void) close(r_exec);
    if (p != (pid_t) -1) {
        (void) close(h_plain);
        (void) waitpid(p, &st, 0);
        (void) close(r_plain);
    }
}

void test_exec(void)
{
    exec_status("true", 0);
    exec_status("false", 1);
    exec_status("path", 0);
    exec_report_case("report");
    exec_failure_case();
    exec_report_case("with-thread");
    exec_report_case("from-thread");
    exec_report_case("with-creator");
    exec_chain_case();
    exec_concurrent_case();
    exec_setpgid_case();
}

#define SPAWN_TIME_RUNS 20

static int spawn_time_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;

    return x < y ? -1 : x > y;
}

/* Spawn argv and wait for it; the microseconds that took, or 0 on failure. */
static uint64_t spawn_time_once(char *const argv[])
{
    struct timespec t0, t1;
    pid_t pid;
    int st;

    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, NULL) != 0 ||
        waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st))
        return 0;
    (void) clock_gettime(CLOCK_MONOTONIC, &t1);
    return (uint64_t) ((int64_t) (t1.tv_sec - t0.tv_sec) * 1000000 +
                       ((int64_t) t1.tv_nsec - t0.tv_nsec) / 1000);
}

static void spawn_time_case(const char *name, char *const argv[])
{
    uint64_t us[SPAWN_TIME_RUNS];
    int n = 0;

    for (int i = 0; i < SPAWN_TIME_RUNS; i++) {
        uint64_t t = spawn_time_once(argv);

        if (t)
            us[n++] = t;
    }
    if (!n) {
        printf("  INFO  %s: every run failed\n", name);
        return;
    }
    qsort(us, (size_t) n, sizeof us[0], spawn_time_cmp);
    printf("  INFO  %s: %d runs, min %llu us, median %llu us, max %llu us\n",
           name, n, (unsigned long long) us[0],
           (unsigned long long) us[n / 2], (unsigned long long) us[n - 1]);
}

/* --spawn-exec-time: how long a spawn, and a spawn whose child execs, take to
 * the parent's waitpid, as measurements only. The figures depend on the
 * host: under QEMU TCG they show the order of magnitude, not a bound.
 */
void test_spawn_exec_time(void)
{
    char *spawn_true[] = {"/bin/true", NULL};
    char *exec_true[] = {"/bin/syscall_testing", "--exec-child", "true", NULL};
    char *self_true[] = {"/bin/syscall_testing", "--version-child", NULL};

    spawn_time_case("spawn /bin/true", spawn_true);
    spawn_time_case("spawn a child that execs /bin/true", exec_true);
    spawn_time_case("spawn the dynamically linked test binary", self_true);
}
