/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/mman.h>
#include <sys/stat.h>

#include <alloca.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "dynlink.h"

static Elf_Ehdr *get_elf_header(int,
                                const char *,
                                const struct stat *,
                                Elf_Phdr **phdr);


static bool elf_addr_add_overflows(Elf_Addr a, Elf_Addr b, Elf_Addr *out)
{
    if (b > (Elf_Addr) UINTPTR_MAX - a)
        return (true);
    *out = a + b;
    return (false);
}

static bool ldso_round_page_overflows(Elf_Addr value, Elf_Addr *out)
{
    Elf_Addr mask = (Elf_Addr) page_size - 1;

    if (value > (Elf_Addr) UINTPTR_MAX - mask)
        return (true);
    *out = ldso_round_page(value);
    return (false);
}

static bool phdr_in_zero_page(const Elf_Ehdr *hdr)
{
    size_t phsize;

    if (hdr->e_phnum > SIZE_MAX / sizeof(Elf_Phdr))
        return (false);
    phsize = hdr->e_phnum * sizeof(Elf_Phdr);
    return (hdr->e_phoff <= (Elf_Off) page_size &&
            phsize <= page_size - (size_t) hdr->e_phoff);
}

/* Map a shared object into memory. The "fd" argument is a file descriptor,
 * which must be open on the object and positioned at its beginning. The "path"
 * argument is a pathname that is used only for error messages.
 *
 * The return value is a pointer to a newly-allocated Obj_Entry structure for
 * the shared object.
 *
 * Returns NULL on failure.
 */
