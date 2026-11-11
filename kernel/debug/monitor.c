/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * One hart is reserved at boot (entry.c::syspage_init decrements num_processors
 * and records monitor_hart_id) and started by cpu_start_others via SBI HSM with
 * _monitor_start as the entry point.
 *
 * The hart is invisible to RTMux: no entry in cpupageptr[] / _actives /
 * actives_prp / ksys_stack at the RTMux index range, no trap handler routing
 * through __strap_entry's per-CPU state, no participation in any kspinlock
 * acquire path. It exists solely to observe.
 *
 * Output goes to a dedicated PCI 16550 UART (QEMU's pci-serial device, vendor
 * 0x1b36 device 0x0002) at a self-assigned BAR. This bypasses BOTH the kernel's
 * kprintf_slock AND the SBI console path — the only shared state with the rest
 * of the kernel is the kspinlock value fields the monitor reads (no locks, no
 * atomics, just plain loads).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include "sbi.h"
#include <startup-api.h> /* pci_fdt */

#define KERN_VA(pa) ((volatile void *) ((uintptr_t) (pa) + KERNEL_VIRT_BASE))

/* Slock list — every global kspinlock the kernel currently has Add new ones
 * here when they appear.
 */

struct monitor_slock {
    const char *name;
    kspinlock_t *slock;
};

extern kspinlock_t alloc_slock, sync_slock, global_channel_slock, as_slock,
    process_vector_slock, cred_slock, limits_slock, trace_slock;

/* Per-CPU dispatch instances each carry their own kspinlock_t; monitor_loop
 * walks them and shows a compact bitmap of held sched locks (see below).
 * Per-CPU state (curthread, idle_thp, last_kernel_pc, last_scause,
 * ipi_handler_count, ipicmds, as_prp) lives in pcpu_array[], visible via
 * core/globals.h's <arch/pcpu.h>.
 */
extern unsigned hart_ids[];
extern unsigned num_processors;
extern unsigned in_kcall[CONFIG_PROCESSORS_MAX];
extern process_t *kernel_threads_prp;
extern vector_t process_vector;

static const struct monitor_slock monitor_slocks[] = {
    {"alloc", &alloc_slock},
    {"sync", &sync_slock},
    {"global_channel", &global_channel_slock},
    {"as", &as_slock},
    {"procvec", &process_vector_slock},
    {"cred", &cred_slock},
    {"limits", &limits_slock},
    {"trace", &trace_slock},
};
#define N_SLOCKS (sizeof(monitor_slocks) / sizeof(monitor_slocks[0]))


/* PCI 16550 (QEMU pci-serial) direct MMIO.
 *
 * We scan ECAM for vendor=0x1b36 device=0x0002, assign BAR0 to a free address
 * inside one of the FDT-published PCI mem windows, enable command-register
 * MEM/IO access, and write directly to the 16550 register block. All happens on
 * the monitor hart before any kspinlock is touched.
 */

#define PCI_VENDOR_OFF 0x00
#define PCI_DEVICE_OFF 0x02
#define PCI_COMMAND_OFF 0x04
#define PCI_BAR0_OFF 0x10

#define PCI_CMD_IO 0x0001
#define PCI_CMD_MEM 0x0002
#define PCI_CMD_BM 0x0004

#define QEMU_PCI_SERIAL_VENDOR 0x1b36
#define QEMU_PCI_SERIAL_DEVICE 0x0002

/* 16550 register offsets */
#define UART_THR 0         /* tx holding (write) */
#define UART_LSR 5         /* line status */
#define UART_LSR_THRE 0x20 /* TX holding empty */
#define UART_LSR_TEMT 0x40 /* TX shift register empty */

/* Set after init.  NULL until init runs / on failure. */
static volatile uint8_t *uart_mmio;

static inline uint32_t mmio_r32(volatile uint32_t *p)
{
    return *p;
}
static inline void mmio_w32(volatile uint32_t *p, uint32_t v)
{
    *p = v;
}
static inline uint16_t mmio_r16(volatile uint16_t *p)
{
    return *p;
}
static inline void mmio_w16(volatile uint16_t *p, uint16_t v)
{
    *p = v;
}

