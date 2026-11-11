/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief startup_entry() — first C function after reset (called from head.S)
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>
#include <sys/storage.h>
#include <sbi.h>
#include <paging.h>
#include <cpu-inline.h>
#include <arch/pcpu.h> /* pcpu_array[], struct pcpu */
#include <options.h>

struct kernel_options kopt = KERNEL_OPTIONS_INIT;

unsigned paddr_bits;

/* These are shared with the kernel (globals.c defines them too). Use 'extern'
 * here to avoid duplicate symbol errors with -fno-common. Startup's
 * setup_cmdline() populates them; the kernel re-sets them later.
 */
extern char **_argv;
extern int _argc;

/* _syspage_ptr is the global syspage pointer shared between startup and kernel.
 * The definition lives in kernel/core/globals.c (via KERNEL_DEFN); startup uses
 * extern.
 */
extern struct syspage_entry *_syspage_ptr;

bool secure_system = 0;

extern int hardware_init(int argc, char **argv, char **envv);
extern void kernel_entry(struct syspage_entry *sysp);
char boot_command_line[CONFIG_MAX_BOOTLINE_SIZE];
/* Pristine copy of the boot command line. setup_cmdline() splits
 * boot_command_line[] in place by replacing inter-token spaces with NULs, so by
 * the time the syspage is built we'd have lost the space-separated form. Save a
 * copy first; ship it to the syspage so /sys/cmdline can present it intact.
 */
char boot_command_line_orig[CONFIG_MAX_BOOTLINE_SIZE];

static char *argv[64];
static char **envv = NULL;


/**
 * \brief Initialize argc and argv variables from boot_command_line.
 */
static void setup_cmdline(void)
{
    char *s = boot_command_line;
    int i = 0, argc = 0;
    int n = (int) strlen(boot_command_line);

    /* Snapshot the pristine command line before destructive split. */
    memcpy(boot_command_line_orig, boot_command_line, (size_t) n + 1);

    while (i < n && argc < (int) NUM_ELTS(argv)) {
        /* Skip consecutive spaces */
        while (i < n && s[i] == ' ')
            i++;
        if (i >= n || s[i] == '\0')
            break;
        argv[argc++] = s + i;
        /* Find end of token (bounded by string length) */
        while (i < n && s[i] != ' ' && s[i] != '\0')
            i++;
        /* Null-terminate and advance past the space */
        if (i < n && s[i] == ' ')
            s[i++] = '\0';
    }
    argv[argc] = NULL;

    _argc = argc;
    _argv = argv;
}

/**
 * \brief startup_entry() — first C function in supervisor mode, called from
 *        head.S.
 *
 * Initializes the board, parses the command line, builds the syspage, switches
 * to high VA, then hands control to the kernel via kernel_entry().
 */
/* tp points at pcpu_array[hart]. This is set up here for the boot hart; other
 * harts get their tp set in _smpstart.
 *
 * pcpu_array[boot_hart].pc_hart is populated below so get_cpunum() (which reads
 * "lw PC_HART(tp)") returns the correct hart ID for the very first kprintf
 * onward.
 */

void startup_entry(void)
{
    {
        extern uint64_t _boot_a0;
        unsigned boot_hart = (unsigned) _boot_a0;
        /* Set up the S-mode register convention immediately:
         *   sscratch = 0 (no U-mode context)
         *   tp = &pcpu_array[boot_hart]
         *
         * get_cpunum() reads "lw PC_HART(tp)" — one load.
         */
        pcpu_array[boot_hart].pc_hart = boot_hart;
        __asm__ volatile("csrw sscratch, zero");
        __asm__ volatile("mv tp, %0" ::"r"(&pcpu_array[boot_hart]));
    }

    paddr_bits = arch_max_paddr_bits();

    board_init();

    setup_cmdline();

#define INIT_SYSPAGE_SIZE 0x600
    init_syspage_memory(ws_alloc(INIT_SYSPAGE_SIZE), INIT_SYSPAGE_SIZE);

    /* Ship the pristine boot cmdline through the syspage so roottask can expose
     * it at /sys/cmdline.
     */
    set_syspage_cmdline(boot_command_line_orig);

    /* Same channel for the FDT /compatible string: /sys/board. */
    {
        extern char board_compatible[];
        set_syspage_board(board_compatible);
    }

    hardware_init(_argc, _argv, envv);

    /* Copy the local version of the system page we've built to the real system
     * page location we allocated in init_system_private().
     */
    write_syspage_memory();

    /* Switch from PA to high VA. After this call, PC, sp, and ra are all in the
     * upper canonical half (KERNEL_VIRT_BASE + PA). Identity mapping is still
     * active, so _syspage_ptr (a PA) remains valid.
     */
    extern void switch_to_high_va(uintptr_t va_offset);
    switch_to_high_va(KERNEL_VIRT_BASE);

    /* print_char was first set in board_init() while running at low-VA, so it
     * holds a low-VA function pointer. That works as long as the identity map
     * is in every active pgdir, but per-process pgdirs (roottask) map only the
     * high-VA kernel half — calling print_char(low_va) then takes an
     * instruction page fault. Re-take the address now while running at high-VA
     * so the stored pointer is canonical for the rest of the kernel's life.
     */
    extern void rvq_putc_ser_dbg(int c);
    set_print_char(rvq_putc_ser_dbg);

    /* Set SUM in sstatus before any heap access. init_mmu marks the kernel-heap
     * range (everything past _bss_end aligned to 2 MiB) with PTE_U=1 so U-mode
     * roottask can dereference kernel-allocated process_t/channel_t/thread_t
     * objects returned by query_object. S-mode loads/stores to PTE_U=1 pages
     * require SUM=1. The syspage itself lives in this heap range, so SUM must
     * be on before syspage_init runs. init_cpu() also sets SUM, but it runs
     * much later (from kernel_init) and APs handle their own SUM via init_cpu
     * in their hot path.
     */
    w_sstatus(r_sstatus() | SSTATUS_SUM);

    kprintf("Startup complete, entering kernel...\n");
    kernel_entry(_syspage_ptr);

    /* Should never return */
    crash("kernel_entry returned!\n");
}

/* Replacement for some C library stuff to minimize startup size Fallback errno
 * used only during very early boot, before any thread has a valid
 * _thread_local_storage. Once threads are running (roottask or even the idle
 * threads), __get_errno_ptr() routes each access to the thread's own
 * TLS->__errptr so there is no cross-thread race on a single global int.
 */
int errno;

/* __get_errno_ptr — return a pointer to the current thread's errno.
 *
 * Uses the running thread's TLS (via tp) when it carries a valid __errptr;
 * otherwise falls back to the global errno used during very early boot before
 * any thread has valid TLS.
 *
 * Return value: pointer to the errno storage to use.
 */
int *__get_errno_ptr(void)
{
    struct _thread_local_storage *tls;
    /* tp (x4) always holds the current thread's TLS pointer when the thread is
     * running; see kernel_exit / idle_tls setup.
     */
    __asm__ volatile("mv %0, tp" : "=r"(tls));
    /* Reject obviously-invalid TLS (NULL or not in kernel VA range) and any TLS
     * that has not been initialized with __errptr.
     */
    if (tls && (uintptr_t) tls >= 0xffffffc000000000UL && tls->__errptr)
        return tls->__errptr;
    return &errno;
}


/* __stackavail — report available stack space.
 *
 * Startup stubs this to "unlimited"; it exists only to satisfy the
 * stack-checking calls in some C-library routines.
 *
 * Return value: SIZE_MAX (effectively unlimited).
 */
size_t __stackavail(void)
{
    return (size_t) ~0;
}
