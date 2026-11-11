/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/cpio.h>
#include <sys/storage.h>
#include <cpu-inline.h>
#include <unistd.h>

#include <module/initramfs-elf.h>

/* Initramfs (CPIO initrd) base and size, populated during init */
const uint8_t *initramfs_base;
size_t initramfs_size;

/* Duplicate a NUL-terminated string into kernel heap memory. Parameters:
 *   s : source string to copy.
 * Return value: pointer to the copy, or NULL if allocation failed.
 */
char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *d = _smalloc(len);
    if (d)
        memcpy(d, s, len);
    return d;
}

/* cpio_iterate callback that logs each non-empty initramfs file. Parameters:
 *   info : the current CPIO file's metadata.
 *   data : unused callback context.
 */
static void initramfs_list_cb(const cpio_file_info_t *info, void *data)
{
    (void) data;
    if (info->filesize == 0)
        return;
    LOGF_D("  initramfs: %s (%u bytes)\n", info->filename, info->filesize);
}

/* walk_asinfo callback that records the ramdisk's base and size into
 * initramfs_base / initramfs_size. Parameters:
 *   as   : the matching asinfo entry (its start/end bound the ramdisk).
 *   name : unused entry name.
 *   data : unused callback context.
 * Return value: 0 to stop the walk after the first match.
 */
static int initramfs_find_ramdisk(struct asinfo_entry *as,
                                  char *name,
                                  void *data)
{
    (void) name;
    (void) data;
    initramfs_base = (const uint8_t *) PHYS_TO_PTR(as->start);
    initramfs_size = (size_t) (as->end - as->start + 1);
    return 0; /* stop after first match */
}

/* Locate, validate, and load the initramfs archive (CPIO initrd).
 *
 * Finds the ramdisk asinfo entry published by startup, verifies it is a valid
 * CPIO archive, lists its contents, then parses the ELF modules and loads the
 * executables. Panics if no valid initramfs is present.
 */
static void initramfs_init(void)
{
    LOGF_D("kernel_init: initramfs_init\n");

    /* Find the ramdisk asinfo entry populated by startup */
    walk_asinfo("ramdisk", initramfs_find_ramdisk, NULL);

    if (!initramfs_base || initramfs_size == 0) {
        kprintf(
            "FATAL: initramfs not found (no initrd/ramdisk passed to "
            "kernel)\n");
        crash();
    }

    if (!cpio_check_valid(initramfs_base, initramfs_size)) {
        kprintf("FATAL: initramfs at %p is not a valid CPIO archive\n",
                initramfs_base);
        crash();
    }

    LOGF_D("initramfs: valid CPIO at %p, %lu bytes\n", initramfs_base,
           (unsigned long) initramfs_size);
    cpio_iterate(initramfs_base, initramfs_size, initramfs_list_cb, NULL);

    /* Parse ELF modules and load executables */
    initramfs_load_modules(initramfs_base, initramfs_size);
}

/* Top-level kernel initialization, called from kernel_entry().
 *
 * Brings up every kernel subsystem in order — VM hooks, traps, heap, threads,
 * core objects, initramfs, timers, interrupts, per-CPU and PLIC state — saves
 * the roottask arguments and environment, optionally arms the tracelogger, then
 * calls ksys_start() (which returns through the kernel with idle() running).
 * Parameters:
 *   argc : argument count from the boot command line.
 *   argv : argument vector; trailing args are forwarded to roottask.
 *   env  : environment vector forwarded to roottask.
 */
void kernel_init(int argc, char *argv[], char *env[])
{
    int i;
    ulong_t n;

    extern int cpunum_ready;

    /* boot_cpunum, hart_ids[], and sscratch are already set by syspage_init in
     * entry.c.
     */
    cpunum_ready = 1;

    /* Stamp this CPU as "doing kernel work" so the timer-trap routing takes its
     * minimal kernel-self path during boot. The stamp is closed out by the
     * first kernel_exit on this CPU's path (in idle() below, which calls
     * cpu_idle which eventually kernel_exit's into the first dispatched thread;
     * kernel_exit decrements in_kcall before __kernel_restore).
     */
    in_kcall_inc();

    LOGF_D("kernel_init: entered\n");

    init_vm_ops();
    init_traps();
    mm_init_mem(0);
    LOGF_D("kernel_init: heap_init(%lu)\n", (unsigned long) kopt.heap_pregrow);
    heap_init(kopt.heap_pregrow);
    LOGF_D("kernel_init: thread_init\n");
    thread_init();
    LOGF_D("kernel_init: init_objects\n");
    init_objects();

    /* tp is invariant per hart — already &pcpu_array[cpu] from startup_entry;
     * do NOT override it.
     *
     * cpupage->tls is still set so userland views of the syspage see the idle
     * thread's TLS pointer.
     */
    {
        unsigned cpu = get_cpunum();
        struct _thread_local_storage *real_tls =
            get_active_thread(cpu)->un.lcl.tls;
        cpupageptr[cpu]->tls = real_tls;
    }

    initramfs_init();
    LOGF_D("kernel_init: timer_init\n");
    timer_init();
    LOGF_D("kernel_init: interrupt_init\n");
    interrupt_init();
    LOGF_D("kernel_init: init_cpu\n");
    init_cpu();

    /* Initialize PLIC: set all priorities and per-hart thresholds. Must be
     * after init_cpu() which sets up the interrupt tables.
     */
    {
        extern void plic_init(void);
        extern void plic_inithart(void);
        plic_init();
        plic_inithart();
    }

    /* Remaining arguments are for roottask, save them in malloc'd storage */
    argc -= (optind - 1);
    argv += (optind - 1);
    _argv = _smalloc(((unsigned) argc + 1) * sizeof(*argv));
    for (i = 1; i < argc; ++i) {
        _argv[i] = strdup(argv[i]);
    }
    _argv[0] = strdup(argv[1 - optind]);
    _argv[i] = NULL;
    _argc = argc;
    for (n = 0; env[n]; ++n) {
        /* nothing to do */
    }
    environ = _smalloc((n + 1) * sizeof(*environ));
    for (i = 0; i < n; ++i) {
        environ[i] = strdup(env[i]);
    }
    environ[i] = NULL;
    optind = -1;

    /* If '-T' was given, arm the tracelogger now — every kernel event from here
     * on will land in the ring buffer, and a crash will dump it via
     * tr_dump_to_console().
     */
    if (kopt.auto_trace) {
        extern void tr_autostart(void);
        tr_autostart();
    }

    /* This will return through the kernel and idle will be running! */

    LOGF_D("kernel_init: ksys_start\n");
    set_inkernel_init(0);
    ksys_start();
}