/* Walk PCI bus 0 in ECAM, return the config-space VA of the first device
 * matching (vendor, device), or NULL. Bus 0 is sufficient for QEMU virt —
 * pci-serial attaches there.
 */
static volatile uint8_t *find_pci_device(uint16_t vendor, uint16_t device)
{
    if (pci_fdt.ecam_base == 0 || pci_fdt.ecam_size == 0)
        return 0;
    volatile uint8_t *ecam = KERN_VA(pci_fdt.ecam_base);
    /* Walk a few buses; QEMU virt puts pci-serial on bus 0. */
    for (unsigned bus = 0; bus < 4; bus++) {
        for (unsigned dev = 0; dev < 32; dev++) {
            for (unsigned func = 0; func < 8; func++) {
                volatile uint8_t *cfg =
                    ecam + (bus << 20) + (dev << 15) + (func << 12);
                uint16_t v =
                    mmio_r16((volatile uint16_t *) (cfg + PCI_VENDOR_OFF));
                if (v == 0xffff)
                    continue;
                uint16_t d =
                    mmio_r16((volatile uint16_t *) (cfg + PCI_DEVICE_OFF));
                if (v == vendor && d == device)
                    return cfg;
            }
        }
    }
    return 0;
}

/* Pick a free address inside the first 32-bit PCI memory window. Window 0 in
 * QEMU virt is the I/O window (0x03000000); window 1 is the 32-bit MMIO window
 * (0x40000000+). We choose offset 0 in the memory window — high enough to be
 * safely past anything userland's PCI server might later auto-assign at the
 * same window's bottom.
 *
 * Returns CPU phys addr to write to BAR0, or 0 on failure.
 */
/* Pick a slot for our BAR.
 *   *out_pci_addr — value to write into the BAR (PCI-bus-side address)
 *   *out_cpu_addr — CPU phys address to use for MMIO accesses
 * Returns 1 on success, 0 on failure.
 */
static int pick_bar_addr(unsigned is_io, uint64_t *out_pci, uint64_t *out_cpu)
{
    /* pci_fdt.windows[].flags uses the HWI_PCI_WINDOW_* taxonomy
     * (internal/include/hw/sysinfo.h): 0x01 = I/O, 0x02 = MEM.
     */
    for (unsigned i = 0; i < pci_fdt.num_windows; i++) {
        uint32_t flags = pci_fdt.windows[i].flags;
        unsigned win_is_io = (flags & 0x01u) ? 1 : 0;
        unsigned win_is_mem = (flags & 0x02u) ? 1 : 0;
        if (is_io ? !win_is_io : !win_is_mem)
            continue;

        /* 0x1000 offset leaves the bottom of the window free for userland's PCI
         * server to use later. BAR holds the PCI address; CPU accesses
         * translate through the window.
         */
        uint64_t off = 0x1000;
        *out_pci = pci_fdt.windows[i].pci_addr + off;
        *out_cpu = pci_fdt.windows[i].cpu_addr + off;
        return 1;
    }
    return 0;
}

/* Set up the pci-serial UART for direct MMIO/IO writes.
 * Returns 1 on success (uart_mmio populated), 0 on failure.
 */
