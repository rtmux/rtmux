/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* lkl-iommu-probe - prove QEMU IOMMU devices are visible to RTMux.
 *
 * This is only a hardware-path gate for the LKL DMA-containment work. The PCI
 * path proves pci-server can enumerate an IOMMU-class function; the system path
 * delegates the RISC-V IOMMU MMIO register block and probes retained DDT modes.
 * The domain path adds synthetic DID-42 command/debug/Sv39x4 probing. No path
 * attaches a real device, binds virtio DMA, or claims containment.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <hw/hwinfo-lookup.h>
#include <sys/mman.h>
#include <sys/iomsg.h>
#include <sys/riscv-iommu.h>
#include <sys/rtmux-core.h>
#include <pci-serv.h>
#include <pci.h>

#define PCI_CLASS_IOMMU 0x0806ffu
#define RISCV_IOMMU_PT_BYTES     (9U * 4096U)
#define RISCV_IOMMU_TEST_IOVA    0x400000ULL
#define RISCV_IOMMU_TEST_DID     42U
#define RISCV_IOMMU_MAX_TEST_IOVA ((512ULL * 512ULL * 512ULL - 2ULL) << 12)

static uint64_t mmio_read64(volatile void *base, size_t off)
{
    return *(volatile uint64_t *) ((volatile char *) base + off);
}

static uint32_t mmio_read32(volatile void *base, size_t off)
{
    return *(volatile uint32_t *) ((volatile char *) base + off);
}

static void mmio_write64(volatile void *base, size_t off, uint64_t value)
{
    *(volatile uint64_t *) ((volatile char *) base + off) = value;
}

static void mmio_write32(volatile void *base, size_t off, uint32_t value)
{
    *(volatile uint32_t *) ((volatile char *) base + off) = value;
}

static int wait_cqcsr(volatile void *mmio, uint32_t mask, uint32_t value,
                      uint32_t *out)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint32_t cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);

        if ((cqcsr & RISCV_IOMMU_CQCSR_ERR) != 0) {
            *out = cqcsr;
            return -1;
        }
        if ((cqcsr & mask) == value) {
            *out = cqcsr;
            return 0;
        }
    }
    *out = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
    return -1;
}

static int wait_cqh(volatile void *mmio, uint32_t value, uint32_t *out_cqh,
                    uint32_t *out_cqcsr)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint32_t cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
        uint32_t cqh = mmio_read32(mmio, RISCV_IOMMU_CQH);

        if ((cqcsr & RISCV_IOMMU_CQCSR_ERR) != 0) {
            *out_cqh = cqh;
            *out_cqcsr = cqcsr;
            return -1;
        }
        if (cqh == value) {
            *out_cqh = cqh;
            *out_cqcsr = cqcsr;
            return 0;
        }
    }
    *out_cqh = mmio_read32(mmio, RISCV_IOMMU_CQH);
    *out_cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
    return -1;
}

static int submit_cq_cmd(volatile void *mmio, volatile struct riscv_iommu_cmd *cq,
                         uint32_t *tail, struct riscv_iommu_cmd cmd,
                         uint32_t *out_cqh, uint32_t *out_cqcsr)
{
    uint32_t next = (*tail + 1U) & 1U;

    cq[*tail] = cmd;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mmio_write32(mmio, RISCV_IOMMU_CQT, next);
    if (wait_cqh(mmio, next, out_cqh, out_cqcsr) != 0)
        return -1;
    *tail = next;
    return 0;
}

#define SUBMIT_CQ_OR_FAIL(mmio, cq, tail, step, command, cqcsr, label)      \
    do {                                                                    \
        const char *step_label = (label);                                    \
                                                                            \
        if (submit_cq_cmd((mmio), (cq), (tail), (command), &step##_cqh,     \
                          &(cqcsr)) != 0) {                                 \
            printf("lkl-iommu-probe: FAIL %s cqh=%u cqcsr=%#x\n",           \
                   step_label[0] != '\0' ? step_label : #step, step##_cqh,  \
                   (cqcsr));                                                \
            rc = 1;                                                         \
            goto out;                                                       \
        }                                                                   \
    } while (0)

