/* SPDX-License-Identifier: Apache-2.0 */
#include <startup.h>
#include <startup-api.h>
#include <paging.h>
#include <arch/pcpu.h>
#include <runtime.h>
#include <cpu-inline.h>
struct kernel_options kopt = KERNEL_OPTIONS_INIT;
unsigned paddr_bits;
bool secure_system;
uint64_t _boot_a0;
unsigned hart_ids[CONFIG_PROCESSORS_MAX];
paddr_t ram_phys_start, uart_base = 0x3f201000;
paddr_t initramfs_phys_start = 0x08000000;
paddr_t initramfs_phys_end = 0x08000000 + PI3_INITRAMFS_SIZE;
void *fdt_addr;
size_t fdt_size;
char boot_command_line[CONFIG_MAX_BOOTLINE_SIZE] = "rtmux";
char boot_command_line_orig[CONFIG_MAX_BOOTLINE_SIZE] = "rtmux";
static char *boot_argv[] = {boot_command_line, NULL};
int errno;
int *__get_errno_ptr(void)
{
    return &errno;
}
size_t __stackavail(void)
{
    return (size_t) -1;
}
static volatile uint32_t *uart = (void *) (KERNEL_VIRT_BASE + 0x3f201000);
void pi3_console_putc(int c)
{
    if (c == '\n')
        pi3_console_putc('\r');
    while (uart[0x18 / 4] & (1u << 5))
        __asm__ volatile("yield");
    uart[0] = (unsigned char) c;
}
/* Kernel diagnostics and user dbgprint share this UART. Serialize each
 * buffer below both callers so their bytes cannot interleave. Disable local
 * IRQs before acquiring the lock: a timer diagnostic can use this same path. */
static unsigned console_write_lock;
void cpu_console_write(const char *s, unsigned long n)
{
    unsigned long flags = cpu_irq_save();
    while (__atomic_exchange_n(&console_write_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield" ::: "memory");
    while (n--)
        pi3_console_putc(*s++);
    __atomic_store_n(&console_write_lock, 0, __ATOMIC_RELEASE);
    cpu_irq_restore(flags);
}
int cpu_console_getc(void)
{
    extern int aarch64_console_irq_owned(void);
    /* /dev/text's type-ahead watcher must not steal bytes from a native UART
     * driver. The last IRQ detachment restores polling, including on SIGKILL. */
    if (aarch64_console_irq_owned())
        return -1;
    return uart[0x18 / 4] & (1u << 4) ? -1 : (int) (uart[0] & 255);
}
unsigned board_smp_num_cpu(void)
{
    return CONFIG_PROCESSORS_MAX < 4 ? CONFIG_PROCESSORS_MAX : 4;
}
unsigned arch_max_paddr_bits(void)
{
    return 32;
}
int cpu_handle_common_option(int opt)
{
    (void) opt;
    return 0;
}
void *startup_memory_map(unsigned n, paddr_t p, unsigned f)
{
    (void) n;
    (void) f;
    return (void *) (uintptr_t) p;
}
void startup_memory_unmap(void *p)
{
    (void) p;
}
void init_raminfo(void)
{
    add_ram(0x1000, (paddr_t) CONFIG_PI3_RAM_SIZE_MB * 1024 * 1024 - 0x1000);
}
void init_asinfo(unsigned mem)
{
    as_add(0x3f000000, 0x400fffff, AS_ATTR_DEV, "io", mem);
}
void init_clocks(void)
{
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(kopt.timebase_freq));
}
void read_rtc_boot_time(void) {}
void print_syspage(void) {}
void init_cpuinfo(void)
{
    set_syspage_section(&lsp.cpuinfo,
                        sizeof(struct cpuinfo_entry) * lsp.syspage.p->num_cpu);
    struct cpuinfo_entry *c = lsp.cpuinfo.p;
    for (unsigned i = 0; i < lsp.syspage.p->num_cpu; i++) {
        memset(&c[i], 0, sizeof(c[i]));
        c[i].cpu = i;
        c[i].name = (uint16_t) add_string("ARM Cortex-A53");
        c[i].flags = CPU_FLAG_MMU | CPU_FLAG_FPU;
        c[i].data_cache = c[i].ins_cache = CACHE_LIST_END;
    }
}
void init_hwinfo(void)
{
    unsigned bus = hwibus_add(HWI_ITEM_BUS_INTERNAL, 0);
    unsigned dev = hwidev_add("pl011", 0, bus);
    hwitag_add_location(dev, 0x3f201000, 0x1000, 0, HWI_ADDRSPACE_MEMORY);
    hwitag_add_irq(dev, 57);
    dev = hwidev_add("bcm2835-system-timer", 0, bus);
    hwitag_add_location(dev, 0x3f003000, 0x1000, 0, HWI_ADDRSPACE_MEMORY);
    hwitag_add_irq(dev, 3); /* compare 3; architectural timer handles scheduling */
}
void init_qtime(void)
{
    set_syspage_section(&lsp.qtime, sizeof(struct qtime_entry));
    systime_t *q = lsp.qtime.p;
    memset(q, 0, sizeof(*q));
    q->cycles_per_sec = kopt.timebase_freq;
    q->timer_rate = 1000000000UL / kopt.timebase_freq;
    q->timer_scale = -9;
    q->intr = 5;
    q->epoch = 1970;
    kprintf("init_qtime: cycles_per_sec=%lu timer_rate=%lu intr=%u\n",
            (unsigned long) q->cycles_per_sec,
            (unsigned long) q->timer_rate, q->intr);
}
static const struct debug_device debug_devices[] = {
    {.name = "pl011", .put = pi3_console_putc}};
void arch_init_debug(void)
{
    select_debug(debug_devices, sizeof(debug_devices));
}
extern void aarch64_vectors(void);
void init_traps(void)
{
    __asm__ volatile("msr vbar_el1,%0; isb" ::"r"(aarch64_vectors) : "memory");
}
void init_cpu(void)
{
    unsigned cpu = get_cpunum();
    *(volatile uint32_t *) (KERNEL_VIRT_BASE + 0x40000040 + 4 * cpu) = 8;
    *(volatile uint32_t *) (KERNEL_VIRT_BASE + 0x40000050 + 4 * cpu) = 1;
}
void cpu_interrupt_init(unsigned n)
{
    (void) n;
}
void startup_entry(void)
{
    for (unsigned cpu = 0; cpu < board_smp_num_cpu(); cpu++)
        hart_ids[cpu] = cpu;
    cpu_set_kernel_tp(&pcpu_array[0]);
    pcpu_array[0].pc_hart = 0;
    init_traps();
    set_print_char(pi3_console_putc);
    kprintf("\nRTMux AArch64 Raspberry Pi 3B\n");
    paddr_bits = 32;
    _argc = 1;
    _argv = boot_argv;
    avoid_ram(STARTUP_V2P(_start), (size_t) (_end - _start));
    init_syspage_memory(ws_alloc(0x600), 0x600);
    set_syspage_cmdline(boot_command_line_orig);
    set_syspage_board("raspberrypi,3-model-b");
    extern int hardware_init(int, char **, char **);
    hardware_init(1, boot_argv, NULL);
    write_syspage_memory();
    kprintf("Startup complete, entering kernel...\n");
    extern void kernel_entry(struct syspage_entry *);
    kernel_entry(_syspage_ptr);
}
