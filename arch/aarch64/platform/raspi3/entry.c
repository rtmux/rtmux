/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

void pi3_primary(unsigned boot_el)
{
    pi3_uart_init();
    pi3_puts(
        "RTMux AArch64/Pi3 bring-up\n"
        "Experimental architecture tests; no scheduler or roottask yet.\n"
        "entry-el=");
    pi3_hex(boot_el);
    pi3_puts("\n");
    uint64_t current;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current));
    if (current != 4 || pi3_cpu() != 0)
        pi3_fail("boot-el1");
    pi3_exception_init();
    pi3_pass("boot-el1-uart");
    uintptr_t entry = pi3_load_fixture();
    pi3_mmu_init();
    pi3_mmu_enable();
    pi3_mmu_test();
    pi3_timer_test();
    pi3_smp_test();
    pi3_enter_user(entry, PI3_USER_STACK_TOP);
}