static int monitor_init_uart(void)
{
    volatile uint8_t *cfg =
        find_pci_device(QEMU_PCI_SERIAL_VENDOR, QEMU_PCI_SERIAL_DEVICE);
    if (!cfg)
        return 0;

    uint32_t bar0 = mmio_r32((volatile uint32_t *) (cfg + PCI_BAR0_OFF));
    unsigned is_io = bar0 & 0x1;

    uint64_t pci_addr, cpu_addr;
    if (!pick_bar_addr(is_io, &pci_addr, &cpu_addr))
        return 0;

    /* Write the PCI-bus-side address into BAR0 (preserve type bit) */
    uint32_t newbar = (uint32_t) pci_addr | (is_io ? 1u : 0u);
    mmio_w32((volatile uint32_t *) (cfg + PCI_BAR0_OFF), newbar);

    /* Enable I/O or MEM access + bus master in command register */
    uint16_t cmd = mmio_r16((volatile uint16_t *) (cfg + PCI_COMMAND_OFF));
    cmd |= (uint16_t) (is_io ? PCI_CMD_IO : PCI_CMD_MEM);
    cmd |= (uint16_t) PCI_CMD_BM;
    mmio_w16((volatile uint16_t *) (cfg + PCI_COMMAND_OFF), cmd);

    /* MMIO accesses go through the CPU side of the PCI window. */
    uart_mmio = KERN_VA(cpu_addr);

    /* Initialize the 16550: 8N1, DLAB=0, FIFO on, interrupts off, DTR+RTS
     * asserted.
     */
    uart_mmio[3] = 0x03;
    uart_mmio[2] = 0x07;
    uart_mmio[1] = 0x00;
    uart_mmio[4] = 0x0b;

    /* KNOWN ISSUE on QEMU virt: byte AND dword CPU writes to the pci-serial's
     * I/O BAR are silently dropped by the platform's pci-host-ecam-generic
     * bridge — LSR reads back the right values (proving the device is
     * reachable), and writes complete without faulting, but they never make it
     * to the chardev backend (pci-serial.log stays empty). Direct UART (PA
     * 0x10000000) writes from the same monitor hart work fine, so the
     * page-table mapping and hart state are sound — the gap is in QEMU's PCI
     * I/O forwarding. Until that's resolved upstream (or we switch to a
     * memory-mapped serial card), disable the direct path and fall back to SBI
     * console output.
     *
     * To re-enable for testing: comment out the uart_mmio=0 line.
     */
    uart_mmio = 0;
    return 0;
}

/* Emit one character to the monitor console (PCI 16550 if available, else SBI
 * console). Parameters:
 *   c : character to emit.
 */
