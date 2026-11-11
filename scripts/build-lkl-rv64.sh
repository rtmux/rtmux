#!/bin/sh
set -eu

artifact=${LKL_ARTIFACT:-externals/lkl-rv64/lkl-rv64.tar.gz}
rtmux_artifact=${LKL_RTMUX_ARTIFACT:-externals/lkl-rv64/lkl-rv64-rtmux.tar.gz}
out=${LKL_OUT:-prebuilt/lkl-rv64/out}
rtmux_out=${LKL_RTMUX_OUT:-prebuilt/lkl-rv64/out-rtmux}
mode=${1:-}
tmp=
lock=
have_lock=0

# Archive acceptance gates, shared by --build-from-source and the extract/
# validate path so the two cannot drift.  Required symbols include lkl_host_ops,
# which the rtmux archive leaves UNDEFINED (posix-host.o is excluded and rtmux
# supplies lkl_host_ops at final link) -- it passes only because `nm -g` also
# lists undefined symbols; do not switch these checks to `nm --defined-only`.
LKL_REQUIRED_SYMS='lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror lkl_disk_add lkl_mount_dev lkl_host_ops lkl_dev_blk_ops'
LKL_FORBIDDEN='^(pthread_|eventfd$|mmap64$|pread64$|pwrite64$|socket$|setsockopt$|poll$|select$)'

cleanup() {
    if test -n "${tmp:-}"; then
        rm -rf "$tmp"
    fi
    if test "$have_lock" = 1; then
        rm -rf "$lock" 2>/dev/null || true
    fi
}
# EXIT alone misses signal deaths (Ctrl-C on `make qemu`, an OOM kill under
# `make -j`); catch the common ones so an interrupted holder releases its lock.
trap cleanup EXIT INT TERM HUP

# Resolve an object-inspecting tool; llvm-nm/llvm-ar read RISC-V archives on any
# host. ABI gates require a real tool; a text-only BUILD_INFO fallback would let
# a forged or wrong-ABI archive pass on a box with no cross binutils.
resolve_tool() {
    for t in "$@"; do
        test -n "$t" || continue
        if command -v "$t" >/dev/null 2>&1; then
            echo "$t"
            return 0
        fi
    done
    return 1
}

expected_rtmux_source() {
    case "${rtmux_out##*/}" in
    out-fsd)
        echo "lkl.o+fs.o+iomem.o+jmp_buf.o+utils.o+virtio_blk.o+virtio.o"
        ;;
    out-netd)
        echo "lkl.o+fs.o+iomem.o+net.o+jmp_buf.o+utils.o+virtio_blk.o+virtio.o+virtio_net.o"
        ;;
    *)
        return 1
        ;;
    esac
}

expected_rtmux_personality() {
    case "${rtmux_out##*/}" in
    out-fsd)
        echo fsd
        ;;
    out-netd)
        echo netd
        ;;
    *)
        return 1
        ;;
    esac
}

expected_rtmux_required_symbols() {
    case "${rtmux_out##*/}" in
    out-fsd)
        echo "lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror \
              lkl_disk_add lkl_mount_dev lkl_host_ops lkl_dev_blk_ops"
        ;;
    out-netd)
        echo "lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror \
              lkl_host_ops lkl_netdev_add"
        ;;
    *)
        echo "$LKL_REQUIRED_SYMS"
        ;;
    esac
}

validate_rtmux_personality() {
    expected_source=$(expected_rtmux_source) || return 0
    expected_personality=$(expected_rtmux_personality) || return 0
    info=$rtmux_out/BUILD_INFO
    test -f "$info" || {
        echo "LKL rv64: missing $info" >&2
        exit 1
    }
    actual_source=$(awk -F= '$1 == "source" { print $2 }' "$info")
    actual_personality=$(awk -F= '$1 == "personality" { print $2 }' "$info")
    test "$actual_source" = "$expected_source" &&
        test "$actual_personality" = "$expected_personality" || {
        echo "LKL rv64: $rtmux_out is not the $expected_personality personality archive" >&2
        echo "LKL rv64: got personality=${actual_personality:-unknown} source=${actual_source:-unknown}" >&2
        echo "LKL rv64: want personality=$expected_personality source=$expected_source" >&2
        echo "LKL rv64: rebuild with LKL_SRC=<lkl-tree> LKL_PERSONALITY=$expected_personality LKL_RTMUX_OUT=$rtmux_out $0 --build-from-source" >&2
        exit 1
    }
}

