#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$TOP_DIR" || exit 1
. "$SCRIPT_DIR/qemu-headless.sh"

# Build directory honours $BUILD_DIR (set by the top-level Makefile or the
# user); default mirrors mk/build.mk's $(TOPDIR)/build.
BUILD_DIR="${BUILD_DIR:-$TOP_DIR/build}"
IMAGE_DIR="$BUILD_DIR/images"
NVME_IMG="$IMAGE_DIR/nvme.img"
NVME_PRE_IMG="$IMAGE_DIR/nvme-pre.dat"
NVME_P8_IMG="$IMAGE_DIR/nvme-p8.dat"
DEFAULT_LKL_EXT4_IMG="$IMAGE_DIR/lkl-ext4.img"

# The block-filesystem syscall regress mode ships the conformance suite on /usr
# and boots it from there. Steer that variant to its own image files so the
# normal dev image (nvme.img) never carries the test binary.
if [[ -n "${EMU_ROMFS_TESTS:-}" ]]; then
    NVME_IMG="$IMAGE_DIR/nvme-tests.img"
    NVME_PRE_IMG="$IMAGE_DIR/nvme-tests-pre.dat"
    NVME_P8_IMG="$IMAGE_DIR/nvme-tests-p8.dat"
fi

# Number of CPUs to emulate. Default to UP for deterministic bring-up; use
# CPUS=N ./scripts/emu.sh for SMP stress runs.
CPUS="${CPUS:-1}"
# QEMU binary. Resolved before the firmware choice below, because the right
# OpenSBI depends on which QEMU will run it.
QEMU="${QEMU:-qemu-system-riscv64}"
# OpenSBI firmware. Set BIOS=<path> in the environment to override, or BIOS=none
# to run without one. Otherwise auto-detect:
#   - Distro QEMU (under /usr/bin or /bin): the distro OpenSBI package,
#     /usr/lib/riscv64-linux-gnu/opensbi/..., which is built for that QEMU.
#   - Any other QEMU (self-built under ~/.local, /opt, Homebrew, ...): let QEMU
#     pick its own bundled firmware via -bios default. An older distro OpenSBI
#     can hang right after its banner under a newer QEMU.
if [[ -z "$BIOS" ]]; then
    BIOS=default
    distro_opensbi=/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin
    if [[ -f $distro_opensbi ]]; then
        qemu_path="$(readlink -f -- "$(command -v "$QEMU")" 2>/dev/null)"
        case "$qemu_path" in
        /usr/bin/* | /bin/*) BIOS=$distro_opensbi ;;
        esac
    fi
fi
# Initramfs archive (CPIO)
INITRD="$BUILD_DIR/initramfs.cpio"

# Portable file-size helper (BSD stat differs from GNU stat).
fsize() { wc -c < "$1" | tr -d ' '; }

LKL_EXT4_IMG="${LKL_EXT4_IMG:-$DEFAULT_LKL_EXT4_IMG}"

find_mkfs_ext4() {
    if [[ -n "${MKFS_EXT4:-}" ]]; then
        command -v "$MKFS_EXT4"
        return
    fi
    command -v mkfs.ext4 2>/dev/null ||
        printf '%s\n' /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4 \
                      /usr/local/opt/e2fsprogs/sbin/mkfs.ext4 |
            while read -r f; do
                [[ -x "$f" ]] && { printf '%s\n' "$f"; break; }
            done
}

make_lkl_ext4_seed() {
    local root
    local mkfs_ext4

    if [[ -f "$LKL_EXT4_IMG" || "$LKL_EXT4_IMG" != "$DEFAULT_LKL_EXT4_IMG" ]]; then
        return
    fi
    mkfs_ext4=$(find_mkfs_ext4)
    if [[ -z "$mkfs_ext4" ]]; then
        return
    fi
    root=$(mktemp -d)
    mkdir -p "$root"
    echo "hello from LKL ext4" > "$root/hello.txt"
    truncate -s 8M "$LKL_EXT4_IMG"
    if ! "$mkfs_ext4" -q -F -d "$root" "$LKL_EXT4_IMG"; then
        rm -rf "$root" "$LKL_EXT4_IMG"
        return
    fi
    rm -rf "$root"
}

refresh_lkl_ext4_partition() {
    if [[ ! -f "$LKL_EXT4_IMG" ]]; then
        return
    fi
    if [[ $(fsize "$LKL_EXT4_IMG") -gt $((16 * 1024 * 1024)) ]]; then
        echo "LKL ext4 image is larger than nvme p1" >&2
        exit 1
    fi
    dd if="$LKL_EXT4_IMG" of="$NVME_IMG" bs=512 seek=34 conv=notrunc \
        status=none
}

# Default mainfs device — passed to the kernel via cmdline so init can read it
# from /sys/cmdline and mount the partition on /usr. Override with -mainfs=<dev>
# on the script command line.
MAINFS_DEV=/dev/nvme0n1p8

QEMUOPTS=()
BASE_QEMU_OPTS=(
    -no-user-config
    -nodefaults
)
add_qemu_opts() { QEMUOPTS+=("$@"); }

enforce_headless_env()
{
    qemu_enforce_headless_env
}

enforce_headless_qemu_opts()
{
    qemu_sanitize_headless_opts "${QEMUOPTS[@]}"
    QEMUOPTS=("${QEMU_HEADLESS_SANITIZED_OPTS[@]}")
}

# Disable QEMU config/default devices before any machine/device options are
# added; the final headless pass below still wins over EMU_QEMU_OPTS.
add_qemu_opts "${BASE_QEMU_OPTS[@]}"

# EMU_KERNEL=<path> boots a kernel from another tree (version-skew testing)
# against this BUILD_DIR's initramfs.
# Default backend: NVMe (the canonical layout used on the SiFive Unmatched).
# Keep QEMU fully headless for both interactive boots and make-driven tests.
# Serial stays on stdio so qemu-regress can capture the boot log.
add_qemu_opts \
    -machine "virt,graphics=off${EMU_QEMU_MACHINE_OPTS:-}" \
    -bios "$BIOS" \
    -kernel "${EMU_KERNEL:-$BUILD_DIR/arch/riscv/rtmux-kernel}" \
    -m "${EMU_MEM_MB:-256}M" \
    -smp "$CPUS"

# Parse options. Args we recognise are consumed; the rest pass through to the
# kernel cmdline via EXTRA_CMDLINE. EMU_EXTRA_CMDLINE overrides the default (e.g.
# set it empty to drop "-T"/auto-trace when a userland tracelogger owns the ring).
EXTRA_CMDLINE="${EMU_EXTRA_CMDLINE--T}"
PASSTHROUGH=()
for arg in "$@"; do
    case "$arg" in
        -gdb)          add_qemu_opts -s -S ;;
        # Live stub: listens on PORT but does not freeze the guest at reset, so a
        # harness can attach after a stall (scripts/qemu-regress.sh --gdb-on-timeout).
        -gdb-live=*)   add_qemu_opts -gdb "tcp::${arg#-gdb-live=}" ;;
        -mainfs=*)     MAINFS_DEV="${arg#-mainfs=}" ;;
        *)             PASSTHROUGH+=("$arg") ;;
    esac
done

# NVMe — always attached. Backing store is build/images/nvme.img (192 MiB), auto-created
# on first use. The image is laid out as 8 × 16 MiB GPT partitions; p8 is romfs
# marked with the RTMux partition type GUID, mirroring the canonical layout used
# on the SiFive Unmatched (NVMe p8 = RTMux). Same initramfs.cpio boots
# bit-identical on QEMU and bare metal — only this image differs.
#
# Two-file split kept around for hardware deployment:
#   build/images/nvme-pre.dat  — primary GPT + p1..p7 (mostly zeros), 112 MiB
#   build/images/nvme-p8.dat   — romfs partition contents, 16 MiB
# Concatenating them and zero-padding to 192 MiB reproduces nvme.img.
# nvme-p8.dat is exactly the bytes you'd `dd` to a real NVMe partition to mirror
# this development image onto silicon.
#
# Init mounts $MAINFS_DEV (default /dev/nvme0n1p8) at /usr after reading the
# device from /sys/cmdline.
mkdir -p "$IMAGE_DIR"
make_lkl_ext4_seed

if [[ ! -s "$NVME_IMG" ]]; then
    echo "Creating $NVME_IMG (192 MiB, GPT, 8 × 16 MiB, p8 = RTMux)..."

    # Step 1: lay the GPT for an 8-partition / 16-MiB-each / 192-MiB disk on a
    # temporary file so mkgpt.py sees the full disk size.
    TMP_GPT=$(mktemp -u /tmp/rtmux-gpt.XXXXXX.img)
    truncate -s 192M "$TMP_GPT"
    ./scripts/mkgpt.py --rtmux 8 "$TMP_GPT" 16 16 16 16 16 16 16 16 \
        >/dev/null

    # Step 2: extract bytes [0 .. p8_start) into nvme-pre.dat. p8 starts at LBA
    # 34 + 7 × 32768 = 229410 (= offset 112 MiB).
    dd if="$TMP_GPT" of="$NVME_PRE_IMG" bs=512 count=229410 \
        status=none
    rm -f "$TMP_GPT"

    # Step 3: build mkromfs on demand, then build the /usr romfs
    # (scripts/mkusr.sh) directly into build/images/nvme-p8.dat.
    case "$BUILD_DIR" in
        /*) mkromfs_target="$BUILD_DIR/tools/host/mkromfs" ;;
        *) mkromfs_target="$TOP_DIR/$BUILD_DIR/tools/host/mkromfs" ;;
    esac
    make --no-print-directory OUT="$BUILD_DIR" "$mkromfs_target" >/dev/null
    MKUSR_ARGS=(-s 16)

    # Ship the syscall conformance suite on /usr for the block-filesystem regress
    # mode (EMU_ROMFS_TESTS), which boots it from the disk to prove the suite does
    # not depend on initramfs.cpio for its binary.
    if [[ -n "${EMU_ROMFS_TESTS:-}" ]]; then
        if [[ ! -f "$BUILD_DIR/tests/userland/syscall_testing" ]]; then
            echo "emu.sh: $BUILD_DIR/tests/userland/syscall_testing missing;" \
                 "run 'make initramfs' first" >&2
            exit 1
        fi
        MKUSR_ARGS+=(-a "$BUILD_DIR/tests/userland/syscall_testing:bin/syscall_testing")
    fi
    if [[ -f "$LKL_EXT4_IMG" ]]; then
        MKUSR_ARGS+=(-a "$LKL_EXT4_IMG:share/lkl-ext4.img")
    fi
    # The zone files come from mkusr.sh too. An existing image keeps what it
    # has; delete it to pick new ones up.
    ./scripts/mkusr.sh "${MKUSR_ARGS[@]}" "$mkromfs_target" "$NVME_P8_IMG"

    # Step 4: assemble nvme.img = nvme-pre.dat + nvme-p8.dat, padded to the full
    # 192 MiB disk size so QEMU sees a sane GPT.
    cat "$NVME_PRE_IMG" "$NVME_P8_IMG" > "$NVME_IMG"
    truncate -s 192M "$NVME_IMG"

    echo "  $NVME_PRE_IMG  ($(fsize "$NVME_PRE_IMG") bytes)  — GPT + p1..p7"
    echo "  $NVME_P8_IMG   ($(fsize "$NVME_P8_IMG") bytes)   — romfs (= p8)"
    echo "  $NVME_IMG      ($(fsize "$NVME_IMG") bytes)  — concatenation, padded"
fi
refresh_lkl_ext4_partition
add_qemu_opts \
    -drive "file=$NVME_IMG,if=none,format=raw,id=nvm0" \
    -device nvme,drive=nvm0,serial=rtmux-test

# Virtio-net uses QEMU user networking, so LKL net domains get a real routed
# packet path without requiring host TAP setup.
add_qemu_opts \
    -netdev user,id=rtmuxnet \
    -device virtio-net-device,netdev=rtmuxnet,mac=52:54:00:12:34:56

if [[ -n "${LKL_PCAP:-}" ]]; then
    add_qemu_opts -object "filter-dump,id=rtmuxpcap,netdev=rtmuxnet,file=$LKL_PCAP"
fi

if [[ -n "${EMU_QEMU_OPTS:-}" ]]; then
    read -r -a EMU_QEMU_EXTRA <<< "$EMU_QEMU_OPTS"
    add_qemu_opts "${EMU_QEMU_EXTRA[@]}"
fi

# PCI serial device
add_qemu_opts \
    -device pci-serial,chardev=pci-ser \
    -chardev file,id=pci-ser,path=/tmp/pci-serial.log

# Any unrecognised script args we pass through to the kernel cmdline.
if [[ ${#PASSTHROUGH[@]} -gt 0 ]]; then
    EXTRA_CMDLINE+=" ${PASSTHROUGH[*]}"
fi

# For SMP controls, also look for: smp_dispatch_enabled

# If the initramfs file exists, use it
[[ -e $INITRD ]] && add_qemu_opts -initrd "$INITRD"

# A guest test whose bound only an instruction-count clock can resolve reads
# rtmux.icount= from /sys/cmdline (see EMU_ICOUNT below).
if [[ -n "${EMU_ICOUNT:-}" ]]; then
    EXTRA_CMDLINE+=" rtmux.icount=${EMU_ICOUNT}"
fi

# Append the command line for the system. Default: INFO level. Use -v for DEBUG,
# -vv for TRACE. mainfs= is read by /sbin/init from /sys/cmdline.
CMDLINE="-Dsbi $EXTRA_CMDLINE mainfs=$MAINFS_DEV"

# EMU_ICOUNT=<shift> runs every hart on one host thread with the virtual clock
# counting guest instructions (one per 2^shift ns) and jumping over idle time,
# so guest-visible timing does not depend on how the host schedules QEMU. The
# RTC follows the same clock. Harts then interleave at translation-block
# boundaries only, so this mode is for timing checks, not for finding races.
if [[ -n "${EMU_ICOUNT:-}" ]]; then
    add_qemu_opts -accel tcg,thread=single \
        -icount "shift=${EMU_ICOUNT},sleep=off" -rtc clock=vm
else
    # One host thread per hart, for SMP debugging.
    add_qemu_opts -accel tcg,thread=multi
fi

enforce_headless_qemu_opts
exec "${QEMU_HEADLESS_ENV[@]}" "$QEMU" "${QEMUOPTS[@]}" -append "$CMDLINE"