static int wait_ddtp_ready(volatile void *mmio, uint64_t *out)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint64_t ddtp = mmio_read64(mmio, RISCV_IOMMU_DDTP);

        if ((ddtp & RISCV_IOMMU_DDTP_BUSY) == 0) {
            *out = ddtp;
            return 0;
        }
    }
    *out = mmio_read64(mmio, RISCV_IOMMU_DDTP);
    return -1;
}

static int ddtp_set_mode(volatile void *mmio, uint64_t ppn, unsigned mode,
                         uint64_t *out)
{
    uint64_t ddtp;
    uint64_t value;

    if (wait_ddtp_ready(mmio, &ddtp) != 0)
        goto fail;
    value = (ppn << 10) | mode;
    if (mode == RISCV_IOMMU_DDTP_MODE_OFF)
        value = (ddtp & RISCV_IOMMU_DDTP_PPN_MASK) | mode;
    mmio_write64(mmio, RISCV_IOMMU_DDTP, value);
    if (wait_ddtp_ready(mmio, out) != 0)
        goto fail;
    return 0;

fail:
    *out = mmio_read64(mmio, RISCV_IOMMU_DDTP);
    return -1;
}

static int ddtp_restore_mode(volatile void *mmio, unsigned mode)
{
    uint64_t ddtp;

    if (ddtp_set_mode(mmio, 0, RISCV_IOMMU_DDTP_MODE_OFF, &ddtp) != 0)
        return -1;
    if (mode != RISCV_IOMMU_DDTP_MODE_OFF &&
        ddtp_set_mode(mmio, 0, mode, &ddtp) != 0)
        return -1;
    return ((ddtp & RISCV_IOMMU_MODE_MASK) == mode) ? 0 : -1;
}

static bool ddtp_may_reference_ddt(uint64_t ddtp)
{
    unsigned mode = (unsigned) (ddtp & RISCV_IOMMU_MODE_MASK);

    return (ddtp & RISCV_IOMMU_DDTP_BUSY) != 0 ||
           (mode >= RISCV_IOMMU_DDTP_MODE_1LVL &&
            mode <= RISCV_IOMMU_DDTP_MODE_3LVL);
}

static int wait_tr_done(volatile void *mmio, uint64_t *out_ctl,
                        uint64_t *out_response)
{
    for (unsigned i = 0; i < 100000U; i++) {
        uint64_t ctl = mmio_read64(mmio, RISCV_IOMMU_TR_REQ_CTL);

        if ((ctl & RISCV_IOMMU_TR_REQ_GO) == 0) {
            *out_ctl = ctl;
            *out_response = mmio_read64(mmio, RISCV_IOMMU_TR_RESPONSE);
            return 0;
        }
    }
    *out_ctl = mmio_read64(mmio, RISCV_IOMMU_TR_REQ_CTL);
    *out_response = mmio_read64(mmio, RISCV_IOMMU_TR_RESPONSE);
    return -1;
}

static uint64_t tr_req_ctl(uint32_t did)
{
    return RISCV_IOMMU_TR_REQ_GO | RISCV_IOMMU_TR_REQ_NW |
           ((uint64_t) did << RISCV_IOMMU_DID_SHIFT);
}

static int debug_translate(volatile void *mmio, uint32_t did, uint64_t iova,
                           uint64_t *out_ctl, uint64_t *out_response,
                           bool *tr_busy)
{
    if (wait_tr_done(mmio, out_ctl, out_response) != 0) {
        if ((*out_ctl & RISCV_IOMMU_TR_REQ_GO) != 0)
            *tr_busy = true;
        return -1;
    }
    mmio_write64(mmio, RISCV_IOMMU_TR_REQ_IOVA, iova);
    mmio_write64(mmio, RISCV_IOMMU_TR_REQ_CTL, tr_req_ctl(did));
    *tr_busy = true;
    if (wait_tr_done(mmio, out_ctl, out_response) != 0) {
        if ((*out_ctl & RISCV_IOMMU_TR_REQ_GO) == 0)
            *tr_busy = false;
        return -1;
    }
    *tr_busy = false;
    return 0;
}

