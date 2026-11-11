/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Print the system page.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include "print-syspage.h"

static void print_typed_strings(void)
{
    struct typed_strings_entry *string =
        _SYSPAGE_ENTRY(PSP_SYSPAGE, typed_strings);
    unsigned type;
    uint32_t *typep;
    size_t i;

    i = 0;
    for (;;) {
        typep = (uint32_t *) &string->data[i];
        type = PSP_NATIVE_ENDIAN32(*typep);
        if (type == _CS_NONE)
            break;
        i += sizeof(uint32_t);
        kprintf("  off:%d type:%d string:'%s'\n", i - sizeof(uint32_t), type,
                &string->data[i]);
        i += strlen(&string->data[i]) + 1;
        i = (i + (sizeof(uint32_t) - 1)) & ~(sizeof(uint32_t) - 1);
    }
}


static void print_strings(void)
{
    char *p = _SYSPAGE_ENTRY(PSP_SYSPAGE, strings)->data;
    char *start = p;
    size_t off;
    size_t len;
    char buff[80];

    kprintf(" ");
    off = 1;
    while (*p != '\0') {
        PSP_SPRINTF(buff, " [%d]'%s'", p - start, p);
        len = strlen(buff);
        if ((off + len) >= 80) {
            kprintf("\n ");
            off = 1;
        }
        kprintf("%s", buff);
        off += len;
        p += strlen(p) + 1;
    }
    kprintf("\n");
}


static void print_system_private(void)
{
    struct system_private_entry *private =
        _SYSPAGE_ENTRY(PSP_SYSPAGE, system_private);

    kprintf("  syspage ptr user:%lx kernel:%lx\n",
            PSP_NATIVE_ENDIANPTR(private->user_syspageptr),
            PSP_NATIVE_ENDIANPTR(private->kern_syspageptr));

    kprintf("  cpupage ptr user:%lx kernel:%lx spacing:%lx\n",
            PSP_NATIVE_ENDIANPTR(private->user_cpupageptr),
            PSP_NATIVE_ENDIANPTR(private->kern_cpupageptr),
            private->cpupage_spacing);
}


static char *get_string(unsigned off)
{
    return &_SYSPAGE_ENTRY(PSP_SYSPAGE, strings)->data[off];
}


#if !defined(CONFIG_PSP_STARTUP)
static void asinfo_string_name(struct asinfo_entry *curr)
{
    struct asinfo_entry *base = _SYSPAGE_ENTRY(PSP_SYSPAGE, asinfo);
    struct list {
        struct list *next;
        struct asinfo_entry *as;
    } *chain, *new;

    chain = NULL;
    for (;;) {
        new = alloca(sizeof(*chain));
        new->next = chain;
        new->as = curr;
        chain = new;
        if (curr->owner == PSP_NATIVE_ENDIAN16(AS_NULL_OFF))
            break;
        curr = (struct asinfo_entry *) ((uint8_t *) base +
                                        PSP_NATIVE_ENDIAN16(curr->owner));
    }
    while (chain) {
        kprintf("/%s", get_string(PSP_NATIVE_ENDIAN16(chain->as->name)));
        chain = chain->next;
    }
}
#endif


static void print_asinfo(void)
{
    struct asinfo_entry *as = _SYSPAGE_ENTRY(PSP_SYSPAGE, asinfo);
    unsigned num;
    unsigned i;

    num = _SYSPAGE_ENTRY_SIZE(PSP_SYSPAGE, asinfo) / sizeof(*as);
    for (i = 0; i < num; ++i) {
        kprintf("  %x) %lx-%lx o:%x a:%x p:%d ", i * sizeof(*as),
                (unsigned long) PSP_NATIVE_ENDIAN64(as->start),
                (unsigned long) PSP_NATIVE_ENDIAN64(as->end),
                (unsigned) PSP_NATIVE_ENDIAN16(as->owner),
                (unsigned) PSP_NATIVE_ENDIAN16(as->attr),
                PSP_NATIVE_ENDIAN16(as->priority));
#if defined(CONFIG_PSP_STARTUP)
        kprintf("c:%lx n:%d\n", as->alloc_checker, as->name);
#else
        kprintf("n:");
        asinfo_string_name(as);
        kprintf("\n");
#endif
        ++as;
    }
}


