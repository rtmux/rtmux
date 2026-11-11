/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* gpio-latency - delegate a SiFive GPIO controller and prepare its pin IRQ for
 * a hardware IRQ-to-IST latency capture.
 *
 * The GPIO controller is requested through the same delegated-capability path
 * as the goldfish-rtc alarm used by irq-notify: rtmux_delegate_device asks the
 * virtio-mmio resource server for the MMIO window and PLIC vector that the
 * kernel published in hwinfo for a "sifive-gpio" node. On real SiFive hardware
 * (FU540/FU740) that yields a delegated pin-interrupt source the caller can
 * time from IRQ entry to first IST instruction.
 *
 * QEMU virt has no sifive,gpio0, so the kernel never publishes the node and the
 * delegation returns ENODEV. That is the expected outcome under QEMU: this
 * probe reports UNSUPPORTED and exits 0 so the emulator run stays green, while
 * a real IPC or delegation fault exits nonzero. The physical pulse capture
 * itself is a separate FU740 hardware runner, board-gated and out of scope
 * here.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/virtio-mmio-msg.h>

int main(void)
{
    struct rtmux_resource_handle h;
    int rc = rtmux_delegate_device(RTMUX_DEVICE_SIFIVE_GPIO, 0x1000,
                                   RTMUX_DELEGATE_ANY_BASE, &h);

    if (rc == ENODEV) {
        /* No sifive,gpio0 on this platform (QEMU virt). Expected skip. */
        printf("gpio-latency: UNSUPPORTED no sifive-gpio on this platform\n");
        return 0;
    }
    if (rc != EOK) {
        fprintf(stderr, "gpio-latency: delegate error: %s\n", strerror(rc));
        return 1;
    }

    /* Delegation succeeded, so a SiFive GPIO is present (FU740-class board).
     * The pin IRQ is now owned by this process; the physical IRQ-to-IST pulse
     * capture runs from a separate hardware runner, so here we only confirm the
     * authority path handed back a usable PLIC vector to time. The MMIO base is
     * not checked: RTMUX_DELEGATE_ANY_BASE permits a real base of 0, and the
     * delegation already validated the handle.
     */
    if (h.irq_vector < 0) {
        fprintf(stderr, "gpio-latency: delegated sifive-gpio has no IRQ\n");
        return 1;
    }
    printf("gpio-latency: delegated sifive-gpio MMIO %#lx irq %d\n",
           (unsigned long) h.base, h.irq_vector);
    return 0;
}