static int probe_system_iommu_hwinfo(void)
{
    static const uint32_t expected_irq[] = { 36, 37, 38, 39 };
    hwi_tag *dev = hwinfo_find_device("riscv-iommu", NULL);
    uint8_t *base;
    char *strings;
    unsigned section_size;
    unsigned found = 0;

    if (!dev) {
        puts("lkl-iommu-probe: FAIL no riscv-iommu hwinfo node");
        return 1;
    }

    base = hwinfo_base();
    strings = hwinfo_strings();
    section_size = _syspage_ptr->hwinfo.entry_size;
    for (hwi_tag *tag = hwinfo_next(dev);
         (uintptr_t) tag < (uintptr_t) base + section_size;
         tag = hwinfo_next(tag)) {
        const char *name;

        if (tag->prefix.size == 0)
            break;
        name = strings + tag->prefix.name;
        if (!strcmp(name, HWI_TAG_NAME_device) ||
            !strcmp(name, HWI_TAG_NAME_bus) ||
            !strcmp(name, HWI_TAG_NAME_group))
            break;
        if (!strcmp(name, HWI_TAG_NAME_irq)) {
            struct hwi_irq *irq = (struct hwi_irq *) tag;

            if (found >= sizeof(expected_irq) / sizeof(expected_irq[0]) ||
                irq->vector != expected_irq[found]) {
                printf("lkl-iommu-probe: FAIL bad hwinfo IOMMU IRQ %u\n",
                       irq->vector);
                return 1;
            }
            found++;
        }
    }
    if (found != sizeof(expected_irq) / sizeof(expected_irq[0])) {
        printf("lkl-iommu-probe: FAIL hwinfo IOMMU IRQ count %u\n", found);
        return 1;
    }
    return 0;
}

static int parse_u64_arg(const char *arg, uint64_t max, uint64_t *out)
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull(arg, &end, 0);
    if (errno != 0 || *arg == '\0' || *end != '\0' || value > max)
        return -1;
    *out = (uint64_t) value;
    return 0;
}