static void print_hwinfo(void)
{
    hwi_tag *tag = (hwi_tag *) _SYSPAGE_ENTRY(PSP_SYSPAGE, hwinfo);
    void *base;
    void *next;
    char *name;


    while (tag->prefix.size != 0) {
        next = (hwi_tag *) ((uint32_t *) tag +
                            PSP_NATIVE_ENDIAN16(tag->prefix.size));
        base = (void *) (&tag->prefix + 1);
        name = get_string(PSP_NATIVE_ENDIAN16(tag->prefix.name));
        kprintf("  %d) size:%d tag:%d(%s)", hwi_tag2off(tag),
                PSP_NATIVE_ENDIAN16(tag->prefix.size),
                PSP_NATIVE_ENDIAN16(tag->prefix.name), name);
        if (*name >= 'A' && *name <= 'Z') {
            base = (void *) (&tag->item + 1);
            kprintf(" isize:%d, iname:%d(%s), owner:%d, kids:%d",
                    PSP_NATIVE_ENDIAN16(tag->item.itemsize),
                    PSP_NATIVE_ENDIAN16(tag->item.itemname),
                    get_string(PSP_NATIVE_ENDIAN16(tag->item.itemname)),
                    PSP_NATIVE_ENDIAN16(tag->item.owner),
                    PSP_NATIVE_ENDIAN16(tag->item.kids));
        }
        if (base != next) {
            kprintf("\n    ");
            while (base < next) {
                uint8_t *p = base;

                kprintf(" %b", *p);
                base = p + 1;
            }
        }
        kprintf("\n");
        tag = next;
    }
}


static void print_qtime(void)
{
    struct qtime_entry *qtime = _SYSPAGE_ENTRY(PSP_SYSPAGE, qtime);

    kprintf("  boot:%lx CPS:%lx rate/scale:%d/-%d intr:%d\n",
            PSP_NATIVE_ENDIAN32(qtime->boot_time),
            (unsigned long) PSP_NATIVE_ENDIAN64(qtime->cycles_per_sec),
            PSP_NATIVE_ENDIAN32(qtime->timer_rate),
            -(int) PSP_NATIVE_ENDIAN32(qtime->timer_scale),
            (int) PSP_NATIVE_ENDIAN32(qtime->intr));
    kprintf("  flags:%lx load:%d epoch:%d rr_mul:%d adj count/inc:%d/%d\n",
            PSP_NATIVE_ENDIAN32(qtime->flags),
            PSP_NATIVE_ENDIAN32(qtime->timer_load),
            PSP_NATIVE_ENDIAN32(qtime->epoch),
            PSP_NATIVE_ENDIAN32(qtime->rr_interval_mul),
            PSP_NATIVE_ENDIAN32(qtime->adjust.tick_count),
            PSP_NATIVE_ENDIAN32(qtime->adjust.tick_nsec_inc));
    kprintf("  nsec:%lx stable:%lx inc:%lx\n",
            (unsigned long) PSP_NATIVE_ENDIAN64(qtime->nsec),
            (unsigned long) PSP_NATIVE_ENDIAN64(qtime->nsec_stable),
            PSP_NATIVE_ENDIAN32(qtime->nsec_inc));
}

static void print_cpuinfo(void)
{
    struct cpuinfo_entry *cpu = _SYSPAGE_ENTRY(PSP_SYSPAGE, cpuinfo);
    unsigned i;

    for (i = 0; i < (unsigned) PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->num_cpu); ++i) {
        kprintf(
            "  %d) cpu:%lx flg:%lx his:%lx spd:%u cache i/d:%d/%d name:%d\n", i,
            PSP_NATIVE_ENDIAN32(cpu[i].cpu), PSP_NATIVE_ENDIAN32(cpu[i].flags),
            (unsigned long) PSP_NATIVE_ENDIAN64(cpu[i].idle_history),
            PSP_NATIVE_ENDIAN32(cpu[i].speed), cpu[i].ins_cache,
            cpu[i].data_cache, PSP_NATIVE_ENDIAN16(cpu[i].name));
    }
}

static void print_cacheattr(void)
{
    struct cacheattr_entry *cache = _SYSPAGE_ENTRY(PSP_SYSPAGE, cacheattr);
    int num;
    int i;

    num = _SYSPAGE_ENTRY_SIZE(PSP_SYSPAGE, cacheattr) / sizeof(*cache);
    for (i = 0; i < num; ++i) {
        kprintf("  %d) flags:%b size:%x #lines:%x control:%lx next:%d\n", i,
                PSP_NATIVE_ENDIAN32(cache[i].flags),
                (unsigned) PSP_NATIVE_ENDIAN32(cache[i].line_size),
                (unsigned) PSP_NATIVE_ENDIAN32(cache[i].num_lines),
                PSP_NATIVE_ENDIANPTR(cache[i].control),
                PSP_NATIVE_ENDIAN32(cache[i].next));
    }
}


