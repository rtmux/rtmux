/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief RAM allocation routines for startup.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>

struct ram_entry {
    paddr64_t addr;
    paddr64_t size;
};

struct avoid_entry {
    paddr_t start;
    paddr_t end;
};

static int sysram_added;
static paddr_t ws_early_next;
static paddr_t temp_syspage = NULL_PADDR;

/* Avoid list address and size */
static struct avoid_entry *avoid_list;
static size_t avoid_list_size;

/* RAM list address and size */
static struct ram_entry *ram_list;
static size_t ram_list_size;

#define TEMP_SYSPAGE_SIZE (ROUNDPG(KILO(32)))
#define MAX_RAM_LIST_SIZE KILO(6)
#define MAX_AVOID_LIST_SIZE KILO(2)

static void ws_init(void)
{
    LOGF_T("%s\n", __func__);

    /* Assume there is some RAM after our kernel Take into account also the
     * stack for ourselves, which is located directly after _end (see head.S for
     * details).
     */
    ws_early_next = ROUND(_end, __PAGESIZE);

    avoid_list = MAKE_1TO1_PTR(ws_early_next);
    avoid_list[0].start = ws_early_next;
    ws_early_next += ROUND(MAX_AVOID_LIST_SIZE, sizeof(uint64_t));
    avoid_list[0].end = ws_early_next;
    avoid_list[1].end = 0;
    avoid_list_size = sizeof(avoid_list[0]) * 2;

    ram_list = ws_alloc(MAX_RAM_LIST_SIZE);
    ram_list->size = 0;
    ram_list_size = sizeof(struct ram_entry);

#ifdef CPU_MEM_AVOID_START
    /* Avoid using memory that might have something interesting */
    avoid_ram(CPU_MEM_AVOID_START, CPU_MEM_AVOID_SIZE);
#endif
}

/**
 * \brief TODO
 */
static int add_mem(paddr_t start, paddr_t size)
{
    paddr64_t e, s;
    struct ram_entry *r;
    struct ram_entry *ram = ram_list;

    LOGF_T("add_mem(%#lx, %#lx)\n", start, size);

    if (size > 0) {
        for (r = ram; r->size != 0; r++) {
            if (r->addr > start)
                break;
        }
        if (ram_list_size > (MAX_RAM_LIST_SIZE - sizeof(*r))) {
            crash("RAM entry array overflow");
        }
        memmove(&r[1], r, ram_list_size - PTR_DIFF(r, ram));
        ram_list_size += sizeof(*r);
        r->addr = start;
        r->size = size;
        for (r = ram + 1; r->size != 0; r++) {
            e = r[-1].addr + r[-1].size;
            s = r->addr;
            if (s <= e) {
                if ((s + r->size) > e)
                    e = s + r->size;
                memmove(r, &r[1], ram_list_size - PTR_DIFF(&r[1], ram));
                ram_list_size -= sizeof(*r);
                --r;
                r->size = e - r->addr;
            }
        }
    }
    return 0;
}

/**
 * \brief TODO
 * \param[in] as "Avoid block" start address
 * \param[in] ae "Avoid block" end address
 */
static int fix_size(paddr64_t *sp, paddr64_t *ep, paddr_t as, paddr_t ae)
{
    paddr64_t s = *sp;
    paddr64_t e = *ep;

    /* LOGF_D("%s: s=%p, e=%p, as=%p, ae=%p\n", __func__, s, e, as, ae); */

    if ((s >= ae) || (e <= as)) {
        /* Candidate mem has no overlap with the avoid range. All is good. */
        return 1;
    }
    if (s >= as) {
        /* candidate mem start within avoid range */
        if (e <= ae) {
            /* Candidate mem wholly contained in avoid range. All is lost. */
            return 0;
        }
        /* Restrict mem start to be after the end of the avoid range */
        *sp = ae;
    } else if (e < ae) {
        /* end of candidate mem is within avoid range Restrict the mem end to be
         * the start of the avoid range (should be -1?)
         */
        *ep = as;
    } else if ((as - s) > (e - ae)) {
        /* avoid range is in the middle of the candidate mem and the larger
         * piece is at the beginning Restrict the mem end to be the start of the
         * avoid range (should be -1?)
         */
        *ep = as;
    } else {
        /* avoid range is in the middle of the candidate mem and the larger
         * piece is at the end Restrict the mem start to be after the end of the
         * avoid range
         */
        *sp = ae;
    }
    return 1;
}