Obj_Entry *map_object(int fd,
                      const char *path,
                      const struct stat *sb,
                      bool ismain)
{
    Obj_Entry *obj;
    Elf_Ehdr *hdr;
    int i;
    Elf_Phdr *phdr;
    Elf_Phdr *phlimit;
    Elf_Phdr **segs;
    int nsegs;
    Elf_Phdr *phdyn;
    Elf_Phdr *phinterp;
    Elf_Phdr *phtls;
    caddr_t mapbase;
    size_t mapsize;
    Elf_Addr base_vaddr;
    Elf_Addr base_vlimit;
    caddr_t base_addr;
    int base_flags;
    Elf_Off data_offset;
    Elf_Addr data_vaddr;
    Elf_Addr data_vlimit;
    caddr_t data_addr;
    int data_prot;
    int data_flags;
    Elf_Addr clear_vaddr;
    caddr_t clear_addr;
    caddr_t clear_page;
    Elf_Addr phdr_vaddr;
    size_t nclear, phsize;
    Elf_Addr bss_vaddr;
    Elf_Addr bss_vlimit;
    caddr_t bss_addr;
    Elf_Word stack_flags;
    Elf_Addr relro_page;
    size_t relro_size;
    Elf_Addr text_end;

    hdr = get_elf_header(fd, path, sb, &phdr);
    if (!hdr)
        return (NULL);

    /* e_phnum is 16-bit, so the SIZE_MAX/sizeof guard can never fire; the real
     * hazard is alloca(segs[]) below on a small loader stack. Cap to a generous
     * bound (well-formed objects have a handful of program headers) so a
     * crafted dlopen'd object cannot blow the stack.
     */
    if (hdr->e_phnum == 0 || hdr->e_phnum > 64) {
        _ldso_error("%s: bad program header count", path);
        goto error;
    }

    /* Scan the program header entries, and save key information. We expect that
     * the loadable segments are ordered by load address.
     */
    phsize = hdr->e_phnum * sizeof(phdr[0]);
    phlimit = phdr + hdr->e_phnum;
    nsegs = -1;
    phdyn = phinterp = phtls = NULL;
    phdr_vaddr = 0;
    relro_page = 0;
    relro_size = 0;
    segs = alloca(sizeof(segs[0]) * hdr->e_phnum);
    stack_flags = PF_X | PF_R | PF_W;
    text_end = 0;
    while (phdr < phlimit) {
        switch (phdr->p_type) {
        case PT_INTERP:
            phinterp = phdr;
            break;

        case PT_LOAD:
            segs[++nsegs] = phdr;
            if (phdr->p_filesz > phdr->p_memsz) {
                _ldso_error("%s: PT_LOAD segment %d filesz exceeds memsz", path,
                            nsegs);
                goto error;
            }
            if ((segs[nsegs]->p_align & (page_size - 1)) != 0) {
                _ldso_error("%s: PT_LOAD segment %d not page-aligned", path,
                            nsegs);
                goto error;
            }
            if ((segs[nsegs]->p_flags & PF_X) == PF_X) {
                Elf_Addr end;
                Elf_Addr rounded;

                if (elf_addr_add_overflows(segs[nsegs]->p_vaddr,
                                           segs[nsegs]->p_memsz, &end) ||
                    ldso_round_page_overflows(end, &rounded)) {
                    _ldso_error("%s: PT_LOAD segment %d address overflow", path,
                                nsegs);
                    goto error;
                }
                text_end = MAX(text_end, rounded);
            }
            break;

        case PT_PHDR:
            phdr_vaddr = phdr->p_vaddr;
            phsize = phdr->p_memsz;
            break;

        case PT_DYNAMIC:
            phdyn = phdr;
            break;

        case PT_TLS:
            phtls = phdr;
            break;

        case PT_GNU_STACK:
            stack_flags = phdr->p_flags;
            break;

        case PT_GNU_RELRO:
            relro_page = phdr->p_vaddr;
            relro_size = phdr->p_memsz;
            break;
        }

        ++phdr;
    }
    if (!phdyn) {
        _ldso_error("%s: object is not dynamically-linked", path);
        goto error;
    }

    if (nsegs < 0) {
        _ldso_error("%s: too few PT_LOAD segments", path);
        goto error;
    }

    /* Map the entire address space of the object, to stake out our contiguous
     * region, and to establish the base address for relocation.
     */
    base_vaddr = ldso_trunc_page(segs[0]->p_vaddr);
    {
        Elf_Addr end;

        if (elf_addr_add_overflows(segs[nsegs]->p_vaddr, segs[nsegs]->p_memsz,
                                   &end) ||
            ldso_round_page_overflows(end, &base_vlimit)) {
            _ldso_error("%s: PT_LOAD span overflow", path);
            goto error;
        }
    }
    mapsize = base_vlimit - base_vaddr;
    base_addr = (caddr_t) base_vaddr;
    base_flags = MAP_PRIVATE | MAP_ANON;
    if (base_vaddr != 0)
        base_flags |= MAP_FIXED;

    mapbase = mmap(base_addr, mapsize, PROT_NONE, base_flags, -1, 0);
    if (mapbase == MAP_FAILED) {
        _ldso_error("%s: mmap of entire address space failed: %s", path,
                    ldso_strerror(errno));
        goto error;
    }
    if (base_addr && mapbase != base_addr) {
        _ldso_error("%s: mmap returned wrong address: wanted %p, got %p", path,
                    base_addr, mapbase);
        goto error1;
    }

    for (i = 0; i <= nsegs; i++) {
        /* Overlay the segment onto the proper region. */
        data_offset = ldso_trunc_page(segs[i]->p_offset);
        data_vaddr = ldso_trunc_page(segs[i]->p_vaddr);
        {
            Elf_Addr end;

            if (elf_addr_add_overflows(segs[i]->p_vaddr, segs[i]->p_filesz,
                                       &end) ||
                ldso_round_page_overflows(end, &data_vlimit)) {
                _ldso_error("%s: PT_LOAD segment %d file range overflow", path,
                            i);
                goto error1;
            }
        }
        data_addr = mapbase + (data_vaddr - base_vaddr);
        data_prot = convert_prot((int) segs[i]->p_flags);
        data_flags = MAP_PRIVATE | MAP_FIXED;
        if (data_vlimit != data_vaddr &&
            mmap(data_addr, data_vlimit - data_vaddr, data_prot, data_flags, fd,
                 (off_t) data_offset) == MAP_FAILED) {
            _ldso_error(
                "%s: mmap of data failed: errno=%d (%s) "
                "addr=%p len=%#lx prot=%d flags=%#x off=%#lx",
                path, errno, ldso_strerror(errno), data_addr,
                (unsigned long) (data_vlimit - data_vaddr), data_prot,
                data_flags, (unsigned long) data_offset);
            goto error1;
        }

        /* Do BSS setup */
        if (segs[i]->p_filesz != segs[i]->p_memsz) {
            /* Clear any BSS in the last page of the segment. */
            if (elf_addr_add_overflows(segs[i]->p_vaddr, segs[i]->p_filesz,
                                       &clear_vaddr)) {
                _ldso_error("%s: PT_LOAD segment %d clear range overflow", path,
                            i);
                goto error1;
            }
            clear_addr = mapbase + (clear_vaddr - base_vaddr);
            clear_page = mapbase + (ldso_trunc_page(clear_vaddr) - base_vaddr);

            if ((nclear = data_vlimit - clear_vaddr) > 0) {
                /* Make sure the end of the segment is writable */
                if ((data_prot & PROT_WRITE) == 0 &&
                    -1 == mprotect(clear_page, page_size,
                                   data_prot | PROT_WRITE)) {
                    _ldso_error("%s: mprotect failed: %s", path,
                                ldso_strerror(errno));
                    goto error1;
                }

                memset(clear_addr, 0, nclear);

                /* Reset the data protection back */
                if ((data_prot & PROT_WRITE) == 0)
                    mprotect(clear_page, page_size, data_prot);
            }

            /* Overlay the BSS segment onto the proper region. */
            bss_vaddr = data_vlimit;
            {
                Elf_Addr end;

                if (elf_addr_add_overflows(segs[i]->p_vaddr, segs[i]->p_memsz,
                                           &end) ||
                    ldso_round_page_overflows(end, &bss_vlimit)) {
                    _ldso_error("%s: PT_LOAD segment %d bss range overflow",
                                path, i);
                    goto error1;
                }
            }
            bss_addr = mapbase + (bss_vaddr - base_vaddr);
            if (bss_vlimit > bss_vaddr) { /* There is something to do */
                if (mmap(bss_addr, bss_vlimit - bss_vaddr, data_prot,
                         data_flags | MAP_ANON, -1, 0) == MAP_FAILED) {
                    _ldso_error("%s: mmap of bss failed: %s", path,
                                ldso_strerror(errno));
                    goto error1;
                }
            }
        }

        if (phdr_vaddr == 0 && data_offset <= hdr->e_phoff) {
            Elf_Addr seg_file_end = data_vlimit - data_vaddr + data_offset;

            if (seg_file_end >= hdr->e_phoff &&
                phsize <= seg_file_end - hdr->e_phoff)
                phdr_vaddr = data_vaddr + hdr->e_phoff - data_offset;
        }
    }

    obj = obj_new();
    if (sb) {
        obj->dev = sb->st_dev;
        obj->ino = sb->st_ino;
    }
    obj->mapbase = mapbase;
    obj->mapsize = mapsize;
    obj->vaddrbase = base_vaddr;
    obj->relocbase = mapbase - base_vaddr;
    obj->dynamic = (const Elf_Dyn *) (obj->relocbase + phdyn->p_vaddr);
    if (hdr->e_entry != 0)
        obj->entry = (caddr_t) (obj->relocbase + hdr->e_entry);
    if (phdr_vaddr != 0) {
        obj->phdr = (const Elf_Phdr *) (obj->relocbase + phdr_vaddr);
    } else {
        obj->phdr = malloc(phsize);
        if (!obj->phdr) {
            obj_free(obj);
            _ldso_error("%s: cannot allocate program header", path);
            goto error1;
        }
        memcpy(__DECONST(char *, obj->phdr), (char *) hdr + hdr->e_phoff,
               phsize);
        obj->phdr_alloc = true;
    }
    obj->phsize = phsize;
    if (phinterp)
        obj->interp = (const char *) (obj->relocbase + phinterp->p_vaddr);
    if (phtls) {
        if (ismain)
            obj->tlsindex = 1;
        else {
            tls_dtv_generation++;
            obj->tlsindex = ++tls_max_index;
        }
        obj->tlssize = phtls->p_memsz;

        /* Untrusted p_align: reject non-power-of-two so the align helpers hold.
         */
        obj->tlsalign =
            lib_is_power_of_two(phtls->p_align) ? phtls->p_align : 1;
        obj->tlspoffset = phtls->p_offset;
        obj->tlsinitsize = phtls->p_filesz;
        obj->tlsinit = obj->relocbase + phtls->p_vaddr;
    }
    obj->stack_flags = stack_flags;
    {
        Elf_Addr relro_end;

        if (elf_addr_add_overflows(relro_page, relro_size, &relro_end)) {
            obj_free(obj);
            _ldso_error("%s: RELRO range overflow", path);
            goto error1;
        }
        obj->relro_page = obj->relocbase + ldso_trunc_page(relro_page);
        obj->relro_size =
            ldso_trunc_page(relro_end) - ldso_trunc_page(relro_page);
    }
    munmap(hdr, page_size);
    return (obj);

error1:
    munmap(mapbase, mapsize);
error:
    if (!phdr_in_zero_page(hdr))
        munmap(phdr, hdr->e_phnum * sizeof(phdr[0]));
    munmap(hdr, page_size);
    return (NULL);
}

