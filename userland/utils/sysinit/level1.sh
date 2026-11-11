#!/bin/sh

# /usr/sbin/sysinit/level1.sh — first stage of the on-disk system init.
#
# By the time this runs, /sbin/init in the cpio initramfs has done the minimum
# needed to get here: brought up logging, the serial console, the NVMe driver,
# and mounted /usr. Everything from here on lives on the disk and can be edited
# without rebuilding the cpio.
#
# Future stages (level2.sh, level3.sh) plug in here as the system grows:
# networking, additional I/O services, etc.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

echo
echo "Sysinit: level1 running."

# Pull in any per-machine config (none yet - placeholder). Configuration files
# live under /usr/conf/sysinit/.
if [[ -x /usr/conf/sysinit/level1.local.sh ]]; then
    . /usr/conf/sysinit/level1.local.sh
fi

# Hand off to getty on the interrupt-driven UART. The inherited boot console is
# /dev/text, which is fine for output but has no reliable input path on modern
# OpenSBI/QEMU debug-console backends.
echo
if test -e /dev/ser1; then
    /sbin/getty /dev/ser1
else
    /sbin/getty
fi
