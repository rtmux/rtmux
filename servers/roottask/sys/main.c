/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <stdalign.h>
#include <startup.h>
#include <sys/rtmux-ident.h>
#include <sys/path-registry.h>
#include <sched.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "as.h"
#include "root-process.h"

/* Mark roottask as a regular ET_EXEC user-mode binary. The kernel boot path
 * spawns it as the first user process.
 */
RTMUX_IDENT(RTMUX_TYPE_EXEC, 0, 1);

/* Cross-boundary helper registration: kernel-side kext_*.c bodies call back
 * into roottask-side helpers via these pointers.
 */
extern void kext_register_mm_helpers(uintptr_t *va_rover_p,
                                     int (*ptem)(address_t *,
                                                 uintptr_t,
                                                 uintptr_t,
                                                 int,
                                                 path_object_t *,
                                                 paddr_t,
                                                 unsigned),
                                     struct pa_restrict **restrict_proc_pp,
                                     struct cpu_mm_as **pgtbl_list_pp);

/* pa allocator now lives in kernel/ext/pa.c — roottask accesses it through
 * __KSYS_PRIV_PA_* slots only. The cross-boundary registration shrank to just
 * restrict_proc / pgtbl_list (still roottask-side pointers, will move with
 * mm_pte / as_switch later).
 */
extern struct pa_restrict *restrict_proc;
extern struct cpu_mm_as *pgtbl_list;

/* Static stack for the heartbeat thread. A static (BSS) stack lets the thread
 * be created with __thread_create before the message pool is up, without the
 * mmap round-trip to VM that pthread_create would need (and that would
 * self-deadlock with no pool receiver yet).
 */
#define ROOT_HEARTBEAT_STACK_SIZE 8192
static alignas(16) char root_heartbeat_stack[ROOT_HEARTBEAT_STACK_SIZE];

static void sleep_msec(unsigned msec)
{
    struct timespec ts = {
        .tv_sec = msec / 1000,
        .tv_nsec = (long) (msec % 1000) * 1000000L,
    };

    (void) nanosleep(&ts, NULL);
}

/* Heartbeat thread.
 *
 * Once roottask is serving, emit a single '+' to the kernel console every five
 * seconds via the __KSYS_PRIV_HEARTBEAT privileged call — a simple liveness
 * signal that the PRIV path stays alive. Parameters:
 *   arg : unused
 * Return value: never returns.
 */
static void *root_heartbeat_thread(void *arg)
{
    (void) arg;
    for (;;) {
        sleep_msec(5000);
        (void) syscall_priv(__KSYS_PRIV_HEARTBEAT, NULL);
    }
    return NULL;
}

/**
 * Main function
 *
 * argc/argv are the boot-time arguments forwarded by the kernel
 * (kernel/init/roottask.c calls us with the kernel's own _argc/_argv). Today
 * only the -v family is recognized; argv may legitimately be NULL when the
 * kernel has no command line, in which case we leave roottask_loglevel at its
 * default of 0.
 */
