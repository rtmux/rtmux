#!/usr/bin/env bash
set -eu

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/arch/riscv" "$tmp/images"
: >"$tmp/arch/riscv/rtmux-kernel"
truncate -s 1M "$tmp/images/nvme.img"

fake_qemu="$tmp/fake-qemu"
cat >"$fake_qemu" <<'EOF'
#!/bin/sh
env | sort
printf 'ARGV:'
printf ' %s' "$@"
printf '\n'
EOF
chmod +x "$fake_qemu"

QEMU=/bin/echo BUILD_DIR="$tmp" "$root/scripts/emu.sh" >"$tmp/argv"

set -- $(cat "$tmp/argv")
[ "$1" = "-no-user-config" ]
[ "$2" = "-nodefaults" ]

grep -q -- '-no-user-config' "$tmp/argv"
grep -q -- '-nodefaults' "$tmp/argv"
grep -q -- '-nographic' "$tmp/argv"
grep -q -- '-display none' "$tmp/argv"
grep -q -- '-vga none' "$tmp/argv"
grep -q -- '-chardev stdio,id=rtmux-stdio,signal=off,mux=on' "$tmp/argv"
grep -q -- '-serial chardev:rtmux-stdio' "$tmp/argv"
grep -q -- '-mon chardev=rtmux-stdio,mode=readline' "$tmp/argv"
grep -q -- 'virtio-net-device,netdev=rtmuxnet' "$tmp/argv"
grep -q -- '-netdev user,id=rtmuxnet' "$tmp/argv"

! grep -Eq -- '-display (gtk|sdl|cocoa|default)' "$tmp/argv"

DISPLAY=:99 WAYLAND_DISPLAY=wayland-0 QEMU_DISPLAY=cocoa \
    GDK_BACKEND=quartz QT_QPA_PLATFORM=cocoa \
    SDL_VIDEODRIVER=cocoa SDL_AUDIODRIVER=coreaudio \
    QEMU_AUDIO_DRV=coreaudio QEMU_AUDIO_BACKEND=coreaudio \
    QEMU="$fake_qemu" BUILD_DIR="$tmp" "$root/scripts/emu.sh" >"$tmp/env-argv"
grep -q '^SDL_VIDEODRIVER=dummy$' "$tmp/env-argv"
grep -q '^SDL_AUDIODRIVER=dummy$' "$tmp/env-argv"
grep -q '^QEMU_AUDIO_DRV=none$' "$tmp/env-argv"
grep -q '^QEMU_AUDIO_BACKEND=none$' "$tmp/env-argv"
! grep -q '^DISPLAY=' "$tmp/env-argv"
! grep -q '^WAYLAND_DISPLAY=' "$tmp/env-argv"
! grep -q '^QEMU_DISPLAY=' "$tmp/env-argv"
! grep -q '^GDK_BACKEND=' "$tmp/env-argv"
! grep -q '^QT_QPA_PLATFORM=' "$tmp/env-argv"

EMU_QEMU_OPTS='-device virtio-iommu-pci -device VGA -device=virtio-gpu-pci -display gtk -display=gtk -display cocoa -display=cocoa -display egl-headless -display=dbus --display sdl --display=sdl --display cocoa --display=cocoa -vga virtio -vga=std -monitor vc -monitor=vc -serial vc -serial=vc -parallel vc -parallel=vc -spice port=5900 -spice=port=5900 -vnc :1 -vnc=:1 -curses --curses -full-screen --full-screen' \
    EMU_QEMU_MACHINE_OPTS=',iommu-sys=on' \
    QEMU=/bin/echo BUILD_DIR="$tmp" "$root/scripts/emu.sh" >"$tmp/argv-extra"

