/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * no-syschan — a process that never registers a system channel.
 *
 * It is linked without crt0, so libc's start-up never runs: no system channel,
 * no system thread, nothing in the process that could act on a signal. The
 * kill-busy suite sends it SIGKILL, which roottask must carry out by itself. It
 * spins for about 20 s of the timebase and then traps, so a kernel that cannot
 * kill it does not keep a hart busy for the rest of the run.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

static unsigned long timebase(void)
{
    unsigned long t;

    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

void _start(void)
{
    /* The QEMU virt timebase runs at 10 MHz. */
    unsigned long t0 = timebase();

    while (timebase() - t0 < 200000000UL)
        ;
    __builtin_trap();
}
