/*
 * cpu-elf-reloc.c — RISC-V ELF relocation engine.
 *
 * Implements cpu_apply_reloc(), cpu_apply_rel_relative(), and
 * cpu_elf_machine_valid() for the RISC-V architecture.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/elf.h>
#include <elf/riscv.h>
#include <cpu-elf.h>

#define KERLINK_MAX_HI20 4096

struct hi20_entry {
    uintptr_t hi20_addr;
    int64_t value;
};

static struct hi20_entry hi20_table[KERLINK_MAX_HI20];
static unsigned hi20_count;

void cpu_reloc_hi20_reset(void)
{
    hi20_count = 0;
}

static void hi20_record(uintptr_t addr, int64_t value)
{
    if (hi20_count < KERLINK_MAX_HI20) {
        hi20_table[hi20_count].hi20_addr = addr;
        hi20_table[hi20_count].value = value;
        hi20_count++;
    } else {
        crash_halt("cpu_elf_reloc: PCREL_HI20 table overflow (%u entries)",
                   KERLINK_MAX_HI20);
    }
}

static int64_t hi20_find(uintptr_t addr)
{
    unsigned i;
    for (i = 0; i < hi20_count; i++) {
        if (hi20_table[hi20_count - 1 - i].hi20_addr == addr)
            return hi20_table[hi20_count - 1 - i].value;
    }
    return 0;
}

static inline uint32_t bits(uint64_t val, int hi, int lo)
{
    return (uint32_t) ((val >> lo) & ((1UL << (hi - lo + 1)) - 1));
}

static void patch_u_type(uint32_t *insn, uint32_t imm20)
{
    *insn = (*insn & 0x00000FFF) | (imm20 << 12);
}

static void patch_i_type(uint32_t *insn, uint32_t imm12)
{
    *insn = (*insn & 0x000FFFFF) | (imm12 << 20);
}

static void patch_s_type(uint32_t *insn, uint32_t imm12)
{
    *insn =
        (*insn & 0x01FFF07F) | ((imm12 & 0xFE0) << 20) | ((imm12 & 0x01F) << 7);
}

static void patch_b_type(uint32_t *insn, uint32_t offset)
{
    *insn = (*insn & 0x01FFF07F) | (bits(offset, 12, 12) << 31) |
            (bits(offset, 10, 5) << 25) | (bits(offset, 4, 1) << 8) |
            (bits(offset, 11, 11) << 7);
}

static void patch_j_type(uint32_t *insn, uint32_t offset)
{
    *insn = (*insn & 0x00000FFF) | (bits(offset, 20, 20) << 31) |
            (bits(offset, 10, 1) << 21) | (bits(offset, 11, 11) << 20) |
            (bits(offset, 19, 12) << 12);
}

int cpu_elf_machine_valid(uint16_t machine)
{
    return machine == EM_RISCV;
}

int cpu_apply_reloc(const Elf64_Rela *rela,
                    uint32_t type,
                    uintptr_t S,
                    uintptr_t P,
                    int sym_resolved,
                    const char *name)
{
    int64_t A = rela->r_addend;

    if (!sym_resolved && type != R_RISCV_NONE)
        return 0;

    switch (type) {
    case R_RISCV_NONE:
        break;

    case R_RISCV_64:
        *(uint64_t *) P = (uint64_t) ((int64_t) S + A);
        break;

    case R_RISCV_32:
        *(uint32_t *) P = (uint32_t) ((int64_t) S + A);
        break;

    case R_RISCV_RELATIVE:
        *(uint64_t *) P = (uint64_t) ((int64_t) S + A);
        break;

    case R_RISCV_JUMP_SLOT:
        *(uint64_t *) P = (uint64_t) S;
        break;

    case R_RISCV_HI20: {
        int64_t val = (int64_t) S + A;
        uint32_t hi = (uint32_t) ((val + 0x800) >> 12);
        patch_u_type((uint32_t *) P, hi);
        break;
    }

    case R_RISCV_LO12_I: {
        int64_t val = (int64_t) S + A;
        patch_i_type((uint32_t *) P, (uint32_t) (val & 0xFFF));
        break;
    }

    case R_RISCV_LO12_S: {
        int64_t val = (int64_t) S + A;
        patch_s_type((uint32_t *) P, (uint32_t) (val & 0xFFF));
        break;
    }

    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT: {
        int64_t offset = (int64_t) S + A - (int64_t) P;
        uint32_t hi = (uint32_t) ((offset + 0x800) >> 12);
        uint32_t lo = (uint32_t) (offset & 0xFFF);
        patch_u_type((uint32_t *) P, hi);
        patch_i_type((uint32_t *) (P + 4), lo);
        break;
    }

    case R_RISCV_PCREL_HI20: {
        int64_t val = (int64_t) S + A - (int64_t) P;
        uint32_t hi = (uint32_t) ((val + 0x800) >> 12);
        patch_u_type((uint32_t *) P, hi);
        hi20_record(P, val);
        break;
    }

    case R_RISCV_PCREL_LO12_I: {
        int64_t hi_val = hi20_find((uintptr_t) ((int64_t) S + A));
        patch_i_type((uint32_t *) P, (uint32_t) (hi_val & 0xFFF));
        break;
    }

    case R_RISCV_PCREL_LO12_S: {
        int64_t hi_val = hi20_find((uintptr_t) ((int64_t) S + A));
        patch_s_type((uint32_t *) P, (uint32_t) (hi_val & 0xFFF));
        break;
    }

    case R_RISCV_BRANCH: {
        int64_t offset = (int64_t) S + A - (int64_t) P;
        patch_b_type((uint32_t *) P, (uint32_t) offset);
        break;
    }

    case R_RISCV_JAL: {
        int64_t offset = (int64_t) S + A - (int64_t) P;
        patch_j_type((uint32_t *) P, (uint32_t) offset);
        break;
    }

    case R_RISCV_ADD8:
        *(uint8_t *) P += (uint8_t) ((int64_t) S + A);
        break;
    case R_RISCV_ADD16:
        *(uint16_t *) P += (uint16_t) ((int64_t) S + A);
        break;
    case R_RISCV_ADD32:
        *(uint32_t *) P += (uint32_t) ((int64_t) S + A);
        break;
    case R_RISCV_ADD64:
        *(uint64_t *) P += (uint64_t) ((int64_t) S + A);
        break;
    case R_RISCV_SUB8:
        *(uint8_t *) P -= (uint8_t) ((int64_t) S + A);
        break;
    case R_RISCV_SUB16:
        *(uint16_t *) P -= (uint16_t) ((int64_t) S + A);
        break;
    case R_RISCV_SUB32:
        *(uint32_t *) P -= (uint32_t) ((int64_t) S + A);
        break;
    case R_RISCV_SUB64:
        *(uint64_t *) P -= (uint64_t) ((int64_t) S + A);
        break;

    case R_RISCV_SET6:
        *(uint8_t *) P =
            (*(uint8_t *) P & 0xC0) | ((uint8_t) ((int64_t) S + A) & 0x3F);
        break;
    case R_RISCV_SUB6:
        *(uint8_t *) P =
            (*(uint8_t *) P & 0xC0) |
            (((*(uint8_t *) P & 0x3F) - ((uint8_t) ((int64_t) S + A) & 0x3F)) &
             0x3F);
        break;
    case R_RISCV_SET8:
        *(uint8_t *) P = (uint8_t) ((int64_t) S + A);
        break;
    case R_RISCV_SET16:
        *(uint16_t *) P = (uint16_t) ((int64_t) S + A);
        break;
    case R_RISCV_SET32:
        *(uint32_t *) P = (uint32_t) ((int64_t) S + A);
        break;

    case R_RISCV_32_PCREL:
        *(uint32_t *) P = (uint32_t) ((int64_t) S + A - (int64_t) P);
        break;

    case R_RISCV_RELAX:
        break;

    case R_RISCV_ALIGN:
        break;

    default:
        kprintf("cpu_elf_reloc: %s: unsupported reloc type %u at %p\n", name,
                type, (void *) P);
        return -1;
    }

    return 1;
}

int cpu_apply_rel_relative(uint32_t type,
                           uintptr_t P,
                           uintptr_t bias,
                           int64_t addend)
{
    if (type != R_RISCV_RELATIVE)
        return 0;
    *(uint64_t *) P = (uint64_t) (bias + (uintptr_t) addend);
    return 1;
}
