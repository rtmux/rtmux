/**
 * \file board.c
 *
 * Board initialization, occurs right after the assembly code in head.S
 *
 * \license SPDX-License-Identifier: BSD-2-Clause
 */

#include <startup.h>
#include <startup-api.h>
#include <sbi.h>
#include <sys/fdt.h>

#define INCPTR(_ptr, _by) _ptr = TOPTR((char *) (_ptr) + (_by))

extern uint64_t _boot_a0, _boot_a1;
extern char boot_command_line[];

void *fdt_addr;
size_t fdt_size;

static unsigned num_cpus_detected;
unsigned hart_ids[CONFIG_PROCESSORS_MAX];

/* PLIC S-mode context index per hart ID.
 *
 * The PLIC's per-context registers (SENABLE, SPRIORITY, SCLAIM) are laid out by
 * CONTEXT, not by hart — and the order of contexts is determined by the PLIC's
 * "interrupts-extended" FDT property.
 *
 * On QEMU virt: every hart has (M, S) contexts, so hart N's S-mode context is
 * (2*N + 1). The old RTMux formula hart*0x100 happened to match this.
 *
 * On SiFive FU540/FU740 (Unleashed/Unmatched): hart 0 is the E-mode monitor
 * core with no MMU — it contributes only one M-only context (context 0). Harts
 * 1..4 each contribute (M, S), so hart 1's S-mode context is 2, hart 2's is 4,
 * etc. The old formula pointed at the wrong context and the OS never received
 * external interrupts.
 *
 * We compute this table in plic_s_ctx_compute() after the FDT walk has seen
 * every cpu@N node's mmu-type property. Entries for harts without S-mode (or
 * without a cpu@ node at all) stay (unsigned)~0u.
 *
 * Sized PLIC_MAX_HARTS so we can index by raw FDT hart ID, not by position in
 * hart_ids[].
 */
#define PLIC_MAX_HARTS 16
unsigned plic_s_ctx[PLIC_MAX_HARTS];
static int hart_has_cpu_node[PLIC_MAX_HARTS];
static int hart_has_mmu[PLIC_MAX_HARTS];

/* Pending hart: handle_node_early records the hart ID from cpu@N, but doesn't
 * add it to hart_ids[] yet. handle_end_node_early commits it only if mmu-type
 * was present and not "riscv,none" and the "status" property is absent, "okay"
 * or "ok" (the properties may arrive in either order, so nothing is decided
 * until the node closes). This filters out cores without virtual memory support
 * (e.g. SiFive S7 monitor core on the U740) and harts firmware marked
 * disabled.
 */
static int pending_hart_valid;
static unsigned pending_hart_id;
static unsigned pending_hart_depth;
static int pending_mmu_seen;
static int pending_mmu_ok;
static int pending_status_ok;
static char pending_status[16];
static unsigned fdt_depth;

/* CPU model info captured from FDT cpu@N nodes */
char cpu_isa_string[64]; /* riscv,isa (e.g., "rv64imafdch_zicsr_...") */
char cpu_compatible[64]; /* compatible (e.g., "riscv" or "sifive,u74-mc") */

/* Board model and compatible strings from FDT root "/" node */
static char board_model[128]; /* /model (e.g., "SiFive HiFive Unmatched A00") */
char board_compatible[128]; /* /compatible (e.g., "sifive,hifive-unmatched-a00")
                             */

/* RTC address captured from FDT (goldfish-rtc on QEMU virt) */
static paddr_t rtc_base;
static bool rtc_is_goldfish;
static paddr_t pending_rtc_base;

/* Device base addresses discovered from FDT. These replace the hardcoded
 * constants that were in qemu_virt.h. A value of 0 means "not found in FDT" —
 * consumers must check.
 */
paddr_t uart_base;      /* Console UART MMIO base */
static uint32_t uart_irq; /* Console UART interrupt number */
paddr_t plic_base;      /* PLIC base address */
size_t plic_size;       /* PLIC region size */
paddr_t clint_base;     /* CLINT base address */
paddr_t ram_phys_start; /* Physical RAM start (from memory@ node) */

/* PCIe controller info — struct defined in startup-api.h */
struct pci_fdt_info pci_fdt;

static bool node_is_pci;
static paddr_t pending_pci_ecam;
static size_t pending_pci_ecam_size;

/* Full reg[] + interrupts[] arrays buffered from the PCI node. The DW PCIe
 * binding (FU740) puts DBI first and config (ECAM) second, and publishes the
 * MSI aggregate PLIC IRQ alongside INTA..INTD in interrupts[]. We buffer both
 * arrays in order and match against reg-names / interrupt-names once they
 * arrive to extract the right blocks. QEMU virt's pci-host-ecam-generic has
 * only one reg entry and uses interrupt-map for INTA..D, so the buffered path
 * degenerates cleanly for it.
 */