# Build liblkl-rtmux.a from an LKL (Linux + arch/lkl) source tree instead of a
# prebuilt tarball.  Point LKL_SRC at the tree; the archive lands in
# $rtmux_out (a gitignored prebuilt/ path), same layout as --extract produces.
if test "$mode" = "--build-from-source"; then
    : "${LKL_SRC:?set LKL_SRC to an LKL source tree (Linux + tools/lkl)}"
    cross=${LKL_CROSS:-riscv64-linux-gnu-}
    jobs=$(nproc 2>/dev/null || echo 4)

    test -f "$LKL_SRC/tools/lkl/Makefile" || {
        echo "LKL rv64: $LKL_SRC has no tools/lkl/Makefile" >&2
        exit 1
    }

    # Serialize builds that share this LKL_SRC tree.  Under `make -j` the lkl-fsd
    # and lkl-netd archive rules invoke this script concurrently, and each
    # rewrites $LKL_SRC/.config -- a data race that corrupts both configs.  A
    # mkdir spin-lock on the tree makes the config+build atomic per personality;
    # cleanup() releases it.  clean-config regeneration means the serialized runs
    # still produce independent, reproducible archives.  A kill -9 (no trap runs)
    # can strand the lock, so record the holder PID and reclaim the dir when that
    # PID is gone; a wait cap is the last-resort backstop for a live-but-hung hold.
    lock=$LKL_SRC/.rtmux-lkl-build.lock
    waited=0
    while ! mkdir "$lock" 2>/dev/null; do
        if test -f "$lock/pid" && ! kill -0 "$(cat "$lock/pid" 2>/dev/null)" 2>/dev/null; then
            rm -rf "$lock" 2>/dev/null || true    # holder died without cleanup
            continue
        fi
        waited=$((waited + 1))
        if test "$waited" -gt 1800; then
            echo "LKL rv64: timed out waiting for $lock; remove it if stale" >&2
            exit 1
        fi
        sleep 1
    done
    have_lock=1
    echo $$ > "$lock/pid"
    ar_tool=$(resolve_tool "${LKL_AR:-}" "${cross}ar" \
        riscv64-unknown-linux-gnu-ar llvm-ar) || {
        echo "LKL rv64: need an ar (${cross}ar / llvm-ar)" >&2
        exit 1
    }
    nm_tool=$(resolve_tool "${LKL_NM:-}" "${cross}nm" \
        riscv64-unknown-linux-gnu-nm llvm-nm) || {
        echo "LKL rv64: need an nm (${cross}nm / llvm-nm)" >&2
        exit 1
    }
    strip_tool=$(resolve_tool "${LKL_STRIP:-}" "${cross}strip" \
        riscv64-unknown-linux-gnu-strip llvm-strip) || {
        echo "LKL rv64: need a strip (${cross}strip / llvm-strip)" >&2
        exit 1
    }

    libdir=$LKL_SRC/tools/lkl/lib

    script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
    base_fragment=${LKL_CONFIG_FRAGMENT:-$script_dir/../configs/lkl-rtmux.config}
    rm -f "$LKL_SRC/tools/lkl/Makefile.conf" \
          "$LKL_SRC/tools/lkl/include/lkl_autoconf.h"

    # LKL_PERSONALITY selects the config overlay, the packaged object set, the
    # required-symbol gate, the objects it drops (BUILD_INFO excluded=), and
    # whether it needs combined headers.  All personalities exclude posix-host.o
    # (rtmux supplies its own lkl_host_ops) and the extra virtio-net backends /
    # vfio-pci / config.  Default (empty) is the combined fs+net archive; fsd drops
    # the net stack from both config and objects; netd keeps upstream virtio_net
    # plus the generic fs/virtio glue needed by the link, and only trims ext4
    # from lkl.o.  combined_headers=yes only for a personality
    # that trims a
    # SYSCALL-gating config (net): its own object-pass lkl.h would keep net wrappers
    # with no prototype, so it must reuse the net-on combined headers.  netd's ext4
    # trim gates no syscall, so its object-pass headers are already complete.
    personality=${LKL_PERSONALITY:-}
    combined_headers=
    case "$personality" in
    fsd)
        overlay=$script_dir/../configs/lkl-fsd.config
        objs="lkl.o fs.o iomem.o jmp_buf.o utils.o virtio_blk.o virtio.o"
        dropped="net.o,virtio_net.o"
        req_syms="lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror \
                  lkl_disk_add lkl_mount_dev lkl_host_ops lkl_dev_blk_ops"
        cfg_assert="# CONFIG_NET is not set"
        combined_headers=yes
        ;;
    netd)
        overlay=$script_dir/../configs/lkl-netd.config
        objs="lkl.o fs.o iomem.o net.o jmp_buf.o utils.o virtio_blk.o virtio.o virtio_net.o"
        dropped=
        req_syms="lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror \
                  lkl_host_ops lkl_netdev_add"
        cfg_assert="# CONFIG_EXT4_FS is not set"
        ;;
    "")
        overlay=
        objs="lkl.o fs.o iomem.o net.o jmp_buf.o utils.o virtio_blk.o virtio.o virtio_net.o"
        dropped=
        req_syms="lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror \
                  lkl_disk_add lkl_mount_dev lkl_host_ops lkl_dev_blk_ops \
                  lkl_netdev_add"
        cfg_assert="CONFIG_NET=y"
        ;;
    *)
        echo "LKL rv64: unknown LKL_PERSONALITY='$personality' (want fsd, netd, or empty)" >&2
        exit 1
        ;;
    esac

    for f in "$base_fragment" ${overlay:+"$overlay"}; do
        test -f "$f" || {
            echo "LKL rv64: missing config fragment $f" >&2
            exit 1
        }
    done

    # rtmux's freestanding libc provides no fortify (__*_chk) or stack-protector
    # (__stack_chk_*) runtime, but a distro cross-gcc (e.g. riscv64-linux-gnu-)
    # enables both by default, and those undefined symbols would break the
    # server link.  Force them off for BOTH the kernel objects (KCFLAGS, which
    # reaches lkl.o) and the tools/lkl glue objects fs.o/net.o/virtio_*.o (their
    # own CFLAGS, which KCFLAGS does not reach); tools/lkl appends its own -I/-O
    # to CFLAGS so passing it in the environment is additive, not a clobber.
    # The disables are mandatory for rtmux linkage; LKL_KCFLAGS only appends.
    kcflags="-fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 ${LKL_KCFLAGS-}"

    # Regenerate a PRISTINE base config each call (the LKL arch defconfig: net +
    # ext4 + virtio), append the base fragment (size-optimized, no debug) and an
    # optional overlay (the feature trim), normalize with olddefconfig, then
    # assert the expected feature state.  Regenerating -- rather than appending to
    # the tree's existing .config -- is what lets the object pass (net off for
    # fsd) and the header pass (net on) run on the SAME tree without overlays
    # accumulating across runs.  A config-step mismatch aborts.
    apply_config() { # $1 overlay-or-empty  $2 cfg_assert
        rm -f "$LKL_SRC/.config" "$LKL_SRC/tools/lkl/kernel.config" \
              "$LKL_SRC/tools/lkl/Makefile.conf" \
              "$LKL_SRC/tools/lkl/include/lkl_autoconf.h"
        make -C "$LKL_SRC/tools/lkl" conf CROSS_COMPILE="$cross" >/dev/null
        cat "$base_fragment" >> "$LKL_SRC/tools/lkl/kernel.config"
        test -z "$1" || cat "$1" >> "$LKL_SRC/tools/lkl/kernel.config"
        make -C "$LKL_SRC" ARCH=lkl CROSS_COMPILE="$cross" defconfig >/dev/null
        cat "$LKL_SRC/tools/lkl/kernel.config" >> "$LKL_SRC/.config"
        make -C "$LKL_SRC" ARCH=lkl CROSS_COMPILE="$cross" olddefconfig >/dev/null
        make -C "$LKL_SRC" ARCH=lkl CROSS_COMPILE="$cross" syncconfig >/dev/null
        grep -qxF "$2" "$LKL_SRC/.config" || {
            echo "LKL rv64: .config does not match (want '$2') after defconfig+overlay" >&2
            exit 1
        }
    }
    build_lkl() {
        CFLAGS="$kcflags" make -C "$LKL_SRC/tools/lkl" -j"$jobs" \
            CROSS_COMPILE="$cross" KCFLAGS="$kcflags" || true
    }

    # Object pass: build with the personality config so lkl.o's text reflects the
    # feature trim (fsd's net-off lkl.o is ~1.4 MB smaller).  Delete the packaged
    # objects first so their presence after the build proves THIS pass produced
    # them (the default target also links optional host tools, e.g. lklfuse, whose
    # tail failure we tolerate; a stale object could otherwise slip through).
    apply_config "$overlay" "$cfg_assert"
    for o in $objs; do
        rm -f "$libdir/$o"
    done
    build_lkl
    for o in $objs; do
        test -f "$libdir/$o" || {
            echo "LKL rv64: build did not produce $libdir/$o" >&2
            exit 1
        }
    done

    # LKL_OUT_NAME (from mk/lkl-server.mk) feeds rtmux_out, which `rm -rf` wipes;
    # reject empty/root/relative-escape/absolute so an override cannot delete
    # outside the prebuilt/ tree.  The legitimate values are bare subdir names
    # (out-rtmux, out-fsd, out-netd) under prebuilt/lkl-rv64.
    case "$rtmux_out" in
    "" | "/" | "." | /* | *..* | */)
        echo "LKL rv64: refusing unsafe LKL_RTMUX_OUT='$rtmux_out'" >&2
        exit 1
        ;;
    esac
    rm -rf "$rtmux_out"
    mkdir -p "$rtmux_out/include"
    rtmux_lib_abs=$(cd "$rtmux_out" && pwd -P)/liblkl-rtmux.a
    (cd "$libdir" && "$ar_tool" rcs "$rtmux_lib_abs" $objs)
    "$strip_tool" --strip-debug "$rtmux_lib_abs"

    # Capture the OBJECT-pass config hash now, before the header pass reconfigures
    # the tree: BUILD_INFO must describe the config that produced liblkl-rtmux.a,
    # not the combined header config used below.
    if command -v sha256sum >/dev/null 2>&1; then
        cfg_sha=$(sha256sum "$LKL_SRC/.config" 2>/dev/null | cut -d' ' -f1)
    else
        cfg_sha=$(shasum -a 256 "$LKL_SRC/.config" 2>/dev/null | cut -d' ' -f1)
    fi

    # Header pass: a personality that trims syscalls (fsd drops CONFIG_NET) leaves
    # tools/lkl/include/lkl.h with wrappers -- e.g. lkl_sys_send -> lkl_sys_sendto
    # -- whose prototypes the trimmed syscall table no longer emits, while the
    # __lkl__NR_* guard stays defined (syscall numbers are config independent), so
    # that header will not compile under -Werror.  The archive is already
    # assembled from the net-off objects, so regenerate the headers from the
    # COMBINED (net-on) config: a strict superset of decls the net-off archive
    # never defines but the fs server never calls.  Only a syscall-trimming
    # personality needs this (combined_headers=yes, i.e. fsd); netd's ext4 trim and
    # the combined default already have complete object-pass headers, so they skip
    # the extra build.  apply_config asserts the tree is net-on, so build_lkl
    # regenerates lkl.h in place as the combined header (it is a generated file the
    # default target rewrites, not one it creates from nothing -- do not delete it
    # first).  No content check here: a stale/wrong header is not silent, it fails
    # the server's own -Werror compile against these headers, which is the real gate.
    header_src=$LKL_SRC/tools/lkl/include/lkl.h
    if test "$combined_headers" = yes; then
        apply_config "" "CONFIG_NET=y"
        build_lkl
    fi
    test -f "$header_src" || {
        echo "LKL rv64: missing generated $header_src" >&2
        exit 1
    }
    cp -R "$LKL_SRC/tools/lkl/include/." "$rtmux_out/include/"

    # ABI + containment gates.  The required-symbol set is personality-specific
    # (fsd has no lkl_netdev_add, netd would have no lkl_disk_add); the forbidden
    # set is shared with the extract/validate path below.
    "$nm_tool" -g "$rtmux_lib_abs" | awk '{print $NF}' > "$rtmux_out/.syms"
    for s in $req_syms; do
        grep -qx "$s" "$rtmux_out/.syms" || {
            echo "LKL rv64: built archive missing symbol $s" >&2
            exit 1
        }
    done
    if "$nm_tool" -u "$rtmux_lib_abs" | awk '/ U / { print $2 }' |
        grep -Eq "$LKL_FORBIDDEN"; then
        echo "LKL rv64: built archive leaks POSIX-host symbols" >&2
        exit 1
    fi
    rm -f "$rtmux_out/.syms"

    # cfg_sha was captured from the object-pass .config before the header pass
    # reconfigured the tree, so it describes the config that built this archive.
    # excluded= lists the shared drops plus this personality's own OBJECT trim
    # (dropped=, e.g. fsd's net stack); netd drops no objects -- its ext4 removal is
    # config-only, so dropped= is empty -- so the record still matches source=.
    excluded="posix-host.o,config.o,virtio_net_fd.o,virtio_net_tap.o,virtio_net_raw.o,virtio_net_macvtap.o,virtio_net_pipe.o,vfio_pci.o"
    test -z "$dropped" || excluded="$dropped,$excluded"
    src_commit=$( (cd "$LKL_SRC" && git rev-parse HEAD 2>/dev/null) || echo unknown)
    cat > "$rtmux_out/BUILD_INFO" <<EOF
