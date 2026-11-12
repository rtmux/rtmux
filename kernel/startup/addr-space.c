/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Address space routines.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>
#include <alloca.h>

/* Address range structure */
struct as_range {
    struct as_range *next;
    paddr_t start;
    paddr_t end;
    unsigned owner;
};

/* Name list structure */
struct name_list {
    struct name_list *prev;
    const char *name;
};

/**
 * \brief Add an entry to the asinfo section of the system page.
 *
 * \note Parameters map one-to-one with field names.
 * \return The offset from the start of the section for the new entry.
 */
unsigned as_add(paddr64_t start,
                paddr64_t end,
                unsigned attr,
                const char *name,
                unsigned owner)
{
    uint8_t *p;
    struct asinfo_entry *as;
    unsigned name_off;
    unsigned off;

    LOGF_T("%s: start=%p, end=%p, name=%s, owner=%#x\n", __func__, start, end,
           name, owner);

    name_off = add_string(name);

    p = grow_syspage_section(&lsp.asinfo, sizeof(*as));
    as = (struct asinfo_entry *) (p + lsp.asinfo.size - sizeof(*as));

    as->start = start;
    as->end = end;
    as->owner = (uint16_t) owner;
    as->name = (uint16_t) name_off;
    as->attr = (uint16_t) (attr & 0xffff);
    as->priority = AS_PRIORITY_DEFAULT;
    as->alloc_checker = NULL;

    if (owner != AS_NULL_OFF) {
        struct asinfo_entry *own_as;

        own_as = (struct asinfo_entry *) (p + owner);

        own_as->attr |= AS_ATTR_KIDS;
    }

    off = (unsigned) ((uintptr_t) as - (uintptr_t) p);

    if (attr & AS_OVERLAY_IO) {
        as_add(start, end, attr & ~AS_OVERLAY_IO, "io", off);
    }

    return off;
}

/**
 * \brief Add default address space (from 0 to paddr_bits)
 */
unsigned as_default(void)
{
    return as_add(0, ((1ULL << paddr_bits) - 1), AS_ATTR_NONE, "cpu_addr_space",
                  AS_NULL_OFF);
}

/**
 * \brief TODO
 */
unsigned as_add_containing(paddr_t start,
                           paddr_t end,
                           unsigned attr,
                           const char *name,
                           const char *container)
{
    unsigned owner;
    unsigned off;
    unsigned start_off;
    struct asinfo_entry *as;
    paddr_t piece_start;
    paddr_t piece_end;
    unsigned piece_attr;
    struct as_range *list;
    struct as_range **list_owner;

    LOGF_T("%s: start=%#llx, end=%#llx, %s@%s attr %d\n", __func__, start, end,
           name, container, attr);

    list = NULL;
    off = 0;
    owner = AS_NULL_OFF;
    for (;;) {
        owner = as_find_containing(owner, start, end, container);
        if (owner == AS_NULL_OFF)
            break;
        as = as_off2info(owner);
        piece_start = start;
        if (as->start > piece_start)
            piece_start = as->start;
        piece_end = end;
        if (as->end < piece_end)
            piece_end = as->end;
        list_owner = &list;
        for (;;) {
            paddr_t old_end;
            struct as_range *curr;
            struct as_range *new;

            curr = *list_owner;
            if (!curr) {
                new = alloca(sizeof(*curr));
                new->start = piece_start;
                new->end = piece_end;
                new->owner = owner;
                new->next = NULL;
                *list_owner = new;
                break;
            }
            if (curr->start <= piece_start && curr->end >= piece_start) {
                if (curr->start == piece_start && curr->end == piece_end) {
                    curr->owner = owner;
                    break;
                }
                new = alloca(sizeof(*curr));
                new->start = piece_start;
                new->end = piece_end;
                new->owner = owner;
                if (curr->start == piece_start) {
                    curr->start = piece_end + 1;
                    new->next = curr;
                    *list_owner = new;
                    break;
                }
                old_end = curr->end;
                curr->end = piece_start - 1;
                new->next = curr->next;
                curr->next = new;
                if (old_end != piece_end) {
                    new = alloca(sizeof(*curr));
                    new->start = piece_end + 1;
                    new->end = old_end;
                    new->owner = curr->owner;
                    new->next = curr->next->next;
                    curr->next->next = new;
                }
                break;
            }
            list_owner = &curr->next;
        }
    }
    piece_attr = (attr & ~AS_OVERLAY_IO) | AS_ATTR_CONTINUED;
    start_off = AS_NULL_OFF;
    while (list) {
        if (list->start <= list->end) {
            off = as_add(list->start, list->end, piece_attr, name, list->owner);
            if (start_off == AS_NULL_OFF)
                start_off = off;
        }
        list = list->next;
    }
    if (start_off != AS_NULL_OFF) {
        as = as_off2info(off);
        as->attr &= (uint16_t) ~(unsigned) AS_ATTR_CONTINUED;
        if (attr & AS_OVERLAY_IO) {
            off = start_off;

            for (;;) {
                as = as_off2info(off);
                as_add(as->start, as->end, attr & ~AS_OVERLAY_IO, name, off);
                if (!(as->attr & AS_ATTR_CONTINUED))
                    break;
                off += sizeof(*as);
            }
        }
    }
    return start_off;
}

