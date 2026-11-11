/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/elf.h>
#include <elf/dyn.h>
#include <elf/notes.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <libgen.h>
#include <cpu-paging.h> /* RTMUX_ROOTTASK_UADDR_BASE / _END */
#include <elf-range.h>
#include "externs.h"
#include "cpudeps.h"
#include "root-log.h"

#define RTMUX_USER_STACK_TOP 0x40000000UL
#define ROOT_PC_IMAGE_VADDR 11


struct seg {
    uintptr_t addr;
    size_t size;
    size_t filesz;
    intptr_t reloc;
    int prot;
    int read_in;
};

static int validate_pt_load_policy(const Elf64_Phdr *phdr, int error)
{
    if (phdr->p_filesz > phdr->p_memsz)
        return error;
    if ((phdr->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
        return error;
    return EOK;
}

/* Notify VM of a loaded segment for debugger support.
 *
 * Sends a _MEM_DEBUG_INFO message describing where a program-header segment was
 * mapped, so the debugger can correlate addresses with the file it came from.
 * Parameters:
 *   vaddr : virtual address the segment was loaded at
 *   phdr  : the program header describing the segment
 *   fd    : file descriptor of the loaded image
 *   name  : base name of the image
 */
static void debug_info(const void *vaddr,
                       const Elf64_Phdr *phdr,
                       int fd,
                       const char *name)
{
    mem_debug_info_t msg;
    iov_t iov[2];

    msg.i.type = _MEM_DEBUG_INFO;
    msg.i.zero = 0;
    msg.i.reserved = 0;
    msg.i.offset = (off64_t) phdr->p_offset;
    msg.i.ino = 0;
    msg.i.vaddr = (uintptr_t) vaddr;
    msg.i.size = phdr->p_memsz;
    msg.i.flags = 0;
    msg.i.dev = 0;
    msg.i.old_vaddr = phdr->p_vaddr;
    SETIOV(iov + 0, &msg.i, offsetof(mem_debug_info_t, i.path));
    SETIOV(iov + 1, name, strlen(name) + 1);

    /* ipc-send: roottask-fsio -> roottask-core */
    msg_sendv(VM_COID, iov, 2, 0, 0);
}

/* Load an ELF executable (and its interpreter, if any) into the address space
 * being prepared for a new process.
 *
 * Validates the ELF header, maps each PT_LOAD segment as anonymous memory and
 * reads its file data in directly (RTMux has no demand paging), processes
 * PT_NOTE / PT_PHDR / PT_INTERP, fills the aux vector, loads the dynamic
 * interpreter when present, and finally sends a _PROC_SPAWN/_PROC_SPAWN_DEBUG
 * message describing the text and data segments. Parameters:
 *   fd        : file descriptor of the executable
 *   path      : pathname of the executable
 *   lsp       : loader-startup state (aux vector, stack, entry point);
 *               NULL for a probe-only load
 *   statlocal : stat of the executable (unused here)
 *   parms     : spawn inheritance parameters (unused here)
 * Return value: EOK on success; a positive errno on failure; -1 if the
 *               file is not a recognizable ELF so the caller can try a
 *               "#!" interpreter.
 */
int elf_load(int fd,
             const char *path,
             struct loader_startup *lsp,
             struct stat *statlocal,
             struct inheritance *parms)
{
    Elf64_Ehdr ehdr;
    Elf64_Phdr *phdr, *phdrs;
    uintptr_t base_addr;
    intptr_t base_reloc;
    char *addr;
    auxv_t *auxv;
    int i;
    int status;
    size_t size;
    int prot;
    int flags;
    char *interp;
    struct seg *seg;
    int text_seg, data_seg;
    char buffer[1024];
    struct {
        Elf32_Nhdr nhdr;
        char strings[1];
    } *note = (void *) buffer;
    struct {
        struct _proc_spawn_debug i;
        char filler[_POSIX_PATH_MAX];
    } debug_msg;


    if (strlen(path) >
        ((sizeof(debug_msg) - offsetof(struct _proc_spawn_debug, name)) - 1)) {
        return ENAMETOOLONG;
    }
    {
        ssize_t nread = proc_read(fd, &ehdr, sizeof ehdr, 0);
        if (nread <= 0 || (size_t) nread != sizeof ehdr) {
            return -1; /* not ELF — let caller try #! interpreter */
        }
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        return -1;
    }
    if (ehdr.e_phentsize != sizeof *phdr)
        return ENOEXEC;
    switch (ehdr.e_machine) {
    case CPU_ELF_NUMBERS:
        break;
    default:
        return ENOEXEC;
    }
    switch (ehdr.e_type) {
    case ET_DYN:
    case ET_EXEC:
        break;
    default:
        return ENOEXEC;
    }

    /* e_phnum is a 16-bit field, so "> SIZE_MAX / sizeof" can never fire and
     * the two alloca()s below would put up to ~65535 * (sizeof phdr + sizeof
     * seg) — several MiB — on the loader thread's tiny (~18 KiB) stack, a stack
     * overflow driven by a crafted exec image. Cap to a generous bound (real
     * binaries have <= 7 program headers; matches lib/ldso map_object()); the
     * alloca is ~64 * (56 + 40) bytes at the cap, well within the stack.
     */
    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 64 ||
        ehdr.e_phoff > (uint64_t) INT64_MAX) {
        return ENOEXEC;
    }

    if (!(phdrs = alloca(sizeof *phdrs * ehdr.e_phnum)) ||
        !(seg = alloca(sizeof *seg * ehdr.e_phnum))) {
        return ENOEXEC;
    }
    memset(seg, 0x00, sizeof *seg * ehdr.e_phnum);
    memset(&debug_msg, 0x00, sizeof debug_msg);

    auxv = 0;
    interp = 0;

    if (lsp) {
        auxv = lsp->aux;
        while (auxv->a_type != AT_NULL) {
            auxv++;
        }
        lsp->stackalloc = 0;
        if (SYSPAGE_ENTRY(cpuinfo)->flags & CPU_FLAG_MMU) {
            lsp->stacksize = DEF_VIRTUAL_FIRST_THREAD_STACKSIZE;
        } else {
            lsp->stacksize = DEF_PHYSICAL_FIRST_THREAD_STACKSIZE;
        }
        lsp->stackaddr = RTMUX_USER_STACK_TOP;
    }

    size = ehdr.e_phnum * sizeof(*phdrs);
    if (size > (size_t) SSIZE_MAX) {
        return ENOEXEC;
    }
    root_trace("elf_load: %s: %d phdrs, phentsz=%u (expect %u), phoff=%lu\n",
               path, ehdr.e_phnum, ehdr.e_phentsize, (unsigned) sizeof(*phdrs),
               (unsigned long) ehdr.e_phoff);
    if ((size_t) proc_read(fd, phdrs, (ssize_t) size, (off64_t) ehdr.e_phoff) !=
        size) {
        root_trace("elf_load: failed to read program headers\n");
        return ENOEXEC;
    }

    base_reloc = 0;
    base_addr = ~(uintptr_t) 0;
    text_seg = data_seg = -1;
    for (i = 0; i < ehdr.e_phnum; i++) {
        phdr = phdrs + i;
        switch (phdr->p_type) {
        case PT_LOAD:
            if ((status = validate_pt_load_policy(phdr, ENOEXEC)) != EOK)
                return status;

            /* Refuse PT_LOAD whose VA range overlaps the U-mode roottask
             * reservation (RTMUX_ROOTTASK_UADDR_BASE..END, L2[3]). During load
             * the loader thread runs with roottask temporarily mapped into the
             * child pgdir at that range; mmap(MAP_FIXED) into here would walk
             * the shared L1 page and rewrite roottask's L0 PTEs, corrupting
             * roottask itself. A well-formed user ELF never targets this range
             * — it's reserved.
             */
            {
                uintptr_t v_start = phdr->p_vaddr;
                uintptr_t v_end;
                if (elf_addr_range_overflows(v_start, phdr->p_memsz, &v_end)) {
                    return ENOEXEC;
                }
                if (v_end > RTMUX_ROOTTASK_UADDR_BASE &&
                    v_start < RTMUX_ROOTTASK_UADDR_END) {
                    root_log(
                        "elf_load: PT_LOAD[%d] vaddr=%lx memsz=%lx "
                        "overlaps reserved roottask range [%lx,%lx) — "
                        "refusing\n",
                        i, (unsigned long) v_start,
                        (unsigned long) phdr->p_memsz,
                        (unsigned long) RTMUX_ROOTTASK_UADDR_BASE,
                        (unsigned long) RTMUX_ROOTTASK_UADDR_END);
                    return ENOEXEC;
                }
            }

            if (phdr->p_flags & PF_W) {
                if (data_seg == -1)
                    data_seg = i;
                flags = MAP_ELF | MAP_FIXED | MAP_PRIVATE | MAP_NOSYNCFILE;
                prot = PROT_WRITE;
            } else {
                flags = MAP_ELF | MAP_FIXED | MAP_SHARED | MAP_NOSYNCFILE;
                prot = 0;
            }
            if (phdr->p_flags & PF_X) {
                if (text_seg == -1)
                    text_seg = i;
                prot |= PROT_EXEC;
            }
            if (phdr->p_flags & PF_R)
                prot |= PROT_READ;
            seg[i].addr = phdr->p_vaddr;
            seg[i].size = phdr->p_memsz;
            seg[i].filesz = phdr->p_filesz;
            seg[i].prot = prot;

            if (lsp) {
                /* RTMux has no demand paging, so we can't use file-backed mmap
                 * (which creates lazy PTEs). Instead, allocate anonymous pages
                 * and read the file data into them directly.
                 */
                addr =
                    mmap((void *) (uintptr_t) phdr->p_vaddr, phdr->p_memsz,
                         prot | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON | MAP_ELF | MAP_FIXED, NOFD, 0);
                if (addr == MAP_FAILED) {
                    root_log(
                        "elf_load: mmap FAILED path=%s phdr[%d] vaddr=%lx "
                        "memsz=%lx prot=0x%x errno=%d\n",
                        path, i, (unsigned long) phdr->p_vaddr,
                        (unsigned long) phdr->p_memsz, prot | PROT_WRITE,
                        errno);
                    return errno;
                }
                if (phdr->p_filesz > 0) {
                    if (phdr->p_filesz > (uint64_t) SSIZE_MAX ||
                        phdr->p_offset > (uint64_t) INT64_MAX) {
                        return ENOEXEC;
                    }
                    ssize_t got = proc_read(fd, addr, (ssize_t) phdr->p_filesz,
                                            (off64_t) phdr->p_offset);
                    if ((size_t) got != phdr->p_filesz) {
                        root_log(
                            "elf_load: FAILED fd=%d off=%lx sz=%lx got=%ld "
                            "errno=%d\n",
                            fd, (unsigned long) phdr->p_offset,
                            (unsigned long) phdr->p_filesz, (long) got, errno);
                        return ENOEXEC;
                    }
                }
                seg[i].read_in = 1;
                seg[i].reloc = (intptr_t) ((uintptr_t) addr - phdr->p_vaddr);
                if (base_addr == ~(uintptr_t) 0) {
                    base_addr = seg[i].addr;
                    base_reloc = seg[i].reloc;
                }
            } else {
                void *vaddr;
                size_t lsize;

                (void) root_vm_ops.mmap(0, NULL, (phdr->p_vaddr),
                                        (phdr->p_memsz), prot,
                                        MAP_PHYS + MAP_SHARED + MAP_SYSRAM,
                                        NULL, 0, 0, 0, NOFD, &vaddr, &lsize);
            }
            break;

        case PT_NOTE:
            if (0 == phdr->p_filesz)
                break;
            if (phdr->p_filesz >= sizeof *note) {
                size = min(phdr->p_filesz, sizeof buffer);
                if ((size_t) proc_read(fd, note, (ssize_t) size,
                                       (off64_t) phdr->p_offset) == size) {
                    while (size > 0) {
                        size_t len;
                        Elf32_Word *p;

                        /* Validate the note fits in the remaining buffer before
                         * touching its name/descriptor. A malformed note (from
                         * a crafted exec image) could otherwise drive the
                         * n_namesz/n_descsz-based offsets below past the
                         * buffer.
                         */
                        if (size < sizeof note->nhdr)
                            break;

                        /* Round up in size_t: n_namesz/n_descsz are 32-bit, so
                         * "(x + 3) & ~3" would wrap a near-UINT32_MAX field to
                         * a tiny value and make a huge note pass the len <=
                         * size check below. size_t (LP64) can't wrap for a
                         * 32-bit input.
                         */
                        len =
                            sizeof note->nhdr +
                            (((size_t) note->nhdr.n_namesz + 3) & ~(size_t) 3) +
                            (((size_t) note->nhdr.n_descsz + 3) & ~(size_t) 3);
                        if (len > size)
                            break;

                        /* memcmp, not strcmp: n_namesz == sizeof
                         * RTMUX_NOTE_NAME bounds the length but does not prove
                         * a NUL terminator in the file data, so strcmp could
                         * read past the name.
                         */
                        if (note->nhdr.n_namesz == sizeof RTMUX_NOTE_NAME &&
                            !memcmp(note->strings, RTMUX_NOTE_NAME,
                                    sizeof RTMUX_NOTE_NAME)) {
                            p = (Elf32_Word *) &note
                                    ->strings[(note->nhdr.n_namesz + 3) & ~3U];
                            switch (note->nhdr.n_type) {
                            case RTMUX_NT_STACK:
                                if (lsp &&
                                    note->nhdr.n_descsz >= sizeof(Elf32_Word)) {
                                    lsp->stackalloc = lsp->stacksize = *p;
                                    if ((note->nhdr.n_descsz >
                                         sizeof(Elf32_Word)) &&
                                        *(++p)) {
                                        lsp->flags |= PROC_LF_LAZYSTACK;
                                        lsp->stackalloc =
                                            (*p + (__PAGESIZE - 1)) &
                                            ~(__PAGESIZE - 1);
                                    }
                                }
                                break;

                            case RTMUX_NT_DEBUG_FULLPATH:
                                break;

                            case RTMUX_NT_DEBUG_RELOC:
                                if (fpathconf(fd, ROOT_PC_IMAGE_VADDR) != -1) {
                                    /* p[i] must lie inside the descriptor. */
                                    if (text_seg != -1 &&
                                        (size_t) (text_seg + 1) *
                                                sizeof(Elf32_Word) <=
                                            note->nhdr.n_descsz) {
                                        seg[text_seg].reloc = p[text_seg];
                                    }
                                    if (data_seg != -1 &&
                                        (size_t) (data_seg + 1) *
                                                sizeof(Elf32_Word) <=
                                            note->nhdr.n_descsz) {
                                        seg[data_seg].reloc = p[data_seg];
                                    }
                                }
                                break;

                            default:
                                break;
                            }
                        }
                        note = (void *) ((char *) note + len);
                        size -= len;
                    }
                } else {
                    return ENOEXEC;
                }
            } else {
                return ENOEXEC;
            }
            break;

        case PT_PHDR:
            if (auxv) {
                auxv->a_type = AT_PHDR;
                auxv->a_un.a_ptr = (void *) (uintptr_t) phdr->p_vaddr;
                auxv++;
                auxv->a_type = AT_PHENT;
                auxv->a_un.a_val = ehdr.e_phentsize;
                auxv++;
                auxv->a_type = AT_PHNUM;
                auxv->a_un.a_val = (long) (phdr->p_memsz / ehdr.e_phentsize);
                auxv++;
            }
            break;

        case PT_INTERP:
            if (auxv) {
                interp = (char *) (uintptr_t) phdr->p_vaddr;
                auxv->a_type = AT_ENTRY;
                auxv->a_un.a_fcn = (void (*)(void))(uintptr_t) ehdr.e_entry;
                auxv++;
            }
            break;

        default:
            break;
        }
    }

    if (text_seg == -1 || data_seg == -1) {
        return ENOEXEC;
    }

    if (lsp) {
        for (i = 0; i < ehdr.e_phnum; i++) {
            if ((seg[i].size != 0) && seg[i].read_in) {
                if ((seg[i].prot & PROT_WRITE) == 0) {
                    mprotect((void *) seg[i].addr, seg[i].size, seg[i].prot);
                }
                if (seg[i].prot & PROT_EXEC) {
                    (void) msync((void *) seg[i].addr, seg[i].filesz,
                                 MS_INVALIDATE_ICACHE);
                }
            }
        }

        lsp->start_ip = ehdr.e_entry + (uintptr_t) base_reloc;
        root_trace(
            "elf_load: main exe start_ip=%p (e_entry=%p base_reloc=%p)\n",
            (void *) lsp->start_ip, (void *) (uintptr_t) ehdr.e_entry,
            (void *) base_reloc);
    }

    if (lsp && interp) {
        struct stat interpstat;
        int fd2;

        CRASHCHECK(!auxv);

        if (seg[text_seg].reloc) {
            auxv_t *p;

            interp += seg[text_seg].reloc;
            for (p = lsp->aux; p < auxv; p++) {
                switch (p->a_type) {
                case AT_ENTRY:
                case AT_PHDR:
                    p->a_un.a_ptr =
                        (char *) p->a_un.a_ptr + seg[text_seg].reloc;
                    break;
                default:
                    break;
                }
            }
        }
        if (seg[text_seg].reloc != seg[data_seg].reloc) {
            auxv->a_type = AT_DATA;
            auxv->a_un.a_ptr = (void *) seg[data_seg].addr;
            auxv++;
        }
        auxv->a_type = AT_PAGESZ;
        auxv->a_un.a_val = root_vm_ops.pagesize;
        auxv++;

        root_trace("elf_load: interp=%s\n", interp);

        /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
        if ((fd2 = open(interp, O_RDONLY)) == -1) {
            root_trace("elf_load: cannot open interpreter: %d\n", errno);
            return errno == EBUSY ? ETXTBSY : ELIBACC;
        }

        if (proc_read(fd2, &ehdr, sizeof ehdr, 0) != sizeof ehdr ||
            memcmp(ehdr.e_ident, ELFMAG, SELFMAG) ||
            ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
            ehdr.e_phentsize != sizeof *phdr) {
            close(fd2);
            return ELIBBAD;
        }
        if (ehdr.e_type != ET_DYN) {
            root_log("elf_load: interpreter %s is not ET_DYN (type=%d)\n",
                     interp, ehdr.e_type);
            close(fd2);
            return ELIBBAD;
        }
        if (ehdr.e_phnum == 0 ||
            ehdr.e_phnum > sizeof(buffer) / sizeof(*phdrs) ||
            ehdr.e_phoff > (uint64_t) INT64_MAX) {
            close(fd2);
            return ELIBBAD;
        }
        phdrs = (Elf64_Phdr *) buffer;
        size = ehdr.e_phnum * sizeof(*phdrs);
        if ((size_t) proc_read(fd2, phdrs, (ssize_t) size,
                               (off64_t) ehdr.e_phoff) != size) {
            close(fd2);
            return ELIBBAD;
        }

        base_addr = ~(uintptr_t) 0;

        /* Page-aligned high-water mark of the previously mapped PT_LOAD,
         * already in the relocated address space. Used to reject interpreters
         * whose page-rounded segment ranges overlap — MAP_FIXED below would
         * silently replace a sibling segment's pages otherwise.
         */
        uintptr_t prev_end = 0;
        for (i = 0, phdr = phdrs; i < ehdr.e_phnum; i++, phdr++) {
            void *vaddr;
            uintptr_t elf_page;
            uintptr_t seg_start;
            uintptr_t map_start;
            uintptr_t page_delta;
            uintptr_t map_size_u;
            size_t map_size;

            if (phdr->p_type != PT_LOAD)
                continue;
            if (validate_pt_load_policy(phdr, ELIBBAD) != EOK) {
                close(fd2);
                return ELIBBAD;
            }
            /* Empty PT_LOAD: nothing to map, advance to the next entry. */
            if (phdr->p_memsz == 0)
                continue;

            /* ELF requires p_vaddr ≡ p_offset (mod PAGESIZE). The seg_start |=
             * p_offset trick below — and the page_delta math — depend on that
             * congruence; reject violators.
             */
            if ((phdr->p_vaddr & (__PAGESIZE - 1)) !=
                (phdr->p_offset & (__PAGESIZE - 1))) {
                close(fd2);
                return ELIBBAD;
            }

            elf_page = ROUNDDOWN(phdr->p_vaddr, __PAGESIZE);
            seg_start = phdr->p_vaddr;
            if (phdr->p_flags & PF_W) {
                if (base_addr == ~(uintptr_t) 0) {
                    close(fd2);
                    return ELIBBAD;
                }
                prot = PROT_READ | PROT_WRITE;
                if (elf_addr_reloc_overflows(seg_start, base_reloc,
                                             &seg_start)) {
                    close(fd2);
                    return ELIBBAD;
                }
            } else {
                prot = PROT_READ | PROT_EXEC;
                if (base_addr != ~(uintptr_t) 0) {
                    if (elf_addr_reloc_overflows(seg_start, base_reloc,
                                                 &seg_start)) {
                        close(fd2);
                        return ELIBBAD;
                    }
                } else if (seg_start == 0) {
                    seg_start = ROUNDDOWN(CPU_LIBC_BASE, __PAGESIZE);
                    seg_start |= phdr->p_offset & (__PAGESIZE - 1);
                }
            }
            map_start = ROUNDDOWN(seg_start, __PAGESIZE);
            page_delta = seg_start - map_start;
            if (elf_addr_range_overflows(page_delta, phdr->p_memsz,
                                         &map_size_u) ||
                elf_roundup_overflows(map_size_u, __PAGESIZE, &map_size_u)) {
                close(fd2);
                return ELIBBAD;
            }
            map_size = (size_t) map_size_u;

            /* Page-rounded ranges must not overlap a sibling already mapped
             * this pass — MAP_FIXED would replace those pages and destroy bytes
             * loaded by the prior iteration. Only meaningful once base_addr is
             * known, since the first segment fixes the load bias.
             */
            if (base_addr != ~(uintptr_t) 0 && map_start < prev_end) {
                close(fd2);
                return ELIBBAD;
            }

            /* No demand paging: allocate anonymous pages and read the
             * interpreter data in directly, same as the main exe.
             */
            flags = MAP_ANON | MAP_PRIVATE | MAP_ELF;
            if (base_addr != ~(uintptr_t) 0)
                flags |= MAP_FIXED;
            vaddr = mmap((void *) map_start, map_size, prot | PROT_WRITE, flags,
                         NOFD, 0);
            if (vaddr == MAP_FAILED) {
                int orig_errno = errno;
                close(fd2);
                return (orig_errno == ENOMEM) ? ENOMEM : ELIBBAD;
            }
            if (base_addr == ~(uintptr_t) 0) {
                base_addr = (uintptr_t) vaddr;
                base_reloc = (intptr_t) ((uintptr_t) vaddr - elf_page);
                if (elf_addr_range_overflows((uintptr_t) vaddr, page_delta,
                                             &seg_start)) {
                    close(fd2);
                    return ELIBBAD;
                }
            }
            if (elf_addr_range_overflows((uintptr_t) vaddr, map_size,
                                         &prev_end)) {
                close(fd2);
                return ELIBBAD;
            }
            if (phdr->p_filesz > 0) {
                if (phdr->p_filesz > (uint64_t) SSIZE_MAX ||
                    phdr->p_offset > (uint64_t) INT64_MAX) {
                    close(fd2);
                    return ELIBBAD;
                }
                if ((size_t) proc_read(
                        fd2, (void *) seg_start, (ssize_t) phdr->p_filesz,
                        (off64_t) phdr->p_offset) != phdr->p_filesz) {
                    close(fd2);
                    return ENOEXEC;
                }
            }

            /* Drop PROT_WRITE on text segments now that content is loaded,
             * mirroring the main-exe path above.
             */
            if ((prot & PROT_WRITE) == 0) {
                mprotect(vaddr, map_size, prot);
                if (prot & PROT_EXEC)
                    msync(vaddr, map_size, MS_INVALIDATE_ICACHE);
            }
            {
                /* basename(3) may mutate its argument; give it a scratch copy.
                 */
                char interp_copy[PATH_MAX];

                snprintf(interp_copy, sizeof interp_copy, "%s", interp);
                debug_info((void *) seg_start, phdr, fd2,
                           basename(interp_copy));
            }
        }

        /* Sanity check: the interpreter must be self-contained (no PLT
         * imports). An ldso with JUMP_SLOT relocations would infinitely recurse
         * when lazy binding tries to resolve symbols through itself. Scan
         * PT_DYNAMIC for DT_PLTRELSZ > 0.
         */
        for (i = 0, phdr = phdrs; i < ehdr.e_phnum; i++, phdr++) {
            if (phdr->p_type == PT_DYNAMIC) {
                Elf64_Dyn *dyn =
                    (Elf64_Dyn *) (phdr->p_vaddr + (uintptr_t) base_reloc);
                for (; dyn->d_tag != DT_NULL; dyn++) {
                    if (dyn->d_tag == DT_PLTRELSZ && dyn->d_un.d_val > 0) {
                        root_log(
                            "elf_load: interpreter %s has PLT relocations"
                            " (DT_PLTRELSZ=%lu) — not self-contained!\n",
                            interp, (unsigned long) dyn->d_un.d_val);
                        close(fd2);
                        return ELIBBAD;
                    }
                }
                break;
            }
        }

        auxv->a_type = AT_BASE;
        auxv->a_un.a_ptr = (void *) base_addr;
        auxv++;

        /* Stuff the aux vector with the DEVICE/INODE of the interpreter */
        interpstat.st_dev = 0;
        interpstat.st_ino = 0;
        (void) fstat(fd2, &interpstat);
        auxv->a_type = AT_INTP_DEVICE;
        auxv->a_un.a_val = interpstat.st_dev;
        auxv++;
        auxv->a_type = AT_INTP_INODE;
        auxv->a_un.a_val = (long) interpstat.st_ino;
        auxv++;

        close(fd2);

        lsp->start_ip = ehdr.e_entry + (uintptr_t) base_reloc;
        root_trace(
            "elf_load: interp start_ip=%p (e_entry=%p base_reloc=%p "
            "base_addr=%p)\n",
            (void *) lsp->start_ip, (void *) (uintptr_t) ehdr.e_entry,
            (void *) base_reloc, (void *) base_addr);
    }

    if (auxv) {
        auxv->a_type = AT_NULL;
        auxv->a_un.a_val = 0;
    }

    debug_msg.i.type = _PROC_SPAWN;
    debug_msg.i.subtype = _PROC_SPAWN_DEBUG;
    debug_msg.i.text_addr = seg[text_seg].addr;
    debug_msg.i.text_size = seg[text_seg].size;
    debug_msg.i.text_reloc = 0;
    debug_msg.i.data_addr = seg[data_seg].addr;
    debug_msg.i.data_size = seg[data_seg].size;
    debug_msg.i.data_reloc = 0;
    if (!debug_msg.i.name[0]) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        STRLCPY(debug_msg.i.name, base,
                sizeof(debug_msg) - offsetof(struct _proc_spawn_debug, name));
    }
    /* ipc-send: roottask-fsio -> roottask-core */
    (void) msg_send(
        VM_COID, &debug_msg.i,
        offsetof(struct _proc_spawn_debug, name) + strlen(debug_msg.i.name) + 1,
        0, 0);

    return EOK;
}