#define MAX_PCI_REG_ENTRIES 4
struct pending_reg_entry {
    paddr_t base;
    size_t size;
};
static struct pending_reg_entry pending_pci_reg[MAX_PCI_REG_ENTRIES];
static unsigned pending_pci_reg_count;
#define MAX_PCI_INTERRUPTS 8
static uint32_t pending_pci_interrupts[MAX_PCI_INTERRUPTS];
static unsigned pending_pci_interrupts_count;
/* reg-names / interrupt-names are null-separated string blobs. We keep them raw
 * and index-scan when needed. Only the first 64 B of each is kept (generously
 * larger than the ~24 B the FU740 DT uses).
 */
static char pending_pci_reg_names[64];
static unsigned pending_pci_reg_names_len;
static char pending_pci_irq_names[64];
static unsigned pending_pci_irq_names_len;

/* Per-node pending state for two-phase FDT device identification. FDT property
 * order is not guaranteed, so "compatible" may arrive before or after "reg". We
 * accumulate both and commit when both have been seen (on node transition or at
 * commit time).
 */
static bool node_is_plic;
static paddr_t pending_plic_base;
static size_t pending_plic_size;

static bool node_is_uart;
static paddr_t pending_uart_base;
static uint32_t pending_uart_irq;

paddr_t initramfs_phys_start, initramfs_phys_end;

void rvq_putc_ser_dbg(int c)
{
    sbi_ecall_console_putc(c);
};

#define _BE(x) ENDIAN_RET32(x)

#define FDT_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define FDT_TAGALIGN(x) (FDT_ALIGN((x), FDT_TAGSIZE))

