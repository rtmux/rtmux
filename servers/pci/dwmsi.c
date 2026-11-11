/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * The FU740 (SiFive Unmatched) root complex uses Synopsys DesignWare PCIe IP
 * with an internal MSI controller. Devices on the bus send MSI writes to an
 * address the controller has been told to trap; the controller catches each
 * write, sets the corresponding bit in MSI_INTR_STATUS, and asserts one
 * aggregate PLIC IRQ (56 on FU740). The driver (us) reads MSI_INTR_STATUS to
 * find out which MSI slot fired, acks by writing the same bits back, and
 * demuxes to the waiting consumer.
 *
 * DBI register offsets (standard Synopsys DW PCIe host IP): 0x820 MSI_ADDR_LO
 * MSI target PA, low 32 bits 0x824 MSI_ADDR_HI MSI target PA, high 32 bits
 * 0x828 MSI_INTR_ENABLE_0 per-vector enable (bit N = vector N) 0x82C
 * MSI_INTR_MASK_0 per-vector mask 0x830 MSI_INTR_STATUS_0 per-vector pending,
 * write-1-to-clear
 *
 * The "_0" suffix means group 0 = vectors 0..31. DW supports up to 8 groups for
 * 256 vectors; we use just group 0 for now (32 MSI slots shared across every
 * attached device, plenty for a development workload).
 *
 * Stage 3: mmap DBI, program target, unmask all 32 slots, attach IST to PLIC
 * msi_irq, demux fires to a log message. Stage 4 will add per-slot notification
 * delivery so drivers get the event.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "globals.h"
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <arch/plic-vector.h>

/* Standard DW PCIe MSI block, relative to DBI base. */
#define DWMSI_ADDR_LO 0x820
#define DWMSI_ADDR_HI 0x824
#define DWMSI_INTR_ENABLE_0 0x828
#define DWMSI_INTR_MASK_0 0x82C
#define DWMSI_INTR_STATUS_0 0x830

#define DWMSI_NVECTORS 32 /* group 0 only */

static paddr_t dwmsi_target_pa; /* MSI target PA     */
static void *dwmsi_target_va;   /* 4 KiB reserved page */
static uint32_t dwmsi_vector_kernel;
static int dwmsi_iid = -1;
static int dwmsi_ready;

static inline uint32_t dbi_rd(unsigned off)
{
    return pci_dw_dbi_read32(off);
}
static inline void dbi_wr(unsigned off, uint32_t val)
{
    pci_dw_dbi_write32(off, val);
}

/* ------------------------------------------------------------------ *
 * IST — runs in its own thread, demuxes MSI_INTR_STATUS. See
 * docs/irq-contract.md for the hard-IRQ versus IST ownership split: the kernel
 * claim path only wakes us, and this thread drains and unmasks after the
 * controller is quiet.
 */

static void *dwmsi_ist_thread(void *arg)
{
    (void) arg;

    {
        /* one above driver ISTs */
        struct sched_param sp = {.sched_priority = 22};
        (void) __sched_set(0, 0, SCHED_RR, &sp);
    }
    thread_ctl(RTMUX_TCTL_IO, 0);

    dwmsi_iid = interrupt_attach_thread((int) dwmsi_vector_kernel, 0);
    if (dwmsi_iid == -1) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR,
                  "DWMSI: interrupt_attach_thread(vector=%u) failed: %s",
                  dwmsi_vector_kernel, strerror(errno));
        return NULL;
    }

    for (;;) {
        interrupt_wait(0, NULL);

        /* Drain the whole status until no bits remain; a burst of MSIs all
         * arriving between two unmask/wait cycles must not be left behind.
         */
        uint32_t status;
        while ((status = dbi_rd(DWMSI_INTR_STATUS_0)) != 0) {
            /* Write-1-to-clear — ack only the bits we observed. */
            dbi_wr(DWMSI_INTR_STATUS_0, status);
            for (int b = 0; b < DWMSI_NVECTORS; b++) {
                if (!(status & (1u << b)))
                    continue;

                /* Stage 3: just log. Stage 4 will forward to the driver that
                 * owns slot b via a notification on its channel.
                 */
                pci_slogf(_SLOGC_PCI, _SLOG_INFO, "DWMSI: vector %d fired", b);
            }
        }
        __interrupt_unmask((int) dwmsi_vector_kernel, dwmsi_iid);
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Public entry points *
 */

int pci_dwmsi_init(paddr_t dbi_base, size_t dbi_size, uint32_t plic_irq)
{
    pthread_t tid;

    if (dbi_size == 0 || plic_irq == 0)
        return -1; /* caller not on a DW platform */
    if (dwmsi_ready)
        return 0; /* already up */
    if (!pci_dw_dbi_ready()) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "DWMSI: DBI window is not mapped");
        return -1;
    }

    (void) dbi_base;

    /* Reserve a page as the MSI target. The DW controller traps posted writes
     * to this PA before they reach RAM, so the page's contents are never
     * touched -- it exists only as a unique address the RC can identify.
     */
    dwmsi_target_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_ANON | MAP_SHARED, NOFD, 0);
    if (dwmsi_target_va == MAP_FAILED) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "DWMSI: target mmap: %s",
                  strerror(errno));
        return -1;
    }
    off_t pa;
    if (mem_offset(dwmsi_target_va, NOFD, 4096, &pa, NULL) == -1) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "DWMSI: target mem_offset: %s",
                  strerror(errno));
        munmap(dwmsi_target_va, 4096);
        return -1;
    }
    dwmsi_target_pa = (paddr_t) pa;

    /* Program the controller.
     *   ADDR_LO / ADDR_HI ← target PA
     *   INTR_ENABLE ← all 1s (all 32 slots active)
     *   INTR_MASK   ← 0     (no per-slot mask)
     *   INTR_STATUS ← all 1s (clear any stale pending)
     */
    dbi_wr(DWMSI_ADDR_LO, (uint32_t) (dwmsi_target_pa & 0xFFFFFFFFu));
    dbi_wr(DWMSI_ADDR_HI, (uint32_t) (dwmsi_target_pa >> 32));
    dbi_wr(DWMSI_INTR_STATUS_0, 0xFFFFFFFFu);
    dbi_wr(DWMSI_INTR_MASK_0, 0x00000000u);
    dbi_wr(DWMSI_INTR_ENABLE_0, 0xFFFFFFFFu);

    dwmsi_vector_kernel = PLIC_VECTOR_BASE + plic_irq;

    if (pthread_create(&tid, NULL, dwmsi_ist_thread, NULL) != 0) {
        pci_slogf(_SLOGC_PCI, _SLOG_ERROR, "DWMSI: pthread_create: %s",
                  strerror(errno));
        munmap(dwmsi_target_va, 4096);
        return -1;
    }

    dwmsi_ready = 1;
    pci_slogf(_SLOGC_PCI, _SLOG_INFO,
              "DWMSI: ready, target PA %#lx, PLIC IRQ %u (vector %u)",
              (unsigned long) dwmsi_target_pa, plic_irq, dwmsi_vector_kernel);
    return 0;
}

uint32_t pci_dwmsi_target_addr(void)
{
    return dwmsi_ready ? (uint32_t) dwmsi_target_pa : 0;
}
