/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * syscall_testing — RTMux Kernel Call Conformance Test Suite
 *
 * Exercises kernel calls with both valid and invalid arguments, verifying
 * return values, errno, and side effects.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <semaphore.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <spawn.h>
#include <time.h>
#include <sys/dispatch.h>
#include <sys/elf.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

struct test_suite {
    const char *name;
    void (*run)(void);
};

static const struct test_suite test_suites[] = {
#define TEST_SUITE(token) {.name = #token, .run = test_##token}
#define TEST_SUITE_NAMED(label, token) {.name = (label), .run = test_##token}
    TEST_SUITE(timers),
    TEST_SUITE(sync),
    TEST_SUITE(printf),
    TEST_SUITE(setjmp),
    TEST_SUITE(signals),
    TEST_SUITE(notifications),
    TEST_SUITE(capgate),
    TEST_SUITE_NAMED("trace-negative", trace_negative),
    TEST_SUITE_NAMED("user-ptr-negative", user_ptr_negative),
    TEST_SUITE(vfs),
    TEST_SUITE_NAMED("posix-options", posix_options),
    TEST_SUITE(gauges),
#undef TEST_SUITE_NAMED
#undef TEST_SUITE
};

/* Test infrastructure */
static int tests_run;
static int tests_passed;
static int tests_failed;
static volatile uintptr_t fault_addr_seed = 1U;

#define FAULT_RECOVERY_CHECK(name, expr, detail) \
    check((expr), "fault-recovery " #name, (detail))

#define FAULT_RECOVERY_EVENT(event, severity, cause, action) \
    printf("fault-event event_id=%s severity=%s subject=self cause=%s " \
           "action=%s authority_delta=none\n", \
           (event), (severity), (cause), (action))

#define FAULT_RECOVERY_SPAWN_SIG(name, event, arg, signo)                  \
    do {                                                                   \
        char *name##_argv[] = {"syscall_testing", (char *) (arg), NULL};   \
        pid_t name##_pid;                                                  \
        int name##_status;                                                 \
        int name##_rc = posix_spawn(&name##_pid, "/bin/syscall_testing",   \
                                    NULL, NULL, name##_argv, NULL);        \
        if (name##_rc != EOK) {                                            \
            test_fail("fault-recovery " #name " spawn",                   \
                      strerror(name##_rc));                                \
        } else {                                                           \
            int name##_ok =                                               \
                waitpid(name##_pid, &name##_status, 0) == name##_pid &&    \
                WIFSIGNALED(name##_status) &&                              \
                WTERMSIG(name##_status) == (signo);                        \
            FAULT_RECOVERY_CHECK(                                          \
                name, name##_ok,                                           \
                "faulting child must be reaped with the documented signal"); \
            if (name##_ok)                                                 \
                FAULT_RECOVERY_EVENT((event), "thread-fatal", arg,         \
                                     "reported-signal");                   \
        }                                                                  \
    } while (0)

struct pthread_cancel_child_ctx {
    sem_t ready;
    sem_t release;
    sem_t block;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t target;
};

void test_ok(const char *name)
{
    tests_run++;
    tests_passed++;
    printf("  PASS  %s\n", name);
}

void test_fail(const char *name, const char *detail)
{
    tests_run++;
    tests_failed++;
    printf("  FAIL  %s: %s\n", name, detail);
}

void check(int cond, const char *name, const char *detail)
{
    if (cond)
        test_ok(name);
    else
        test_fail(name, detail);
}

static int join_must_be_canceled(pthread_t tid)
{
    void *ret = NULL;
    int rc = pthread_join(tid, &ret);

    return rc == EOK && ret == PTHREAD_CANCELED ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void *cancel_testcancel_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;

    sem_post(&ctx->ready);
    for (;;) {
        pthread_testcancel();
        (void) __sched_yield();
    }
    return NULL;
}

static void *cancel_disabled_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;

    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) != EOK)
        return (void *) 1;
    sem_post(&ctx->ready);
    (void) sem_wait(&ctx->release);
    if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != EOK)
        return (void *) 2;
    return (void *) 3;
}

static void *cancel_async_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;

    if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) != EOK)
        return (void *) 1;
    sem_post(&ctx->ready);
    for (;;)
        (void) __sched_yield();
    return NULL;
}

static void *cancel_sem_wait_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;

    sem_post(&ctx->ready);
    (void) sem_wait(&ctx->block);
    return (void *) 1;
}

static void *cancel_cond_wait_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;

    if (pthread_mutex_lock(&ctx->mutex) != EOK)
        return (void *) 1;
    sem_post(&ctx->ready);
    (void) pthread_cond_wait(&ctx->cond, &ctx->mutex);
    (void) pthread_mutex_unlock(&ctx->mutex);
    return (void *) 2;
}

static void *cancel_cond_timedwait_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return (void *) 1;
    ts.tv_sec += 30;
    if (pthread_mutex_lock(&ctx->mutex) != EOK)
        return (void *) 2;
    sem_post(&ctx->ready);
    (void) pthread_cond_timedwait(&ctx->cond, &ctx->mutex, &ts);
    (void) pthread_mutex_unlock(&ctx->mutex);
    return (void *) 3;
}