#ifdef CONFIG_FDT_DEBUG
#define TRACETAG(fmt, ...)                          \
    do {                                            \
        if (kopt.loglevel >= 2) {                   \
            for (int _i = 0; _i < indent_lvl; _i++) \
                kprintf(" ");                       \
            kprintf(fmt, ##__VA_ARGS__);            \
        }                                           \
    } while (0)
#define IF_FDT_DEBUG(x) x
#else
#define TRACETAG(fmt, ...) \
    do {                   \
    } while (0)
#define IF_FDT_DEBUG(x)
#endif

/* Find the index of <target> in a null-separated string blob (like reg-names or
 * interrupt-names).
 *
 * Returns the ordinal (0-based) or -1 if not found. An empty blob returns -1.
 */
static int ns_list_index(const char *blob,
                         unsigned blob_len,
                         const char *target)
{
    unsigned off = 0;
    int idx = 0;
    size_t tlen = strlen(target);
    while (off < blob_len) {
        const char *s = blob + off;
        size_t slen = strlen(s);
        if (slen == tlen && memcmp(s, target, tlen) == 0)
            return idx;
        off += (unsigned) (slen + 1);
        idx++;
    }
    return -1;
}

/**
 * \brief Check if a prefix appears in a null-separated compatible string list.
 *
 * FDT "compatible" properties contain multiple null-separated strings (e.g.,
 * "sifive,fu540-c000-plic\0sifive,plic-1.0.0"). This function scans all of
 * them.
 *
 * \param data Raw property data
 * \param datalen Total length of the property (including all nulls)
 * \param prefix Prefix to match against each string
 * \param pfxlen Length of prefix
 * \return true if any string starts with prefix
 */
static bool compat_match(const void *data,
                         unsigned datalen,
                         const char *prefix,
                         unsigned pfxlen)
{
    const char *p = data;
    const char *end = p + datalen;

    while (p < end) {
        if (!strncmp(p, prefix, pfxlen))
            return true;
        /* Skip to the next null-separated string */
        while (p < end && *p)
            p++;
        p++; /* skip the '\0' */
    }
    return false;
}

/* Callbacks to be called when we parse FDT */
void (*fdt_handle_node_cb)(const char *);
void (*fdt_handle_end_node_cb)(void);
void (*fdt_handle_property_cb)(const char *,
                               const char *,
                               void *,
                               const unsigned);

/**
 * \brief Handle a given node.
 */
static void handle_node_early(const char *name)
{
#define NODE_MATCH(_n) if (strncmp(name, _n, strlen(_n)) == 0)
    fdt_depth++;
    NODE_MATCH("cpu@")
    {
        /* Extract hart ID from "cpu@X" node name. Don't add to hart_ids[] yet —
         * wait for the node to close so mmu-type and status are both known.
         */
        const char *p = name + 4;
        unsigned id = 0;
        while (*p) {
            if (*p >= '0' && *p <= '9')
                id = id * 16 + (unsigned) (*p - '0');
            else if (*p >= 'a' && *p <= 'f')
                id = id * 16 + (unsigned) (*p - 'a' + 10);
            else
                break;
            p++;
        }
        pending_hart_valid = 1;
        pending_hart_id = id;
        pending_hart_depth = fdt_depth;
        pending_mmu_seen = 0;
        pending_mmu_ok = 0;
        pending_status_ok = 1;
        pending_status[0] = '\0';
        /* Record the presence of this cpu@N node immediately — it must NOT be
         * gated on the optional "mmu-type" property. On SiFive FU740
         * (Unmatched), cpu@0 is the E-mode S7 monitor core; it has neither
         * mmu-type nor a usable MMU, but the PLIC still assigns it an
         * M-mode-only context (context 0) that shifts every subsequent S-mode
         * context by +1. Missing this caused plic_s_ctx[1..4] to be off-by-one
         * and the UART IRQ to land in hart N's M-mode context instead of
         * S-mode.
         */
        if (id < PLIC_MAX_HARTS)
            hart_has_cpu_node[id] = 1;
    }

    /* Commit any pending device from the previous node, then reset. This
     * handles the case where "compatible" arrived after "reg".
     */
    if (node_is_pci && pending_pci_ecam && !pci_fdt.ecam_base) {
        pci_fdt.ecam_base = pending_pci_ecam;
        pci_fdt.ecam_size = pending_pci_ecam_size;
        LOGF_D("PCIe ECAM at %#lx, size %#lx\n",
               (unsigned long) pci_fdt.ecam_base,
               (unsigned long) pci_fdt.ecam_size);
    }
    if (node_is_plic && pending_plic_base && !plic_base) {
        plic_base = pending_plic_base;
        plic_size = pending_plic_size;
        LOGF_D("PLIC at %#lx, size %#lx\n", (unsigned long) plic_base,
               (unsigned long) plic_size);
    }
    if (node_is_uart && pending_uart_base && !uart_base) {
        uart_base = pending_uart_base;
        uart_irq = pending_uart_irq;
        LOGF_D("UART at %#lx, irq %u\n", (unsigned long) uart_base, uart_irq);
    }
    if (rtc_is_goldfish && pending_rtc_base && !rtc_base) {
        rtc_base = pending_rtc_base;
        LOGF_D("RTC (goldfish) at %#lx\n", (unsigned long) rtc_base);
    }
    node_is_plic = false;
    node_is_uart = false;
    node_is_pci = false;
    rtc_is_goldfish = false;
    pending_plic_base = 0;
    pending_plic_size = 0;
    pending_uart_base = 0;
    pending_uart_irq = 0;
    pending_rtc_base = 0;
    pending_pci_ecam = 0;
    pending_pci_ecam_size = 0;
}

/**
 * \brief Close a node: admit the pending cpu@N hart if every gate holds.
 *
 * The boot hart is never withheld on status: it is already running this
 * kernel, so a firmware "disabled" on it can only be stale.
 */
static void handle_end_node_early(void)
{
    if (pending_hart_valid && fdt_depth == pending_hart_depth) {
        int boot = (pending_hart_id == (unsigned) _boot_a0);

        if (pending_mmu_seen && pending_mmu_ok && !pending_status_ok) {
            if (boot)
                kprintf("fdt: hart %u is the boot hart, status=%s ignored\n",
                        pending_hart_id, pending_status);
            else
                kprintf("fdt: hart %u withheld (status=%s)\n",
                        pending_hart_id, pending_status);
        }
        if (pending_mmu_seen && pending_mmu_ok &&
            (pending_status_ok || boot) &&
            num_cpus_detected < CONFIG_PROCESSORS_MAX)
            hart_ids[num_cpus_detected++] = pending_hart_id;
        pending_hart_valid = 0;
    }
    if (fdt_depth)
        fdt_depth--;
}

#define WORD_AT(x) ENDIAN_RET32(*((uint32_t *) x))
#define DWORD_AT(x) ENDIAN_RET64(*((uint64_t *) x))

/**
 * \brief Callback to set boot command line, initrd addresses and CPU frequency.
 */
static void handle_property_early(const char *node,
                                  const char *prop,
                                  void *data,
                                  const unsigned datalen)
{
#define NODE_PROP_MATCH(_n, _p) if (!strcmp(node, _n) && !strcmp(prop, _p))

    /* Root node properties — board identity */
    if (node[0] == '\0' && data && datalen > 0) {
        if (!strcmp(prop, "model") && !board_model[0]) {
            size_t n = datalen < sizeof(board_model) ? datalen
                                                     : sizeof(board_model) - 1;
            memcpy(board_model, data, n);
            board_model[n] = '\0';
        } else if (!strcmp(prop, "compatible") && !board_compatible[0]) {
            size_t n = datalen < sizeof(board_compatible)
                           ? datalen
                           : sizeof(board_compatible) - 1;
            memcpy(board_compatible, data, n);
            board_compatible[n] = '\0';
        }
    }

    if (!strcmp(node, "chosen")) {
        if (!strcmp(prop, "bootargs")) {
            size_t n = strlen(data);
            /* strcpy writes n + 1 bytes (data + NUL) into a
             * CONFIG_MAX_BOOTLINE_SIZE-byte buffer; need room for the
             * terminator, so n must be strictly less than the cap.
             */
            if (n >= CONFIG_MAX_BOOTLINE_SIZE)
                crash(
                    "Boot command line is too long (%d bytes, maximum %d bytes "
                    "allowed)\n",
                    n, CONFIG_MAX_BOOTLINE_SIZE - 1);
            strcpy(boot_command_line, data);
            kprintf("Boot command line: %s\n", boot_command_line);
        } else if (!strcmp(prop, "linux,initrd-start")) {
            initramfs_phys_start = (datalen == sizeof(uint64_t))
                                       ? ENDIAN_RET64(*(uint64_t *) data)
                                       : WORD_AT(data);
        } else if (!strcmp(prop, "linux,initrd-end")) {
            initramfs_phys_end = (datalen == sizeof(uint64_t))
                                     ? ENDIAN_RET64(*(uint64_t *) data)
                                     : WORD_AT(data);
        }
    } else

        NODE_PROP_MATCH("cpus", "timebase-frequency")
        {
            if (datalen == sizeof(uint32_t)) {
                kopt.timebase_freq = WORD_AT(data);
                LOGF_D("timebase frequency: %lu Hz\n", kopt.timebase_freq);
            }
        }
    else

        /* Process cpu@N properties: filter by mmu-type, capture model info */
        if (!strncmp(node, "cpu@", 4))
    {
        if (!strcmp(prop, "mmu-type") && pending_hart_valid && datalen > 0) {
            /* Record MMU status for the PLIC context calculation.
             * hart_has_cpu_node[] was already set on node entry in
             * handle_node_early — monitor cores without mmu-type (FU740 cpu@0)
             * still need to be counted for PLIC offset arithmetic.
             */
            pending_mmu_seen = 1;
            pending_mmu_ok = (strcmp((const char *) data, "riscv,none") != 0);
            if (pending_hart_id < PLIC_MAX_HARTS)
                hart_has_mmu[pending_hart_id] = pending_mmu_ok;
            if (!pending_mmu_ok)
                kprintf("cpu@%u: skipped (mmu-type=%s)\n", pending_hart_id,
                        (const char *) data);
        } else if (!strcmp(prop, "status") && pending_hart_valid) {
            /* An empty or malformed status is not "okay": withhold. */
            size_t n = data ? datalen : 0;

            if (n > sizeof(pending_status) - 1)
                n = sizeof(pending_status) - 1;
            if (n)
                memcpy(pending_status, data, n);
            pending_status[n] = '\0';
            pending_status_ok = !strcmp(pending_status, "okay") ||
                                !strcmp(pending_status, "ok");
        } else if (!strcmp(prop, "riscv,isa") && !cpu_isa_string[0] &&
                   datalen > 0) {
            size_t n = datalen < sizeof(cpu_isa_string)
                           ? datalen
                           : sizeof(cpu_isa_string) - 1;
            memcpy(cpu_isa_string, data, n);
            cpu_isa_string[n] = '\0';
        } else if (!strcmp(prop, "compatible") && !cpu_compatible[0] &&
                   datalen > 0) {
            size_t n = datalen < sizeof(cpu_compatible)
                           ? datalen
                           : sizeof(cpu_compatible) - 1;
            memcpy(cpu_compatible, data, n);
            cpu_compatible[n] = '\0';
        }
    }
    else

        /* Capture RTC base address, but only for goldfish-rtc (QEMU virt) */
        if (!strncmp(node, "rtc@", 4))
    {
        if (!strcmp(prop, "compatible") && datalen > 0) {
            rtc_is_goldfish =
                !strcmp((const char *) data, "google,goldfish-rtc");
        }
        if (!strcmp(prop, "reg") && datalen >= 16) {
            uint32_t *cells = data;
            pending_rtc_base = ((uint64_t) ENDIAN_RET32(cells[0]) << 32) |
                               ENDIAN_RET32(cells[1]);
        }
        if (rtc_is_goldfish && pending_rtc_base && !rtc_base) {
            rtc_base = pending_rtc_base;
            LOGF_D("RTC (goldfish) at %#lx\n", (unsigned long) rtc_base);
        }
    }
    else

        /* PLIC — may appear as "plic@..." or "interrupt-controller@...".
         * Identified by compatible string containing "plic". Properties may
         * arrive in any order, so we accumulate into pending_plic_* and commit
         * when both compatible and reg are seen.
         */
        if (!strncmp(node, "plic@", 5) ||
            !strncmp(node, "interrupt-controller@", 21))
    {
        if (!strcmp(prop, "compatible") && datalen > 0) {
            node_is_plic = (compat_match(data, datalen, "sifive,plic", 11) ||
                            compat_match(data, datalen, "riscv,plic", 10));
        }
        if (!strcmp(prop, "reg") && datalen >= 16) {
            uint32_t *cells = data;
            pending_plic_base = ((uint64_t) ENDIAN_RET32(cells[0]) << 32) |
                                ENDIAN_RET32(cells[1]);
            pending_plic_size = ((uint64_t) ENDIAN_RET32(cells[2]) << 32) |
                                ENDIAN_RET32(cells[3]);
        }
        /* Commit as soon as both compatible and reg are known */
        if (node_is_plic && pending_plic_base && !plic_base) {
            plic_base = pending_plic_base;
            plic_size = pending_plic_size;
            LOGF_D("PLIC at %#lx, size %#lx\n", (unsigned long) plic_base,
                   (unsigned long) plic_size);
        }
    }
    else

        /* CLINT — "clint@..." node */
        if (!strncmp(node, "clint@", 6))
    {
        if (!strcmp(prop, "reg") && datalen >= 16) {
            uint32_t *cells = data;
            clint_base = ((uint64_t) ENDIAN_RET32(cells[0]) << 32) |
                         ENDIAN_RET32(cells[1]);
            LOGF_D("CLINT at %#lx\n", (unsigned long) clint_base);
        }
    }
    else

        /* Console UART — "serial@..." or "uart@..." node. Identified by
         * compatible string containing "16550". Only capture the first one
         * found. Properties may arrive in any order, so accumulate into
         * pending_uart_* first.
         */
        if ((!strncmp(node, "serial@", 7) || !strncmp(node, "uart@", 5)) &&
            !uart_base)
    {
        if (!strcmp(prop, "compatible") && datalen > 0) {
            const char *s = (const char *) data;
            node_is_uart = (!strncmp(s, "ns16550", 7));
        }
        if (!strcmp(prop, "reg") && datalen >= 16) {
            uint32_t *cells = data;
            pending_uart_base = ((uint64_t) ENDIAN_RET32(cells[0]) << 32) |
                                ENDIAN_RET32(cells[1]);
        }
        if (!strcmp(prop, "interrupts") && datalen >= 4) {
            pending_uart_irq = ENDIAN_RET32(*(uint32_t *) data);
        }
        /* Commit as soon as both compatible and reg are known */
        if (node_is_uart && pending_uart_base && !uart_base) {
            uart_base = pending_uart_base;
            uart_irq = pending_uart_irq;
            LOGF_D("UART at %#lx, irq %u\n", (unsigned long) uart_base,
                   uart_irq);
        }
    }
    else

        /* Physical RAM start — from "memory@..." node */
        if (!strncmp(node, "memory@", 7) && !ram_phys_start)
    {
        if (!strcmp(prop, "reg") && datalen >= 16) {
            uint64_t *ptr = data;
            ram_phys_start = ENDIAN_RET64(ptr[0]);
            LOGF_D("RAM starts at %#lx\n", (unsigned long) ram_phys_start);
        }
    }
    else

        /* PCIe controller — "pci@..." or "pcie@..." node. Captures ECAM base
         * (reg), address windows (ranges), IRQs (interrupt-map). Identified by
         * compatible string containing "pci" or "pcie".
         */
        if (!strncmp(node, "pci@", 4) || !strncmp(node, "pcie@", 5))
    {
        /* Diagnostic: echo every property we see under the PCI node at D-level
         * so it stays out of a normal boot but a U740 boot with -v shows
         * exactly what the U-Boot DTB exposes. Removed once U740 MSI-X is
         * stable.
         */
        LOGF_D("fdt: pcie node '%s' prop '%s' datalen %u\n", node, prop,
               (unsigned) datalen);
        if (!strcmp(prop, "compatible") && datalen > 0) {
            node_is_pci =
                (compat_match(data, datalen, "pci-host-ecam", 13) ||
                 compat_match(data, datalen, "sifive,fu740-pcie", 17));
            if (node_is_pci) {
                size_t n = datalen < sizeof(pci_fdt.compatible)
                               ? datalen
                               : sizeof(pci_fdt.compatible) - 1;
                memcpy(pci_fdt.compatible, data, n);
                pci_fdt.compatible[n] = '\0';
            }
        }
        if (!strcmp(prop, "reg") && datalen >= 16) {
            uint32_t *cells = data;
            unsigned nreg = (unsigned) (datalen / 16);
            if (nreg > MAX_PCI_REG_ENTRIES)
                nreg = MAX_PCI_REG_ENTRIES;
            for (unsigned i = 0; i < nreg; i++) {
                uint32_t *e = cells + i * 4;
                pending_pci_reg[i].base =
                    ((uint64_t) ENDIAN_RET32(e[0]) << 32) | ENDIAN_RET32(e[1]);
                pending_pci_reg[i].size =
                    ((uint64_t) ENDIAN_RET32(e[2]) << 32) | ENDIAN_RET32(e[3]);
            }
            pending_pci_reg_count = nreg;
            /* Default ECAM guess = reg[0]; overridden at commit time if
             * reg-names says otherwise.
             */
            pending_pci_ecam = pending_pci_reg[0].base;
            pending_pci_ecam_size = pending_pci_reg[0].size;
        }
        if (!strcmp(prop, "reg-names") && datalen > 0) {
            unsigned n = datalen < sizeof(pending_pci_reg_names)
                             ? datalen
                             : sizeof(pending_pci_reg_names) - 1;
            memcpy(pending_pci_reg_names, data, n);
            pending_pci_reg_names[n] = '\0';
            pending_pci_reg_names_len = n;
        }
        if (!strcmp(prop, "interrupts") && datalen > 0) {
            unsigned n = (unsigned) (datalen / sizeof(uint32_t));
            if (n > MAX_PCI_INTERRUPTS)
                n = MAX_PCI_INTERRUPTS;
            uint32_t *cells = data;
            for (unsigned i = 0; i < n; i++)
                pending_pci_interrupts[i] = ENDIAN_RET32(cells[i]);
            pending_pci_interrupts_count = n;
        }
        if (!strcmp(prop, "interrupt-names") && datalen > 0) {
            unsigned n = datalen < sizeof(pending_pci_irq_names)
                             ? datalen
                             : sizeof(pending_pci_irq_names) - 1;
            memcpy(pending_pci_irq_names, data, n);
            pending_pci_irq_names[n] = '\0';
            pending_pci_irq_names_len = n;
        }
        /* Parse "ranges" — PCI address windows. Format per entry (7 cells):
         * pci_flags pci_hi pci_lo cpu_hi cpu_lo size_hi size_lo Per IEEE 1275
         * PCI binding, pci_flags (phys.hi cell):
         *   bit 30 (p)    : prefetchable
         *   bits 25..24   : space code — 01=I/O, 10=MEM32, 11=MEM64
         * Translate into HWI_PCI_WINDOW_* bits so hwinfo consumers don't have
         * to know the OF encoding.
         */
        if (!strcmp(prop, "ranges") && datalen >= 28 &&
            pci_fdt.num_windows == 0) {
            unsigned entry_cells = 7;
            unsigned nentries =
                (unsigned) (datalen / (entry_cells * sizeof(uint32_t)));
            uint32_t *cells = data;

            for (unsigned i = 0;
                 i < nentries && pci_fdt.num_windows < MAX_PCI_WINDOWS; i++) {
                uint32_t *e = cells + i * entry_cells;
                uint32_t of_flags = ENDIAN_RET32(e[0]);
                uint64_t pci_addr =
                    ((uint64_t) ENDIAN_RET32(e[1]) << 32) | ENDIAN_RET32(e[2]);
                uint64_t cpu_addr =
                    ((uint64_t) ENDIAN_RET32(e[3]) << 32) | ENDIAN_RET32(e[4]);
                uint64_t size =
                    ((uint64_t) ENDIAN_RET32(e[5]) << 32) | ENDIAN_RET32(e[6]);
                uint32_t ss = (of_flags >> 24) & 0x03;
                uint32_t hwi_flags = 0;

                if (ss == 0x01)
                    hwi_flags = HWI_PCI_WINDOW_IO;
                else if (ss == 0x02 || ss == 0x03)
                    hwi_flags = HWI_PCI_WINDOW_MEM;
                if (of_flags & (1U << 30))
                    hwi_flags |= HWI_PCI_WINDOW_PREFETCH;

                /* Skip config-space entries (ss=00) — not an address window */
                if (hwi_flags == 0)
                    continue;

                unsigned w = pci_fdt.num_windows++;
                pci_fdt.windows[w].pci_addr = pci_addr;
                pci_fdt.windows[w].cpu_addr = cpu_addr;
                pci_fdt.windows[w].size = size;
                pci_fdt.windows[w].flags = hwi_flags;
                LOGF_D(
                    "PCIe window %u: flags=%02x cpu=%#lx pci=%#lx size=%#lx\n",
                    w, pci_fdt.windows[w].flags, (unsigned long) cpu_addr,
                    (unsigned long) pci_addr, (unsigned long) size);
            }
        }
        /* Parse interrupt-map to find INTA IRQ number. First entry with pin=1
         * gives us the base IRQ.
         */
        if (!strcmp(prop, "interrupt-map") && datalen >= 40 &&
            !pci_fdt.irq_base) {
            uint32_t *cells = data;
            /* interrupt-map entry: dev_hi dev_mid dev_lo pin phandle irq_no For
             * QEMU virt: 6 cells per entry; pin is at [3], irq at [5] For
             * FU740: same layout
             */
            if (datalen >= 6 * sizeof(uint32_t)) {
                uint32_t pin = ENDIAN_RET32(cells[3]);
                uint32_t irq = ENDIAN_RET32(cells[5]);
                if (pin == 1) {
                    pci_fdt.irq_base = irq;
                    LOGF_D("PCIe INTA IRQ = %u\n", irq);
                }
            }
        }
        /* Commits are deliberately idempotent and re-runnable: FDT property
         * order is not guaranteed, so when "reg" arrives before "reg-names" we
         * first commit ecam_base = reg[0] as a best guess, and later when
         * "reg-names" arrives we re-commit pointing at the real "config" entry.
         * Same story for interrupts / interrupt-names. Using "last write wins"
         * rather than "first wins" ends up with the correct final state
         * regardless of order.
         */
        if (node_is_pci && pending_pci_reg_count > 0) {
            int cfg_idx = ns_list_index(pending_pci_reg_names,
                                        pending_pci_reg_names_len, "config");
            int dbi_idx = ns_list_index(pending_pci_reg_names,
                                        pending_pci_reg_names_len, "dbi");
            unsigned ecam_i = 0;
            if (cfg_idx >= 0 && (unsigned) cfg_idx < pending_pci_reg_count)
                ecam_i = (unsigned) cfg_idx;
            pci_fdt.ecam_base = pending_pci_reg[ecam_i].base;
            pci_fdt.ecam_size = pending_pci_reg[ecam_i].size;
            if (dbi_idx >= 0 && (unsigned) dbi_idx < pending_pci_reg_count) {
                pci_fdt.dbi_base = pending_pci_reg[(unsigned) dbi_idx].base;
                pci_fdt.dbi_size = pending_pci_reg[(unsigned) dbi_idx].size;
            }
        }
        if (node_is_pci && pending_pci_interrupts_count > 0) {
            int msi_idx = ns_list_index(pending_pci_irq_names,
                                        pending_pci_irq_names_len, "msi");
            int inta_idx = ns_list_index(pending_pci_irq_names,
                                         pending_pci_irq_names_len, "inta");
            if (msi_idx >= 0 &&
                (unsigned) msi_idx < pending_pci_interrupts_count)
                pci_fdt.msi_irq = pending_pci_interrupts[msi_idx];
            if (inta_idx >= 0 &&
                (unsigned) inta_idx < pending_pci_interrupts_count)
                pci_fdt.irq_base = pending_pci_interrupts[(unsigned) inta_idx];
        }
    }
    else

        if (!strncmp(node, "mmode_resv0@", 12) && !strcmp(prop, "reg"))
    {
        /* Proceed through all base/len pairs */
        for (unsigned i = 0; i < datalen / (2 * sizeof(uint64_t)); i++) {
            uint64_t *ptr = data;
            uint64_t regbase = ENDIAN_RET64(ptr[0]);
            uint64_t regsize = ENDIAN_RET64(ptr[1]);

            avoid_ram(regbase, regsize);
            LOGF_D("Reserved memory at %#p, size %#p\n", regbase, regsize);
        }
    }
}


/**
 * \brief Parse the FDT and initialize all required variables.
 *
 * \param[out] fdt_size Address of variable to record FDT size
 */
void parse_fdt(size_t *fdt_size)
{
    const struct fdt_header *fdt = fdt_addr;
    struct fdt_node_header *node =
        TOPTR((uintptr_t) fdt + _BE(fdt->off_dt_struct));
    const char *values = TOPTR((uintptr_t) fdt + _BE(fdt->off_dt_strings));
    const char *curr_node = "";
#ifdef CONFIG_FDT_DEBUG
    int indent_lvl = 0;
#endif
    bool finished = false;

    do {
        uint32_t tag = _BE(node->tag);

        switch (tag) {
        case FDT_NOP:
            INCPTR(node, FDT_TAGSIZE);
            break;

        case FDT_BEGIN_NODE:
            curr_node = node->name;
            TRACETAG("Node: %s\n", curr_node);
            if (fdt_handle_node_cb)
                fdt_handle_node_cb(curr_node);
            /* Take the trailing zero into account */
            INCPTR(node, FDT_TAGSIZE + strlen(node->name) + 1);
            node = TOPTR(FDT_TAGALIGN((uintptr_t) node));
            if (_BE(node->tag) == FDT_END) {
                crash("Invalid tag %d in FDT\n", tag);
            }
            IF_FDT_DEBUG(indent_lvl += 2);
            break;

        case FDT_PROP: {
            struct fdt_property *prop = TOPTR(node);
            unsigned len = _BE(prop->len);
            unsigned offset = _BE(prop->nameoff);

#ifdef CONFIG_FDT_DEBUG
            if (kopt.loglevel >= 2) {
                for (int _i = 0; _i < indent_lvl; _i++)
                    kprintf(" ");
                kprintf("PROP: name=%s, len=%d", values + offset, len);
                if (*(char *) prop->data)
                    kprintf(", string=%s\n", prop->data);
                else
                    kprintf("\n");
            }
#endif
            if (fdt_handle_property_cb)
                fdt_handle_property_cb(curr_node, values + offset,
                                       len ? prop->data : NULL, len);

            INCPTR(node, sizeof(*prop));
            INCPTR(node, len);
            node = TOPTR(FDT_TAGALIGN((uintptr_t) node));
            break;
        }

        case FDT_END_NODE:
            TRACETAG("(END Node)\n");
            if (fdt_handle_end_node_cb)
                fdt_handle_end_node_cb();
            INCPTR(node, FDT_TAGSIZE);
            if (_BE(node->tag) == FDT_PROP) {
                crash("\nInvalid tag %d in FDT\n", tag);
            }
            IF_FDT_DEBUG(indent_lvl -= 2);
            break;

        case FDT_END:
            TRACETAG("(END FDT)\n");
            if (fdt_size)
                *fdt_size = ROUND(_BE(fdt->totalsize), __PAGESIZE);
            finished = 1;
            break;

        default:
            crash("Unknown tag %d in FDT @ %#p\n", tag, &node->tag);
            break;
        }
    } while (!finished);
}

/**
 * \brief Early board initialization.
 * \note Register our putchar routine here.
 *       Remove after the select_debug mechanism is ready.
 */
void board_init(void)
{
    set_print_char(rvq_putc_ser_dbg);
    for (int i = 0; i < 79; i++)
        sbi_ecall_console_putc('=');
    sbi_ecall_console_puts("\n\n");

    fdt_addr = (void *) _boot_a1;
    const struct fdt_header *fdt = fdt_addr;

    LOGF_D("Boot hart %d, FDT @ %#P\n", _boot_a0, fdt_addr);

    /* Avoid using kernel memory */
    avoid_ram((paddr_t) _start, (size_t) (_end - _start));

    /* Check the signature. If device tree is not there, we cannot continue */
    if (_BE(fdt->magic) != FDT_MAGIC) {
        crash("FDT not detected, cannot continue.\n");
    }

    LOGF_D("Devicetree version %d\n", _BE(fdt->version));

    fdt_handle_node_cb = handle_node_early;
    fdt_handle_end_node_cb = handle_end_node_early;
    fdt_handle_property_cb = handle_property_early;
    fdt_depth = 0;
    parse_fdt(&fdt_size);
    fdt_handle_end_node_cb = NULL;

    /* Compute per-hart PLIC S-mode context indices from the physical hart
     * layout we just discovered. Assumes the standard Linux/SBI convention:
     * PLIC contexts are listed in hart-number order; each hart contributes 1
     * (M-only) or 2 (M + S) contexts based on whether it has an MMU. The S-mode
     * context for hart H is the index of the M-mode context for H, plus one.
     *
     * Must run after parse_fdt so hart_has_cpu_node / hart_has_mmu are
     * populated, and before init_intrinfo / the first PLIC register access. Any
     * hart without a cpu@ node or without an MMU keeps plic_s_ctx[h] == ~0u so
     * a stray access will crash loudly rather than silently targeting the wrong
     * context.
     */
    {
        unsigned ctx = 0;
        for (unsigned h = 0; h < PLIC_MAX_HARTS; h++)
            plic_s_ctx[h] = ~0u;
        for (unsigned h = 0; h < PLIC_MAX_HARTS; h++) {
            if (!hart_has_cpu_node[h])
                continue;
            if (hart_has_mmu[h]) {
                plic_s_ctx[h] = ctx + 1;
                ctx += 2;
            } else {
                ctx += 1;
            }
        }
    }

    /* Avoid everything below _start — this is where OpenSBI firmware lives. The
     * FDT only declares a small scratch region (mmode_resv0), not the firmware
     * code itself, so we must protect it explicitly. ram_phys_start was just
     * set by parse_fdt from the "memory@" node.
     */
    volatile paddr_t start_pa = (paddr_t) _start;
    if (ram_phys_start && ram_phys_start < start_pa)
        avoid_ram(ram_phys_start, (size_t) (start_pa - ram_phys_start));

    if (board_model[0])
        kprintf("Board: %s\n", board_model);
    if (board_compatible[0])
        kprintf("Compatible: %s\n", board_compatible);
    LOGF_D("FDT size: %d bytes\n", fdt_size);

    if (initramfs_phys_start && initramfs_phys_end)
        LOGF_D("Initramfs archive (initramfs) at %p - %p\n",
               initramfs_phys_start, initramfs_phys_end);

    /* Claim RAM used by the FDT */
    avoid_ram((paddr_t) fdt_addr, fdt_size);
}

/**
 * \brief Return the number of CPUs in the system.
 */
unsigned board_smp_num_cpu(void)
{
    return num_cpus_detected;
}

/* Read the boot time from the goldfish-rtc (or any MMIO RTC whose base was
 * captured from the FDT "rtc@" node).
 *
 * Must be called BEFORE init_mmu() — the RTC physical address is accessed
 * directly (VA == PA, no mapping needed).
 *
 * The goldfish-rtc returns nanoseconds since the Unix epoch:
 *   offset 0x00: time_low  (32-bit LE)
 *   offset 0x04: time_high (32-bit LE)
 */
uint32_t rtc_boot_time;

void read_rtc_boot_time(void)
{
    if (rtc_base == 0)
        return;

    volatile uint32_t *rtc = (volatile uint32_t *) rtc_base;
    uint32_t lo = rtc[0];
    uint32_t hi = rtc[1];
    uint64_t nsec = ((uint64_t) hi << 32) | lo;
    rtc_boot_time = (uint32_t) (nsec / 1000000000ULL);
    kprintf("RTC: boot time = %u (Unix epoch)\n", rtc_boot_time);
}
