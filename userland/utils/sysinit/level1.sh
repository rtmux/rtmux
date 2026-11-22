#!/bin/sh

# /usr/sbin/sysinit/level1.sh — first stage of the on-disk system init.
#
# By the time this runs, /sbin/init in the cpio initramfs has done the minimum
# needed to get here: brought up logging and NVMe, mounted /usr and started
# the pipe server. Everything from here on lives on the disk and can be edited
# without rebuilding the cpio.
#
# Future stages (level2.sh, level3.sh) plug in here as the system grows:
# networking, additional I/O services, etc.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

echo
echo "Starting on-disk sysinit level 1."

# Pull in any per-machine config (none yet - placeholder). Configuration files
# live under /usr/conf/sysinit/.
if test -x /usr/conf/sysinit/level1.local.sh; then
    . /usr/conf/sysinit/level1.local.sh
fi

# The SBI console and dev-serial-8250 share the QEMU UART. Keep the
# inherited /dev/text console as its only reader through login.
echo
/sbin/getty