bool check_elf_headers(const Elf_Ehdr *hdr, const char *path)
{
    if (!IS_ELF(*hdr)) {
        _ldso_error("%s: invalid file format", path);
        return (false);
    }
    if (hdr->e_ident[EI_CLASS] != ELF_TARG_CLASS ||
        hdr->e_ident[EI_DATA] != ELF_TARG_DATA) {
        _ldso_error("%s: unsupported file layout", path);
        return (false);
    }
    if (hdr->e_ident[EI_VERSION] != EV_CURRENT ||
        hdr->e_version != EV_CURRENT) {
        _ldso_error("%s: unsupported file version", path);
        return (false);
    }
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        _ldso_error("%s: unsupported file type", path);
        return (false);
    }
    if (hdr->e_machine != ELF_TARG_MACH) {
        _ldso_error("%s: unsupported machine", path);
        return (false);
    }
    if (hdr->e_phentsize != sizeof(Elf_Phdr)) {
        _ldso_error(
            "%s: invalid shared object: e_phentsize != sizeof(Elf_Phdr)", path);
        return (false);
    }
    return (true);
}

static Elf_Ehdr *get_elf_header(int fd,
                                const char *path,
                                const struct stat *sbp,
                                Elf_Phdr **phdr_p)
{
    Elf_Ehdr *hdr;
    Elf_Phdr *phdr;

    /* Make sure file has enough data for the ELF header */
    if (sbp && sbp->st_size < (off_t) sizeof(Elf_Ehdr)) {
        _ldso_error("%s: invalid file format", path);
        return (NULL);
    }

    hdr = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (hdr == MAP_FAILED) {
        _ldso_error("%s: read error: %s", path, ldso_strerror(errno));
        return (NULL);
    }

    /* Make sure the file is valid */
    if (!check_elf_headers(hdr, path))
        goto error;
    if (hdr->e_phnum > SIZE_MAX / sizeof(phdr[0])) {
        _ldso_error("%s: program header table too large", path);
        goto error;
    }

    /* We rely on the program header being in the first page. This is not
     * strictly required by the ABI specification, but it seems to always true
     * in practice. And, it simplifies things considerably.
     */
    if (phdr_in_zero_page(hdr)) {
        phdr = (Elf_Phdr *) ((char *) hdr + hdr->e_phoff);
    } else {
        phdr = mmap(NULL, hdr->e_phnum * sizeof(phdr[0]), PROT_READ,
                    MAP_PRIVATE, fd, (off_t) hdr->e_phoff);
        if (phdr == MAP_FAILED) {
            _ldso_error("%s: error mapping phdr: %s", path,
                        ldso_strerror(errno));
            goto error;
        }
    }
    *phdr_p = phdr;
    return (hdr);

error:
    munmap(hdr, page_size);
    return (NULL);
}