static void uart_putc(char c)
{
    if (!uart_mmio) {
        sbi_ecall_console_putc((int) (unsigned char) c);
        return;
    }
    /* Diagnostic single byte write with full fence around it. */
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    uart_mmio[0] = (uint8_t) c;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

/* Emit a NUL-terminated string to the monitor console. Parameters:
 *   s : string to emit.
 */
static void mon_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/* Emit a single character to the monitor console. Parameters:
 *   c : character to emit.
 */
static void mon_putc(char c)
{
    uart_putc(c);
}

/* Emit a 32-bit value as "0x" + 8 hex digits to the monitor console.
 * Parameters:
 *   v : value to print.
 */
static void mon_put_hex(uint32_t v)
{
    int i;
    mon_puts("0x");
    for (i = 7; i >= 0; i--) {
        unsigned nib = (v >> (i * 4)) & 0xfu;
        mon_putc((char) (nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

/* Emit a 64-bit value as "0x" + 16 hex digits to the monitor console.
 * Parameters:
 *   v : value to print.
 */
static void mon_put_hex64(uint64_t v)
{
    int i;
    mon_puts("0x");
    for (i = 15; i >= 0; i--) {
        unsigned nib = (unsigned) (v >> (i * 4)) & 0xfu;
        mon_putc((char) (nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

/* Emit an unsigned value as decimal digits to the monitor console. Parameters:
 *   v : value to print.
 */
static void mon_put_dec(unsigned v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        mon_putc('0');
        return;
    }
    while (v) {
        buf[i++] = (char) ('0' + (v % 10));
        v /= 10;
    }
    while (--i >= 0)
        mon_putc(buf[i]);
}


/* Dedicated 16 KiB stack for the monitor thread. */

#define MONITOR_STACK_NBYTES (16 * 1024)
static uint64_t monitor_stack[MONITOR_STACK_NBYTES / sizeof(uint64_t)]
    __attribute__((aligned(16)));
uintptr_t monitor_stack_top =
    (uintptr_t) &monitor_stack[MONITOR_STACK_NBYTES / sizeof(uint64_t)];


/* Main loop */

/* Walk every process's thread vector and tally each thread state. Lock-free
 * reads; benign data races.
 */
static void mon_dump_thread_states(void)
{
    unsigned tot = 0, run = 0, rdy = 0, snd = 0, rcv = 0, rep = 0;
    unsigned mtx = 0, cv = 0, sem = 0, intr = 0, nslp = 0, other = 0;
    unsigned nproc = process_vector.nentries;
    if (nproc > 4096)
        return;
    for (unsigned p = 0; p < nproc; p++) {
        process_t *prp = (process_t *) process_vector.vector[p];
        if (!prp || ((uintptr_t) prp & 3))
            continue;
        unsigned nth = prp->threads.nentries;
        if (nth > 4096)
            continue;
        for (unsigned t = 0; t < nth; t++) {
            thread_t *thp = (thread_t *) prp->threads.vector[t];
            if (!thp || ((uintptr_t) thp & 3))
                continue;
            tot++;
            switch (thp->state) {
            case STATE_RUNNING:
                run++;
                break;
            case STATE_READY:
                rdy++;
                break;
            case STATE_SEND:
                snd++;
                break;
            case STATE_RECEIVE:
                rcv++;
                break;
            case STATE_REPLY:
                rep++;
                break;
            case STATE_MUTEX:
                mtx++;
                break;
            case STATE_CONDVAR:
                cv++;
                break;
            case STATE_SEM:
                sem++;
                break;
            case STATE_INTR:
                intr++;
                break;
            case STATE_NANOSLEEP:
                nslp++;
                break;
            default:
                other++;
                break;
            }
        }
    }
    mon_puts(" thr:tot=");
    mon_put_dec(tot);
    mon_puts(" R=");
    mon_put_dec(run);
    mon_puts(" Y=");
    mon_put_dec(rdy);
    mon_puts(" S=");
    mon_put_dec(snd);
    mon_puts(" r=");
    mon_put_dec(rcv);
    mon_puts(" p=");
    mon_put_dec(rep);
    mon_puts(" m=");
    mon_put_dec(mtx);
    mon_puts(" c=");
    mon_put_dec(cv);
    mon_puts(" e=");
    mon_put_dec(sem);
    mon_puts(" i=");
    mon_put_dec(intr);
    mon_puts(" n=");
    mon_put_dec(nslp);
    mon_puts(" ?=");
    mon_put_dec(other);
}

/* Dump every blocked thread's tid/pid/state/blocked_on/syscall — so the
 * wake-chain can be traced backwards from "who's stuck" to "who should wake
 * them".
 */
static void mon_dump_blocked_threads(void)
{
    unsigned nproc = process_vector.nentries;
    if (nproc > 4096)
        return;
    for (unsigned p = 0; p < nproc; p++) {
        process_t *prp = (process_t *) process_vector.vector[p];
        if (!prp || ((uintptr_t) prp & 3))
            continue;
        unsigned nth = prp->threads.nentries;
        if (nth > 4096)
            continue;
        for (unsigned t = 0; t < nth; t++) {
            thread_t *thp = (thread_t *) prp->threads.vector[t];
            if (!thp || ((uintptr_t) thp & 3))
                continue;

            /* Skip only RUNNING (idle threads + the genuinely-running). READY
             * (st2) and DEAD (st0) are SHOWN: a child that "vanishes" from the
             * census while its parent's waitpid hangs is exactly a thread stuck
             * READY-undispatched or DEAD-unreaped — those must not hide.
             */
            if (thp->state == STATE_RUNNING)
                continue;
            mon_puts("\n  t");
            mon_put_dec(thp->tid + 1);
            mon_putc('p');
            mon_put_dec((unsigned) prp->pid);
            mon_puts(" st");
            mon_put_dec(thp->state);
            mon_puts(" sc");
            mon_put_dec((unsigned) (thp->syscall & 0xff));
            if (thp->blocked_on) {
                mon_puts(" bo");
                mon_put_hex64((uint64_t) (uintptr_t) thp->blocked_on);
            }
        }
    }
}

/* Trace "ghost-RUNNING" threads.
 *
 * Ghost signature: all harts idle in WFI, ipicmds=0, yet the global state tally
 * reports N threads in STATE_RUNNING. Those threads' state field was never
 * cleared on context-switch — they're ghosts. Any thread blocked waiting on a
 * ghost (e.g. process_destroy spinning on RTMUX_ITF_MSG_DELIVERY or
 * IPI_TLB_SAFE drain) will hang forever.
 *
 * For every thread observed in STATE_RUNNING we print:
 *   tid/pid, syscall, runcpu, real_priority, flags, internal_flags
 * and, on a second pass, which hart (if any) actually has it as
 * pcpu_array[hart].pc_curthread. A genuine running thread shows up on exactly
 * one hart; a ghost shows "owner=NONE". A ghost whose runcpu != owner-hart is a
 * cross-CPU active-thread write hazard caught in the act.
 *
 * Lock-free reads; benign data races (we may print transient mid-context-switch
 * state, that's fine — repeated ticks make the pattern obvious).
 */
static void mon_dump_ghost_running(void)
{
    unsigned nproc = process_vector.nentries;
    if (nproc > 4096)
        return;

    int header_printed = 0;
    for (unsigned p = 0; p < nproc; p++) {
        process_t *prp = (process_t *) process_vector.vector[p];
        if (!prp || ((uintptr_t) prp & 3))
            continue;
        unsigned nth = prp->threads.nentries;
        if (nth > 4096)
            continue;
        for (unsigned t = 0; t < nth; t++) {
            thread_t *thp = (thread_t *) prp->threads.vector[t];
            if (!thp || ((uintptr_t) thp & 3))
                continue;
            if (thp->state != STATE_RUNNING)
                continue;

            /* Skip idle threads. cpu_idle() flips pc_idle_thp->state to
             * STATE_RUNNING while WFI-spinning and leaves
             * pcpu_array[hart].pc_curthread NULL — that's by-design (see
             * arch/riscv/cpu-misc.c:401-408), not a ghost. Any thread that is
             * some hart's pc_idle_thp is filtered out here.
             */
            bool is_idle = false;
            for (unsigned i = 0; i < num_processors; i++) {
                if (pcpu_array[hart_ids[i]].pc_idle_thp == thp) {
                    is_idle = true;
                    break;
                }
            }
            if (is_idle)
                continue;

            /* Which hart (if any) actually has this thp as pc_curthread? -1 if
             * none — that's a real ghost.
             */
            int owner = -1;
            for (unsigned i = 0; i < num_processors; i++) {
                unsigned hart = hart_ids[i];
                if (pcpu_array[hart].pc_curthread == thp) {
                    owner = (int) hart;
                    break;
                }
            }

            if (!header_printed) {
                mon_puts("\n  -- RUNNING threads --");
                header_printed = 1;
            }
            mon_puts("\n  t");
            mon_put_dec(thp->tid + 1);
            mon_putc('p');
            mon_put_dec((unsigned) prp->pid);
            mon_puts(" sc");
            mon_put_dec((unsigned) (thp->syscall & 0xff));
            mon_puts(" rc");
            mon_put_dec((unsigned) thp->runcpu);
            mon_puts(" pr");
            mon_put_dec((unsigned) thp->real_priority);
            mon_puts(" fl=");
            mon_put_hex((uint32_t) thp->flags);
            mon_puts(" if=");
            mon_put_hex((uint32_t) thp->internal_flags);
            mon_puts(" owner=");
            if (owner < 0)
                mon_puts("NONE *GHOST*");
            else if ((unsigned) owner != thp->runcpu) {
                mon_putc('h');
                mon_put_dec((unsigned) owner);
                mon_puts(" *RUNCPU-MISMATCH*");
            } else {
                mon_putc('h');
                mon_put_dec((unsigned) owner);
            }
        }
    }
}

#if defined(CONFIG_INSTRUMENTATION)

/* Dump the kernel trace ring buffer exactly as "-T" crash-time
 * tr_dump_to_console() would, but using monitor-native I/O (no kprintf / no
 * kprintf_slock contention) so it runs cleanly during an active stall. Called
 * once at tick==5 to capture "what led up to the wedge".
 *
 * Layout: trace_masks.buff_0_ptr -> first tracebuf_t in a linked ring. Each
 * tracebuf has h.num_events valid entries starting at h.begin_ptr. Each
 * traceevent is 4 x uint32_t (header + 3 data words); header
 * decodes as struct[31:30] | cpu[29:24] | class[13:10] | ev[9:0].
 *
 * Output is capped at MON_TRACE_MAX_PER_BUF events per buffer to bound total
 * volume — events within a buffer are chronological, so we keep the most recent
 * slice (the tail).
 */
#define MON_TRACE_MAX_PER_BUF 64

/* Map a trace-event class number to its short display name. Parameters:
 *   cl : trace class number.
 * Return value: a static name string ("??? " for unknown classes).
 */
static const char *mon_trace_class(unsigned cl)
{
    switch (cl) {
    case 0:
        return "EMPTY  ";
    case 1:
        return "CONTROL";
    case 2:
        return "KCALL";
    case 3:
        return "INT    ";
    case 4:
        return "PR_TH  ";
    case 5:
        return "SYSTEM ";
    case 6:
        return "USER   ";
    case 7:
        return "COMM   ";
    default:
        return "???    ";
    }
}

/* Map a trace-event struct-type number to its two-letter display code.
 * Parameters:
 *   s : trace struct-type number.
 * Return value: a static code string ("??" for unknown types).
 */
static const char *mon_trace_struct(unsigned s)
{
    switch (s) {
    case 0:
        return "S ";
    case 1:
        return "CB";
    case 2:
        return "CC";
    case 3:
        return "CE";
    default:
        return "??";
    }
}

/* Emit a 32-bit value as 8 hex digits (no "0x" prefix) to the monitor console.
 * Parameters:
 *   v : value to print.
 */
static void mon_put_hex32(uint32_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        unsigned nib = (v >> (i * 4)) & 0xfu;
        mon_putc((char) (nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

/* Dump one trace ring buffer to the monitor console, showing at most the most
 * recent MON_TRACE_MAX_PER_BUF events. Parameters:
 *   b : the trace buffer to dump.
 */
static void mon_dump_one_buf(const tracebuf_t *b)
{
    uint32_t n = b->h.num_events;
    if (n == 0)
        return;

    /* Chronological events are in begin_ptr[0..n-1].  Keep the tail. */
    uint32_t start = 0;
    if (n > MON_TRACE_MAX_PER_BUF) {
        start = n - MON_TRACE_MAX_PER_BUF;
    }

    mon_puts("--- buf seq=");
    mon_put_dec((unsigned) b->h.seq_buff_num);
    mon_puts(" num=");
    mon_put_dec((unsigned) n);
    mon_puts(" showing=");
    mon_put_dec((unsigned) (n - start));
    mon_puts(" ---\n");

    const traceevent_t *ev = (const traceevent_t *) b->h.begin_ptr;
    for (uint32_t i = start; i < n; i++) {
        uint32_t h = ev[i].header;
        unsigned st = (h >> 30) & 0x3;
        unsigned cp = (h >> 24) & 0x3f;
        unsigned cl = (h >> 10) & 0xf;
        unsigned ec = h & 0x3ff;
        mon_puts("  [");
        mon_put_dec((unsigned) i);
        mon_puts("] cpu");
        mon_put_dec(cp);
        mon_putc(' ');
        mon_puts(mon_trace_struct(st));
        mon_putc(' ');
        mon_puts(mon_trace_class(cl));
        mon_puts(" ev=");
        mon_put_dec(ec);
        mon_puts("  ");
        mon_put_hex32((uint32_t) ev[i].data[0]);
        mon_putc(' ');
        mon_put_hex32((uint32_t) ev[i].data[1]);
        mon_putc(' ');
        mon_put_hex32((uint32_t) ev[i].data[2]);
        mon_putc('\n');
    }
}

static void mon_dump_tracebuf(void)
{
    tracebuf_t *b0 = (tracebuf_t *) trace_masks.buff_0_ptr;
    uint32_t n = trace_masks.buff_num;

    if (!b0 || n == 0) {
        mon_puts("[monitor] trace buffer not allocated\n");
        return;
    }

    mon_puts("[monitor] *** trace buffer dump (");
    mon_put_dec((unsigned) n);
    mon_puts(" buffers) ***\n");

    tracebuf_t *b = b0;
    for (uint32_t i = 0; i < n; i++) {
        mon_dump_one_buf(b);
        b = b->h.next;
        if (!b || b == b0)
            break;
    }
    mon_puts("[monitor] *** trace buffer dump end ***\n");
}
#else
static inline void mon_dump_tracebuf(void) {}
#endif

/* Read the time CSR. QEMU virt timer base = 10 MHz. Real hardware may differ;
 * for 1-second tick precision the exact frequency doesn't matter — we just want
 * a roughly-one-second cadence.
 */
static inline uint64_t mon_read_time(void)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

void __attribute__((noreturn)) monitor_loop(void)
{
    const uint64_t period = 10000000ULL; /* 1 second @ 10 MHz */
    uint64_t deadline;
    uint64_t tick = 0;

    sbi_ecall_console_puts("\n[monitor] kthread up on captured hart\n");
    /* One-shot startup probe: dump kernel_threads_prp->memory so we can tell
     * whether set_safe_as would be a no-op (memory=NULL → as_switch
     * early-returns, as_prp unchanged).
     */
    {
        sbi_ecall_console_puts("[monitor] kernel_threads_prp->memory = ");
        if (!kernel_threads_prp) {
            sbi_ecall_console_puts("(prp NULL)");
        } else if (!kernel_threads_prp->memory) {
            sbi_ecall_console_puts("NULL  *** set_safe_as IS A NO-OP ***");
        } else {
            sbi_ecall_console_puts("non-NULL (set_safe_as OK)");
        }
        sbi_ecall_console_puts("\n");
    }

    if (monitor_init_uart()) {
        sbi_ecall_console_puts("[monitor] using direct PCI 16550 for output\n");
    } else {
        sbi_ecall_console_puts(
            "[monitor] PCI 16550 direct path disabled; using SBI console\n");
    }

    mon_puts("[monitor] watching ");
    mon_put_dec((unsigned) N_SLOCKS);
    mon_puts(" global kspinlocks\n");

    deadline = mon_read_time() + period;
    for (;;) {
        while (mon_read_time() < deadline)
            __asm__ volatile("" ::: "memory");
        deadline += period;
        tick++;

        /* Emit a full census of global locks and thread state once per period
         * so a wedged hart can be identified from the log.
         */

        mon_puts("[monitor t=");
        mon_put_dec((unsigned) tick);
        mon_puts("] ");
        for (unsigned i = 0; i < N_SLOCKS; i++) {
            unsigned v = monitor_slocks[i].slock->spin.value;
            mon_puts(monitor_slocks[i].name);
            mon_putc('=');
            mon_putc(v ? 'L' : 'u');
            mon_putc(' ');
        }

        /* Per-CPU sched locks: one char per active hart in hart_ids[] order (L
         * = held, u = unlocked).
         */
        mon_puts("sched=");
        for (unsigned i = 0; i < num_processors; i++) {
            unsigned hart = hart_ids[i];
            struct dispatch_entry *dpp = pcpu_array[hart].pc_dispatch;
            if (!dpp) {
                mon_putc('?');
                continue;
            }
            unsigned v = ((kspinlock_t *) &dpp->lock)->spin.value;
            mon_putc(v ? 'L' : 'u');
        }
        mon_dump_thread_states();

        /* Per-hart active thread (concise) + in_kcall depth + sepc.
         * last_kernel_pc is the PC the last trap on this hart was at; for a
         * hart spinning in kernel-mode, after one timer tick this will be the
         * kernel spin PC. Cross-ref with kernel.sym / addr2line to identify the
         * stuck kernel extension.
         */
        for (unsigned i = 0; i < num_processors; i++) {
            unsigned hart = hart_ids[i];
            thread_t *thp = get_active_thread(hart);
            mon_puts(" h");
            mon_put_dec(hart);
            mon_putc(':');
            if (!thp)
                mon_puts("NULL");
            else if (thp == pcpu_array[hart].pc_idle_thp)
                mon_puts("idle");
            else {
                mon_putc('t');
                mon_put_dec((unsigned) thp->tid + 1);
                mon_putc('p');
                mon_put_dec((unsigned) (thp->process ? thp->process->pid : 0));
                mon_putc('/');
                mon_put_dec((unsigned) thp->state);
                mon_putc('/');
                mon_put_dec((unsigned) (thp->syscall & 0xff));
            }
            mon_putc('@');
            mon_put_dec(in_kcall[hart]);
            mon_putc('|');
            mon_put_hex64(pcpu_array[hart].pc_last_kernel_pc);

            /* ipi state: ipicmds + handler invocation count + last scause
             * (cause==1 → SSW/IPI taken, cause==5 → STIMER, 0x8000000000000001
             * already masked).
             */
            mon_puts(" ipi=");
            mon_put_hex((uint32_t) atomic_load_explicit(
                &pcpu_array[hart].pc_ipicmds, memory_order_acquire));
            mon_putc('/');
            mon_put_dec((unsigned) pcpu_array[hart].pc_ipi_handler_count);
            mon_puts(" sc=");
            mon_put_hex(
                (uint32_t) (pcpu_array[hart].pc_last_scause & 0xffffffff));

            /* as_prp pid — if a hart's as_prp doesn't change across
             * IPI_TLB_SAFE rounds, set_safe_as is no-op-ing.
             */
            mon_puts(" asp=");
            {
                process_t *_ap = pcpu_array[hart].pc_as_prp;
                if (!_ap)
                    mon_putc('-');
                else
                    mon_put_dec((unsigned) _ap->pid);
            }
        }

        /* On every 3rd tick (cadence: 3s) also dump every blocked thread's
         * tid/pid/state/blocked_on so the wake-chain is traceable during a
         * stall.
         */
        mon_dump_blocked_threads();

        /* Every tick: dump RUNNING threads with their pc_curthread owner so
         * ghost-RUNNING ("STATE_RUNNING but no hart owns it") is visible. Cheap
         * when nothing's ghosting — only prints when at least one thread is in
         * STATE_RUNNING.
         */
        mon_dump_ghost_running();
        mon_putc('\n');

        /* At tick==5: one-shot trace-buffer dump (same content as "-T" crash
         * handler). Reveals the sequence of kernel events leading up to a
         * lost-wakeup stall.
         */
        mon_dump_tracebuf();
    }
}


/* Bring-up: SBI HSM-start entry for the monitor hart Mirrors _smpstart's
 * MMU+stack init (arch/riscv/cpu-misc.c) but skips all per-CPU RTMux
 * registration (actives, cpupageptr, etc.). After MMU is up and sp is set,
 * jumps to monitor_loop at high VA.
 *
 * a0 = hartid (from SBI HSM start).
 */
void __attribute__((naked)) _monitor_start(void)
{
    __asm__ volatile(
        "csrw sscratch, zero\n"
        "li   t0, 0x40000\n" /* SSTATUS_SUM */
        "csrs sstatus, t0\n"

        "la   t2, boot_satp_val\n"
        "ld   t2, 0(t2)\n"
        "csrw satp, t2\n"
        "sfence.vma\n"

        "la   t3, kern_va_offset\n"
        "ld   t3, 0(t3)\n"

        "la   t0, __strap_entry\n"
        "add  t0, t0, t3\n"
        "csrw stvec, t0\n"

        "la   t0, monitor_stack_top\n"
        "add  t0, t0, t3\n"
        "ld   sp, 0(t0)\n"

        "la   t0, monitor_loop\n"
        "add  t0, t0, t3\n"
        "jr   t0\n"

        "1: wfi\n"
        "   j 1b\n" ::
            :);
}
