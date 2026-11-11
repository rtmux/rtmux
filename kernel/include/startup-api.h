/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \file startup-api.h
 *
 * Startup function prototypes.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#ifndef _STARTUP_API_H
#define _STARTUP_API_H

#include <posix-types.h>
#include <log.h>
#include <time.h>

/* This function is called at the start of startup_entry() */
void board_init(void);

/* If set_print_char() is called early, many interesting early messages will be
 * seen on the console.
 */
void set_print_char(void (*)(int));

void select_debug(const struct debug_device *dev, unsigned size);
void arch_init_debug(void);

/* These are board specific routines that need to be implemented. There may be
 * more syspage sections that need to be implemented for a particular CPU. Check
 * the appropriate cpu-startup.h. There may be default implementations of these
 * routines in the startup library that* will work for a particular board, or
 * set of boards.
 */
void init_raminfo(void);
void init_intrinfo(void);
void init_clocks(void);
void init_qtime(void);
void init_cpuinfo(void);
void init_hwinfo(void);
void init_asinfo(unsigned mem);

/* Only needed for SMP systems */
void init_smp(void);
unsigned board_smp_num_cpu(void);
extern void (*smp_spin_vaddr)(void);

/* Read RTC before MMU is enabled (PA == VA) */
void read_rtc_boot_time(void);

/* Only needed on systems with an MMU */
void init_mmu(void);

void cpu_init_syspage_memory(void);
struct syspage_entry *cpu_alloc_syspage_memory(paddr_t *, paddr_t *, unsigned);

/* These are CPU/board independent routines that can be used 'as is'. */
void init_system_private(void);
void set_syspage_cmdline(const char *);
void set_syspage_board(const char *);
void set_syspage_build_info(const char *release, const char *build_id);

paddr_t strtopaddr(const char *nptr, char **endptr, int base);

/* FDT routines */
void parse_fdt(size_t *fdt_size);

struct qtime_entry *alloc_qtime(void);
struct tm *_gmtime(const time_t *timer, struct tm *tmbuf);

/* Memory management */
void *ws_alloc(size_t);
void add_sysram(void);
void avoid_ram(paddr_t, size_t size);
void add_ram(paddr_t start, uint64_t size);
paddr_t alloc_ram(paddr_t addr, paddr_t size, unsigned align);
paddr_t calloc_ram(size_t size, unsigned align);
void reserve_ram(unsigned size, unsigned align, unsigned pagesize);
paddr_t ram_top(void);

/* Command-line option handling */
int getopt(int argc, char *const argv[], const char *opts);
int cpu_handle_common_option(int opt);
void handle_common_option(int opt);
paddr_t getsize(char *, char **);

void print_syspage(void);

/* Syspage routines */
void init_syspage_memory(void *, unsigned max_size);
void *grow_syspage_section(void *, ulong_t size);
void *set_syspage_section(void *, unsigned size);
void reloc_syspage_memory(void *, unsigned max_size);
void alloc_syspage_memory(void);
void write_syspage_memory(void);

int find_typed_string(int type_index);
int del_typed_string(int type_index);
unsigned add_typed_string(int type_index, const char *name);
unsigned add_string(const char *name);

struct intrinfo_entry *add_interrupt(const struct startup_intrinfo *);

#define HWI_TAG_INFO(type) \
    HWI_TAG_NAME_##type, sizeof(struct hwi_##type), HWI_TAG_ALIGN_##type

/* Prototypes for generic hwi routines. Note there are specific bus and device
 * add functions defined in startup/lib/public/sys/hwinfo.h
 */
void hwi_default(void);
unsigned hwi_find_as(paddr_t base, int mmap);

/* Prototypes for adding buses and devices */
unsigned hwibus_add(const char *bus_name, unsigned hwi_off);
unsigned hwidev_add(const char *device_name,
                    unsigned pnp,
                    unsigned bus_hwi_off);

/* Prototypes for adding tags */
void hwitag_add_optstr(unsigned hwi_off, const char *string);
void hwitag_add_location(unsigned hwi_off,
                         paddr_t base,
                         paddr_t len,
                         unsigned reg_shift,
                         unsigned addr_space);
void hwitag_add_irq(unsigned hwi_off, unsigned vector);
void hwitag_add_pci_window(unsigned hwi_off,
                           uint64_t pci_addr,
                           uint64_t cpu_addr,
                           uint64_t size,
                           uint32_t flags);

unsigned as_default(void);
unsigned as_add(paddr_t start,
                paddr_t end,
                unsigned attr,
                const char *name,
                unsigned owner);
unsigned as_add_containing(paddr_t start,
                           paddr_t end,
                           unsigned attr,
                           const char *name,
                           const char *container);
unsigned as_find(unsigned start, ...);
unsigned as_find_containing(unsigned off,
                            paddr_t start,
                            paddr_t end,
                            const char *container);

extern struct local_syspage lsp;

static inline ptrdiff_t as_info2off(const struct asinfo_entry *as)
{
    return as - lsp.asinfo.p;
}

static inline struct asinfo_entry *as_off2info(unsigned off)
{
    return (struct asinfo_entry *) ((uintptr_t) lsp.asinfo.p + off);
}

void *startup_memory_map(unsigned size, paddr_t phys, unsigned prot_flags);
void startup_memory_unmap(void *);

/** Variable prototypes * */
extern unsigned paddr_bits;
extern int _argc;
extern char **_argv;
extern char *optarg;
extern int optind;
extern bool secure_system;

/* Location of initramfs */
extern paddr_t initramfs_phys_start, initramfs_phys_end;

/* Device base addresses discovered from FDT (set by board.c) */
extern paddr_t uart_base;
extern paddr_t plic_base;
extern size_t plic_size;
extern paddr_t clint_base;
extern paddr_t ram_phys_start;

/* Start and end addresses of our kernel */
extern const uint8_t _start[], _end[];

extern void *fdt_addr;
extern size_t fdt_size;

/* Callbacks to be called when we parse FDT */
extern void (*fdt_handle_node_cb)(const char *);
extern void (*fdt_handle_end_node_cb)(void);
extern void (*fdt_handle_property_cb)(const char *,
                                      const char *,
                                      void *,
                                      const unsigned);

/* PCIe controller info captured from FDT. Populated by board.c, consumed by
 * init-asinfo.c.
 */
#define MAX_PCI_WINDOWS 8

struct pci_fdt_window {
    uint64_t pci_addr;
    uint64_t cpu_addr;
    uint64_t size;
    uint32_t flags; /* 0x01=IO, 0x02=MEM32, 0x03=MEM64+pref */
};

struct pci_fdt_info {
    paddr_t ecam_base;
    size_t ecam_size;
    char compatible[64];
    unsigned num_windows;
    struct pci_fdt_window windows[MAX_PCI_WINDOWS];
    uint32_t irq_base; /* PLIC IRQ for INTA */
    /* DesignWare PCIe controller-specific (FU740 / Unmatched). dbi_base is the
     * DBI register window (contains iATU + MSI controller regs); msi_irq is the
     * PLIC IRQ the MSI controller asserts when any vector fires. Both zero when
     * the platform doesn't expose a DW controller (e.g. QEMU virt's generic
     * ECAM).
     */
    paddr_t dbi_base;
    size_t dbi_size;
    uint32_t msi_irq; /* PLIC IRQ for MSI aggregate */
};

extern struct pci_fdt_info pci_fdt;

#endif
