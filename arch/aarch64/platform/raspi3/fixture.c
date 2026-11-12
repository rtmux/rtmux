/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"
#include <string.h>
#include <sys/cpio.h>
#include <module/initramfs-elf.h>

extern const uint8_t pi3_archive[], pi3_archive_end[];

/* Bounded loader for the two-page architecture fixture. This does not replace
 * kernel/module/initramfs-elf.c or implement process address spaces. Reuse the
 * actual RTMux CPIO and ELF validation code before copying fixture segments. */
uintptr_t pi3_load_fixture(void)
{
    cpio_file_info_t file;
    size_t archive_size = (size_t) (pi3_archive_end - pi3_archive);
    if (!cpio_check_valid(pi3_archive, archive_size) ||
        !cpio_find_file(pi3_archive, archive_size, "boot/pi3-user", &file))
        pi3_fail("rtmux-cpio-fixture");
    if (!initramfs_elf_check(file.data, file.filesize) ||
        !initramfs_elf_find_rtmux(file.data, file.filesize))
        pi3_fail("rtmux-elf-fixture");

    /* Cross-ISA and truncated binaries must be rejected by the shared parser.
     */
    Elf64_Ehdr bad;
    memcpy(&bad, file.data, sizeof(bad));
    bad.e_machine = EM_RISCV;
    if (initramfs_elf_check(&bad, sizeof(bad)) ||
        initramfs_elf_check(file.data, sizeof(bad) - 1))
        pi3_fail("rtmux-elf-negative");

    const Elf64_Ehdr *eh = (const void *) file.data;
    if (eh->e_type != ET_EXEC || eh->e_version != EV_CURRENT ||
        eh->e_ehsize != sizeof(*eh) || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phnum != 2 || eh->e_entry != PI3_USER_TEXT ||
        eh->e_phoff > file.filesize ||
        2 * sizeof(Elf64_Phdr) > file.filesize - eh->e_phoff)
        pi3_fail("fixture-elf-layout");
    const Elf64_Phdr *ph = (const void *) (file.data + eh->e_phoff);
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t target = i == 0 ? PI3_USER_TEXT : PI3_USER_DATA;
        unsigned flags = i == 0 ? PF_R | PF_X : PF_R | PF_W;
        if (ph[i].p_type != PT_LOAD || ph[i].p_vaddr != target ||
            ph[i].p_paddr != target || ph[i].p_flags != flags ||
            ph[i].p_memsz > 4096 || ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_offset > file.filesize ||
            ph[i].p_filesz > file.filesize - ph[i].p_offset)
            pi3_fail("fixture-elf-segment");
        memset((void *) target, 0, 4096);
        memcpy((void *) target, file.data + ph[i].p_offset, ph[i].p_filesz);
    }
    memset((void *) (uintptr_t) PI3_USER_STACK, 0, 4096);
    pi3_pass("rtmux-cpio-elf-aarch64");
    return eh->e_entry;
}
