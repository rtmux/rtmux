#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Populate the FAT boot partition of a Milk-V Duo 256M SD card.
# See docs/milkv-duo.md.
set -eu

usage()
{
    cat >&2 <<EOF
usage: $0 --vendor-fip <fip.bin> [--build-dir <dir>] [--force-uenv] <out-dir>

  --vendor-fip   fip.bin from the official Milk-V Duo 256M image
  --build-dir    RTMux build directory (default: build)
  --force-uenv   overwrite an existing uEnv.txt in <out-dir>
  <out-dir>      mounted FAT partition of the SD card, or any directory
EOF
    exit 2
}

OPENSBI_URL=${OPENSBI_URL:-https://github.com/riscv-software-src/opensbi.git}
OPENSBI_TAG=v1.9
KERNEL_ADDR=0x80200000
INITRD_ADDR=0x88000000
# --limit-ram=160M ends RTMux RAM at 0x8a000000.
INITRD_MAX=$((0x8a000000 - INITRD_ADDR))
UENV_ADDR=0x81000000

vendor_fip=
build_dir=build
force_uenv=0
out_dir=
while [ $# -gt 0 ]; do
    case $1 in
    --vendor-fip) [ $# -ge 2 ] || usage; vendor_fip=$2; shift 2 ;;
    --build-dir) [ $# -ge 2 ] || usage; build_dir=$2; shift 2 ;;
    --force-uenv) force_uenv=1; shift ;;
    -h | --help) usage ;;
    -*) usage ;;
    *) [ -z "$out_dir" ] || usage; out_dir=$1; shift ;;
    esac
done
[ -n "$vendor_fip" ] && [ -n "$out_dir" ] || usage

die() { echo "mksd: $*" >&2; exit 1; }

[ -f "$vendor_fip" ] || die "$vendor_fip: not a file"
[ -d "$out_dir" ] || die "$out_dir: not a directory (mount the SD card's FAT partition first)"
for tool in python3 dtc git make clang ld.lld; do
    command -v "$tool" >/dev/null || die "$tool not found"
done

script_dir=$(cd "$(dirname "$0")" && pwd)
fip_py="python3 $script_dir/fip.py"
kernel=$build_dir/arch/riscv/rtmux-kernel.bin
initrd=$build_dir/initramfs.cpio
[ -f "$kernel" ] && [ -f "$initrd" ] || die "$kernel or $initrd missing; run make first"
initrd_size=$(wc -c < "$initrd")
[ "$initrd_size" -le "$INITRD_MAX" ] ||
    die "$initrd is $initrd_size bytes, more than the $INITRD_MAX that fit below 0x8a000000"

work=$build_dir/milkv-duo
mkdir -p "$work"
work=$(cd "$work" && pwd)

# 1. The vendor OpenSBI embeds the board DTB; reuse it for upstream OpenSBI.
#    Upstream drivers bind to the first matching compatible, so put the T-Head
#    strings first: the SG2002 PMU quirk on the root node, a CLINT without
#    64-bit MMIO, and a PLIC that delegates access to S-mode. The original
#    strings stay behind them for RTMux and the vendor U-Boot.
$fip_py parse "$vendor_fip" --extract "$work/vendor" >/dev/null ||
    die "$vendor_fip: checksum mismatch"
python3 - "$work/vendor/opensbi.bin" "$work/vendor.dtb" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
i = b.find(bytes.fromhex('d00dfeed'))
if i < 0:
    sys.exit('no DTB embedded in the vendor OpenSBI')
open(sys.argv[2], 'wb').write(b[i:i + struct.unpack('>I', b[i + 4:i + 8])[0]])
EOF
dtc -q -I dtb -O dts "$work/vendor.dtb" -o "$work/vendor.dts"
sed -e 's/^\tcompatible = "cvitek,cv181x";/\tcompatible = "sophgo,sg2002", "cvitek,cv181x";/' \
    -e 's/^\t\t\tcompatible = "riscv,clint0";/\t\t\tcompatible = "thead,c900-clint", "riscv,clint0";/' \
    -e 's/^\t\t\tcompatible = "riscv,plic0";/\t\t\tcompatible = "thead,c900-plic", "riscv,plic0";/' \
    "$work/vendor.dts" > "$work/opensbi.dts"
for s in '"sophgo,sg2002", "cvitek,cv181x"' '"thead,c900-clint", "riscv,clint0"' \
    '"thead,c900-plic", "riscv,plic0"'; do
    grep -q "$s" "$work/opensbi.dts" || die "vendor DTB layout changed: $s not applied"
done
dtc -q -I dts -O dtb "$work/opensbi.dts" -o "$work/opensbi.dtb"

# 2. Upstream OpenSBI for SBI v2.0 DBCN (the vendor build is v0.9). It needs a
#    PIE-capable linker, which the bare-metal GNU toolchain lacks; use LLVM.
if [ ! -d "$work/opensbi" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 -b "$OPENSBI_TAG" "$OPENSBI_URL" "$work/opensbi"
fi
[ "$(git -C "$work/opensbi" describe --tags)" = "$OPENSBI_TAG" ] ||
    die "$work/opensbi is not $OPENSBI_TAG"
make -s -C "$work/opensbi" O="$work/opensbi-build" PLATFORM=generic LLVM=1 \
    FW_FDT_PATH="$work/opensbi.dtb" >/dev/null
fw=$work/opensbi-build/platform/generic/firmware/fw_dynamic.bin

# 3. The vendor U-Boot has no saveenv. Point its default sdboot at uEnv.txt.
sdboot="echo Boot RTMux from uEnv.txt ...;fatload mmc 0 $UENV_ADDR uEnv.txt && env import -t -r $UENV_ADDR \${filesize} && run uenvcmd"
$fip_py replace-opensbi "$vendor_fip" "$fw" "$work/fip-opensbi.bin" >/dev/null
$fip_py patch-env "$work/fip-opensbi.bin" sdboot "$sdboot" "$work/fip.bin" >/dev/null
$fip_py parse "$work/fip.bin" >/dev/null || die "generated fip.bin fails its checksums"

# 4. Copy everything to the card.
cp "$work/fip.bin" "$out_dir/fip.bin"
cp "$kernel" "$out_dir/rtmux-kernel.bin"
cp "$initrd" "$out_dir/initramfs.cpio"
if [ -e "$out_dir/uEnv.txt" ] && [ "$force_uenv" = 0 ]; then
    echo "mksd: keeping existing $out_dir/uEnv.txt (--force-uenv to replace)"
else
    cat > "$out_dir/uEnv.txt" <<EOF
rtmux_args=-Dsbi --limit-ram=160M
uenvcmd=fatload mmc 0 $KERNEL_ADDR rtmux-kernel.bin; fatload mmc 0 $INITRD_ADDR initramfs.cpio; setenv initrd_high 0xffffffffffffffff; setenv bootargs \${rtmux_args}; booti $KERNEL_ADDR $INITRD_ADDR:\${filesize} \${fdtcontroladdr}
EOF
fi
[ ! -e "$out_dir/boot.sd" ] ||
    echo "mksd: note: $out_dir/boot.sd is no longer read and can be removed"
sync

cd "$out_dir"
sha256sum fip.bin rtmux-kernel.bin initramfs.cpio uEnv.txt
echo "mksd: $out_dir is ready; U-Boot loads uEnv.txt and boots RTMux on power-up"
