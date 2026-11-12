/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_PI3_BOOTSTRAP_H
#define RTMUX_PI3_BOOTSTRAP_H

#define PI3_CPUS 4
#define PI3_STACK_SIZE 16384
#define PI3_UART 0x3f201000
#define PI3_LOCAL 0x40000000
#define PI3_SPIN_TABLE 0xd8
#define PI3_RAM_END 0x3c000000
#define PI3_HIGH_BASE 0xffffffc000000000

/* Fixed addresses of the architecture-test fixture, not a user ABI. */
#define PI3_USER_TEXT 0x200000
#define PI3_USER_DATA 0x201000
#define PI3_USER_STACK 0x204000
#define PI3_USER_STACK_TOP 0x205000
#define PI3_PROBE_VA 0x210000
#define PI3_TEST_SVC 0x5254

#ifndef __ASSEMBLY__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <exception.h>

#define BIT64(n) (1UL << (n))

static inline unsigned pi3_cpu(void)
{
    uint64_t id;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(id));
    return (unsigned) (id & 0xff);
}

static inline uint64_t pi3_counter(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value));
    return value;
}

static inline uint64_t pi3_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

static inline void pi3_write32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *) address = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

static inline uint32_t pi3_read32(uintptr_t address)
{
    uint32_t value = *(volatile uint32_t *) address;
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static inline void pi3_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void pi3_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

void pi3_uart_init(void);
void pi3_puts(const char *text);
void pi3_hex(uint64_t value);
void pi3_pass(const char *test);
_Noreturn void pi3_fail(const char *test);
_Noreturn void pi3_halt(void);
void pi3_mmu_init(void);
void pi3_mmu_enable(void);
void pi3_mmu_test(void);
void pi3_mmu_probe_map(uintptr_t physical);
void pi3_exception_init(void);
void pi3_exception(struct a64_exception_frame *frame, unsigned vector);
void pi3_expect_fault(uint64_t ec,
                      uint64_t fsc,
                      uint64_t far,
                      uint64_t pc,
                      uint64_t resume,
                      bool write);
void pi3_fault_consumed(void);
void pi3_irq(bool from_user);
void pi3_timer_test(void);
void pi3_user_timer_start(void);
void pi3_user_timer_finish(void);
void pi3_smp_test(void);
void pi3_primary(unsigned boot_el);
_Noreturn void pi3_secondary(void);
_Noreturn void pi3_enter_user(uintptr_t entry, uintptr_t stack);
uintptr_t pi3_load_fixture(void);

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __bss_start[], __bss_end[], __image_end[];
extern char pi3_vectors[], pi3_secondary_start[];
extern char pi3_fault_read[], pi3_fault_read_resume[];
extern char pi3_fault_write[], pi3_fault_write_resume[];
uint64_t pi3_test_read(uintptr_t address);
void pi3_test_write(uintptr_t address);
#endif
#endif
