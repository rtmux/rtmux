/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

/* These are normally defined in servers/roottask/sys/timestamp.c. Provide weak
 * defaults for the kernel-only build.
 */
__attribute__((weak)) const char timestamp[] = __DATE__ " " __TIME__;
__attribute__((weak)) const char os_version_string[] = "RTMux";

/* Hex-dump a region of memory as 32-bit words, 8 per line. Parameters:
 *   name : label printed before the dump.
 *   p    : start of the region.
 *   size : region size in bytes.
 */
static void dump_mem32(const char *name, const uint32_t *p, unsigned size)
{
    unsigned i;

    kprintf("\n%s[%p]:", name, p);
    for (i = 0; i < size; i += sizeof(uint32_t)) {
        if ((i % (8 * sizeof(uint32_t))) == 0) {
            kprintf("\n%04x:", i);
        }
        kprintf(" %08x", *p++);
    }
}

#define VTHP_CHAR ' '
#define INVTHP_CHAR '?'

/* Crash-shutdown handler: print a diagnostic banner and reboot.
 *
 * Dumps the fault's signal/code/fault breakdown, the in-kernel state word, each
 * CPU's active process/thread and address-space, the faulting CPU register
 * context, the instruction bytes at the fault PC, and a slice of the stack —
 * then reboots the system. Guarded against re-entry by a static flag so a fault
 * during the dump still reboots. Parameters:
 *   sigcode : encoded signal/code/fault that triggered the shutdown.
 *   reg     : CPU register context at the point of the fault.
 */
void shutdown(unsigned sigcode, const CPU_REGISTERS *reg)
{
    unsigned inkernel_state;
    thread_t *thp;
    process_t *prp;
    process_t *prp2;
    unsigned i;
    uint32_t *stack;
    uint8_t *ip;
    char valid_thp;
    static int shutting_down = 0; /* In case of fault during shutdown() */
    if (!shutting_down) {
        shutting_down = 1;
        SPINUNLOCK(&debug_slock);

        kprintf("\nShutdown[%d] S/C/F=%d/%d/%d", get_cpunum(),
                SIGCODE_SIGNO(sigcode), SIGCODE_CODE(sigcode),
                SIGCODE_FAULT(sigcode));

        valid_thp = VTHP_CHAR;
        inkernel_state = get_inkernel();
        if (inkernel_state) {
            kprintf(" state(%x)=", inkernel_state);
            if (inkernel_state & INKERNEL_NOW) {
                kprintf(" now");
            }
            if (inkernel_state & INKERNEL_LOCK) {
                kprintf(" lock");
                valid_thp = INVTHP_CHAR;
            }
            if (inkernel_state & INKERNEL_EXIT) {
                kprintf(" exit");
            }
            if (inkernel_state & INKERNEL_SPECRET) {
                kprintf(" specret");
                valid_thp =
                    VTHP_CHAR; /* SPECRET overrides LOCK state for valid thp */
            }
            if (inkernel_state & INKERNEL_INTRMASK) {
                kprintf(" %d", inkernel_state & INKERNEL_INTRMASK);
                valid_thp = INVTHP_CHAR;
            }
        }
        kprintf("\nRTMux %s (%s)", os_version_string, timestamp);
        extern unsigned hart_ids[];
        for (int _hi = 0; _hi < num_processors; ++_hi) {
            i = hart_ids[_hi];
            thp = get_active_thread(i);
            if (!thp)
                continue;
            prp = thp->process;

            /* prp->debug_name moved to root_proc(pid)->debug_name (roottask
             * heap), unreachable from kernel S-mode while roottask's pgdir
             * isn't loaded. Crash banners now print pid only.
             */
            kprintf("\n[%d]PID-TID=%d-%d%c P/T FL=%08x/%08x", i, prp->pid,
                    thp->tid + 1, valid_thp, prp->flags, thp->flags);
            prp2 = pcpu_array[i].pc_active_prp;
            if (prp != prp2) {
                kprintf("\n[%d]ACTIVE PID=%d PF=%08x", i, prp2->pid,
                        prp2->flags);
            }
            prp2 = pcpu_array[i].pc_as_prp;
            if (!prp2) {
                kprintf("\n[%d]AS=>NULL", i);
            } else if (prp != prp2) {
                kprintf("\n[%d]AS PID=%d PF=%08x", i, prp2->pid, prp2->flags);
            }
        }

        dump_mem32(CPU_STRINGNAME " context", (void *) reg,
                   sizeof(CPU_REGISTERS));

        ip = (uint8_t *) REGIP(reg);
        kprintf("\ninstruction[%p]:\n", ip);
        for (i = 0; i < 26; ++i) {
            kprintf("%02x ", *ip++);
        }

        stack = (uint32_t *) REGSP(reg);
        dump_mem32("stack", stack, 32 * 4);
        kprintf("\n");
    }

    system_reboot(1);
}