static int probe_system_iommu(bool domain, uint32_t test_did,
                              uint64_t test_iova)
{
    struct rtmux_resource_handle h = {0};
    volatile void *mmio = MAP_FAILED;
    void *cq = MAP_FAILED;
    void *ddt = MAP_FAILED;
    void *pt = MAP_FAILED;
    uint64_t capabilities;
    uint64_t ddtp;
    uint64_t ddtp_modes = 0;
    off_t cq_pa;
    off_t ddt_pa;
    off_t pt_pa;
    off_t root_pa;
    size_t cq_contig;
    size_t ddt_contig;
    size_t pt_contig;
    unsigned version;
    unsigned mode;
    bool release_handle = false;
    bool cq_enabled = false;
    bool ddt_programmed = false;
    bool safe_release = true;
    bool tr_busy = false;
    int rc;

    if (probe_system_iommu_hwinfo() != 0)
        return 1;
    rc = rtmux_delegate_device(RTMUX_DEVICE_RISCV_IOMMU, 0x1000,
                               RTMUX_DELEGATE_ANY_BASE, &h);
    if (rc != EOK) {
        printf("lkl-iommu-probe: FAIL no system IOMMU resource: %s\n",
               strerror(rc));
        return 1;
    }
    release_handle = h.length > 0;
    if (h.kind != RTMUX_RESOURCE_MMIO ||
        h.device_id != RTMUX_DEVICE_RISCV_IOMMU ||
        h.length < 0x1000 || h.irq_vector < 0) {
        printf("lkl-iommu-probe: FAIL bad system IOMMU handle\n");
        rc = 1;
        goto out;
    }

    printf("lkl-iommu-probe: found system IOMMU MMIO %#lx irq %d\n",
           (unsigned long) h.base, h.irq_vector);
    mmio = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                MAP_PHYS | MAP_SHARED, NOFD, (off_t) h.base);
    if (mmio == MAP_FAILED) {
        printf("lkl-iommu-probe: FAIL map system IOMMU MMIO: %s\n",
               strerror(errno));
        rc = 1;
        goto out;
    }

    capabilities = mmio_read64(mmio, RISCV_IOMMU_CAPABILITIES);
    ddtp = mmio_read64(mmio, RISCV_IOMMU_DDTP);
    version = (unsigned) (capabilities & 0xffU);
    mode = (unsigned) (ddtp & RISCV_IOMMU_MODE_MASK);
    if (version != RISCV_IOMMU_VERSION_1_0) {
        printf("lkl-iommu-probe: FAIL system IOMMU version %#x\n", version);
        rc = 1;
        goto out;
    }
    if (mode > 1) {
        printf("lkl-iommu-probe: FAIL reset IOMMU mode %u\n", mode);
        rc = 1;
        goto out;
    }

    ddt = mmap(NULL, RISCV_IOMMU_DDT_BYTES,
               PROT_READ | PROT_WRITE | PROT_NOCACHE,
               MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (ddt == MAP_FAILED) {
        printf("lkl-iommu-probe: FAIL allocate DDT root: %s\n",
               strerror(errno));
        rc = 1;
        goto out;
    }
    if (mem_offset(ddt, NOFD, RISCV_IOMMU_DDT_BYTES,
                   &ddt_pa, &ddt_contig) == -1 ||
        ddt_contig < RISCV_IOMMU_DDT_BYTES ||
        ((uint64_t) ddt_pa & (RISCV_IOMMU_DDT_BYTES - 1U)) != 0) {
        printf("lkl-iommu-probe: FAIL DDT root physical span\n");
        rc = 1;
        goto out;
    }
    memset(ddt, 0, RISCV_IOMMU_DDT_BYTES);

    for (unsigned m = RISCV_IOMMU_DDTP_MODE_1LVL;
         m <= RISCV_IOMMU_DDTP_MODE_3LVL; m++) {
        uint64_t probed;

        if (ddtp_set_mode(mmio, 0, RISCV_IOMMU_DDTP_MODE_OFF, &probed) != 0) {
            printf("lkl-iommu-probe: FAIL ddtp off before mode %u ddtp=%#lx\n",
                   m, (unsigned long) probed);
            rc = 1;
            goto out;
        }
        ddt_programmed = false;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (ddtp_set_mode(mmio, (uint64_t) ddt_pa >> 12, m, &probed) != 0) {
            printf("lkl-iommu-probe: FAIL ddtp mode %u busy ddtp=%#lx\n",
                   m, (unsigned long) probed);
            ddt_programmed = ddtp_may_reference_ddt(probed);
            rc = 1;
            goto out;
        }
        unsigned retained = (unsigned) (probed & RISCV_IOMMU_MODE_MASK);

        ddt_programmed = ddtp_may_reference_ddt(probed);
        if (retained >= RISCV_IOMMU_DDTP_MODE_1LVL &&
            retained <= RISCV_IOMMU_DDTP_MODE_3LVL)
            ddtp_modes |= 1ULL << retained;
    }
    if (ddtp_restore_mode(mmio, mode) != 0) {
        printf("lkl-iommu-probe: FAIL restore ddtp mode %u\n", mode);
        rc = 1;
        goto out;
    }
    ddt_programmed = false;
    if (ddtp_modes == 0) {
        puts("lkl-iommu-probe: FAIL no DDT modes retained");
        rc = 1;
        goto out;
    }
    if ((ddtp_modes & (1ULL << RISCV_IOMMU_DDTP_MODE_1LVL)) == 0) {
        puts("lkl-iommu-probe: FAIL 1LVL DDT mode not retained");
        rc = 1;
        goto out;
    }
    printf("lkl-iommu-probe: capabilities=%#lx ddtp=%#lx mode=%u\n",
           (unsigned long) capabilities, (unsigned long) ddtp, mode);
    printf("lkl-iommu-probe: DDT modes mask=%#lx\n", (unsigned long) ddtp_modes);
    if (!domain) {
        rc = 0;
        goto out;
    }
    if ((capabilities & RISCV_IOMMU_CAP_SV39X4) == 0) {
        puts("lkl-iommu-probe: FAIL Sv39x4 unsupported");
        rc = 1;
        goto out;
    }

    cq = mmap(NULL, RISCV_IOMMU_CQ_BYTES, PROT_READ | PROT_WRITE | PROT_NOCACHE,
              MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (cq == MAP_FAILED) {
        printf("lkl-iommu-probe: FAIL allocate command queue: %s\n",
               strerror(errno));
        rc = 1;
        goto out;
    }
    if (mem_offset(cq, NOFD, RISCV_IOMMU_CQ_BYTES, &cq_pa, &cq_contig) == -1 ||
        cq_contig < RISCV_IOMMU_CQ_BYTES ||
        ((uint64_t) cq_pa & (RISCV_IOMMU_CQ_BYTES - 1U)) != 0) {
        printf("lkl-iommu-probe: FAIL command queue physical span\n");
        rc = 1;
        goto out;
    }
    memset(cq, 0, RISCV_IOMMU_CQ_BYTES);

    pt = mmap(NULL, RISCV_IOMMU_PT_BYTES,
              PROT_READ | PROT_WRITE | PROT_NOCACHE,
              MAP_PHYS | MAP_ANON | MAP_SHARED, NOFD, 0);
    if (pt == MAP_FAILED) {
        printf("lkl-iommu-probe: FAIL allocate translation page table: %s\n",
               strerror(errno));
        rc = 1;
        goto out;
    }
    if (mem_offset(pt, NOFD, RISCV_IOMMU_PT_BYTES, &pt_pa, &pt_contig) == -1 ||
        pt_contig < RISCV_IOMMU_PT_BYTES ||
        ((uint64_t) pt_pa & 0xfffU) != 0) {
        puts("lkl-iommu-probe: FAIL translation page-table physical span");
        rc = 1;
        goto out;
    }
    memset(pt, 0, RISCV_IOMMU_PT_BYTES);

    uint32_t cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);
    if ((cqcsr & RISCV_IOMMU_CQCSR_BUSY) != 0) {
        printf("lkl-iommu-probe: FAIL command queue busy before setup %#x\n",
               cqcsr);
        rc = 1;
        goto out;
    }
    mmio_write64(mmio, RISCV_IOMMU_CQB, ((uint64_t) cq_pa >> 12) << 10);
    mmio_write32(mmio, RISCV_IOMMU_CQT, 0);
    mmio_write32(mmio, RISCV_IOMMU_CQCSR, RISCV_IOMMU_CQCSR_CQEN);
    cq_enabled = true;
    if (wait_cqcsr(mmio, RISCV_IOMMU_CQCSR_CQON,
                   RISCV_IOMMU_CQCSR_CQON, &cqcsr) != 0) {
        printf("lkl-iommu-probe: FAIL command queue enable cqcsr=%#x\n",
               cqcsr);
        rc = 1;
        goto out;
    }

    volatile struct riscv_iommu_cmd *cmd = cq;
    uint32_t cqh = mmio_read32(mmio, RISCV_IOMMU_CQH);

    if (cqh != 0) {
        printf("lkl-iommu-probe: FAIL command queue initial cqh=%u\n", cqh);
        rc = 1;
        goto out;
    }

    volatile struct riscv_iommu_dc *dc = ddt;
    uint32_t cq_tail = 0;
    uint32_t iodir_cqh;
    uint32_t iofence_cqh;
    uint32_t translated_iodir_cqh;
    uint32_t translated_iofence_cqh;
    uint32_t iotinval_cqh;
    uint32_t post_clear_iofence_cqh;
    uint64_t tr_ctl;
    uint64_t tr_response;
    uint64_t bare_response;
    uint64_t translated_response;
    uint64_t tr_fault_response;
    uint64_t tr_clear_response;
    uint64_t probed;
    size_t root_off =
        (size_t) (((uint64_t) pt_pa + RISCV_IOMMU_ROOT_BYTES - 1U) &
                  ~(uint64_t) (RISCV_IOMMU_ROOT_BYTES - 1U)) -
        (size_t) pt_pa;
    volatile uint64_t *pt_root = (volatile uint64_t *) ((char *) pt + root_off);
    volatile uint64_t *pt_l1 =
        (volatile uint64_t *) ((char *) pt_root + RISCV_IOMMU_ROOT_BYTES);
    volatile uint64_t *pt_l0 = (volatile uint64_t *) ((char *) pt_l1 + 4096);

    root_pa = pt_pa + (off_t) root_off;
    pt_root[(test_iova >> 30) & 0x7ffU] =
        riscv_iommu_pte((uint64_t) root_pa + RISCV_IOMMU_ROOT_BYTES,
                        RISCV_IOMMU_PTE_V);
    pt_l1[(test_iova >> 21) & 0x1ffU] =
        riscv_iommu_pte((uint64_t) root_pa + RISCV_IOMMU_ROOT_BYTES + 4096,
                        RISCV_IOMMU_PTE_V);
    pt_l0[(test_iova >> 12) & 0x1ffU] =
        riscv_iommu_pte((uint64_t) ddt_pa,
                        RISCV_IOMMU_PTE_V | RISCV_IOMMU_PTE_R |
                            RISCV_IOMMU_PTE_W | RISCV_IOMMU_PTE_U |
                            RISCV_IOMMU_PTE_A | RISCV_IOMMU_PTE_D);

    dc[test_did] = (struct riscv_iommu_dc) {
        .tc = RISCV_IOMMU_DC_TC_V,
    };
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ddtp_set_mode(mmio, (uint64_t) ddt_pa >> 12,
                      RISCV_IOMMU_DDTP_MODE_1LVL, &probed) != 0) {
        printf("lkl-iommu-probe: FAIL ddtp 1LVL DC ddtp=%#lx\n",
               (unsigned long) probed);
        ddt_programmed = ddtp_may_reference_ddt(probed);
        rc = 1;
        goto out;
    }
    ddt_programmed = ddtp_may_reference_ddt(probed);
    if ((probed & RISCV_IOMMU_MODE_MASK) != RISCV_IOMMU_DDTP_MODE_1LVL) {
        printf("lkl-iommu-probe: FAIL ddtp 1LVL DC retained %#lx\n",
               (unsigned long) probed);
        rc = 1;
        goto out;
    }

    struct riscv_iommu_cmd iodir = riscv_iommu_iodir_inval_ddt_cmd(test_did);
    struct riscv_iommu_cmd iofence = riscv_iommu_iofence_cmd();
    SUBMIT_CQ_OR_FAIL(mmio, cmd, &cq_tail, iodir, iodir, cqcsr,
                      "IODIR.INVAL_DDT");
    SUBMIT_CQ_OR_FAIL(mmio, cmd, &cq_tail, iofence, iofence, cqcsr,
                      "IOFENCE.C");
    if ((capabilities & RISCV_IOMMU_CAP_DBG) == 0) {
        puts("lkl-iommu-probe: FAIL debug translation unsupported");
        rc = 1;
        goto out;
    }
    if (debug_translate(mmio, test_did, (uint64_t) ddt_pa,
                        &tr_ctl, &tr_response, &tr_busy) != 0) {
        printf("lkl-iommu-probe: FAIL debug translation ctl=%#lx resp=%#lx\n",
               (unsigned long) tr_ctl, (unsigned long) tr_response);
        rc = 1;
        goto out;
    }
    tr_busy = false;
    if ((tr_response & RISCV_IOMMU_TR_RESPONSE_FAULT) != 0 ||
        (tr_response & RISCV_IOMMU_TR_RESPONSE_PPN_MASK) !=
        (((uint64_t) ddt_pa >> 12) << 10)) {
        printf("lkl-iommu-probe: FAIL debug translation ctl=%#lx resp=%#lx\n",
               (unsigned long) tr_ctl, (unsigned long) tr_response);
        rc = 1;
        goto out;
    }
    bare_response = tr_response;
    dc[test_did] = (struct riscv_iommu_dc) {
        .tc = RISCV_IOMMU_DC_TC_V,
        .iohgatp = (RISCV_IOMMU_IOHGATP_MODE_SV39X4 << 60) |
                   (((uint64_t) root_pa >> 12) & ((1ULL << 44) - 1ULL)),
    };
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    SUBMIT_CQ_OR_FAIL(mmio, cmd, &cq_tail, translated_iodir, iodir, cqcsr,
                      "translated IODIR.INVAL_DDT");
    SUBMIT_CQ_OR_FAIL(mmio, cmd, &cq_tail, translated_iofence, iofence, cqcsr,
                      "translated IOFENCE.C");
    if (debug_translate(mmio, test_did, test_iova,
                        &tr_ctl, &tr_response, &tr_busy) != 0 ||
        (tr_response & RISCV_IOMMU_TR_RESPONSE_FAULT) != 0 ||
        (tr_response & RISCV_IOMMU_TR_RESPONSE_PPN_MASK) !=
        (((uint64_t) ddt_pa >> 12) << 10)) {
        printf("lkl-iommu-probe: FAIL translated debug ctl=%#lx resp=%#lx\n",
               (unsigned long) tr_ctl, (unsigned long) tr_response);
        rc = 1;
        goto out;
    }
    translated_response = tr_response;
    if (debug_translate(mmio, test_did, test_iova + 4096, &tr_ctl,
                        &tr_fault_response, &tr_busy) != 0 ||
        (tr_fault_response & RISCV_IOMMU_TR_RESPONSE_FAULT) == 0) {
        printf("lkl-iommu-probe: FAIL translated deny ctl=%#lx resp=%#lx\n",
               (unsigned long) tr_ctl, (unsigned long) tr_fault_response);
        rc = 1;
        goto out;
    }
    pt_l0[(test_iova >> 12) & 0x1ffU] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct riscv_iommu_cmd iotinval = riscv_iommu_iotinval_gvma_cmd(test_iova);
    SUBMIT_CQ_OR_FAIL(mmio, cmd, &cq_tail, iotinval, iotinval, cqcsr,
                      "IOTINVAL.GVMA");
    SUBMIT_CQ_OR_FAIL(mmio, cmd, &cq_tail, post_clear_iofence, iofence, cqcsr,
                      "post-clear IOFENCE.C");
    if (debug_translate(mmio, test_did, test_iova,
                        &tr_ctl, &tr_clear_response, &tr_busy) != 0 ||
        (tr_clear_response & RISCV_IOMMU_TR_RESPONSE_FAULT) == 0) {
        printf("lkl-iommu-probe: FAIL translated clear ctl=%#lx resp=%#lx\n",
               (unsigned long) tr_ctl, (unsigned long) tr_clear_response);
        rc = 1;
        goto out;
    }
    if (ddtp_restore_mode(mmio, mode) != 0) {
        printf("lkl-iommu-probe: FAIL restore ddtp after DC mode %u\n", mode);
        rc = 1;
        goto out;
    }
    ddt_programmed = false;

    printf("lkl-iommu-probe: Bare device context installed for DID %u\n",
           test_did);
    printf("lkl-iommu-probe: IODIR.INVAL_DDT fetched cqh=%u\n", iodir_cqh);
    printf("lkl-iommu-probe: debug translation resp=%#lx\n",
           (unsigned long) bare_response);
    printf("lkl-iommu-probe: translated IOVA %#lx resp=%#lx deny=%#lx\n",
           (unsigned long) test_iova, (unsigned long) translated_response,
           (unsigned long) tr_fault_response);
    printf("lkl-iommu-probe: IOTINVAL.GVMA post-clear resp=%#lx\n",
           (unsigned long) tr_clear_response);
    printf("lkl-iommu-probe: command queue enabled cqcsr=%#x\n", cqcsr);
    printf("lkl-iommu-probe: IOFENCE.C completed cqh=%u\n",
           post_clear_iofence_cqh);
    rc = 0;

