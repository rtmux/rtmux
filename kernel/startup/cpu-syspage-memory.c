/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Initialize architecture-specific parts of syspage.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>
#include <paging.h>

void cpu_init_syspage_memory()
{
    set_syspage_section(&lsp.cpu.riscv_cpu, sizeof(*lsp.cpu.riscv_cpu.p));
}

/**
 * \brief Allocate CPU syspage memory.
 * \param[out] cpupagep Variable to store CPU page address
 * \param[out] syspagep Variable to store syspage address
 * \param[in] spsize Syspage size
 */
struct syspage_entry *cpu_alloc_syspage_memory(paddr_t *cpupagep,
                                               paddr_t *syspagep,
                                               unsigned spsize)
{
    struct syspage_entry *sp = lsp.syspage.p;
    struct system_private_entry *private;
    unsigned size;
    unsigned cpsize;
    unsigned spacing;

#define SP_OFFSET(field) PTR_DIFF(lsp.cpu.field.p, sp)
#define INIT_ENTRY(_cpu, _field)                                          \
    sp->_cpu._field.entry_size = (uint16_t) lsp.cpu._cpu##_##_field.size; \
    sp->_cpu._field.entry_off = (uint16_t) SP_OFFSET(_cpu##_##_field)

    /* We assume that SMP processor has physical cache. We allocate the cpupages
     * in contiguous memory so that the kernel's copy_page_ptr[] pointers point
     * at virtually contiguous mappings into the physical pages. The user
     * _cpupage_ptr is a different virtual address where the same virtual
     * address on each CPU maps the different physical address for that CPU's
     * cpupage.
     */
    spacing = __PAGESIZE;
    spsize = ROUND(spsize, __PAGESIZE);
    cpsize = sp->num_cpu * spacing;

    /* Allocate the system page (and cpupage entries) and save it away. The
     * system page must be 4K aligned in a virtual system
     */
    size = spsize + cpsize;

    /* Syspage size vs the U-mode window reserved before the cpupage VA. If
     * spsize exceeds (RTMUX_CPUPAGE_UADDR - RTMUX_SYSPAGE_UADDR), the user-side
     * cpupage/kcall mappings clobber the middle of the syspage (real-hardware
     * syspages are far bigger than QEMU's).
     */
    LOGF_D(
        "syspage: size=%u bytes (%u pages), cpupage=%u, total=%u; "
        "U-mode syspage window=%#lx bytes\n",
        spsize, spsize / __PAGESIZE, cpsize, size,
        (unsigned long) (RTMUX_CPUPAGE_UADDR - RTMUX_SYSPAGE_UADDR));

    paddr_t sp_phys =
        alloc_ram(NULL_PADDR, size, lsp.system_private.p->pagesize);
    if (sp_phys == NULL_PADDR) {
        crash("could not allocate %#x bytes for syspage/cpupage\n", size);
    }

private
    = lsp.system_private.p;

    /* Map syspage+cpupage as kernel RW (identity-mapped). */
private
    ->kern_syspageptr =
        TOPTR(riscv_map(~(uintptr_t) 0, sp_phys, size, RISCV_MAP_SYSPAGE));
private
    ->kern_cpupageptr = TOPTR((char *) private->kern_syspageptr + spsize);

    /* Map syspage read-only at a fixed user VA. VPN[2]=1 (0x40000000+) is
     * copied from the boot page table into every new process page table by
     * cpu_vmm_mcreate(), so no per-process setup is needed.
     */
    riscv_map(RTMUX_SYSPAGE_UADDR, sp_phys, spsize, RISCV_MAP_SYSPAGE_RO);
private
    ->user_syspageptr = TOPTR(RTMUX_SYSPAGE_UADDR);

    /* Map CPU 0 cpupage read-only at a fixed user VA. Only CPU 0's cpupage is
     * mapped here; per-CPU remapping (same VA, different PA per hart) will be
     * wired in later when needed.
     */
    riscv_map(RTMUX_CPUPAGE_UADDR, sp_phys + spsize, spacing,
              RISCV_MAP_SYSPAGE_RO);
private
    ->user_cpupageptr = TOPTR(RTMUX_CPUPAGE_UADDR);

    /* Allocate and map a single page for the kcall trampoline. Contains just
     * ecall + ebreak — used by cpu_force_thread_destroyall and similar to force
     * a thread back into the kernel. Mapped R+X (no write) so the syspage
     * itself can be pure R/O.
     */
    paddr_t tramp_pa = calloc_ram(__PAGESIZE, __PAGESIZE);
    if (tramp_pa == NULL_PADDR)
        crash("failed to allocate kcall trampoline page\n");
    uint32_t *tramp = MAKE_1TO1_PTR(tramp_pa);
    tramp[0] = 0x00000073; /* ecall */
    tramp[1] = 0x00100073; /* ebreak */
    riscv_map(RTMUX_KCALL_UADDR, tramp_pa, __PAGESIZE, RISCV_MAP_USER_RX);

    LOGF_D(
        "syspage: kern_syspage=%p kern_cpupage=%p user_syspage=%p "
        "user_cpupage=%p\n",
        private->kern_syspageptr, private->kern_cpupageptr,
        private->user_syspageptr, private->user_cpupageptr);

    /* Map the page directory pages so the vm_ops can keep all the cpus' page
     * table mappings consistent. NYI: the vm_ops will need to know the page
     * directory location for SMP page table synchronization.
     */

private
    ->cpupage_spacing = spacing;

    *syspagep = sp_phys;
    *cpupagep = sp_phys + spsize;

    INIT_ENTRY(riscv, cpu);

    return (void *) sp_phys;
}