commit=$src_commit
config-sha256=${cfg_sha:-unknown}
date=$(date -u +%Y-%m-%dT%H:%M:%SZ)
arch=riscv64
output-format=elf64-littleriscv
personality=${personality:-combined}
source=$(echo $objs | tr -s ' ' '+')
excluded=$excluded
stripped=debug
EOF
    echo "LKL rv64: built $rtmux_lib_abs from source ($LKL_SRC)"
    exit 0
fi

if test "$mode" = "--extract"; then
    lock=prebuilt/lkl-rv64/.extract.lock
    mkdir -p "$(dirname "$lock")"
    while ! mkdir "$lock" 2>/dev/null; do
        if test -f "$out/liblkl.a" && test -f "$out/include/lkl.h" &&
           { test ! -f "$rtmux_artifact" ||
             { test -f "$rtmux_out/liblkl-rtmux.a" &&
               test -f "$rtmux_out/include/lkl.h"; }; }; then
            validate_rtmux_personality
            exit 0
        fi
        sleep 1
    done
    have_lock=1
    if test -f "$out/liblkl.a" && test -f "$out/include/lkl.h" &&
       { test ! -f "$rtmux_artifact" ||
         { test -f "$rtmux_out/liblkl-rtmux.a" &&
           test -f "$rtmux_out/include/lkl.h"; }; }; then
        validate_rtmux_personality
        exit 0
    fi
    test -f "$artifact" || {
        echo "LKL rv64: missing $artifact" >&2
        exit 1
    }
    rm -rf "$out"
    mkdir -p "$out"
    tar -xzf "$artifact" -C "$out"
    if test -f "$rtmux_artifact"; then
        rm -rf "$rtmux_out"
        mkdir -p "$rtmux_out"
        tar -xzf "$rtmux_artifact" -C "$rtmux_out"
        validate_rtmux_personality
    fi
    lib=$out/liblkl.a