/**
 * \brief TODO
 */
static paddr_t find_top_ram_aligned(paddr_t size,
                                    unsigned align,
                                    unsigned pagesize)
{
    struct ram_entry *r;
    struct avoid_entry *a;
    paddr64_t s;
    paddr64_t e;
    paddr_t chk;
    paddr_t top = 0;

    size = ROUND(size, pagesize);
    for (r = ram_list; r->size != 0; r++) {
        s = r->addr;
        e = s + r->size;
        for (a = avoid_list; a->end != 0; ++a) {
            if (!fix_size(&s, &e, a->start, a->end))
                goto next_one;
        }

        /* Find an address at the end of the ram block. If align is given,
         * truncate to the alignment value.
         */
        if ((e - s) >= size) {
            chk = align ? TRUNC(e - size, align) : e - size;
            if (chk >= s) {
                if (chk > top)
                    top = chk;
            }
        }
    next_one:;
    }
    if (top == 0)
        top = NULL_PADDR;
    return top;
}

/**
 * \brief Find free memory block in a given range.
 *
 * \param[in] color TODO
 */
static paddr_t find_ram_in_range(paddr_t range_start,
                                 paddr_t range_end,
                                 size_t size,
                                 unsigned align,
                                 unsigned color,
                                 unsigned mask)
{
    struct ram_entry *ram = ram_list;
    struct ram_entry *r;
    paddr64_t s;
    paddr64_t e;
    paddr_t addr;
    struct avoid_entry *a;

    /* LOGF_D("%s: searching from %p to %p of block %x\n", __func__,
     * range_start, range_end, size);
     */

    for (r = ram; r->size != 0; r++) {
        int avoided = 0;
        s = r->addr;
        e = s + r->size;

        /* LOGF_D("%s: Candidate s:%#llx e:%#llx size:%#llx rqsize:%x\n",
         * __func__, s, e, r->size, size);
         */
        for (a = avoid_list; a->end != 0; ++a) {
            if (!fix_size(&s, &e, a->start, a->end)) {
                avoided = 1;
                break;
            }
        }
        if (avoided) {
            /* No point in continuing to check this one */
            continue;
        }

        addr = ROUND(s, align);

        if ((addr & mask) != color) {
            unsigned incr;

            incr = ((mask - 1) | mask) + 1;
            addr = ((addr + incr) & ~mask) | color;
        }

        if (range_start) {
            if (addr < range_start) {
                addr = range_start;
            }
        }
        if (range_end) {
            if (e > range_end) {
                e = range_end;
            }
        }

        if (addr >= e) {
            /* Adjusted out of contention by alignment or ranges */
            continue;
        }

        /* LOGF_D("%s: Adjusted addr: %#llx e:%#llx size:%#llx\n", __func__,
         * addr, e, e-addr);
         */
        if (size <= (e - addr)) {
            /* This one is big enough */
            if (!addr)
                crash("%s: address is NULL\n", __func__);
            return addr;
        }
    }
    return NULL_PADDR;
}

/**
 * \brief Find a free block of a given size.
 */
static paddr_t find_ram(size_t size,
                        unsigned align,
                        unsigned color,
                        unsigned mask)
{
    paddr_t result;

    /* We need to restrict this to MAX_PADDR */
    result = find_ram_in_range(0, MAX_PADDR, size, align, color, mask);
    if (result == NULL_PADDR)
        LOGF_W("%s: NULL_PADDR when requesting size %#x (align=%#x)\n",
               __func__, size, align);

    return result;
}

/**
 * \brief Allocate a block of memory of a given size.
 *
 * \param[in] addr The address at which to allocate (or NULL_PADDR
 *                  to use an arbitrary address).
 * \param[in] size Block size
 * \param[in] align Alignment
 */
