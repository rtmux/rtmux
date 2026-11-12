/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/calls.h>
#include <sys/syspage.h>
#include <sys/slog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern int pi3_simd_probe(uint64_t pattern, unsigned call);
static int failed;
#define CHECK(name, cond)                                         \
    do {                                                          \
        int ok = !!(cond);                                        \
        printf("pi3-runtime: %s %s\n", name, ok ? "ok" : "FAIL"); \
        if (!ok)                                                  \
            failed++;                                             \
    } while (0)

static void *worker(void *arg)
{
    unsigned cpu = (unsigned) (uintptr_t) arg;
    struct {
        struct _thread_runmask hdr;
        unsigned run, inherit;
    } mask = {.hdr.size = 1, .run = 1u << cpu, .inherit = 1u << cpu};
    if (thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, &mask) == -1)
        return (void *) 1;
    errno = (int) (120 + cpu);
    for (unsigned i = 0; i < 128; i++) {
        if (pi3_simd_probe(0x123456789abcdef0ULL + cpu * 256 + i,
                           __KSYS_SCHED_YIELD))
            return (void *) 2;
        if (errno != (int) (120 + cpu))
            return (void *) 3;
    }
    return NULL;
}

/* Hold one child thread in REPLY on an UNBLOCK channel while its sibling
 * exits the process. The server deliberately never acknowledges cancellation;
 * process teardown must drain the blocked thread and complete waitpid anyway. */
static void *exit_sibling(void *arg)
{
    int tag = 2;
    if (msg_send((int) (intptr_t) arg, &tag, sizeof(tag), NULL, 0) == 0)
        _exit(37);
    _exit(97);
}

static int exit_with_unreplied_ipc(void)
{
    for (unsigned i = 0; i < 16; i++) {
        int chid = channel_create(RTMUX_FLG_CHF_UNBLOCK);
        if (chid < 0)
            return 0;
        char parent[24], channel[24];
        snprintf(parent, sizeof(parent), "%d", getpid());
        snprintf(channel, sizeof(channel), "%d", chid);
        char *args[] = {"/bin/pi3-runtime-test", "--exit-blocked", parent,
                        channel, NULL};
        pid_t pid;
        if (posix_spawn(&pid, args[0], NULL, NULL, args, environ)) {
            channel_destroy(chid);
            return 0;
        }
        int exiter = -1, held = -1;
        for (unsigned j = 0; j < 2; j++) {
            int packet[8] = {0};
            int rcvid = msg_receive(chid, packet, sizeof(packet), NULL);
            if (rcvid <= 0)
                return 0;
            if (packet[0] == 1)
                held = rcvid;
            else if (packet[0] == 2)
                exiter = rcvid;
        }
        if (held <= 0 || exiter <= 0 || msg_reply(exiter, 0, NULL, 0))
            return 0;
        int status = -1;
        int ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                 WEXITSTATUS(status) == 37;
        channel_destroy(chid);
        if (!ok)
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--fault")) {
        *(volatile unsigned long *) 0x1000 = 42;
        return 99;
    }
    if (argc == 4 && !strcmp(argv[1], "--exit-blocked")) {
        int coid = connect_attach(0, atoi(argv[2]), atoi(argv[3]),
                                   RTMUX_SIDE_CHANNEL, 0);
        pthread_t exiter;
        if (coid < 0 || pthread_create(&exiter, NULL, exit_sibling,
                                       (void *) (intptr_t) coid))
            return 98;
        int tag = 1;
        (void) msg_send(coid, &tag, sizeof(tag), NULL, 0);
        return 99;
    }
    jmp_buf jb;
    int jumped = setjmp(jb);
    if (!jumped)
        longjmp(jb, 42);
    CHECK("setjmp-longjmp", jumped == 42);

    struct timespec a, b, delay = {.tv_nsec = 20000000};
    int rc = clock_gettime(CLOCK_MONOTONIC, &a);
    rc |= nanosleep(&delay, NULL);
    rc |= clock_gettime(CLOCK_MONOTONIC, &b);
    int64_t elapsed =
        (b.tv_sec - a.tv_sec) * 1000000000LL + b.tv_nsec - a.tv_nsec;
    CHECK("monotonic-nanosleep", rc == 0 && elapsed >= 10000000);

    pthread_t threads[4];
    unsigned n = _syspage_ptr->num_cpu;
    CHECK("four-cpus", n == 4);
    if (n > 4)
        n = 4;
    for (unsigned cpu = 0; cpu < n; cpu++) {
        rc = pthread_create(&threads[cpu], NULL, worker,
                            (void *) (uintptr_t) cpu);
        CHECK("pthread-create", rc == 0);
        if (rc)
            return 1;
    }
    /* Also exercise joining workers after they have returned and roottask has
     * reclaimed their dynamically allocated stacks. */
    struct timespec reap_delay = {.tv_nsec = 100000000};
    nanosleep(&reap_delay, NULL);
    for (unsigned cpu = 0; cpu < n; cpu++) {
        void *result = (void *) -1;
        rc = pthread_join(threads[cpu], &result);
        printf("pi3-runtime: cpu=%u join=%d result=%lu\n", cpu, rc,
               (unsigned long) result);
        CHECK("cpu-affinity-simd-tls-join", rc == 0 && result == NULL);
    }

    int fd[2];
    char buf[16] = {0};
    rc = pipe(fd);
    CHECK("pipe-create", rc == 0);
    if (!rc) {
        CHECK("pipe-ipc", write(fd[1], "native-ipc", 10) == 10 &&
                              read(fd[0], buf, 10) == 10 &&
                              !memcmp(buf, "native-ipc", 10));
        close(fd[0]);
        close(fd[1]);
    }
    for (unsigned i = 0; i < 8; i++) {
        pid_t pid;
        char *args[] = {i & 1 ? "/bin/false" : "/bin/true", NULL};
        int status = -1;
        rc = posix_spawn(&pid, args[0], NULL, NULL, args, environ);
        CHECK("spawn-wait-exit", rc == 0 && waitpid(pid, &status, 0) == pid &&
                                     WIFEXITED(status) &&
                                     WEXITSTATUS(status) == (int) (i & 1));
    }
    CHECK("process-exit-unreplied-ipc", exit_with_unreplied_ipc());

    pid_t fault_pid;
    int fault_status = 0;
    char *fault_args[] = {"/bin/pi3-runtime-test", "--fault", NULL};
    rc =
        posix_spawn(&fault_pid, fault_args[0], NULL, NULL, fault_args, environ);
    CHECK("user-fault-isolation",
          rc == 0 && waitpid(fault_pid, &fault_status, 0) == fault_pid &&
              WIFSIGNALED(fault_status) && WTERMSIG(fault_status) == SIGSEGV);

    int packet[16] = {_SLOG_INFO | _SLOG_TEXTBIT, 0, 0};
    const char text[] = "Pi3 native runtime verified";
    memcpy(packet + _SLOG_HDRINTS, text, sizeof(text));
    int logfd = open("/dev/slog", O_WRONLY);
    size_t bytes = _SLOG_HDRINTS * sizeof(int) + sizeof(text);
    CHECK("logger-ipc",
          logfd >= 0 && write(logfd, packet, bytes) == (ssize_t) bytes);
    if (logfd >= 0)
        close(logfd);
    puts(failed ? "pi3-runtime: FAIL" : "pi3-runtime: PASS");
    return failed != 0;
}