else
    lib=${LKL_LIB:-}
    if test -z "$lib" && test -f "$out/liblkl.a"; then
        lib=$out/liblkl.a
    fi
fi

if test -z "${lib:-}"; then
    if test -f "$artifact"; then
        arch=$(tar -xOf "$artifact" ./BUILD_INFO | awk -F= '$1 == "arch" { print $2 }')
        fmt=$(tar -xOf "$artifact" ./BUILD_INFO | awk -F= '$1 == "output-format" { print $2 }')
        test "$arch" = "riscv64" && test "$fmt" = "elf64-littleriscv" || {
            echo "LKL rv64: $artifact has arch=$arch output-format=$fmt" >&2
            exit 1
        }
        if test -f "$rtmux_artifact"; then
            arch=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO |
                awk -F= '$1 == "arch" { print $2 }')
            fmt=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO |
                awk -F= '$1 == "output-format" { print $2 }')
            excluded=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO |
                awk -F= '$1 == "excluded" { print $2 }')
            stripped=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO |
                awk -F= '$1 == "stripped" { print $2 }')
            test "$arch" = "riscv64" && test "$fmt" = "elf64-littleriscv" || {
                echo "LKL rv64: $rtmux_artifact has arch=$arch output-format=$fmt" >&2
                exit 1
            }
            test "$stripped" = "debug" || {
                echo "LKL rv64: $rtmux_artifact is not stripped for server links" >&2
                exit 1
            }
            echo "$excluded" | grep -q 'posix-host.o' || {
                echo "LKL rv64: $rtmux_artifact does not exclude posix-host.o" >&2
                exit 1
            }
        fi
        exit 0
    fi
    echo "LKL rv64: no liblkl.a found; run $0 --extract or set LKL_LIB" >&2
    exit 1