grep -q -- '-machine virt,graphics=off,iommu-sys=on' "$tmp/argv-extra"
grep -q -- '-device virtio-iommu-pci' "$tmp/argv-extra"
! grep -q -- '-device VGA' "$tmp/argv-extra"
! grep -q -- '-device=virtio-gpu-pci' "$tmp/argv-extra"
! grep -q -- '-display gtk' "$tmp/argv-extra"
! grep -q -- '-display=gtk' "$tmp/argv-extra"
! grep -q -- '-display cocoa' "$tmp/argv-extra"
! grep -q -- '-display=cocoa' "$tmp/argv-extra"
! grep -q -- '-display egl-headless' "$tmp/argv-extra"
! grep -q -- '-display=dbus' "$tmp/argv-extra"
! grep -q -- '--display sdl' "$tmp/argv-extra"
! grep -q -- '--display=sdl' "$tmp/argv-extra"
! grep -q -- '--display cocoa' "$tmp/argv-extra"
! grep -q -- '--display=cocoa' "$tmp/argv-extra"
! grep -q -- '-vga virtio' "$tmp/argv-extra"
! grep -q -- '-vga=std' "$tmp/argv-extra"
! grep -q -- '-monitor vc' "$tmp/argv-extra"
! grep -q -- '-monitor=vc' "$tmp/argv-extra"
! grep -q -- '-serial vc' "$tmp/argv-extra"
! grep -q -- '-serial=vc' "$tmp/argv-extra"
! grep -q -- '-parallel vc' "$tmp/argv-extra"
! grep -q -- '-parallel=vc' "$tmp/argv-extra"
! grep -q -- '-spice port=5900' "$tmp/argv-extra"
! grep -q -- '-spice=port=5900' "$tmp/argv-extra"
! grep -q -- '-vnc :1' "$tmp/argv-extra"
! grep -q -- '-vnc=:1' "$tmp/argv-extra"
! grep -q -- '-curses' "$tmp/argv-extra"
! grep -q -- '--curses' "$tmp/argv-extra"
! grep -q -- '-full-screen' "$tmp/argv-extra"
! grep -q -- '--full-screen' "$tmp/argv-extra"
grep -q -- '-display none' "$tmp/argv-extra"
grep -q -- '-vga none' "$tmp/argv-extra"

cat >"$tmp/helper-test" <<EOF
#!/usr/bin/env bash
set -eu
. "$root/scripts/qemu-headless.sh"
opts=(-machine virt -display gtk -serial vc -device VGA -device nvme -monitor vc)
qemu_sanitize_headless_opts "\${opts[@]}"
printf '%s\n' "\${QEMU_HEADLESS_SANITIZED_OPTS[@]}"
EOF
chmod +x "$tmp/helper-test"
"$tmp/helper-test" >"$tmp/helper-argv"
! grep -qx -- '-display' "$tmp/helper-argv"
! grep -qx -- 'gtk' "$tmp/helper-argv"
! grep -qx -- '-serial' "$tmp/helper-argv"
! grep -qx -- 'vc' "$tmp/helper-argv"
! grep -qx -- 'VGA' "$tmp/helper-argv"
grep -qx -- 'nvme' "$tmp/helper-argv"
tail -n 11 "$tmp/helper-argv" | tr '\n' ' ' |
    grep -qx -- '-nographic -display none -vga none -chardev stdio,id=rtmux-stdio,signal=off,mux=on -serial chardev:rtmux-stdio -mon chardev=rtmux-stdio,mode=readline '

sed -n '/qemu_has_device()/,/^}/p' "$root/scripts/qemu-regress.sh" \
    >"$tmp/qemu-has-device"
grep -q -- '-machine none' "$tmp/qemu-has-device"
grep -q -- 'QEMU_HEADLESS_PROBE_OPTS' "$tmp/qemu-has-device"

grep -q -- 'qemu-headless.sh' "$root/scripts/emu.sh"
grep -q -- 'qemu-headless.sh' "$root/scripts/qemu-regress.sh"

sed -n '/append_qemu_machine_opt()/,/^}/p' "$root/scripts/qemu-regress.sh" \
    >"$tmp/append-machine-opt"
. "$tmp/append-machine-opt"
unset EMU_QEMU_MACHINE_OPTS
append_qemu_machine_opt iommu-sys=on
[ "$EMU_QEMU_MACHINE_OPTS" = ",iommu-sys=on" ]
EMU_QEMU_MACHINE_OPTS=",aclint=on"
append_qemu_machine_opt iommu-sys=on
[ "$EMU_QEMU_MACHINE_OPTS" = ",aclint=on,iommu-sys=on" ]
append_qemu_machine_opt iommu-sys=on
[ "$EMU_QEMU_MACHINE_OPTS" = ",aclint=on,iommu-sys=on" ]