static void *cancel_nanosleep_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;
    struct timespec ts = {
        .tv_sec = 30,
        .tv_nsec = 0,
    };

    sem_post(&ctx->ready);
    (void) nanosleep(&ts, NULL);
    return (void *) 1;
}

static void *cancel_join_target_worker(void *arg)
{
    struct timespec ts = {
        .tv_sec = 30,
        .tv_nsec = 0,
    };

    (void) arg;
    (void) nanosleep(&ts, NULL);
    return NULL;
}

static void *cancel_join_waiter_worker(void *arg)
{
    struct pthread_cancel_child_ctx *ctx = arg;

    sem_post(&ctx->ready);
    (void) pthread_join(ctx->target, NULL);
    return (void *) 1;
}

static int cancel_child_common(void *(*worker)(void *), bool post_release)
{
    struct pthread_cancel_child_ctx ctx;
    pthread_t tid;
    int rc;

    if (sem_init(&ctx.ready, 0, 0) != 0 ||
        sem_init(&ctx.release, 0, 0) != 0 ||
        sem_init(&ctx.block, 0, 0) != 0 ||
        pthread_mutex_init(&ctx.mutex, NULL) != EOK ||
        pthread_cond_init(&ctx.cond, NULL) != EOK)
        return EXIT_FAILURE;

    rc = pthread_create(&tid, NULL, worker, &ctx);
    if (rc != EOK)
        return EXIT_FAILURE;
    (void) sem_wait(&ctx.ready);
    (void) nanosleep(&(struct timespec){.tv_nsec = 100000000L}, NULL);
    if (pthread_cancel(tid) != EOK)
        return EXIT_FAILURE;
    if (post_release)
        (void) sem_post(&ctx.release);
    return join_must_be_canceled(tid);
}

static int pthread_cancel_join_child(void)
{
    struct pthread_cancel_child_ctx ctx;
    pthread_t waiter;

    if (sem_init(&ctx.ready, 0, 0) != 0)
        return EXIT_FAILURE;
    if (pthread_create(&ctx.target, NULL, cancel_join_target_worker, NULL) !=
        EOK)
        return EXIT_FAILURE;
    if (pthread_create(&waiter, NULL, cancel_join_waiter_worker, &ctx) != EOK)
        return EXIT_FAILURE;
    (void) sem_wait(&ctx.ready);
    (void) nanosleep(&(struct timespec){.tv_nsec = 100000000L}, NULL);
    if (pthread_cancel(waiter) != EOK)
        return EXIT_FAILURE;
    if (join_must_be_canceled(waiter) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    (void) pthread_cancel(ctx.target);
    return join_must_be_canceled(ctx.target);
}

static int pthread_cancel_child(const char *name)
{
    if (!strcmp(name, "testcancel"))
        return cancel_child_common(cancel_testcancel_worker, false);
    if (!strcmp(name, "disabled-enable"))
        return cancel_child_common(cancel_disabled_worker, true);
    if (!strcmp(name, "async"))
        return cancel_child_common(cancel_async_worker, false);
    if (!strcmp(name, "sem-wait"))
        return cancel_child_common(cancel_sem_wait_worker, false);
    if (!strcmp(name, "cond-wait"))
        return cancel_child_common(cancel_cond_wait_worker, false);
    if (!strcmp(name, "cond-timedwait"))
        return cancel_child_common(cancel_cond_timedwait_worker, false);
    if (!strcmp(name, "nanosleep"))
        return cancel_child_common(cancel_nanosleep_worker, false);
    if (!strcmp(name, "join"))
        return pthread_cancel_join_child();
    return EXIT_FAILURE;
}

enum loader_mutation {
    LOADER_MUTATE_PHENTSIZE,
    LOADER_MUTATE_LOAD_SIZE,
    LOADER_MUTATE_LOAD_WX,
};

enum { LOADER_EXPECTED_ENOEXEC = 42 };

static int copy_loader_fixture(const char *dst)
{
    int in = open("/bin/true", O_RDONLY);

    if (in == -1)
        return errno;
    struct stat st;
    if (fstat(in, &st) != 0) {
        int err = errno;

        (void) close(in);
        return err;
    }
    (void) unlink(dst);
    int out = open(dst, O_CREAT | O_TRUNC | O_RDWR, 0700);
    if (out == -1) {
        int err = errno;

        (void) close(in);
        return err;
    }
    flock_t space = {
        .l_whence = SEEK_SET,
        .l_len = st.st_size,
    };
    if (fcntl(out, F_ALLOCSP, &space) == -1) {
        int err = errno;

        (void) close(in);
        (void) close(out);
        return err;
    }
    char *map = mmap(NULL, (size_t) st.st_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, out, 0);
    if (map == MAP_FAILED) {
        int err = errno;

        (void) close(in);
        (void) close(out);
        return err;
    }
    size_t done = 0;
    while (done < (size_t) st.st_size) {
        ssize_t n = read(in, map + done, (size_t) st.st_size - done);

        if (n < 0) {
            int err = errno;

            (void) munmap(map, (size_t) st.st_size);
            (void) close(in);
            (void) close(out);
            return err;
        }
        if (n == 0)
            break;
        done += (size_t) n;
    }
    if (done != (size_t) st.st_size) {
        (void) munmap(map, (size_t) st.st_size);
        (void) close(in);
        (void) close(out);
        return EIO;
    }
    (void) munmap(map, (size_t) st.st_size);
    (void) close(in);
    if (close(out) != 0)
        return errno;
    return EOK;
}

static int find_loader_pt_load(int fd, Elf64_Ehdr *ehdr, Elf64_Phdr *phdr,
                               off_t *offp)
{
    if (lseek(fd, 0, SEEK_SET) == (off_t) -1)
        return errno;
    if (read(fd, ehdr, sizeof(*ehdr)) != (ssize_t) sizeof(*ehdr))
        return EIO;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_phentsize != sizeof(*phdr))
        return ENOEXEC;

    for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
        off_t off = (off_t) ehdr->e_phoff + (off_t) i * ehdr->e_phentsize;

        if (lseek(fd, off, SEEK_SET) == (off_t) -1)
            return errno;
        if (read(fd, phdr, sizeof(*phdr)) != (ssize_t) sizeof(*phdr))
            return EIO;
        if (phdr->p_type == PT_LOAD) {
            *offp = off;
            return EOK;
        }
    }
    return ENOEXEC;
}