int roottask_main(int argc, char **argv)
{
    /* Parse the boot command line for roottask's tunables. We accept the same
     * short options the kernel parses for kopt ("-v", "-F", "-u") so a single
     * boot command line drives both sides. kopt itself is kernel-only now;
     * roottask owns its own scalars.
     *
     *   -v / -vv / -vvv ...  → roottask_loglevel = number of 'v's
     *   -F <count> / -F<count>  → roottask_max_fds
     *   -u <umask> / -u<umask>  → roottask_procfs_umask (octal/auto)
     *
     * Any unrelated '-' arguments on the kernel command line are silently
     * ignored — the kernel sees them, roottask doesn't.
     */
    if (argv) {
        for (int i = 0; i < argc; i++) {
            const char *a = argv[i];
            if (!a || a[0] != '-' || a[1] == '\0')
                continue;

            /* -vvv: count consecutive v's, reject trailing junk. */
            if (a[1] == 'v') {
                int n = 0;
                for (const char *p = a + 1; *p == 'v'; p++)
                    n++;
                if (a[1 + n] != '\0')
                    continue;
                if (n > roottask_loglevel)
                    roottask_loglevel = n;
                continue;
            }

            /* -F<count> / -F <count> — max open files. */
            if (a[1] == 'F') {
                const char *val = (a[2] != '\0')   ? a + 2
                                  : (i + 1 < argc) ? argv[++i]
                                                   : NULL;
                if (val)
                    roottask_max_fds = (unsigned) strtoul(val, NULL, 0);
                continue;
            }

            /* -u<umask> / -u <umask> — umask used for /proc/<pid>/ entries. */
            if (a[1] == 'u') {
                const char *val = (a[2] != '\0')   ? a + 2
                                  : (i + 1 < argc) ? argv[++i]
                                                   : NULL;
                if (val)
                    roottask_procfs_umask = (mode_t) strtoul(val, NULL, 0);
                continue;
            }
        }
    }

    /* Resolve our own process_t pointer via a syscall_priv callback. The kernel
     * runs kext_self_prp in this thread's context and returns act->process —
     * works whether roottask shares the kernel as or runs U-mode with its own
     * as. No kernel global is ever dereferenced from roottask.
     */
    {
        roottask_prp = (process_t *) syscall_priv(__KSYS_PRIV_SELF_PRP, NULL);

        /* Claim roottask's own root_process[] slot. Subsequent path_init,
         * proc_dispatch_init etc. write to root_proc(ROOTTASK_PID)->{root,cwd,
         * events,wap,debug_waiting}, so the slot must exist before any of those
         * run. roottask is pid 1; it has no parent (parent_pid = 0).
         */
        root_process_init(ROOTTASK_PID, 0);
        root_proc(ROOTTASK_PID)->debug_name = strdup("roottask");

        /* Cache the adp pointer too. process_create's kcall_args adp_out
         * handles this for spawned children, but ROOTTASK_PID is created by the
         * kernel before roottask runs, so we have to fetch it explicitly here.
         * Without this, as_mmap on ROOTTASK_PID's own as passes adp=NULL into
         * pte_map, and the kernel-side cpu_pte_manipulate falls back to the
         * boot pgdir — wrong target.
         */
        root_proc(ROOTTASK_PID)->adp =
            (struct mm_as *) syscall_priv(__KSYS_PRIV_SELF_ADP, NULL);
    }

    /* Bump our priority to the roottask base (10) before creating any child
     * threads. We arrived here running on the CPU's idle thread (priority 0,
     * set by kernel/init/init.c); without this, every pool worker
     * pthread_create would inherit priority 0 and the whole roottask service
     * would run at idle priority. Roottask's base priority is 10r.
     */
    {
        struct sched_param sp = {.sched_priority = 10};
        (void) __sched_set(0, 0, SCHED_RR, &sp);
    }

    if (!elf_load_hook) {
        elf_load_hook = elf_load;
    }

    /* Hand kernel-side kernel extension bodies the few roottask pointers they
     * still call into (restrict_proc, pgtbl_list — to be migrated in a future
     * slice along with mm_pte / as_switch). pa.c is now fully kernel-resident,
     * so no pa_* registration needed.
     */
    kext_register_mm_helpers(&va_rover, pte_map, &restrict_proc, &pgtbl_list);

    /* One-shot fetch of LEGAL_CPU_BITMASK — kernel-built from hart_ids[] at SMP
     * bring-up; cached here for spawn-time runmask validation in proc_loader.
     */
    legal_cpu_bitmask =
        (unsigned) syscall_priv(__KSYS_PRIV_LEGAL_CPU_MASK, NULL);

    /* This is not debug printout: it's the greeting printed for the user */
    dbgprintf("\nRTMux User-Mode Roottask starting\n\n");

    /* Initialize message subsystem. This should be the first thing done. */
    root_log("roottask: message_init...\n");
    message_init();

    /* Start the hardware-resource service used for driver resource allocation.
     */
    root_log("roottask: hwres_init...\n");
    hwres_init();

    /* Initialize roottask configuration state. */
    root_log("roottask: conf init...\n");
    roottask_conf_init();

    /* Start pathname resolution service. */
    root_log("roottask: path_init...\n");
    path_init();

    /* Start /dev/null. */
    devnull_init();

    /* Start /dev/text (kernel SBI console, polling). */
    devtext_init();

    /* Start /dev/tty redirection to the controlling terminal. */
    devtty_init();

    /* Start /dev/std[in|out|err]. */
    devstd_init();

    /* Start VM service. It will install /dev/mem, /dev/zero and /dev/shmem/. */
    vm_init();

    /* Start process service. It will handle process commands (e.g. spawning,
     * sessions, terminating)
     */
    proc_dispatch_init();

    /* Start /proc for process inspection. */
    procfs_init();

    /* Start CPIO filesystem service at /rd. TODO: spawn /rd/sbin/init. */
    root_log("roottask: bootimage_init...\n");
    bootimage_init();

    /* Start named semaphore service at /dev/sem/. */
    root_log("roottask: namedsem_init...\n");
    namedsem_init();

    /* Everything is initialized; start the heartbeat thread before we begin
     * serving.
     */
    {
        struct _thread_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.stackaddr = root_heartbeat_stack;
        attr.stacksize = sizeof(root_heartbeat_stack);
        attr.flags = PTHREAD_CREATE_DETACHED;
        (void) __thread_create(0, root_heartbeat_thread, NULL, &attr);
    }

    /* Start message pool threads and wait until they are in msg_receive. */
    root_log("roottask: message_start...\n");
    return message_start();
}