out:
    if (tr_busy) {
        puts("lkl-iommu-probe: FAIL debug translation still busy");
        safe_release = false;
        rc = 1;
    }
    if (!tr_busy && ddt_programmed && mmio != MAP_FAILED) {
        if (ddtp_restore_mode(mmio, mode) != 0) {
            printf("lkl-iommu-probe: FAIL restore active ddtp mode %u\n",
                   mode);
            safe_release = false;
            rc = 1;
        } else {
            ddt_programmed = false;
        }
    }
    if (!tr_busy && cq_enabled) {
        uint32_t cqcsr = mmio_read32(mmio, RISCV_IOMMU_CQCSR);

        if ((cqcsr & RISCV_IOMMU_CQCSR_BUSY) != 0 &&
            wait_cqcsr(mmio, RISCV_IOMMU_CQCSR_BUSY, 0, &cqcsr) != 0) {
            printf("lkl-iommu-probe: FAIL command queue busy on disable "
                   "cqcsr=%#x\n",
                   cqcsr);
            safe_release = false;
            rc = 1;
        } else {
            mmio_write32(mmio, RISCV_IOMMU_CQCSR, 0);
            if (wait_cqcsr(mmio, RISCV_IOMMU_CQCSR_CQON, 0, &cqcsr) != 0) {
                printf("lkl-iommu-probe: FAIL command queue disable "
                       "cqcsr=%#x\n",
                       cqcsr);
                safe_release = false;
                rc = 1;
            }
        }
    }
    if (rc == 0)
        puts("lkl-iommu-probe: PASS");
    if (safe_release && cq != MAP_FAILED)
        munmap(cq, RISCV_IOMMU_CQ_BYTES);
    if (safe_release && ddt != MAP_FAILED)
        munmap(ddt, RISCV_IOMMU_DDT_BYTES);
    if (safe_release && pt != MAP_FAILED)
        munmap(pt, RISCV_IOMMU_PT_BYTES);
    if (safe_release && mmio != MAP_FAILED)
        munmap((void *) mmio, 0x1000);
    if (safe_release && release_handle)
        rtmux_hwres_release_range(h.base, h.length, RTMUX_HWRES_MEMORY);
    return rc;
}