sed -n '/system_iommu_mmio_binding_in_dts()/,/^}/p' \
    "$root/scripts/qemu-regress.sh" >"$tmp/iommu-dts-helper"
. "$tmp/iommu-dts-helper"
sed -n '/patch_system_iommu_metadata_dts()/,/^}/p' \
    "$root/scripts/qemu-regress.sh" >"$tmp/iommu-patch-helper"
. "$tmp/iommu-patch-helper"
sed -n '/keep_system_iommu_dtb_artifacts()/,/^}/p' \
    "$root/scripts/qemu-regress.sh" >"$tmp/iommu-keep-helper"
grep -q -- 'build/lkl-logs/${MODE}-iommu' "$tmp/iommu-keep-helper"
grep -q -- 'kept decoded QEMU system-IOMMU DTS' "$tmp/iommu-keep-helper"
. "$tmp/iommu-keep-helper"
(
    cd "$tmp"
    MODE=lkl-iommu-domain
    KEEP_LOGS=1
    printf dtb >iommu.dtb
    printf dts >iommu.dts
    : >iommu.err
    keep_system_iommu_dtb_artifacts iommu.dtb iommu.dts iommu.err >/dev/null
    grep -q dtb build/lkl-logs/lkl-iommu-domain-iommu.dtb
    grep -q dts build/lkl-logs/lkl-iommu-domain-iommu.dts
    [ ! -e build/lkl-logs/lkl-iommu-domain-iommu.err ]
    rm -f iommu.dts
    printf err >iommu.err
    keep_system_iommu_dtb_artifacts iommu.dtb iommu.dts iommu.err >/dev/null
    grep -q dtb build/lkl-logs/lkl-iommu-domain-iommu.dtb
    grep -q err build/lkl-logs/lkl-iommu-domain-iommu.err
    [ ! -e build/lkl-logs/lkl-iommu-domain-iommu.dts ]
)
cat >"$tmp/mmio-iommu.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu.dts"

cat >"$tmp/mmio-iommu-reordered.dts" <<'EOF'
virtio_mmio@10008000 {
    iommus = <0x01>;
};

iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu-reordered.dts"

cat >"$tmp/mmio-iommu-multiple.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};