static int mutate_loader_fixture(const char *path, enum loader_mutation mutation)
{
    int fd = open(path, O_RDWR);

    if (fd == -1)
        return errno;
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;
    off_t phoff = 0;
    int status = find_loader_pt_load(fd, &ehdr, &phdr, &phoff);
    if (status != EOK) {
        (void) close(fd);
        return status;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        status = errno;
        (void) close(fd);
        return status;
    }
    char *map = mmap(NULL, (size_t) st.st_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        status = errno;
        (void) close(fd);
        return status;
    }

    switch (mutation) {
    case LOADER_MUTATE_PHENTSIZE:
        ehdr.e_phentsize = 0;
        memcpy(map + offsetof(Elf64_Ehdr, e_phentsize), &ehdr.e_phentsize,
               sizeof(ehdr.e_phentsize));
        break;
    case LOADER_MUTATE_LOAD_SIZE:
        phdr.p_filesz = 1;
        phdr.p_memsz = 0;
        memcpy(map + (size_t) phoff, &phdr, sizeof(phdr));
        break;
    case LOADER_MUTATE_LOAD_WX:
        phdr.p_flags |= PF_R | PF_W | PF_X;
        memcpy(map + (size_t) phoff, &phdr, sizeof(phdr));
        break;
    }

    (void) munmap(map, (size_t) st.st_size);
    if (close(fd) != 0 && status == EOK)
        status = errno;
    return status;
}

static void check_loader_rejects(const char *name,
                                 enum loader_mutation mutation)
{
    char path[64];

    (void) snprintf(path, sizeof(path), "/dev/shmem/%s", name);
    int status = copy_loader_fixture(path);
    if (status == EOK)
        status = mutate_loader_fixture(path, mutation);
    if (status != EOK) {
        test_fail(name, strerror(status));
        (void) unlink(path);
        return;
    }

    char *argv[] = {(char *) "bad-loader-image", NULL};
    pid_t pid;
    status = posix_spawn(&pid, path, NULL, NULL, argv, NULL);
    if (status == EOK) {
        int child_status;

        (void) waitpid(pid, &child_status, 0);
        test_fail(name, "malformed image executed");
    } else {
        check(status == ENOEXEC, name, "loader must reject malformed images");
    }
    (void) unlink(path);
}

static int loader_execvp_child(const char *name)
{
    char *argv[] = {(char *) name, NULL};

    if (setenv("PATH", "/dev/shmem", 1) != 0)
        return EXIT_FAILURE;
    errno = 0;
    (void) execvp(name, argv);
    return errno == ENOEXEC ? LOADER_EXPECTED_ENOEXEC : EXIT_FAILURE;
}