static int pci_find_class(int fd, uint32_t class, uint32_t index,
                          uint32_t *bus, uint32_t *devfn)
{
    struct _pci_message msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len =
        sizeof(msg.msg_hdr.i) + sizeof(msg.rep_hdr) + sizeof(msg.msg);
    msg.msg_hdr.i.protocol = _IOMSG_PCI;
    msg.msg_hdr.i.subtype = IOM_PCI_FIND_CLASS;
    msg.msg.class.class = class;
    msg.msg.class.index = index;

    if (msg_send(fd, &msg, sizeof(msg), &msg, sizeof(msg)) == -1)
        return -1;
    if (msg.rep_hdr.reply_status != PCI_SUCCESS) {
        errno = ENODEV;
        return -1;
    }
    *bus = msg.msg.class.busnum;
    *devfn = msg.msg.class.devfuncnum;
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t bus, devfn;
    bool domain = false;
    bool explicit_domain_arg = false;
    bool system = false;
    uint64_t parsed;
    uint64_t max_did = (RISCV_IOMMU_DDT_BYTES /
                        sizeof(struct riscv_iommu_dc)) -
                       1U;
    uint64_t test_iova = RISCV_IOMMU_TEST_IOVA;
    uint32_t test_did = RISCV_IOMMU_TEST_DID;
    int fd;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--system")) {
            system = true;
        } else if (!strcmp(argv[i], "--domain")) {
            domain = true;
            system = true;
        } else if (!strcmp(argv[i], "--did") && i + 1 < argc) {
            explicit_domain_arg = true;
            if (parse_u64_arg(argv[++i], max_did, &parsed) != 0) {
                fputs("lkl-iommu-probe: invalid DID\n", stderr);
                return 1;
            }
            test_did = (uint32_t) parsed;
        } else if (!strcmp(argv[i], "--iova") && i + 1 < argc) {
            explicit_domain_arg = true;
            if (parse_u64_arg(argv[++i], RISCV_IOMMU_MAX_TEST_IOVA,
                              &test_iova) != 0 ||
                (test_iova & 0xfffU) != 0) {
                fputs("lkl-iommu-probe: invalid IOVA\n", stderr);
                return 1;
            }
        } else {
            fputs("usage: lkl-iommu-probe [--system|--domain] "
                  "[--did N] [--iova ADDR]\n",
                  stderr);
            return 1;
        }
    }
    if (!domain && explicit_domain_arg) {
        fputs("lkl-iommu-probe: --did/--iova require --domain\n", stderr);
        return 1;
    }
    if (system)
        return probe_system_iommu(domain, test_did, test_iova);

    fd = open("/dev/pci", O_RDONLY);
    if (fd < 0) {
        printf("lkl-iommu-probe: FAIL open /dev/pci: %s\n", strerror(errno));
        return 1;
    }

    if (pci_find_class(fd, PCI_CLASS_IOMMU, 0, &bus, &devfn) != 0) {
        printf("lkl-iommu-probe: FAIL no PCI IOMMU class device: %s\n",
               strerror(errno));
        close(fd);
        return 1;
    }

    printf("lkl-iommu-probe: found PCI IOMMU bus=%u devfn=0x%x\n",
           bus, devfn);
    puts("lkl-iommu-probe: PASS");
    close(fd);
    return 0;
}