paddr_t alloc_ram(paddr_t addr, paddr_t size, unsigned align)
{
    struct ram_entry *ram = ram_list;
    struct ram_entry *r;
    paddr64_t s, e, end;

    LOGF_T("alloc_ram(%lx, %lx, %d) => ", (paddr64_t) addr, (paddr64_t) size,
           align);

    if (addr == NULL_PADDR) {
        addr = find_ram(size, align, 0, 0);
        if (addr == NULL_PADDR) {
            LOGF_T("NULL_PADDR\n");
            return NULL_PADDR;
        }
        if (secure_system) {
            /* On a secure system, we want to zero everything */
            memset(MAKE_1TO1_PTR(addr), 0, size);
        }
    }

    LOGF_T("%lx\n", (paddr64_t) addr);
    end = addr + size;
    for (r = ram; r->size != 0; r++) {
        s = r->addr;
        e = s + r->size;
        if ((addr <= s && end > s) || (addr >= s && addr < e) ||
            (end >= s && end < e)) {
            if (end > e)
                e = end;
            if (addr < s)
                s = addr;
            if (addr == s && end == e) {
                memmove(r, &r[1], ram_list_size - PTR_DIFF(&r[1], ram));
                ram_list_size -= sizeof(*r);
                --r;
            } else if (addr == s) {
                r->size = e - end;
                r->addr = end;
            } else {
                r->size = addr - s;
                if (e != end) {
                    add_mem(end, e - end);
                }
            }
        }
    }

    /* If add_sysram() has been called, we need to adjust the start/end fields
     */
    if (sysram_added) {
        unsigned off;
        struct asinfo_entry *as;

        --end;
        off = AS_NULL_OFF;
        for (;;) {
            off = as_find_containing(off, addr, end, "sysram");
            if (off == AS_NULL_OFF)
                break;
            as = as_off2info(off);
            s = as->start;
            e = as->end;
            if (end > e)
                e = end;
            if (addr < s)
                s = addr;
            if (addr == s && end == e) {
                crash("Can't remove whole sysram entry.\n");
            } else if (addr == s) {
                as->start = end + 1;
            } else {
                as->end = addr - 1;
                if (e != end) {
                    as_add(end + 1, e, AS_ATTR_RAM, "sysram", as->owner);
                }
            }
        }
    }
    return addr;
}

/**
 * \brief Allocate a block of memory and zero it.
 *
 * \param[in] size Block size
 * \param[in] align Alignment
 */
paddr_t calloc_ram(size_t size, unsigned align)
{
    paddr_t a;

    a = alloc_ram(NULL_PADDR, size, align);

    /* If a secure system, we can assume alloc_ram has initialized the memory */
    if ((a != NULL_PADDR) && !secure_system) {
        memset(MAKE_1TO1_PTR(a), 0, size);
    }
    return a;
}

/**
 * \brief Call add_mem() and TODO
 */
void add_ram(paddr_t start, uint64_t size)
{
    unsigned mem;

    /* LOGF_T("%s: %#p, size %#lx\n", __func__, start, size); */

    if (lsp.asinfo.size == 0) {
        mem = as_default();

        /* "memory" is a root-level entry (owner = AS_NULL_OFF) that encompasses
         * all physical address space. find_last_paddr() treats owner ==
         * AS_NULL_OFF as the root of the physical-memory namespace.
         */
        paddr64_t mem_end = ((paddr64_t) 1 << paddr_bits) - 1;
        mem = as_add(0, mem_end, AS_ATTR_NONE, "memory", AS_NULL_OFF);
        init_asinfo(mem);
    }
    as_add_containing(start, start + size - 1, AS_ATTR_RAM, "ram", "memory");

    add_mem(start, size);

    /* Avoid using memory reserved for the CPU (exception table) */
#if defined(CPU_MEM_RESERVE_SIZE) && (CPU_MEM_RESERVE_SIZE > 0)
    alloc_ram(CPU_MEM_RESERVE_START, CPU_MEM_RESERVE_SIZE, 1);
#endif

    /* Get more space for the temporary system page we're building. */
    if (temp_syspage == NULL_PADDR) {
        temp_syspage = find_ram(TEMP_SYSPAGE_SIZE, __PAGESIZE, 0, 0);
        if (temp_syspage != NULL_PADDR) {
            avoid_ram(temp_syspage, TEMP_SYSPAGE_SIZE);
            reloc_syspage_memory(MAKE_1TO1_PTR(temp_syspage),
                                 TEMP_SYSPAGE_SIZE);
        }
    }
}