static int spawn_full_fd_child(void)
{
    /* Sized above the RTMux fd ceiling (_IOSERV_CLIENT_FD_MAX == 1024, default
     * _SC_OPEN_MAX 1000) so the fill loop is guaranteed to hit EMFILE on the
     * target. This test runs on RTMux, not the host — do not expect it to fill
     * a table whose RLIMIT_NOFILE exceeds this bound.
     */
    enum { FILL_FD_MAX = 1100 };
    posix_spawn_file_actions_t actions;
    char *argv[] = {"true", NULL};
    int fds[FILL_FD_MAX];
    unsigned nfds = 0;
    bool filled = false;
    pid_t pid = (pid_t) -1;
    int status = 0;
    int rc;

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        printf("spawn-full-fd: init rc=%d\n", rc);
        return EXIT_FAILURE;
    }

    for (unsigned i = 0; i < FILL_FD_MAX; i++) {
        int fd = open("/dev/null", O_RDONLY);

        if (fd == -1) {
            filled = errno == EMFILE;
            break;
        }
        fds[nfds++] = fd;
    }

    if (!filled || nfds == 0) {
        printf("spawn-full-fd: did not fill table nfds=%u errno=%d\n", nfds,
               errno);
        goto fail;
    }
    rc = posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (rc != EOK) {
        printf("spawn-full-fd: addclose rc=%d fd=%d nfds=%u\n", rc, fds[0],
               nfds);
        goto fail;
    }

    rc = posix_spawn(&pid, "/bin/true", &actions, NULL, argv, NULL);
    if (rc != EOK || pid <= 0) {
        printf("spawn-full-fd: spawn rc=%d pid=%d nfds=%u\n", rc, pid, nfds);
        goto fail;
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("spawn-full-fd: child status=0x%x nfds=%u\n", status, nfds);
        goto fail;
    }

    (void) posix_spawn_file_actions_destroy(&actions);
    while (nfds)
        (void) close(fds[--nfds]);
    return EXIT_SUCCESS;

fail:
    (void) posix_spawn_file_actions_destroy(&actions);
    while (nfds)
        (void) close(fds[--nfds]);
    return EXIT_FAILURE;
}

static void check_loader_execvp_rejects(const char *name,
                                        enum loader_mutation mutation)
{
    char path[64];

    (void) snprintf(path, sizeof(path), "/dev/shmem/%s", name);
    int status = copy_loader_fixture(path);
    if (status == EOK)
        status = mutate_loader_fixture(path, mutation);
    if (status != EOK) {
        test_fail(name, strerror(status));
        (void) unlink(path);
        return;
    }

    char *argv[] = {
        "syscall_testing",
        "--loader-execvp-child",
        (char *) name,
        NULL,
    };
    pid_t pid;
    status = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (status != EOK) {
        test_fail(name, strerror(status));
    } else {
        int child_status;

        status = waitpid(pid, &child_status, 0);
        check(status == pid && WIFEXITED(child_status) &&
                  WEXITSTATUS(child_status) == LOADER_EXPECTED_ENOEXEC,
              name, "execvp must reject malformed ELF before shell fallback");
    }
    (void) unlink(path);
}

/* Child half of the set-id loader test: run as uid 1000 and spawn the set-id
 * probes and their plain twin with LD_PRELOAD naming either a file that does
 * not exist or a real object (preload-probe.so). A loader that honours
 * LD_PRELOAD rejects the missing library before main() runs, and puts the real
 * one in the probe's symbol scope (status bit 5); a set-id image must ignore
 * the variable (secure mode) and run without it. Status bits are described in
 * setuid-probe.c.
 */
#define PRELOAD_BAD "LD_PRELOAD=/nonexistent/preload.so"
#define PRELOAD_REAL "LD_PRELOAD=/lib/preload-probe.so"
#define PROBE_PRELOADED 32

static int setid_probe_status(const char *path, const char *preload)
{
    char *env[] = {(char *) preload, NULL};
    char *env_none[] = {NULL};
    char *argv[] = {"probe", NULL};
    int status;
    pid_t pid;

    if (posix_spawn(&pid, path, NULL, NULL, argv, preload ? env : env_none) != 0 ||
        waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/* Child half: become (ruid, euid) with gid 1000, then spawn the root-owned
 * set-uid probe. Secure mode follows the spawner's REAL uid versus the image
 * owner (0): real != 0 means secure (LD_PRELOAD ignored, issetugid() true) even
 * when the effective uid is 0; real == 0 means a normal load that honours
 * LD_PRELOAD. The probe's effective uid is 0 either way. The gid side is the
 * root-owned set-gid probe: the spawner's gid 1000 differs from the owner's 0,
 * so it is always secure.
 */
static int setid_loader_child(uid_t ruid, uid_t euid)
{
    int st;

    if (setgid(1000) != 0)
        return 2;
    if (ruid == 1000 && euid == 1000) {
        if (setuid(1000) != 0)
            return 2;
    } else if (setreuid(ruid, euid) != 0) {
        return 2;
    }
    if (getuid() != ruid || geteuid() != euid)
        return 2;
    st = setid_probe_status("/bin/setuid-probe", PRELOAD_BAD);
    if (ruid != 0) {
        if (st != 8 + 2 + 4)
            return 3;
    } else if (st < 0 || (st & 8)) {
        return 4; /* real uid is the owner: LD_PRELOAD must be honoured */
    }
    /* real object: ignored when secure, in scope otherwise */
    st = setid_probe_status("/bin/setuid-probe", PRELOAD_REAL);
    if (st != (ruid != 0 ? 8 + 2 + 4 : 8 + 4 + PROBE_PRELOADED))
        return 8;
    if (setid_probe_status("/bin/setuid-probe", NULL) !=
        (ruid != 0 ? 8 + 2 + 4 : 8 + 4))
        return 5;
    if (ruid != 1000 || euid != 1000)
        return 0;
    /* plain, no preload: runs, nothing set-id */
    if (setid_probe_status("/bin/setuid-probe-plain", NULL) != 8)
        return 6;
    /* plain, real preload: honoured, and its libc reference resolves */
    if (setid_probe_status("/bin/setuid-probe-plain", PRELOAD_REAL) !=
        8 + PROBE_PRELOADED)
        return 9;
    /* set-gid: secure by gid alone, so the preload is ignored; egid is 0 */
    st = setid_probe_status("/bin/setgid-probe", PRELOAD_REAL);
    if (st < 0 || (st & (8 + 2 + 16 + PROBE_PRELOADED)) != 8 + 2 + 16)
        return 10;
    /* plain, bad preload: the loader honours it and refuses to start main() */
    return setid_probe_status("/bin/setuid-probe-plain", PRELOAD_BAD) == 0 ? 0
                                                                           : 7;
}

static void test_setid_loader(void)
{
    static const struct {
        const char *ids;
        const char *name;
    } cases[] = {
        {"1000 1000", "loader-negative setid ruid=euid=1000"},
        {"1000 0", "loader-negative setid ruid=1000 euid=0"},
        {"0 1000", "loader-negative setid ruid=0 euid=1000"},
        {"0 0", "loader-negative setid ruid=euid=0"},
    };
    char buf[160];

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *argv[] = {"syscall_testing", "--setid-loader-child", NULL, NULL,
                        NULL};
        char ids[16];
        char *sp;
        int status = -1;
        pid_t pid;

        snprintf(ids, sizeof ids, "%s", cases[i].ids);
        sp = strchr(ids, ' ');
        *sp = '\0';
        argv[2] = ids;
        argv[3] = sp + 1;
        if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv,
                        NULL) != 0) {
            test_fail(cases[i].name, strerror(errno));
            continue;
        }
        snprintf(buf, sizeof buf,
                 "set-id secure mode must follow the real uid, plain images "
                 "must honour LD_PRELOAD (child rc %d)",
                 (waitpid(pid, &status, 0) == pid && WIFEXITED(status))
                     ? WEXITSTATUS(status)
                     : -1);
        check(status == 0, cases[i].name, buf);
    }
}

