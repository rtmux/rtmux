/* SPDX-License-Identifier: Apache-2.0 */
#include <startup.h>
#include <startup-api.h>
#include <paging.h>
static int mask(struct syspage_entry *s, int v)
{
    (void) s;
    (void) v;
    __asm__ volatile("msr cntv_ctl_el0,%0; isb" ::"r"(3UL) : "memory");
    return 0;
}
static int unmask(struct syspage_entry *s, int v)
{
    (void) s;
    (void) v;
    __asm__ volatile("msr cntv_ctl_el0,%0; isb" ::"r"(1UL) : "memory");
    return 0;
}
static int peripheral_mask(struct syspage_entry *s, int irq)
{
    (void) s;
    volatile uint32_t *ic = (void *) (KERNEL_VIRT_BASE + 0x3f00b200);
    if ((unsigned) irq >= 72)
        return -1;
    ic[(0x1c + ((unsigned) irq / 32) * 4) / 4] = 1U << (irq % 32);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
static int peripheral_unmask(struct syspage_entry *s, int irq)
{
    (void) s;
    volatile uint32_t *ic = (void *) (KERNEL_VIRT_BASE + 0x3f00b200);
    if ((unsigned) irq >= 72)
        return -1;
    ic[(0x10 + ((unsigned) irq / 32) * 4) / 4] = 1U << (irq % 32);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
void init_intrinfo(void)
{
    set_syspage_section(&lsp.intrinfo, 2 * sizeof(struct intrinfo_entry));
    struct intrinfo_entry *i = lsp.intrinfo.p;
    memset(i, 0, 2 * sizeof(*i));
    i->vector_base = 5;
    i->num_vectors = 1;
    i->cascade_vector = ~0U;
    i->mask = mask;
    i->unmask = unmask;
    i[1].vector_base = 32;
    i[1].num_vectors = 72;
    i[1].cascade_vector = ~0U;
    i[1].mask = peripheral_mask;
    i[1].unmask = peripheral_unmask;
    volatile uint32_t *ic = (void *) (KERNEL_VIRT_BASE + 0x3f00b200);
    ic[0x1c / 4] = ic[0x20 / 4] = ic[0x24 / 4] = ~0U;
    *(volatile uint32_t *) (KERNEL_VIRT_BASE + 0x4000000c) = 0;
    __asm__ volatile("dsb sy" ::: "memory");
}
