/*
 * Fuzz harness for the kernel initramfs ELF validator initramfs_elf_find_rtmux.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* initramfs_elf_find_rtmux (kernel/module/elf-validate.c) walks an untrusted
 * ELF image: it validates the header, then indexes the section-header table and
 * the section-name string table by 64-bit offsets taken straight from the
 * image. Those offset arithmetic paths are exactly where a bad bound lets a
 * read run off the buffer, so the whole fuzz value is aiming ASan at that walk.
 * The harness hands the raw fuzz bytes in as the image and folds the returned
 * pointer into a volatile sink so the call is not elided.
 *
 * cpu_elf_machine_valid is the one external the validator calls; the harness
 * stubs it to accept every machine so a fuzzed e_machine never gates off the
 * section walk.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/rtmux-ident.h>

const struct rtmux_ident *initramfs_elf_find_rtmux(const void *base,
                                                   size_t size);

int cpu_elf_machine_valid(uint16_t machine)
{
    (void) machine;
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static volatile uintptr_t sink;

    /* The validator reads Elf64 fields at file offsets taken from the image, so
     * a fuzzed e_shoff makes those reads unaligned. That is a load-alignment
     * property of the loader as a whole (RV64/OpenSBI define and emulate
     * misaligned loads) and stays in bounds, so it is orthogonal to the offset
     * math under test; the elf target builds with -fno-sanitize=alignment so
     * the fuzzer stays aimed at the bounds logic. AddressSanitizer still flags
     * any genuine out-of-bounds read regardless of alignment.
     */
    sink = (uintptr_t) initramfs_elf_find_rtmux(data, size);
    return 0;
}
