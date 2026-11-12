/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

/* Only CPU 0 runs synchronous fault tests. Expectations are exact; unexpected
 * exceptions never skip an instruction or produce a success marker. */
static struct {
    bool armed;
    bool write;
    uint64_t ec, fsc, far, pc, resume;
} expected;
static unsigned user_faults;
static unsigned user_calls;
static unsigned user_timer_state;

void pi3_exception_init(void)
{
    __asm__ volatile("msr vbar_el1, %0; isb" ::"r"(pi3_vectors) : "memory");
}

void pi3_expect_fault(uint64_t ec,
                      uint64_t fsc,
                      uint64_t far,
                      uint64_t pc,
                      uint64_t resume,
                      bool write)
{
    if (expected.armed)
        pi3_fail("fault-already-armed");
    expected.ec = ec;
    expected.fsc = fsc;
    expected.far = far;
    expected.pc = pc;
    expected.resume = resume;
    expected.write = write;
    expected.armed = true;
}

void pi3_fault_consumed(void)
{
    if (expected.armed)
        pi3_fail("expected-fault-not-taken");
}

/* Private test protocol (SVC #0x5254). This is deliberately NOT RTMux's
 * syscall dispatch ABI: no process, capability or scheduler exists yet. */
static void user_svc(struct a64_exception_frame *f)
{
    if ((f->pstate & 15) || (f->esr & 0xffff) != PI3_TEST_SVC ||
        f->sp != PI3_USER_STACK_TOP || f->pc < PI3_USER_TEXT ||
        f->pc >= PI3_USER_TEXT + 4096)
        pi3_fail("el0-svc-context");
    switch (f->x[8]) {
    case 0:
        if (user_calls++ || f->x[0] != 19 || f->x[1] != 23 ||
            f->x[19] != 0x1919 || (f->pstate >> 28) != 6)
            pi3_fail("el0-register-save");
        for (unsigned reg = 2; reg < 31; reg++) {
            uint64_t value = reg == 8    ? 0
                             : reg == 9  ? 0x9999
                             : reg == 19 ? 0x1919
                                         : reg;
            if (f->x[reg] != value)
                pi3_fail("el0-general-register-save");
        }
        f->x[0] = f->x[0] + f->x[1];
        break;
    case 1:
        pi3_fault_consumed();
        if (f->x[0] || user_calls != 1 || user_faults != 5 ||
            user_timer_state != 2)
            pi3_fail("el0-fixture-result");
        pi3_pass("el0-svc-registers");
        pi3_pass("el0-memory-isolation");
        pi3_puts("PI3 BRINGUP PASS\n");
        pi3_halt();
    case 2: {
        pi3_fault_consumed();
        /* Fixture requests five specific protection/translation faults. */
        static const uint64_t fault_ec[] = {0x24, 0x24, 0x20, 0x24, 0x24};
        /* Hierarchical APTable restrictions are combined with leaf AP; the
         * resulting permission fault reports the terminating L3 level. */
        static const uint64_t fault_fsc[] = {15, 15, 15, 15, 7};
        static const uint64_t fault_far[] = {
            0x80000, PI3_USER_TEXT, PI3_USER_DATA,
            PI3_HIGH_BASE + PI3_USER_DATA, PI3_USER_STACK - 4096};
        if (user_faults >= 5 || f->x[0] != fault_far[user_faults] ||
            f->x[1] < PI3_USER_TEXT || f->x[1] >= PI3_USER_TEXT + 4096 ||
            (f->x[1] & 3))
            pi3_fail("el0-fault-request");
        uint64_t pc = user_faults == 2 ? PI3_USER_DATA : f->x[1] - 4;
        pi3_expect_fault(fault_ec[user_faults], fault_fsc[user_faults],
                         fault_far[user_faults], pc, f->x[1], user_faults == 1);
        break;
    }
    case 3:
        if (user_timer_state != 0)
            pi3_fail("el0-timer-start-state");
        user_timer_state = 1;
        pi3_user_timer_start();
        break;
    case 4:
        if (user_timer_state != 1)
            pi3_fail("el0-timer-finish-state");
        pi3_user_timer_finish();
        user_timer_state = 2;
        break;
    default:
        f->x[0] = (uint64_t) -1;
        break;
    }
}

void pi3_exception(struct a64_exception_frame *f, unsigned vector)
{
    if (vector == 5 || vector == 9) {
        pi3_irq(vector == 9);
        return;
    }
    uint64_t ec = f->esr >> 26;
    if (pi3_cpu() == 0 && vector == 8 && ec == 0x15) {
        user_svc(f);
        return;
    }
    bool data_abort = ec == 0x24 || ec == 0x25;
    bool instruction_abort = ec == 0x20;
    if (pi3_cpu() == 0 && (vector == 4 || vector == 8) && expected.armed &&
        (data_abort || instruction_abort) && ec == expected.ec &&
        (f->esr & 0x3f) == expected.fsc && f->far == expected.far &&
        f->pc == expected.pc &&
        (!data_abort || !!(f->esr & BIT64(6)) == expected.write)) {
        f->pc = expected.resume;
        expected.armed = false;
        if (vector == 8)
            user_faults++;
        return;
    }
    pi3_puts("exception vector=");
    pi3_hex(vector);
    pi3_puts(" esr=");
    pi3_hex(f->esr);
    pi3_puts(" far=");
    pi3_hex(f->far);
    pi3_puts(" pc=");
    pi3_hex(f->pc);
    pi3_puts("\n");
    pi3_fail("unexpected-exception");
}
