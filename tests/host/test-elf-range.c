/*
 * Host-side tests for ELF loader range arithmetic.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <elf-range.h>

#define ELF64_PHDR_SIZE 56
#define PT_LOAD 1
#define PAGE_SIZE 4096UL

struct phdr_fixture {
    uint32_t type;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

static int validate_loads(const struct phdr_fixture *ph,
                          size_t n,
                          size_t file_size)
{
    for (size_t i = 0; i < n; i++) {
        uintptr_t start;
        uintptr_t end;

        if (ph[i].type != PT_LOAD)
            continue;
        if (ph[i].filesz > ph[i].memsz)
            return -1;
        if ((ph[i].align & (PAGE_SIZE - 1)) != 0)
            return -1;
        if (elf_file_range_overflows(ph[i].offset, ph[i].filesz, file_size))
            return -1;
        if (elf_addr_range_overflows(ph[i].vaddr, ph[i].memsz, &end))
            return -1;
        start = (uintptr_t) ph[i].vaddr;
        for (size_t j = 0; j < i; j++) {
            uintptr_t prev_start;
            uintptr_t prev_end;

            if (ph[j].type != PT_LOAD)
                continue;
            if (elf_addr_range_overflows(ph[j].vaddr, ph[j].memsz, &prev_end))
                return -1;
            prev_start = (uintptr_t) ph[j].vaddr;
            if (start < prev_end && prev_start < end)
                return -1;
        }
    }
    return 0;
}

static void test_file_ranges(void)
{
    assert(!elf_file_range_overflows(8, 4, 16));
    assert(elf_file_range_overflows(17, 0, 16));
    assert(elf_file_range_overflows(12, 5, 16));
    assert(elf_file_range_overflows(UINT64_MAX - 1, 8, 16));
}

static void test_address_ranges(void)
{
    uintptr_t end;

    assert(!elf_addr_range_overflows(0x1000, 0x20, &end));
    assert(end == 0x1020);
    assert(elf_addr_range_overflows(UINTPTR_MAX - 7, 8, &end));
    assert(!elf_addr_reloc_overflows(0x1000, -0x100, &end));
    assert(end == 0xf00);
    assert(elf_addr_reloc_overflows(0x80, -0x100, &end));
}

static void test_roundup_and_phdr_table(void)
{
    uintptr_t rounded;
    assert(!elf_roundup_overflows(0x1001, 0x1000, &rounded));
    assert(rounded == 0x2000);
    assert(elf_roundup_overflows(UINTPTR_MAX - 7, 0x1000, &rounded));

    assert(!elf_phdr_table_overflows(64, 2, ELF64_PHDR_SIZE,
                                     64 + 2 * ELF64_PHDR_SIZE));
    assert(elf_phdr_table_overflows(64, 2, ELF64_PHDR_SIZE,
                                    64 + 2 * ELF64_PHDR_SIZE - 1));
    assert(!elf_phdr_table_overflows(64, 2, 4, 72));
    assert(elf_phdr_table_overflows(64, 2, ELF64_PHDR_SIZE, 72));
    assert(elf_phdr_table_overflows(UINT64_MAX - 4, 2, ELF64_PHDR_SIZE,
                                    UINTPTR_MAX));
}

static void test_pt_load_fixtures(void)
{
    static const struct phdr_fixture valid_gap[] = {
        {PT_LOAD, 0x100, 0x1000, 0x20, 0x100, PAGE_SIZE},
        {PT_LOAD, 0x200, 0x3000, 0x10, 0x80, PAGE_SIZE},
    };
    static const struct phdr_fixture overlap[] = {
        {PT_LOAD, 0x100, 0x1000, 0x20, 0x100, PAGE_SIZE},
        {PT_LOAD, 0x200, 0x1080, 0x10, 0x80, PAGE_SIZE},
    };
    static const struct phdr_fixture bad_align[] = {
        {PT_LOAD, 0x100, 0x1000, 0x20, 0x100, 512},
    };
    static const struct phdr_fixture file_overflow[] = {
        {PT_LOAD, 0xff0, 0x1000, 0x20, 0x100, PAGE_SIZE},
    };

    assert(validate_loads(valid_gap, 2, 0x1000) == 0);
    assert(validate_loads(overlap, 2, 0x1000) == -1);
    assert(validate_loads(bad_align, 1, 0x1000) == -1);
    assert(validate_loads(file_overflow, 1, 0x1000) == -1);
}

int main(void)
{
    test_file_ranges();
    test_address_ranges();
    test_roundup_and_phdr_table();
    test_pt_load_fixtures();
    return 0;
}