/**
 * \brief Add a given region to the avoid list.
 */
void avoid_ram(paddr_t start, size_t size)
{
    struct avoid_entry *avoid;

    LOGF_T("%s (%#p, %#x)\n", __func__, start, size);

    if (!avoid_list) {
        ws_init();
    }
    avoid = (void *) ((uint8_t *) avoid_list + avoid_list_size);

    avoid[-1].start = start;
    avoid[-1].end = start + size;
    avoid[0].end = 0;
    avoid_list_size += sizeof(*avoid);
}

/* Add all the "sysram" entries to the asinfo section. Note that we haven't
 * allocated the system page storage yet, so we're going to have adjust the
 * start/end fields when that happens.
 */
void add_sysram(void)
{
    struct ram_entry *ram;
    struct avoid_entry *a;

    LOGF_T("*************\n%s: no more RAM will be added\n*************\n",
           __func__);

    /* Remove all avoided regions from ram_list before creating sysram entries.
     * This ensures memory marked as reserved (OpenSBI firmware, FDT, workspace,
     * etc.) doesn't end up as free "sysram" available to the kernel.
     * alloc_ram() is safe to call even if the region was already removed.
     */
    for (a = avoid_list; a->end != 0; ++a) {
        if (a->end > a->start) {
            alloc_ram(a->start, a->end - a->start, 1);
        }
    }

    for (ram = ram_list; ram->size != 0; ++ram) {
        as_add_containing(ram->addr, ram->addr + ram->size - 1, AS_ATTR_RAM,
                          "sysram", "ram");
    }
    sysram_added = 1;
}

/**
 * \brief TODO
 */
void *ws_alloc(size_t size)
{
    paddr_t paddr;

    if (!avoid_list) {
        ws_init();
    }

    size = ROUND(size, sizeof(uint64_t));
    if (ram_list_size <= sizeof(struct ram_entry)) {
        /* No add_ram()'s have been done yet */
        paddr = ws_early_next;
        ws_early_next += size;
    } else {
        /* We've been told where memory is */
        paddr = find_ram(size, sizeof(uint64_t), 0, 0);
        if (paddr == NULL_PADDR) {
            return NULL;
        }
    }

    /* LOGF_T("%s: paddr=%p, size=%x\n", __func__, paddr, size); */

    avoid_ram(paddr, size);
    return MAKE_1TO1_PTR(paddr);
}

/**
 * \brief Allocate some RAM and create a "reserved" asinfo for it.
 *
 * \param[in] size Size in bytes of the RAM to allocate.
 * \param[in] align Alignment requirement for the ram or 0 for no alignment.
 * \param[in] pagesize System page size. Block will be rounded up to this size.
 */
void reserve_ram(unsigned size, unsigned align, unsigned pagesize)
{
    paddr_t reserved;

    LOGF_T("%s: size %#x, align %#x, pagesize %#x\n", size, align, pagesize);

    if (size == 0) {
        return;
    }

    reserved = find_top_ram_aligned(size, align ? align : pagesize, pagesize);
    if (reserved == NULL_PADDR) {
        crash("%s: could not reserve %#lx bytes of memory\n", __func__, size);
    }

    alloc_ram(reserved, size, align ? align : pagesize);
    as_add_containing(reserved, (reserved + size - 1), AS_ATTR_RAM, "reserved",
                      "ram");
}

paddr_t ram_top(void)
{
    paddr_t top = 0;
    for (struct ram_entry *r = ram_list; r->size != 0; r++) {
        paddr_t end = r->addr + r->size;
        if (end > top)
            top = end;
    }
    return top;
}
