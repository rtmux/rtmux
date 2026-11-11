/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * ELF64 trust-boundary validation for CPIO initramfs modules
 *
 * The pure-parsing half of the initramfs ELF loader: header sanity and the
 * .rtmux identity-section lookup. It reads an untrusted image buffer and never
 * touches paging, allocation, or per-CPU state, so it builds host-side under a
 * fuzz harness (see tests/host/fuzz/fuzz-elf.c) independent of the Sv39/bare
 * loaders in initramfs-elf.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/elf.h>
#include <sys/rtmux-ident.h>
#include <cpu-elf.h>
#include <elf-range.h>
#include <module/initramfs-elf.h>

/* Validate that a buffer holds a little-endian ELF64 image for a supported
 * machine. Parameters:
 *   base : buffer to check.
 *   size : buffer size in bytes.
 * Return value: true if it is a valid ELF64 image, false otherwise.
 */
bool initramfs_elf_check(const void *base, size_t size)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;

    if (size < sizeof(Elf64_Ehdr))
        return false;

    /* Check ELF magic */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return false;

    /* Must be ELF64 */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        return false;

    /* Must be little-endian */
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return false;

    if (!cpu_elf_machine_valid(ehdr->e_machine))
        return false;

    return true;
}

/* Locate and validate the .rtmux identity section of an ELF image. Parameters:
 *   base : ELF image buffer.
 *   size : buffer size in bytes.
 * Return value: pointer to the rtmux_ident, or NULL if absent/invalid.
 *
 * Every offset in the section-header table is attacker-controlled and 64-bit
 * wide, so each range test goes through elf_file_range_overflows rather than a
 * raw off + len compare that a near-UINT64_MAX offset could wrap past the
 * buffer end.
 */
const struct rtmux_ident *initramfs_elf_find_rtmux(const void *base,
                                                   size_t size)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;
    const Elf64_Shdr *shdr;
    const char *shstrtab;
    int i;

    if (!initramfs_elf_check(base, size))
        return NULL;

    /* Need section headers to find .rtmux */
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0)
        return NULL;
    if (ehdr->e_shstrndx == 0 || ehdr->e_shstrndx >= ehdr->e_shnum)
        return NULL;

    /* Entries are indexed with a fixed Elf64_Shdr stride below, so reject a
     * declared entry size that would not match that layout (mirrors the
     * e_phentsize check on the program-header path).
     */
    if (ehdr->e_shentsize != sizeof(Elf64_Shdr))
        return NULL;

    /* Bounds check for section header table */
    if (elf_file_range_overflows(
            ehdr->e_shoff, (uint64_t) ehdr->e_shnum * sizeof(Elf64_Shdr), size))
        return NULL;
    shdr = (const Elf64_Shdr *) ((const uint8_t *) base + ehdr->e_shoff);

    /* Get the section name string table */
    const Elf64_Shdr *strtab_sh = &shdr[ehdr->e_shstrndx];
    if (elf_file_range_overflows(strtab_sh->sh_offset, strtab_sh->sh_size,
                                 size))
        return NULL;
    shstrtab = (const char *) base + strtab_sh->sh_offset;

    /* Search for .rtmux section */
    for (i = 0; i < ehdr->e_shnum; i++) {
        /* sh_name indexes into the string table; the ".rtmux\0" compare below
         * reads seven bytes, so require that many to stay inside the table.
         */
        if (shdr[i].sh_name >= strtab_sh->sh_size ||
            strtab_sh->sh_size - shdr[i].sh_name < 7)
            continue;
        const char *name = shstrtab + shdr[i].sh_name;
        if (name[0] == '.' && name[1] == 'r' && name[2] == 't' &&
            name[3] == 'm' && name[4] == 'u' && name[5] == 'x' &&
            name[6] == '\0') {
            if (shdr[i].sh_size < sizeof(struct rtmux_ident))
                return NULL;
            if (elf_file_range_overflows(shdr[i].sh_offset, shdr[i].sh_size,
                                         size))
                return NULL;
            const struct rtmux_ident *qi =
                (const struct rtmux_ident *) ((const uint8_t *) base +
                                              shdr[i].sh_offset);
            if (qi->qi_magic != RTMUX_MAGIC)
                return NULL;
            return qi;
        }
    }

    return NULL;
}