iommu@10200000 {
    compatible = "riscv,iommu";
    phandle = <0x02>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu-multiple.dts"

cat >"$tmp/mmio-iommu-compatible-list.dts" <<'EOF'
iommu@10100000 {
    compatible = "vendor,shim", "riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu-compatible-list.dts"

cat >"$tmp/mmio-iommu-compatible-split.dts" <<'EOF'
iommu@10100000 {
    compatible = "vendor,shim",
                 "riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu-compatible-split.dts"

cat >"$tmp/mmio-iommu-linux-phandle.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    linux,phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu-linux-phandle.dts"

cat >"$tmp/mmio-iommu-split-iommus.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <
        0x01 0x00>;
};
EOF
system_iommu_mmio_binding_in_dts "$tmp/mmio-iommu-split-iommus.dts"

cat >"$tmp/pci-only-iommu.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};

pci@30000000 {
    iommu-map = <0x00 0x01 0x00 0x01>;
};

virtio_mmio@10008000 {
    interrupts = <0x08>;
};
EOF
if system_iommu_mmio_binding_in_dts "$tmp/pci-only-iommu.dts"; then
    echo "PCI-only iommu-map must not satisfy MMIO virtio IOMMU binding" >&2
    exit 1
fi

cat >"$tmp/non-riscv-iommu.dts" <<'EOF'
iommu@10100000 {
    compatible = "vendor,other-iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
if system_iommu_mmio_binding_in_dts "$tmp/non-riscv-iommu.dts"; then
    echo "non-RISC-V IOMMU must not satisfy MMIO virtio IOMMU binding" >&2
    exit 1
fi

cat >"$tmp/suffix-iommu.dts" <<'EOF'
iommu@10100000 {
    compatible = "vendor,not-riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x01>;
};
EOF
if system_iommu_mmio_binding_in_dts "$tmp/suffix-iommu.dts"; then
    echo "suffix-compatible IOMMU must not satisfy RISC-V IOMMU binding" >&2
    exit 1
fi

cat >"$tmp/split-iommus-argument.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x02
        0x01>;
};
EOF
if system_iommu_mmio_binding_in_dts "$tmp/split-iommus-argument.dts"; then
    echo "split iommus argument must not satisfy MMIO virtio IOMMU binding" >&2
    exit 1
fi

patch_system_iommu_metadata_dts "$tmp/pci-only-iommu.dts" \
    "$tmp/patched-metadata.dts" 42
grep -q 'iommus = <0x01 42>;' "$tmp/patched-metadata.dts"
system_iommu_mmio_binding_in_dts "$tmp/patched-metadata.dts"

cat >"$tmp/multi-virtio-iommu.dts" <<'EOF'
iommu@10100000 {
    compatible = "riscv,iommu";
    phandle = <0x01>;
};

virtio_mmio@10008000 {
    iommus = <0x09 0x07>;
};

virtio_mmio@10009000 {
    interrupts = <0x09>;
};
EOF
patch_system_iommu_metadata_dts "$tmp/multi-virtio-iommu.dts" \
    "$tmp/patched-multi-virtio.dts" 42
grep -Fc 'iommus = <0x01 42>;' "$tmp/patched-multi-virtio.dts" |
    grep -qx 2
! grep -q 'iommus = <0x09 0x07>;' "$tmp/patched-multi-virtio.dts"
system_iommu_mmio_binding_in_dts "$tmp/patched-multi-virtio.dts"

sed -n '/QEMU_HEADLESS_PROBE_OPTS=(/,/)/p' "$root/scripts/qemu-headless.sh" \
    >"$tmp/qemu-headless-probe-opts"
grep -q -- '-no-user-config' "$tmp/qemu-headless-probe-opts"
grep -q -- '-nodefaults' "$tmp/qemu-headless-probe-opts"
grep -q -- '-nographic' "$tmp/qemu-headless-probe-opts"
grep -q -- '-display none' "$tmp/qemu-headless-probe-opts"
grep -q -- '-vga none' "$tmp/qemu-headless-probe-opts"
grep -q -- '-monitor none' "$tmp/qemu-headless-probe-opts"
grep -q -- '-serial none' "$tmp/qemu-headless-probe-opts"

sed -n '/QEMU_HEADLESS_ENV=(/,/)/p' "$root/scripts/qemu-headless.sh" \
    >"$tmp/qemu-headless-env"
grep -q -- '-u DISPLAY' "$tmp/qemu-headless-env"
grep -q -- '-u WAYLAND_DISPLAY' "$tmp/qemu-headless-env"
grep -q -- '-u QEMU_DISPLAY' "$tmp/qemu-headless-env"
grep -q -- 'SDL_VIDEODRIVER=dummy' "$tmp/qemu-headless-env"
grep -q -- 'SDL_AUDIODRIVER=dummy' "$tmp/qemu-headless-env"
grep -q -- 'QEMU_AUDIO_DRV=none' "$tmp/qemu-headless-env"
grep -q -- 'QEMU_AUDIO_BACKEND=none' "$tmp/qemu-headless-env"
grep -q -- '-u GDK_BACKEND' "$tmp/qemu-headless-env"
grep -q -- '-u QT_QPA_PLATFORM' "$tmp/qemu-headless-env"

grep -Fc 'cmd=("${QEMU_HEADLESS_ENV[@]}"' "$root/scripts/qemu-regress.sh" |
    grep -qx 3
grep -q -- 'exec "${QEMU_HEADLESS_ENV\[@\]}" "$QEMU"' "$root/scripts/emu.sh"

for mode in lkl-iommu-probe lkl-iommu-sys-probe lkl-iommu-metadata \
        lkl-iommu-domain-preflight lkl-netd-iommu-fail-closed \
        lkl-iommu-domain lkl-iommu-dma-deny lkl-iommu-revoke; do
    QEMU=/bin/false "$root/scripts/qemu-regress.sh" --mode "$mode" \
        >"$tmp/iommu-skip"
    grep -q -- "UNSUPPORTED $mode" "$tmp/iommu-skip"

    if QEMU=/bin/false "$root/scripts/qemu-regress.sh" --mode "$mode" \
            --require-supported >"$tmp/iommu-required"; then
        echo "$mode --require-supported must fail without host support" >&2
        exit 1
    fi
    grep -q -- "UNSUPPORTED $mode" "$tmp/iommu-required"
done

echo "emu-headless-test: PASS"