void obj_free(Obj_Entry *obj)
{
    Objlist_Entry *elm;

    if (obj->tls_static)
        free_tls_offset(obj);
    while (obj->needed) {
        Needed_Entry *needed = obj->needed;
        obj->needed = needed->next;
        free(needed);
    }
    while (!STAILQ_EMPTY(&obj->names)) {
        Name_Entry *entry = STAILQ_FIRST(&obj->names);
        STAILQ_REMOVE_HEAD(&obj->names, link);
        free(entry);
    }
    while (!STAILQ_EMPTY(&obj->dldags)) {
        elm = STAILQ_FIRST(&obj->dldags);
        STAILQ_REMOVE_HEAD(&obj->dldags, link);
        free(elm);
    }
    while (!STAILQ_EMPTY(&obj->dagmembers)) {
        elm = STAILQ_FIRST(&obj->dagmembers);
        STAILQ_REMOVE_HEAD(&obj->dagmembers, link);
        free(elm);
    }
    if (obj->vertab)
        free(obj->vertab);
    if (obj->origin_path)
        free(obj->origin_path);
    if (obj->z_origin)
        free(__DECONST(void *, obj->rpath));
    if (obj->priv)
        free(obj->priv);
    if (obj->path)
        free(obj->path);
    if (obj->phdr_alloc)
        free(__DECONST(void *, obj->phdr));
    free(obj);
}

Obj_Entry *obj_new(void)
{
    Obj_Entry *obj;

    obj = CNEW(Obj_Entry);
    STAILQ_INIT(&obj->dldags);
    STAILQ_INIT(&obj->dagmembers);
    STAILQ_INIT(&obj->names);
    return obj;
}

/* Given a set of ELF protection flags, return the corresponding protection
 * flags for MMAP.
 */
int convert_prot(int elfflags)
{
    int prot = 0;
    if (elfflags & PF_R)
        prot |= PROT_READ;
    if (elfflags & PF_W)
        prot |= PROT_WRITE;
    if (elfflags & PF_X)
        prot |= PROT_EXEC;
    return prot;
}