static void print_intrgen(char *name, struct __intrgen_data *gen)
{
    kprintf("     %s => flags:%x, size:%x, rtn:%lx\n", name,
            (unsigned) PSP_NATIVE_ENDIAN16(gen->genflags),
            (unsigned) PSP_NATIVE_ENDIAN16(gen->size),
            PSP_NATIVE_ENDIANPTR(gen->rtn));
}

static void print_intrinfo(void)
{
    struct intrinfo_entry *intr = _SYSPAGE_ENTRY(PSP_SYSPAGE, intrinfo);
    int num;
    int i;

    num = _SYSPAGE_ENTRY_SIZE(PSP_SYSPAGE, intrinfo) / sizeof(*intr);
    for (i = 0; i < num; ++i) {
        kprintf("  %d) vector_base:%lx, #vectors:%d, cascade_vector:%lx\n", i,
                PSP_NATIVE_ENDIAN32(intr[i].vector_base),
                PSP_NATIVE_ENDIAN32(intr[i].num_vectors),
                PSP_NATIVE_ENDIAN32(intr[i].cascade_vector));
        kprintf("     cpu_intr_base:%lx, cpu_intr_stride:%d, flags:%x\n",
                PSP_NATIVE_ENDIAN32(intr[i].cpu_intr_base),
                PSP_NATIVE_ENDIAN16(intr[i].cpu_intr_stride),
                (unsigned) PSP_NATIVE_ENDIAN16(intr[i].flags));
        print_intrgen(" id", &intr[i].id);
        print_intrgen("eoi", &intr[i].eoi);
        kprintf("     mask:%lx, unmask:%lx, config:%lx\n",
                PSP_NATIVE_ENDIANPTR(intr[i].mask),
                PSP_NATIVE_ENDIANPTR(intr[i].unmask),
                PSP_NATIVE_ENDIANPTR(intr[i].config));
    }
}

static void print_smp(void)
{
    struct smp_entry *smp = _SYSPAGE_ENTRY(PSP_SYSPAGE, smp);

    kprintf("  send_ipi:%lx cpu:%lx\n", PSP_NATIVE_ENDIANPTR(smp->send_ipi),
            PSP_NATIVE_ENDIAN32(smp->cpu));
}

static void print_pminfo(void)
{
    struct pminfo_entry *pm = _SYSPAGE_ENTRY(PSP_SYSPAGE, pminfo);

    kprintf("  wakeup_condition:%lx\n",
            PSP_NATIVE_ENDIAN32(pm->wakeup_condition));
}

static void print_build_info(void)
{
    struct build_info_entry *bi = _SYSPAGE_ENTRY(PSP_SYSPAGE, build_info);

    kprintf("  release:'%s' build_id:'%s'\n", bi->release, bi->build_id);
}

#define INFO_SECTION 0x0001
#define EXPLICIT_ENABLE 0x8000
#define EXPLICIT_DISABLE 0x4000
#define IMPLICIT_DISABLE 0x2000
#define SYSPAGE_TYPE_SHIFT 4
#define SYSPAGE_TYPE_MASK 0xf

struct debug_syspage_section {
    const char *name;
    unsigned short loc;
    unsigned short flags;
    void (*print)(void);
};

#define PRT_SYSPAGE_RTN(section)                      \
    {                                                 \
        .name = #section,                             \
        .loc = offsetof(struct syspage_entry, section), \
        .flags = INFO_SECTION,                        \
        .print = print_##section,                     \
    }