static void test_loader_negative_subset(void)
{
    check_loader_rejects("loader-negative malformed-phdr",
                         LOADER_MUTATE_PHENTSIZE);
    check_loader_execvp_rejects("loader-negative execvp-phdr",
                                LOADER_MUTATE_PHENTSIZE);
    check_loader_rejects("loader-negative bad-size", LOADER_MUTATE_LOAD_SIZE);
    check_loader_rejects("loader-negative writable-exec",
                         LOADER_MUTATE_LOAD_WX);
    test_setid_loader();
}

static void test_fault_recovery_subset(void)
{
    int msg = 0;
    int reply = 0;
    int rc;

    rc = syscall_priv(999, NULL) == -EPERM;
    FAULT_RECOVERY_CHECK(illegal_syscall, rc,
                         "unsupported private calls must fail closed");
    if (rc)
        FAULT_RECOVERY_EVENT("illegal-syscall", "deny",
                             "unsupported-private-call", "return-EPERM");

    errno = 0;
    rc = msg_send_cap(0x100000000ULL, &msg, sizeof(msg), &reply,
                      sizeof(reply));
    rc = rc == -1 && errno == EBADF;
    FAULT_RECOVERY_CHECK(invalid_capability, rc,
                         "malformed endpoint caps must not authorize IPC");
    if (rc)
        FAULT_RECOVERY_EVENT("invalid-capability", "deny", "malformed-cap",
                             "return-EBADF");

    FAULT_RECOVERY_SPAWN_SIG(user_page_fault, "user-page-fault",
                             "--fault-sigsegv", SIGSEGV);
    FAULT_RECOVERY_SPAWN_SIG(readonly_write_fault, "user-page-fault",
                             "--fault-mprotect-ro", SIGSEGV);
}

/* --laps N: rerun every suite N times in one boot. Each lap resets the
 * per-lap counters, and leak_lap_check() compares the live kernel gauges with
 * their lap-1 values; the cumulative failure count decides the exit status.
 */