fi

test -f "$lib" || {
    echo "LKL rv64: $lib not found" >&2
    exit 1
}

info=$(dirname "$lib")/BUILD_INFO
test -f "$info" || {
    echo "LKL rv64: missing $info" >&2
    exit 1
}
arch=$(awk -F= '$1 == "arch" { print $2 }' "$info")
fmt=$(awk -F= '$1 == "output-format" { print $2 }' "$info")
test "$arch" = "riscv64" && test "$fmt" = "elf64-littleriscv" || {
    echo "LKL rv64: $lib BUILD_INFO arch=$arch output-format=$fmt" >&2
    exit 1
}

ar_tool=$(resolve_tool "${LKL_AR:-}" riscv64-unknown-linux-gnu-ar llvm-ar \
    riscv-none-elf-ar) || {
    echo "LKL rv64: need an ar (llvm-ar or riscv64-*-ar) to verify $lib" >&2
    exit 1
}
nm_tool=$(resolve_tool "${LKL_NM:-}" riscv64-unknown-linux-gnu-nm llvm-nm \
    riscv-none-elf-nm) || {
    echo "LKL rv64: need an nm (llvm-nm or riscv64-*-nm) to verify $lib ABI" >&2
    exit 1
}

tmp=$(mktemp -d)
lib_abs=$(cd "$(dirname "$lib")" && pwd -P)/$(basename "$lib")
member=$("$ar_tool" t "$lib_abs" | grep '\.o' | head -1)
(cd "$tmp" && "$ar_tool" x "$lib_abs" "$member")
if ! file "$tmp"/*.o | grep -q 'UCB RISC-V'; then
    echo "LKL rv64: $lib is not a RISC-V archive" >&2
    exit 1
fi

missing=0
symbols_file=$tmp/symbols
"$nm_tool" -g "$lib" | awk '{print $NF}' > "$symbols_file"
for s in $LKL_REQUIRED_SYMS; do
    if ! grep -qx "$s" "$symbols_file"; then
        echo "LKL rv64: missing symbol $s in $lib" >&2
        missing=1
    fi
done
test "$missing" = 0

rtmux_lib=${LKL_RTMUX_LIB:-}
if test -z "$rtmux_lib" && test -f "$rtmux_out/liblkl-rtmux.a"; then
    rtmux_lib=$rtmux_out/liblkl-rtmux.a
fi
if test -z "$rtmux_lib"; then
    if test -f "$rtmux_artifact"; then
        arch=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO | awk -F= '$1 == "arch" { print $2 }')
        fmt=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO | awk -F= '$1 == "output-format" { print $2 }')
        excluded=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO |
            awk -F= '$1 == "excluded" { print $2 }')
        stripped=$(tar -xOf "$rtmux_artifact" ./BUILD_INFO |
            awk -F= '$1 == "stripped" { print $2 }')
        test "$arch" = "riscv64" && test "$fmt" = "elf64-littleriscv" || {
            echo "LKL rv64: $rtmux_artifact has arch=$arch output-format=$fmt" >&2
            exit 1
        }
        test "$stripped" = "debug" || {
            echo "LKL rv64: $rtmux_artifact is not stripped for server links" >&2
            exit 1
        }
        echo "$excluded" | grep -q 'posix-host.o' || {
            echo "LKL rv64: $rtmux_artifact does not exclude posix-host.o" >&2
            exit 1
        }
    fi
    exit 0
fi

test -f "$rtmux_lib" || {
    echo "LKL rv64: $rtmux_lib not found" >&2
    exit 1
}

rtmux_info=$(dirname "$rtmux_lib")/BUILD_INFO
test -f "$rtmux_info" || {
    echo "LKL rv64: missing $rtmux_info" >&2
    exit 1
}
arch=$(awk -F= '$1 == "arch" { print $2 }' "$rtmux_info")
fmt=$(awk -F= '$1 == "output-format" { print $2 }' "$rtmux_info")
stripped=$(awk -F= '$1 == "stripped" { print $2 }' "$rtmux_info")
test "$arch" = "riscv64" && test "$fmt" = "elf64-littleriscv" || {
    echo "LKL rv64: $rtmux_lib BUILD_INFO arch=$arch output-format=$fmt" >&2
    exit 1
}
test "$stripped" = "debug" || {
    echo "LKL rv64: $rtmux_lib is not stripped for server links" >&2
    exit 1
}
validate_rtmux_personality

rtmux_required=$(expected_rtmux_required_symbols)
rtmux_symbols_file=$tmp/rtmux-symbols
"$nm_tool" -g "$rtmux_lib" | awk '{print $NF}' > "$rtmux_symbols_file"
missing=0
for s in $rtmux_required; do
    if ! grep -qx "$s" "$rtmux_symbols_file"; then
        echo "LKL rv64: missing symbol $s in $rtmux_lib" >&2
        missing=1
    fi
done
test "$missing" = 0

# ar_tool/nm_tool were resolved (and required) for the base archive above.
rm -rf "$tmp"
tmp=$(mktemp -d)
lib_abs=$(cd "$(dirname "$rtmux_lib")" && pwd -P)/$(basename "$rtmux_lib")
member=$("$ar_tool" t "$lib_abs" | grep '\.o' | head -1)
(cd "$tmp" && "$ar_tool" x "$lib_abs" "$member")
if ! file "$tmp"/*.o | grep -q 'UCB RISC-V'; then
    echo "LKL rv64: $rtmux_lib is not a RISC-V archive" >&2
    exit 1
fi

"$nm_tool" -u "$rtmux_lib" | awk '/ U / { print $2 }' > "$tmp/rtmux-undefined"
if grep -Eq "$LKL_FORBIDDEN" "$tmp/rtmux-undefined"; then
    echo "LKL rv64: $rtmux_lib leaks POSIX-host symbols" >&2
    exit 1
fi