/**
 * \brief TODO
 */
static int match_name(struct asinfo_entry *base,
                      struct asinfo_entry *as,
                      const char *name)
{
    /* An owner chain is shorter than the table; a longer walk is a cycle. */
    unsigned hops = (unsigned) (lsp.asinfo.size / sizeof(*as));

    for (;;) {
        if (!strcmp(__hwi_find_string(as->name), name))
            return 1;
        if (as->owner == AS_NULL_OFF || hops-- == 0)
            return 0;
        as = ((struct asinfo_entry *) ((uint8_t *) base + as->owner));
    }
}

/**
 * \brief Find an asinfo entry with the name pointed to by container that at
 *        least partially covers
 *        the range given by start and end.
 *
 * \note Follows the same rules as as_find() to know where the search starts.
 * \return The offset of the matching entry or AS_NULL_OFF if none is found.
 */
unsigned as_find_containing(unsigned off,
                            paddr_t start,
                            paddr_t end,
                            const char *container)
{
    struct asinfo_entry *as;
    struct asinfo_entry *base;
    struct asinfo_entry *stop;

    as = base = lsp.asinfo.p;
    if (off != AS_NULL_OFF) {
        as = ((struct asinfo_entry *) ((uint8_t *) as + off)) + 1;
    }
    stop = (struct asinfo_entry *) ((uint8_t *) base + lsp.asinfo.size);
    for (;;) {
        if (as >= stop)
            return AS_NULL_OFF;
        if (match_name(base, as, container)) {
            if ((start >= as->start) && (start <= as->end))
                break;
            if ((end >= as->start) && (end <= as->end))
                break;
            if ((start <= as->start) && (end >= as->end))
                break;
        }
        ++as;
    }
    return (unsigned) ((uintptr_t) as - (uintptr_t) base);
}

/**
 * \brief TODO
 */
static int match_item(struct asinfo_entry *as, struct name_list *name)
{
    for (;;) {
        if (strcmp(name->name, __hwi_find_string(as->name)) != 0)
            return 0;
        name = name->prev;
        if (!name)
            return 1;
        if (as->owner == AS_NULL_OFF)
            return 0;
        as = (struct asinfo_entry *) ((uint8_t *) lsp.asinfo.p + as->owner);
    }
}

/* \brief TODO */
unsigned as_find(unsigned start, ...)
{
    struct asinfo_entry *as;
    struct asinfo_entry *base;
    struct asinfo_entry *end;
    va_list args;
    struct name_list *list;
    struct name_list *curr;
    const char *name;

    list = NULL;
    va_start(args, start);
    for (;;) {
        name = va_arg(args, char *);
        if (!name)
            break;
        curr = alloca(sizeof(*curr));
        curr->name = name;
        curr->prev = list;
        list = curr;
    }
    if (!name && !list) {
        crash("%s: NULL name argument\n", __func__);
    }
    va_end(args);
    as = base = lsp.asinfo.p;
    if (start != AS_NULL_OFF) {
        as = ((struct asinfo_entry *) ((uint8_t *) as + start)) + 1;
    }
    end = (struct asinfo_entry *) ((uint8_t *) base + lsp.asinfo.size);
    for (;;) {
        if (as >= end)
            return AS_NULL_OFF;
        if (match_item(as, list))
            return (unsigned) ((uintptr_t) as - (uintptr_t) base);
        ++as;
    }
}