static int run_laps(int laps)
{
    int total_failed = 0;

    for (int lap = 1; lap <= laps; lap++) {
        int grown;

        tests_run = tests_passed = tests_failed = 0;
        printf("lap=%d BEGIN\n", lap);
        for (size_t i = 0; i < sizeof test_suites / sizeof test_suites[0]; i++) {
            printf("%s[%s]\n", i == 0 ? "" : "\n", test_suites[i].name);
            test_suites[i].run();
            leak_suite_delta(lap, test_suites[i].name);
        }
        grown = leak_lap_check(lap);
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n", tests_run,
               tests_passed, tests_failed);
        total_failed += tests_failed + grown;
        printf("lap=%d %s\n", lap, tests_failed || grown ? "FAIL" : "PASS");
    }
    printf("laps=%d cumulative_failures=%d\n", laps, total_failed);
    return total_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    size_t i;

    if (argc == 3 && !strcmp(argv[1], "--pthread-cancel-child"))
        return pthread_cancel_child(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--killspawn-child"))
        return killspawn_child(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--rlimit-cpu-child"))
        return rlimit_cpu_child();
    if (argc == 2 && !strcmp(argv[1], "--rlimit-inherit-child"))
        return rlimit_inherit_child();
    if (argc == 2 && !strcmp(argv[1], "--rlimit-inherit-grandchild"))
        return rlimit_inherit_grandchild();
    if (argc >= 2 && !strncmp(argv[1], "--gauge-", 8))
        return gauge_child(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--fp-isolation-child"))
        return fp_isolation_child();
    if (argc == 2 && !strcmp(argv[1], "--cancel-tls-child"))
        return cancel_tls_child();
    if (argc == 4 && !strcmp(argv[1], "--setid-loader-child"))
        return setid_loader_child((uid_t) atoi(argv[2]), (uid_t) atoi(argv[3]));
    if (argc >= 2 && !strncmp(argv[1], "--kill-", 7))
        return kill_child(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--nvme-raw-child"))
        return nvme_raw_child();
    if (argc == 2 && !strcmp(argv[1], "--sys-log-child"))
        return sys_log_child();
    if (argc == 2 && !strcmp(argv[1], "--sys-log-wrap")) {
        printf("=== RTMux /sys/log Wrap Test ===\n\n[sys_log_wrap]\n");
        test_sys_log_wrap();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--posix-cap-boundary-child"))
        return posix_cap_boundary_child();
    if (argc == 2 && !strcmp(argv[1], "--posix-cap-boundary")) {
        printf("=== RTMux POSIX Capability Boundary Test ===\n\n");
        printf("[posix-cap-boundary]\n");
        test_posix_cap_boundary();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 4 && !strcmp(argv[1], "--disconnect-child"))
        return disconnect_child(argv[2], argv[3]);
    if (argc == 2 && !strcmp(argv[1], "--disconnect-alloc-failure")) {
        int skipped;

        printf("[disconnect_under_alloc_failure]\n");
        skipped = disconnect_under_alloc_failure() == DISCONNECT_SKIP;
        if (skipped)
            printf("  FAIL  disconnect_under_alloc_failure: fault injection "
                   "unavailable\n");
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed || skipped || !tests_passed ? EXIT_FAILURE
                                                         : EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--serial-unblock-child"))
        return serial_unblock_child(!strcmp(argv[2], "announce"));
    if (argc == 2 && !strcmp(argv[1], "--serial-unblock")) {
        printf("=== RTMux Serial Unblock Test ===\n\n");
        test_serial_unblock();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--spawn-storm")) {
        printf("=== RTMux Concurrent Spawn Storm ===\n\n[spawn-storm]\n");
        test_spawn_concurrent(1);
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--trace-negative")) {
        printf("=== RTMux Trace Negative Test ===\n\n[trace-negative]\n");
        test_trace_negative();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--fault-recovery")) {
        printf("=== RTMux Fault Recovery Test ===\n\n");
        printf("[fault-recovery]\n");
        test_fault_recovery_subset();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--loader-execvp-child"))
        return loader_execvp_child(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--spawn-echo-child"))
        return spawn_echo_child(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--spawn-full-fd-child"))
        return spawn_full_fd_child();
    if (argc == 2 && !strcmp(argv[1], "--loader-negative")) {
        printf("=== RTMux Loader Negative Test ===\n\n");
        printf("[loader-negative]\n");
        test_loader_negative_subset();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
               tests_run, tests_passed, tests_failed);
        return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--connect-gated")) {
        /* Child fixture for test_capgate: a fresh process holding no gating cap
         * must be refused a name-only connect to a gated global channel.
         */
        return capgate_child_connect(argv[2]);
    }
    if (argc >= 2 && !strcmp(argv[1], "--dup-attack"))
        return capgate_child_dup_attack(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "--dup-replay-child"))
        return capgate_child_dup_replay(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--pi-large-pid"))
        return sync_pi_large_pid_child();
    if (argc == 2 && !strcmp(argv[1], "--proc-cap-child")) {
        /* Child fixture for test_proc_cap_spawn: a plain child that exits, so
         * the parent can check it received a process cap in the spawn reply.
         */
        return EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--proc-cap-hold")) {
        /* Child fixture for test_proc_cap_thread_authorized: stay alive on a
         * named semaphore so the parent has a live target to authorize a
         * cross-process thread create against, then exit when released.
         */
        sem_t *ready = sem_open("/proccap-ready", 0);
        sem_t *release = sem_open("/proccap-release", 0);
        if (ready == SEM_FAILED || release == SEM_FAILED)
            return EXIT_FAILURE;
        sem_post(ready);
        sem_wait(release);
        sem_close(ready);
        sem_close(release);
        return EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--expect-fd-closed")) {
        struct _server_info info;
        int fd = atoi(argv[2]);

        return connect_server_info(0, (uint64_t) fd, &info) == fd
                   ? EXIT_FAILURE
                   : EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--expect-fd-readable")) {
        char byte;
        int fd = atoi(argv[2]);

        return read(fd, &byte, 1) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 4 && !strcmp(argv[1], "--expect-fd-readable-closed")) {
        struct _server_info info;
        char byte;
        int rfd = atoi(argv[2]);
        int cfd = atoi(argv[3]);

        return read(rfd, &byte, 1) == 1 &&
                       connect_server_info(0, (uint64_t) cfd, &info) != cfd
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (argc == 4 && !strcmp(argv[1], "--expect-inherited-atfd")) {
        struct stat st;
        int dfd = atoi(argv[2]);
        const char *name = argv[3];
        int fd;

        if (fstatat(dfd, name, &st, 0) != 0)
            return EXIT_FAILURE;
        if (faccessat(dfd, name, F_OK, 0) != 0)
            return EXIT_FAILURE;
        fd = openat(dfd, name, O_RDONLY);
        if (fd == -1)
            return EXIT_FAILURE;
        return close(fd) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 4 && !strcmp(argv[1], "--selftest-inherited-atfd")) {
        char fdarg[16];
        char *child_argv[] = {
            "syscall_testing",
            "--expect-inherited-atfd",
            fdarg,
            argv[3],
            NULL,
        };
        pid_t pid;
        int status;
        int dfd;
        int rc;

        dfd = open(argv[2], O_RDONLY);
        if (dfd == -1)
            return EXIT_FAILURE;
        (void) snprintf(fdarg, sizeof fdarg, "%d", dfd);
        rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, child_argv,
                         NULL);
        if (rc != EOK) {
            (void) close(dfd);
            return EXIT_FAILURE;
        }
        rc = waitpid(pid, &status, 0) == pid && WIFEXITED(status)
                 ? WEXITSTATUS(status)
                 : EXIT_FAILURE;
        (void) close(dfd);
        return rc;
    }
    if (argc == 4 && !strcmp(argv[1], "--send-forged-pid")) {
        pid_t claimed = (pid_t) atoi(argv[3]);
        int reply = -1;
        int fd = name_open(argv[2], 0);

        if (fd == -1)
            return EXIT_FAILURE;
        if (msg_send(fd, &claimed, sizeof(claimed), &reply, sizeof(reply)) ==
            -1) {
            (void) name_close(fd);
            return EXIT_FAILURE;
        }
        (void) name_close(fd);
        return reply == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && !strcmp(argv[1], "--fault-sigsegv")) {
        uintptr_t addr = fault_addr_seed;
        volatile uint32_t *bad = (volatile uint32_t *) addr;

        *bad = 0x51525354U;
        return EXIT_FAILURE;
    }
    if (argc == 2 && (!strcmp(argv[1], "--fault-mprotect-ro") ||
                      !strcmp(argv[1], "--mprotect-ro-readable"))) {
        /* Two halves of the page-isolation proof dev-nvme relies on to make its
         * DMA completion queues host-read-only once filled. Both map a page
         * writable with the driver's flags, fill it, and flip it read-only.
         *
         * --mprotect-ro-readable is the positive control: it then reads the
         * page back and exits 0, proving the map, the fill write, the mprotect,
         * and the post-flip read all succeed. The parent requires a clean exit.
         *
         * --fault-mprotect-ro is the negative case: it writes the read-only
         * page, which must fault. Because the positive control proves every
         * step up to the final write succeeds, a SIGSEGV here is attributable
         * to that write and not to an earlier faulting step. A clean exit here
         * (40 mprotect failed, 41 map failed, 43 write not rejected) is a
         * failure.
         */
        int expect_fault = !strcmp(argv[1], "--fault-mprotect-ro");
        volatile uint32_t *p =
            mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                 MAP_ANON | MAP_SHARED, NOFD, 0);
        if (p == MAP_FAILED)
            _exit(41);
        p[0] = 0x9e3779b9U;
        if (mprotect((void *) p, 4096, PROT_READ | PROT_NOCACHE) == -1)
            _exit(40);
        if (!expect_fault)
            _exit(p[0] == 0x9e3779b9U ? 0 : 42);
        p[0] = 0xdeadbeefU; /* read-only now: this store must fault */
        _exit(43);
    }
    if (argc == 3 && !strcmp(argv[1], "--exit-code")) {
        /* _exit terminates immediately with the exact status, bypassing the
         * stdio flush and atexit handlers that exit() would run.
         */
        _exit(atoi(argv[2]));
    }
    if (argc == 2 && !strcmp(argv[1], "--expect-resetids")) {
        return getuid() == geteuid() && getgid() == getegid() ? EXIT_SUCCESS
                                                              : EXIT_FAILURE;
    }
    if (argc == 3 && !strcmp(argv[1], "--flock-hold")) {
        /* Acquire a write lock on bytes [0,100) of argv[2], then block on a
         * named semaphore (reliable, unlike a timed sleep in a spawned child)
         * until the parent has probed the conflict and posts the release, then
         * exit (dropping the lock). "/flock-locked" tells the parent we hold
         * it; "/flock-release" is the parent's go-ahead.
         */
        struct flock fl = {.l_type = F_WRLCK,
                           .l_whence = SEEK_SET,
                           .l_start = 0,
                           .l_len = 100};
        sem_t *locked = sem_open("/flock-locked", O_CREAT, 0600, 0);
        sem_t *release = sem_open("/flock-release", O_CREAT, 0600, 0);
        int fd = open(argv[2], O_RDWR);

        if (fd == -1 || locked == SEM_FAILED || release == SEM_FAILED)
            _exit(2);
        if (fcntl(fd, F_SETLK, &fl) != 0)
            _exit(3);
        (void) sem_post(locked);
        while (sem_wait(release) == -1 && errno == EINTR)
            ;
        _exit(0);
    }
    if (argc == 3 && !strcmp(argv[1], "--flock-setlkw-wait")) {
        /* Block in F_SETLKW on bytes [0,100) of argv[2] until the holder drops
         * it, then post "/flock-acquired" and exit. Proves the server blocks
         * the request (rather than failing it) and later wakes and grants it.
         */
        struct flock fl = {.l_type = F_WRLCK,
                           .l_whence = SEEK_SET,
                           .l_start = 0,
                           .l_len = 100};
        sem_t *acquired = sem_open("/flock-acquired", O_CREAT, 0600, 0);
        int fd = open(argv[2], O_RDWR);

        if (fd == -1 || acquired == SEM_FAILED)
            _exit(2);
        if (fcntl(fd, F_SETLKW, &fl) != 0)
            _exit(3);
        (void) sem_post(acquired);
        _exit(0);
    }
    if (argc == 2 && !strcmp(argv[1], "--munmap-stale-read")) {
        enum { MAP_LEN = 4096 };
        volatile unsigned char *p;
        int fd = open("/dev/zero", O_RDONLY);

        if (fd == -1)
            return EXIT_FAILURE;
        p = mmap(NULL, MAP_LEN, PROT_READ, MAP_SHARED, fd, 0);
        (void) close(fd);
        if (p == MAP_FAILED)
            return EXIT_FAILURE;
        if (munmap((void *) p, MAP_LEN) == -1)
            return EXIT_FAILURE;
        (void) p[0];
        return EXIT_FAILURE;
    }
    if (argc == 4 && !strcmp(argv[1], "--pshared-count")) {
        long n = atol(argv[3]);
        int fd = shm_open(argv[2], O_RDWR, 0600);
        struct pshared_region *r;

        if (fd == -1)
            return EXIT_FAILURE;
        r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        (void) close(fd);
        if (r == MAP_FAILED)
            return EXIT_FAILURE;
        for (long i = 0; i < n; i++) {
            if (pthread_mutex_lock(&r->mtx) != 0) {
                (void) munmap(r, sizeof *r);
                return EXIT_FAILURE;
            }
            r->counter++;
            (void) pthread_mutex_unlock(&r->mtx);
        }
        (void) munmap(r, sizeof *r);
        return EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--pshared-cond-wait")) {
        int fd = shm_open(argv[2], O_RDWR, 0600);
        struct pshared_region *r;
        struct timespec ts;
        int rc = EXIT_FAILURE;

        if (fd == -1)
            return EXIT_FAILURE;
        r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        (void) close(fd);
        if (r == MAP_FAILED)
            return EXIT_FAILURE;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
            pthread_mutex_lock(&r->mtx) == 0) {
            ts.tv_sec += 5;
            r->state = 1;
            while (r->state == 1 &&
                   pthread_cond_timedwait(&r->cv, &r->mtx, &ts) == 0)
                ;
            if (r->state == 2) {
                r->state = 3;
                rc = EXIT_SUCCESS;
            }
            (void) pthread_mutex_unlock(&r->mtx);
        }
        (void) munmap(r, sizeof *r);
        return rc;
    }
    if (argc == 2 && !strcmp(argv[1], "--gauges")) { /* opt-in: just the gauge regressions */
        printf("=== RTMux gauge regressions ===\n\n[gauges]\n");
        test_gauges();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n", tests_run,
               tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--orphan-send")) { /* opt-in reproducer, see leak.c */
        printf("=== RTMux orphan-send reproducer ===\n\n[gauges]\n");
        test_orphan_send_kill();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n", tests_run,
               tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2 && !strcmp(argv[1], "--killspawn")) { /* opt-in: not in test_suites[] */
        printf("=== RTMux Leak Gate (killspawn) ===\n\n[leak]\n");
        test_leak();
        printf("\n=== Results: %d tests, %d passed, %d failed ===\n", tests_run,
               tests_passed, tests_failed);
        return tests_failed || !tests_passed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--laps") && atoi(argv[2]) > 0) {
        printf("=== RTMux Syscall Conformance Test Suite ===\n\n");
        return run_laps(atoi(argv[2]));
    }
    printf("=== RTMux Syscall Conformance Test Suite ===\n\n");

    for (i = 0; i < sizeof test_suites / sizeof test_suites[0]; i++) {
        printf("%s[%s]\n", i == 0 ? "" : "\n", test_suites[i].name);
        test_suites[i].run();
    }

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n", tests_run,
           tests_passed, tests_failed);

    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
