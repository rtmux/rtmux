/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <startup.h>
#include <asm/boot-stack.h>
#include <minmax.h>
#include <startup-api.h>
#include <rtmux-version.h>

#define STARTUP_DEBUG_LEVEL 4

/* The kernel object heap and the VM physical allocator own separate RAM. */
#define BOOT_HEAP_MIN MEG(8)
paddr_t boot_heap_paddr;
size_t boot_heap_size;

/* main() Startup program executing out of RAM
 *
 * 1. It gathers information about the system and places it in a structure
 *    called the system page. The kernel references this structure to
 *    determine everything it needs to know about the system. This structure
 *    is also available to user programs (read only if protection is on)
 *    via _syspage->.
 *
 * 2. It (optionally) turns on the MMU and starts the next program
 *    in the image file system.
 */
/**
 * \brief Pre-scan argv for --long-options before getopt.
 *
 * getopt only handles single-char options. This function extracts recognized
 * long options and removes them from argv so they don't confuse getopt.
 */
static void prescan_long_opts(int *argc, char **argv)
{
    int src, dst;

    for (src = 1, dst = 1; src < *argc; src++) {
        if (!strncmp(argv[src], "--limit-ram=", 12)) {
            char *end;
            kopt.ram_limit = getsize(argv[src] + 12, &end);
            /* Skip this arg — don't copy to dst */
        } else if (!strcmp(argv[src], "--disable-timer")) {
            kopt.disable_timer = true;
            /* Skip this arg — don't copy to dst */
        } else {
            argv[dst++] = argv[src];
        }
    }
    argv[dst] = NULL;
    *argc = dst;
}

int hardware_init(int argc, char **argv, char **envv)
{
    int opt;

    prescan_long_opts(&argc, argv);

    /* Keep the externally-visible _argc consistent with the stripped argv so
     * kernel_init() does not walk past the NULL terminator when it strdup()'s
     * each remaining argument.
     */
    _argc = argc;

    while ((opt = getopt(argc, argv, COMMON_OPTIONS_STRING "d")) != -1) {
        switch (opt) {
        default:
            handle_common_option(opt);
            break;
        }
    }

    /* Initialize debugging output. kprintf will not work before this point. */
    arch_init_debug();

    {
        const char *banner = "RTMux version " RTMUX_VERSION_STRING;
        size_t len = strlen(banner) + 4; /* "| " + banner + " |" */
        kprintf("+");
        for (size_t i = 0; i < len - 2; i++)
            kprintf("-");
        kprintf("+\n| %s |\n+", banner);
        for (size_t i = 0; i < len - 2; i++)
            kprintf("-");
        kprintf("+\n");
    }

    /* Initialize clocks and RAM info */
    init_clocks();

    init_raminfo();

    if (kopt.ram_limit)
        kprintf("--limit-ram: %lu MiB\n",
                (unsigned long) (kopt.ram_limit / (1024 * 1024)));

    /* Timebase frequency (FDT cpus/timebase-frequency) */
    if (kopt.timebase_freq == 0) {
        LOGF_W("kopt.timebase_freq = 0\n");
    }

    /* Claim the RAM used by kernel + startup stack (above _end) */
    alloc_ram((paddr_t) _start, (paddr_t) (_end - _start) + BOOT_STACK_SIZE, 1);
    if (initramfs_phys_start)
        alloc_ram(initramfs_phys_start,
                  initramfs_phys_end - initramfs_phys_start, 1);

    /* Pre-allocate a heap region for the kernel to use before as (pa_init)
     * takes over. calloc_ram removes this region from the ram_list, so it won't
     * appear in sysram asinfo entries — preventing pa_init from double-managing
     * the same memory.
     */
    /* The original 8 MiB reserve covers the default 256 MiB machine.
     * Larger systems also need kernel objects and page tables for their
     * residents. Try the same fraction of available RAM there, retaining
     * the original minimum on small or fragmented machines.
     */
    boot_heap_size = max_size(BOOT_HEAP_MIN, TRUNC(ram_bytes() / 32, __PAGESIZE));
    for (;;) {
        boot_heap_paddr = calloc_ram(boot_heap_size, __PAGESIZE);
        if (boot_heap_paddr != NULL_PADDR)
            break;
        if (boot_heap_size == BOOT_HEAP_MIN)
            crash("failed to allocate boot heap\n");
        boot_heap_size = max_size(BOOT_HEAP_MIN, TRUNC(boot_heap_size / 2, __PAGESIZE));
    }

    /* Initialize CPU sections in the syspage. This must be done before
     * init_mmu().
     */
    init_smp();

    /* Read the RTC before the MMU is enabled — the RTC's physical address is
     * accessed directly (VA == PA).
     */
    read_rtc_boot_time();

    /* Initialize the MMU: build page tables and enable paging. The kernel is
     * identity-mapped (VA == PA), so execution continues seamlessly after the
     * switch.
     */
    init_mmu();

    init_intrinfo();

    init_qtime();

    init_cpuinfo();

    init_hwinfo();

    add_typed_string(_CS_MACHINE, ARCH_MACHINE_STRING);

    /* Publish the kernel identity for uname() and roottask's skew check. This
     * must precede init_system_private(), which freezes the syspage layout.
     */
    set_syspage_build_info(RTMUX_VERSION_STRING, RTMUX_BUILD_ID);

    /* Load bootstrap executables in the image file system and initialize
     * various syspage pointers. This must be the _last_ initialization done
     * before transferring control to the next program.
     */
    init_system_private();

    /* This is handy for debugging a new version of the startup program.
     * Commenting this line out will save a great deal of code.
     */
    print_syspage();

    return 0;
}