#define CPU_PRT_SYSPAGE_RTN(upper_cpu, lower_cpu, flags, section) \
    {                                                             \
        .name = #section,                                         \
        .loc = offsetof(struct syspage_entry, un.lower_cpu.section), \
        .flags = (flags) +                                        \
                 ((SYSPAGE_##upper_cpu + 1) << SYSPAGE_TYPE_SHIFT), \
        .print = lower_cpu##_print_##section,                     \
    }

static struct debug_syspage_section sp_section[] = {
    PRT_SYSPAGE_RTN(system_private), PRT_SYSPAGE_RTN(qtime),
    PRT_SYSPAGE_RTN(cpuinfo),        PRT_SYSPAGE_RTN(cacheattr),
    PRT_SYSPAGE_RTN(asinfo),         PRT_SYSPAGE_RTN(hwinfo),
    PRT_SYSPAGE_RTN(typed_strings),  PRT_SYSPAGE_RTN(strings),
    PRT_SYSPAGE_RTN(intrinfo),       PRT_SYSPAGE_RTN(smp),
    PRT_SYSPAGE_RTN(pminfo),         PRT_SYSPAGE_RTN(build_info),
/* This second include of print_sysp.h will cause the CPU_PRT_SYSPAGE_RTN
 * definitions for the various routines to be added.
 */
#include "print-syspage.h"
};

void print_syspage_enable(const char *name)
{
    unsigned i;
    unsigned on_bit;
    unsigned off_mask;

    if (*name == '~') {
        ++name;
        on_bit = EXPLICIT_DISABLE;
        off_mask = ~(unsigned) EXPLICIT_ENABLE;
    } else {
        on_bit = EXPLICIT_ENABLE;
        off_mask = ~(unsigned) EXPLICIT_DISABLE;
    }
    for (i = 0; i < NUM_ELTS(sp_section); ++i) {
        if (!strcmp(sp_section[i].name, name)) {
            sp_section[i].flags =
                (unsigned short) (sp_section[i].flags & off_mask);
            sp_section[i].flags =
                (unsigned short) (sp_section[i].flags | on_bit);
        }
        if (on_bit & EXPLICIT_ENABLE) {
            /* If we have an explict enable, we mark all the sections as
             * implicitly disabled so that we don't print them out unless we end
             * up with an explict enablement of the entry
             */
            sp_section[i].flags |= IMPLICIT_DISABLE;
        }
    }
}

void print_syspage_sections(void)
{
    unsigned i;
    unsigned flags;
    unsigned type;

    kprintf("Header size=0x%x, Total Size=0x%x, #Cpu=%d, Type=%d\n",
            PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->size),
            PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->total_size),
            PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->num_cpu),
            PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->type));

#if !defined(CONFIG_PSP_STARTUP)
    if (syspage_cross_endian) {
        /* need to swap the various section's entry_off/entry_size fields ahead
         * of time since some sections depend on other sections ( for example
         * asinfo and hwinfo are printing data from the strings section
         */
        for (i = 0; i < NUM_ELTS(sp_section); ++i) {
            flags = sp_section[i].flags;
            if (flags & EXPLICIT_ENABLE) {
                flags &= ~(unsigned) IMPLICIT_DISABLE;
            }
            type = (flags >> SYSPAGE_TYPE_SHIFT) & SYSPAGE_TYPE_MASK;
            if (!((type == 0) ||
                  (type == (PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->type) + 1)))) {
                /* Not a section on this system page */
                flags |= EXPLICIT_DISABLE;
            }
            if (!(flags & (EXPLICIT_DISABLE | IMPLICIT_DISABLE))) {
                if (sp_section[i].flags & INFO_SECTION) {
                    syspage_entry_info *info =
                        (void *) ((uint8_t *) PSP_SYSPAGE + sp_section[i].loc);
                    ENDIAN_SWAP16(&info->entry_off);
                    ENDIAN_SWAP16(&info->entry_size);
                }
            }
        }
    }
#endif

    for (i = 0; i < NUM_ELTS(sp_section); ++i) {
        flags = sp_section[i].flags;
        if (flags & EXPLICIT_ENABLE) {
            flags &= ~(unsigned) IMPLICIT_DISABLE;
        }
        type = (flags >> SYSPAGE_TYPE_SHIFT) & SYSPAGE_TYPE_MASK;
        if (!((type == 0) ||
              (type == (PSP_NATIVE_ENDIAN16(PSP_SYSPAGE->type) + 1)))) {
            /* Not a section on this system page */
            flags |= EXPLICIT_DISABLE;
        }
        if (!(flags & (EXPLICIT_DISABLE | IMPLICIT_DISABLE))) {
            kprintf("Section:%s ", sp_section[i].name);
            if (sp_section[i].flags & INFO_SECTION) {
                syspage_entry_info *info;

                info = (void *) ((uint8_t *) PSP_SYSPAGE + sp_section[i].loc);
                kprintf("offset:0x%x size:0x%x\n", info->entry_off,
                        info->entry_size);
                if (info->entry_size > 0 && PSP_VERBOSE(2)) {
                    sp_section[i].print();
                }
            } else {
                kprintf("offset:0x%x\n", sp_section[i].loc);
                if (PSP_VERBOSE(2)) {
                    sp_section[i].print();
                }
            }
        }
    }
}
