#! /bin/bash

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$TOP_DIR" || exit 1
. "$SCRIPT_DIR/qemu-headless.sh"
AUTOCONF_H=internal/include/generated/autoconf.h

MODE=syscall-testing
TIMEOUT=180
TIMEOUT_SET=0
REPEAT=1
LAPS=
KEEP_LOGS=0
GDB_ON_TIMEOUT=0
REQUIRE_SUPPORTED=0
DISK_SUITE=0
NVME_SMOKE=0
STRESS=0
DIFFERENTIAL=0
NET_LOG_DIFF=0
EXTRA_ARGS=()
TIMEOUT_CMD=
NEEDS_LKL_SERVERS=0
NEEDS_INSTRUMENTATION=0
NEEDS_DEBUG_INJECT=0
NEEDS_STALL_DETECTOR=0
NEEDS_SYSLOG_SMALL=0
EMU_BUILD_DIR=
LKL_EXT4_IMG=
IMAGE_DIR=
NVME_IMG=
NVME_PRE_IMG=
NVME_P8_IMG=
RESTORE_CONFIG=0
CONFIG_SAVE=
AUTOCONF_SAVE=
HAD_CONFIG=0
HAD_AUTOCONF=0
http_pid=
udp_pid=
dns_pid=
payload_pid=
LAST_LOG=
IOMMU_METADATA_DTB=
IOMMU_METADATA_DTS=
IOMMU_METADATA_ERR=
FDT_CPU_DTB=
FDT_CPU_DTS=
FDT_CPU_SEEN=
FDT_CPU_BASE_OPTS=

cleanup()
{
    kill "${http_pid:-}" "${udp_pid:-}" "${dns_pid:-}" "${payload_pid:-}" \
        2>/dev/null || true
    rm -f "${IOMMU_METADATA_DTB:-}" "${IOMMU_METADATA_DTS:-}" \
        "${IOMMU_METADATA_ERR:-}" "${FDT_CPU_DTB:-}" "${FDT_CPU_DTS:-}"
    if [ "$RESTORE_CONFIG" -eq 1 ]; then
        if [ "$HAD_CONFIG" -eq 1 ]; then
            cp "$CONFIG_SAVE" .config
        else
            rm -f .config
        fi
        # Always drop autoconf.h rather than restoring a saved copy. Restoring
        # it preserved any .config/autoconf.h mismatch forever, and since the
        # copy lands newer than the restored .config, make considered it
        # up-to-date: a later build then silently used the wrong CONFIG_* set.
        # That is how an instrumented kernel got booted for a non-instrumented
        # mode. Deleting it costs one regeneration and cannot go stale.
        rm -f "$AUTOCONF_H"
        rm -f "$CONFIG_SAVE" "$AUTOCONF_SAVE"
        RESTORE_CONFIG=0
    fi
}

trap cleanup EXIT INT TERM

find_mkfs_ext4()
{
    if [ -n "${MKFS_EXT4:-}" ]; then
        command -v "$MKFS_EXT4"
        return
    fi
    command -v mkfs.ext4 2>/dev/null ||
        for f in /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4 \
                 /usr/local/opt/e2fsprogs/sbin/mkfs.ext4; do
            if [ -x "$f" ]; then
                printf '%s\n' "$f"
                break
            fi
        done
}

find_fsck_ext4()
{
    if [ -n "${FSCK_EXT4:-}" ]; then
        command -v "$FSCK_EXT4"
        return
    fi
    command -v fsck.ext4 2>/dev/null || command -v e2fsck 2>/dev/null ||
        for f in /opt/homebrew/opt/e2fsprogs/sbin/fsck.ext4 \
                 /opt/homebrew/opt/e2fsprogs/sbin/e2fsck \
                 /usr/local/opt/e2fsprogs/sbin/fsck.ext4 \
                 /usr/local/opt/e2fsprogs/sbin/e2fsck \
                 /usr/sbin/fsck.ext4 /usr/sbin/e2fsck \
                 /sbin/fsck.ext4 /sbin/e2fsck; do
            if [ -x "$f" ]; then
                printf '%s\n' "$f"
                break
            fi
        done
}

find_debugfs()
{
    if [ -n "${DEBUGFS:-}" ]; then
        command -v "$DEBUGFS"
        return
    fi
    command -v debugfs 2>/dev/null ||
        for f in /usr/sbin/debugfs /sbin/debugfs \
                 /opt/homebrew/opt/e2fsprogs/sbin/debugfs \
                 /usr/local/opt/e2fsprogs/sbin/debugfs; do
            if [ -x "$f" ]; then
                printf '%s\n' "$f"
                break
            fi
        done
}

resolve_tool()
{
    for t in "$@"; do
        [ -n "$t" ] || continue
        if command -v "$t" >/dev/null 2>&1; then
            echo "$t"
            return 0
        fi
    done
    return 1
}

# Differential FS oracle: extract the ext4 the guest wrote (it lives at sector 34
# of NVME_IMG, sized like LKL_EXT4_IMG -- see scripts/emu.sh
# refresh_lkl_ext4_partition) and check it with host e2fsprogs.
#
# The guest never cleanly unmounts, so the ext4 journal holds committed but
# uncheckpointed transactions.  Replay the journal first on the temp copy
# (-E journal_only, which ONLY replays -- it runs no repair passes, so unlike
# -fy/-fp it cannot silently launder genuine lkl-fsd corruption), so the check
# sees the filesystem a normal mount would observe, then gate on a read-only
# `e2fsck -fn`: exit non-zero on any real structural damage (bad inode, bitmap,
# directory).  Verified both directions with e2fsprogs 1.47: the guest image
# passes, and corrupting its inode table makes the -fn pass exit non-zero.
# (journal_only needs a Linux e2fsck; on a host where it errors the replay is a
# no-op and the -fn pass still catches structural damage in the on-disk image.)
differential_fs_check()
{
    local fsck debugfs size sectors work dump rc

    fsck="$(find_fsck_ext4 || true)"
    if [ -z "$fsck" ]; then
        echo "  DIFF     FAIL: fsck.ext4/e2fsck not found; install e2fsprogs" >&2
        return 1
    fi
    debugfs="$(find_debugfs || true)"
    if [ -z "$debugfs" ]; then
        echo "  DIFF     FAIL: debugfs not found; install e2fsprogs" >&2
        return 1
    fi
    if [ ! -f "$NVME_IMG" ] || [ ! -f "$LKL_EXT4_IMG" ]; then
        echo "  DIFF     FAIL: nvme/ext4 image missing for differential check" >&2
        return 1
    fi
    size="$(wc -c <"$LKL_EXT4_IMG")"
    sectors=$(( size / 512 ))
    work="$(mktemp /tmp/rtmux-diff-ext4.XXXXXX)"
    dump="$(mktemp -d /tmp/rtmux-diff-dump.XXXXXX)"
    dd if="$NVME_IMG" bs=512 skip=34 count="$sectors" of="$work" status=none

    echo "  DIFF     replaying journal (e2fsck -E journal_only)"
    "$fsck" -y -E journal_only "$work" >/tmp/rtmux-diff-replay.log 2>&1 || true
    echo "  DIFF     e2fsck -fn on the recovered ext4"
    "$fsck" -fn "$work" >/tmp/rtmux-diff-fsck.log 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  DIFF     FAIL: guest-written ext4 not clean under e2fsck (rc=$rc)"
        cat /tmp/rtmux-diff-fsck.log >&2
        rm -rf "$dump" "$work"
        return 1
    fi

    echo "  DIFF     reading recovered ext4 with debugfs"
    if ! "$debugfs" -R "cat /hello.txt" "$work" >"$dump/hello.txt" \
            2>/tmp/rtmux-diff-debugfs.log ||
       ! printf 'hello from lkl-fsd\n' | cmp -s - "$dump/hello.txt"; then
        echo "  DIFF     FAIL: Linux read-back of hello.txt mismatched" >&2
        cat /tmp/rtmux-diff-debugfs.log >&2
        rm -rf "$dump" "$work"
        return 1
    fi
    for f in /moved.txt /touched.txt /pipe.txt; do
        "$debugfs" -R "stat $f" "$work" >"$dump/stat.out" 2>&1 || true
        if ! grep -Eq 'File not found|not found' "$dump/stat.out"; then
            echo "  DIFF     FAIL: removed lkl-fsd-tree scratch file persisted: $f" >&2
            cat "$dump/stat.out" >&2
            rm -rf "$dump" "$work"
            return 1
        fi
    done
    if [ "$MODE" = "lkl-multi" ] || [ "$MODE" = "lkl-multi-stress" ]; then
        if ! "$debugfs" -R "dump /dl.bin $dump/dl.bin" "$work" \
                >/tmp/rtmux-diff-debugfs.log 2>&1; then
            echo "  DIFF     FAIL: could not read dl.bin with debugfs" >&2
            cat /tmp/rtmux-diff-debugfs.log >&2
            rm -rf "$dump" "$work"
            return 1
        fi
        python3 - "$dump/dl.bin" <<'PY'
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
want = bytes(i & 0xff for i in range(8192))
if p.read_bytes() != want:
    raise SystemExit(1)
PY
        if [ "$?" -ne 0 ]; then
            echo "  DIFF     FAIL: Linux read-back of dl.bin mismatched" >&2
            rm -rf "$dump" "$work"
            return 1
        fi
    fi
    rm -rf "$dump" "$work"
    echo "  DIFF     PASS: guest-written ext4 is clean and readable by host e2fsprogs"
    return 0
}

# Host oracle for the shutdown-sync pair: the guest wrote an un-fsynced
# sentinel and shut down within a second, so only the roottask sync sweep can
# have put it on the disk.  shutdown-sync must find it intact; the negative
# variant (shutdown -n) must not, or the oracle proves nothing.
differential_sync_check()
{
    local fsck debugfs size sectors work dump rc

    fsck="$(find_fsck_ext4 || true)"
    debugfs="$(find_debugfs || true)"
    if [ -z "$fsck" ] || [ -z "$debugfs" ]; then
        echo "  DIFF     FAIL: e2fsck/debugfs not found; install e2fsprogs" >&2
        return 1
    fi
    if [ ! -f "$NVME_IMG" ] || [ ! -f "$LKL_EXT4_IMG" ]; then
        echo "  DIFF     FAIL: nvme/ext4 image missing for differential check" >&2
        return 1
    fi
    size="$(wc -c <"$LKL_EXT4_IMG")"
    sectors=$(( size / 512 ))
    work="$(mktemp /tmp/rtmux-diff-ext4.XXXXXX)"
    dump="$(mktemp -d /tmp/rtmux-diff-dump.XXXXXX)"
    dd if="$NVME_IMG" bs=512 skip=34 count="$sectors" of="$work" status=none

    "$fsck" -y -E journal_only "$work" >/tmp/rtmux-diff-replay.log 2>&1 || true
    "$fsck" -fn "$work" >/tmp/rtmux-diff-fsck.log 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  DIFF     FAIL: ext4 not clean under e2fsck (rc=$rc)" >&2
        cat /tmp/rtmux-diff-fsck.log >&2
        rm -rf "$dump" "$work"
        return 1
    fi
    # The guest copied /hello.txt to /sync-sentinel.txt, so a surviving
    # sentinel must match the seed byte for byte.
    "$debugfs" -R "cat /hello.txt" "$work" >"$dump/hello.txt" \
        2>/tmp/rtmux-diff-debugfs.log || true
    "$debugfs" -R "cat /sync-sentinel.txt" "$work" >"$dump/sentinel.txt" \
        2>>/tmp/rtmux-diff-debugfs.log || true
    if [ ! -s "$dump/hello.txt" ]; then
        echo "  DIFF     FAIL: could not read /hello.txt with debugfs" >&2
        cat /tmp/rtmux-diff-debugfs.log >&2
        rm -rf "$dump" "$work"
        return 1
    fi
    if [ "$MODE" = "shutdown-sync" ] || [ "$MODE" = "shutdown-sync-ignore" ]; then
        if ! cmp -s "$dump/hello.txt" "$dump/sentinel.txt"; then
            echo "  DIFF     FAIL: sentinel did not survive shutdown" >&2
            cat /tmp/rtmux-diff-debugfs.log >&2
            rm -rf "$dump" "$work"
            return 1
        fi
        echo "  DIFF     PASS: shutdown flushed the sentinel; ext4 is clean"
    else
        if [ -s "$dump/sentinel.txt" ]; then
            echo "  DIFF     FAIL: sentinel survived 'shutdown -n'; the oracle does not bite" >&2
            rm -rf "$dump" "$work"
            return 1
        fi
        echo "  DIFF     PASS: 'shutdown -n' lost the sentinel as expected; ext4 is clean"
    fi
    rm -rf "$dump" "$work"
    return 0
}

differential_net_check()
{
    if [ -z "$LAST_LOG" ] || [ ! -s "$LAST_LOG" ]; then
        echo "  DIFF     FAIL: net guest log missing for virtio-net oracle" >&2
        return 1
    fi
    echo "  DIFF     checking virtio-net behavior in $LAST_LOG"
    require_lkl_net_oracle "$LAST_LOG" || return 1
    require_log_patterns "$LAST_LOG" "lkl-netd latency" \
        '^lkl-measure: mode=net samples=' \
        '^lkl-measure: mode=net-rt samples=' || return 1
    echo "  DIFF     PASS: virtio-net HTTP/UDP smoke, latency rows, and copy counters present"
    return 0
}

require_log_patterns()
{
    local log="$1"
    local label="$2"
    local re

    shift 2
    for re in "$@"; do
        if ! grep -Eq "$re" "$log"; then
            echo "  FAIL     ${MODE} missing ${label} evidence: ${re}"
            tail -n 160 "$log"
            return 1
        fi
    done
    return 0
}

require_log_patterns_in_order()
{
    local log="$1"
    local label="$2"

    shift 2
    if ! awk -v mode="$MODE" -v label="$label" '
        BEGIN {
            next_pat = 1
            pat_count = ARGC - 2
            for (i = 2; i < ARGC; i++)
                pat[i - 1] = ARGV[i]
            ARGC = 2
        }
        {
            if (next_pat <= pat_count && $0 ~ pat[next_pat])
                next_pat++
        }
        END {
            if (next_pat <= pat_count) {
                printf("  FAIL     %s missing or out-of-order %s evidence: %s\n",
                       mode, label, pat[next_pat])
                exit 1
            }
        }
    ' "$log" "$@"; then
        tail -n 160 "$log"
        return 1
    fi
    return 0
}

require_lkl_net_oracle()
{
    local log="$1"

    require_log_patterns "$log" "lkl-netd bring-up" \
        'lkl-netd: lkl_start_kernel returned' \
        'lkl-netd: ifindex=[1-9][0-9]*' \
        'lkl-netd: if0 10\.0\.2\.15/24 gw 10\.0\.2\.2' \
        'lkl-netd: serving /net' || return 1
    require_log_patterns "$log" "lkl-netd smoke" \
        'verify-lkl-net: ping' \
        'verify-lkl-net: httpget body length ok' \
        'verify-lkl-net: wget body length ok' \
        'verify-lkl-net: nc http body length ok' \
        'verify-lkl-net: udp payload ok' || return 1
    require_log_patterns "$log" "net bounce" \
        '^net_bounce_rx_bytes [1-9][0-9]*' \
        '^net_bounce_tx_bytes [1-9][0-9]*'
}

require_lkl_fsd_oracle()
{
    local log="$1"
    local require_write="$2"

    require_log_patterns "$log" "lkl-fsd bring-up" \
        'lkl-fsd: lkl_start_kernel returned' \
        'lkl-fsd: serving /dev/nvme0n1p1 at /fs/root' || return 1
    if [ "$require_write" -eq 1 ]; then
        require_log_patterns "$log" "LKL block copy" \
            '^lkl_block_read_bytes [1-9][0-9]*' \
            '^lkl_block_write_bytes [1-9][0-9]*'
    else
        require_log_patterns "$log" "LKL block copy" \
            '^lkl_block_read_bytes [1-9][0-9]*'
    fi
}

require_lkl_mode_oracle()
{
    local log="$1"

    case "$MODE" in
        lkl-netd-http|lkl-netd-http-isolated|lkl-netd-http-split)
            require_lkl_net_oracle "$log"
            ;;
        lkl-fsd-tree)
            require_lkl_fsd_oracle "$log" 1
            ;;
        lkl-fsd-tree-isolated|lkl-fsd-tree-split)
            require_lkl_fsd_oracle "$log" 0
            ;;
        lkl-multi)
            require_lkl_net_oracle "$log" || return 1
            require_lkl_fsd_oracle "$log" 1
            ;;
    esac
}

# The roottask sync sweep logs "sync: start" and "sync: done t=<ms>"; the
# modes below pin what it did.  The toolbox suite runs the sync applet and then
# shutdown, so it sweeps twice.
require_sync_log_oracle()
{
    local log="$1" n t

    case "$MODE" in
        toolbox)
            n="$(grep -c 'sync: done t=' "$log")"
            if [ "$n" -lt 2 ]; then
                echo "  FAIL     ${MODE} expected 2 sync sweeps (sync + shutdown), saw ${n}"
                tail -n 160 "$log"
                return 1
            fi
            require_log_patterns "$log" "sync sweep" 'sync: start' || return 1
            ;;
        shutdown-sync)
            require_log_patterns "$log" "sync sweep" \
                'sync: start' 'sync: done t=' || return 1
            ;;
        shutdown-sync-negative)
            if grep -q 'sync: start' "$log"; then
                echo "  FAIL     ${MODE} shutdown -n still ran the sync sweep"
                tail -n 160 "$log"
                return 1
            fi
            ;;
        shutdown-sync-wedge)
            n="$(grep -c 'sync timeout: ' "$log")"
            t="$(sed -n 's/.*sync: done t=\([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)"
            # The only mount is wedged past the kernel unblock request: the
            # sweep abandons its worker at the 2 s budget plus a 250 ms grace.
            if [ "$n" -ne 1 ] || [ -z "$t" ] || [ "$t" -lt 2000 ] ||
               [ "$t" -gt 2500 ]; then
                echo "  FAIL     ${MODE} want one sync timeout and 2000<=t<=2500 ms, saw ${n} timeout(s), t=${t:-none}"
                tail -n 160 "$log"
                return 1
            fi
            ;;
        shutdown-sync-ignore)
            n="$(grep -c 'sync timeout: ' "$log")"
            t="$(sed -n 's/.*sync: done t=\([0-9][0-9]*\).*/\1/p' "$log" | tail -n 1)"
            # One wedged mount costs its 2 s budget; the healthy ext4 flush
            # after it adds its commit time, well inside the 8 s sweep cap.
            if [ "$n" -ne 1 ] || [ -z "$t" ] || [ "$t" -lt 2000 ] ||
               [ "$t" -gt 7000 ]; then
                echo "  FAIL     ${MODE} want one sync timeout and 2000<=t<=7000 ms, saw ${n} timeout(s), t=${t:-none}"
                tail -n 160 "$log"
                return 1
            fi
            ;;
    esac
    return 0
}

usage()
{
    cat <<'EOF'
usage: scripts/qemu-regress.sh [options] [-- extra emu args...]

Options:
  --mode NAME       Guest rtmux.run mode: syscall-testing,
                    posix-cap-boundary, loader-negative, syscall-testing-disk,
                    spawn-storm (8x100 concurrent posix_spawn; open SMP defect),
                    pci-nvme-smoke, rt-check, leak-gate (opt-in killspawn + laps),
                    version-skew (kernel and initramfs from two trees; expects one
                    "version skew:" line),
                    rt-check-instr (rt-check with CONFIG_INSTRUMENTATION=y),
                    debug-inject (CONFIG_DEBUG_INJECT=y; stall hart 1 for 3 s),
                    hart-watchdog (CONFIG_STALL_DETECTOR=y + CONFIG_DEBUG_INJECT=y;
                    stall hart 2 for 3 s, expect one neighbour-watchdog report),
                    hart-watchdog-console (as hart-watchdog, with the stalled hart
                    holding the console lock: the report must still appear),
                    hart-watchdog-idle (CONFIG_STALL_DETECTOR=y; 60 s idle,
                    expect no report),
                    roottask-wait-watch (CONFIG_STALL_DETECTOR=y; a server
                    holds two sync requests for 8 s, expect two
                    "liveness:" lines naming the reply-blocked roottask thread),
                    hang-inject (hang all but one hart; needs --gdb-on-timeout,
                    passes on a complete per-hart dump),
                    tickless-idle (quiet-hart tick drop + one-shot deadline),
                    fault-recovery (fault taxonomy smoke),
                    monotonic-time (continuous clock across a skipped-tick gap),
                    pi-inversion-xfail, rcu-lookup-measure, revoke-stress,
                    smp-invariants (CPUS=8 bounded SMP invariant smoke),
                    syslog-wrap (CONFIG_ROOTTASK_SYSLOG_SIZE=4096; /sys/log
                    wrap and EOVERFLOW),
                    serial-unblock (dev-serial-8250 drops a SIGKILLed parked
                    reader; the host injects one UART byte),
                    irq-notify (guest IRQ notification smoke; run it with
                    EMU_QEMU_OPTS="-dtb X" to boot a patched DTB; every
                    latency attempt must end ok, none early or timeout),
                    irq-notify-suppress (irq-notify --suppress-one: one seeded
                    attempt has its IRQ gated off and must time out 1.0-1.1 s
                    after arm),
                    fdt-cpu-status (disables cpu@1 in the QEMU DTB, once with
                    status before mmu-type and once after, and requires the
                    kernel to withhold that hart yet pass irq-notify),
                    interactive-smoke, boot,
                    lkl-host-selftest, lkl-fsd-crash, lkl-fsd-tree,
                    lkl-fsd-tree-split (pins lkl-measure load to CPU1 and the
                    native RT probe to CPU0), lkl-fsd-tree-isolated (also pins
                    lkl-fsd to CPU1), lkl-fsd-tricky (lkl-fsd reads a
                    host-authored ext4 with hardlinks/sparse/big dir),
                    lkl-netd-crash, lkl-netd-direct-denied, lkl-netd-http,
                    lkl-netd-http-split, lkl-netd-http-isolated, lkl-netd-revoke,
                    lkl-netd-stale,
                    lkl-multi (boots lkl-fsd and lkl-netd together and
                    exercises both domains concurrently), or lkl-multi-crash
                    (crashes lkl-netd mid-workload and checks lkl-fsd survives),
                    lkl-iommu-probe (boots QEMU with virtio-iommu-pci and
                    requires pci-server to enumerate it),
                    lkl-iommu-sys-probe (boots QEMU with virt,iommu-sys=on and
                    requires the typed system-IOMMU resource plus retained DDT
                    modes),
                    lkl-iommu-metadata (boots a patched DTB fixture proving
                    MMIO virtio IOMMU metadata preservation),
                    lkl-iommu-domain-preflight (same patched DTB fixture,
                    plus synthetic DID-42 domain allow/deny probing),
                    lkl-netd-iommu-fail-closed (same patched DTB fixture,
                    but verifies lkl-netd refuses the delegated device),
                    lkl-iommu-domain, lkl-iommu-dma-deny, lkl-iommu-revoke
                    (also require an MMIO virtio iommus binding),
                    tracelogger, tracelogger-kcall, tracelogger-lkl-fsd,
                    tracelogger-lkl-netd,
                    opts-pse51,
                    shutdown-sync (leave an un-fsynced file on the LKL ext4
                    mount and shut down; with --differential the host checks
                    the file reached the disk), shutdown-sync-negative (same
                    with `shutdown -n`, which must lose the file),
                    shutdown-sync-wedge (a mount whose only dispatch thread
                    never takes the unblock request; the sweep must still end
                    in 2.0-2.5 s), shutdown-sync-ignore (a mount that never answers the sync
                    request, visited before a healthy LKL ext4 mount; the sweep must time
                    out once on it, in 2-7 s, and with --differential still
                    flush the ext4 sentinel), or
                    tracelogger-lkl-netd-default (default: syscall-testing)
                    syscall-testing-disk boots the suite from the /usr romfs
                    block device (a dedicated nvme-tests.img) instead of the cpio.
                    pci-nvme-smoke asserts the PCI/NVMe bring-up diagnostics
                    (ECAM, BAR0 map, vendor id, GPT, IRQ/MSI vector, mainfs mount).
  --timeout SEC     Per-boot timeout in seconds (default: 180; LKL
                    single-server modes: 60; lkl-multi/lkl-multi-crash: 180;
                    lkl-multi --stress: 360)
  --repeat N        Number of boots to run (default: 1)
  --laps N          syscall-testing/leak-gate: run the suite N times in one boot
                    (guest rtmux.laps=N) and require N 'lap=<n> PASS' lines
  --gdb-on-timeout  On a timeout, dump every hart through a live gdb stub
                    (scripts/gdb/dump-state.gdb) into build/lkl-logs/MODE-N.dump
                    before killing QEMU; implies --keep-logs
  --keep-logs       Keep passing logs (at build/lkl-logs/MODE-idx.log) instead
                    of deleting them; IOMMU DTB preflights also keep
                    build/lkl-logs/MODE-iommu.{dtb,dts}
  --require-supported
                    Fail instead of skipping an unsupported host-gated mode
  --stress          With --mode lkl-multi, run the sustained dual-LKL stress
                    loop (many net->fs cycles) to guard the timer-fire ceiling
  --differential    Post-boot oracle: for an ext4-producing mode
                    (lkl-fsd-tree/lkl-multi/lkl-multi --stress) e2fsck the
                    guest-written ext4; for shutdown-sync[-negative|-ignore] e2fsck it
                    and read the sentinel back; for lkl-netd-http, check delegated
                    virtio-net smoke, latency rows, and copy counters
EOF
}

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD=gtimeout
else
    echo "qemu-regress: need timeout or gtimeout in PATH" >&2
    exit 2
fi

qemu_has_device()
{
    "${QEMU_HEADLESS_ENV[@]}" "${QEMU:-qemu-system-riscv64}" \
        -machine none \
        "${QEMU_HEADLESS_PROBE_OPTS[@]}" \
        -device help 2>/dev/null |
        grep -q "name \"$1\""
}

qemu_device_usable_on_virt()
{
    local device="$1"

    "$TIMEOUT_CMD" 3 "${QEMU_HEADLESS_ENV[@]}" "${QEMU:-qemu-system-riscv64}" \
        -machine virt,graphics=off \
        "${QEMU_HEADLESS_PROBE_OPTS[@]}" \
        -device "$device" \
        -S </dev/null >/dev/null 2>&1
    [ "$?" -eq 124 ]
}

qemu_machine_usable()
{
    local machine="$1"

    "$TIMEOUT_CMD" 3 "${QEMU_HEADLESS_ENV[@]}" "${QEMU:-qemu-system-riscv64}" \
        -machine "$machine" \
        "${QEMU_HEADLESS_PROBE_OPTS[@]}" \
        -S </dev/null >/dev/null 2>&1
    [ "$?" -eq 124 ]
}

append_qemu_machine_opt()
{
    local opt="$1"

    case ",${EMU_QEMU_MACHINE_OPTS:-}," in
        *,"$opt",*) return 0 ;;
    esac
    export EMU_QEMU_MACHINE_OPTS="${EMU_QEMU_MACHINE_OPTS:-},$opt"
}

setup_pci_iommu_or_skip()
{
    if qemu_has_device riscv-iommu-pci &&
       qemu_device_usable_on_virt riscv-iommu-pci; then
        export EMU_QEMU_OPTS="${EMU_QEMU_OPTS:+$EMU_QEMU_OPTS }-device riscv-iommu-pci"
        return 0
    fi
    if qemu_has_device virtio-iommu-pci &&
       qemu_device_usable_on_virt virtio-iommu-pci,class=0x0806; then
        export EMU_QEMU_OPTS="${EMU_QEMU_OPTS:+$EMU_QEMU_OPTS }-device virtio-iommu-pci,class=0x0806"
        return 0
    fi

    echo "  UNSUPPORTED ${MODE}: no usable QEMU PCI IOMMU device for virt"
    if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
        exit 2
    fi
    exit 0
}

setup_system_iommu_or_skip()
{
    if qemu_machine_usable virt,graphics=off,iommu-sys=on; then
        append_qemu_machine_opt iommu-sys=on
        return 0
    fi

    echo "  UNSUPPORTED ${MODE}: no usable QEMU system IOMMU for virt"
    if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
        exit 2
    fi
    exit 0
}

system_iommu_mmio_binding_in_dts()
{
    awk '
        /iommu@/ {
            in_iommu = 1
            in_compat = 0
            iommu_compat = 0
            iommu_phandle_candidate = ""
        }
        in_iommu && /compatible[[:space:]]*=/ { in_compat = 1 }
        in_iommu && in_compat {
            n = split($0, compat, /"/)
            for (i = 2; i <= n; i += 2) {
                if (compat[i] == "riscv,iommu")
                    iommu_compat = 1
            }
            if ($0 ~ /;/)
                in_compat = 0
        }
        in_iommu && /(^|[[:space:]])(linux,)?phandle[[:space:]]*=/ {
            line = $0
            sub(/^.*(linux,)?phandle[[:space:]]*=[[:space:]]*</, "", line)
            split(line, cells, /[[:space:]>]/)
            iommu_phandle_candidate = cells[1]
        }
        in_iommu && /^[[:space:]]*};/ {
            if (iommu_compat && iommu_phandle_candidate != "")
                iommu_phandles[iommu_phandle_candidate] = 1
            in_iommu = 0
        }
        /virtio_mmio@/ {
            in_virtio = 1
            in_iommus = 0
            seen_iommus_cell = 0
        }
        in_virtio && /iommus[[:space:]]*=/ {
            in_iommus = 1
            seen_iommus_cell = 0
        }
        in_virtio && in_iommus {
            line = $0
            sub(/^.*iommus[[:space:]]*=[[:space:]]*</, "", line)
            sub(/^[[:space:]]*/, "", line)
            sub(/^[[:space:]]*</, "", line)
            split(line, cells, /[[:space:]>]/)
            if (!seen_iommus_cell && cells[1] != "") {
                virtio_iommus[cells[1]] = 1
                seen_iommus_cell = 1
            }
            if ($0 ~ /;/)
                in_iommus = 0
        }
        in_virtio && /^[[:space:]]*};/ {
            in_virtio = 0
        }
        END {
            for (phandle in iommu_phandles) {
                if (virtio_iommus[phandle])
                    found = 1
            }
            exit found ? 0 : 1
        }
    ' "$1"
}

keep_system_iommu_dtb_artifacts()
{
    local dtb="$1"
    local dts="$2"
    local err="$3"
    local out="build/lkl-logs/${MODE}-iommu"
    if [ -n "${QEMU_REGRESS_LOG_DIR:-}" ]; then
        out="$QEMU_REGRESS_LOG_DIR/${MODE}-iommu"
    fi

    [ "$KEEP_LOGS" -eq 1 ] || return 0
    mkdir -p "$(dirname "$out")"
    rm -f "${out}.dtb" "${out}.dts" "${out}.err"
    if [ -s "$dtb" ]; then
        cp "$dtb" "${out}.dtb"
        echo "  INFO ${MODE}: kept QEMU system-IOMMU DTB ${out}.dtb"
    fi
    if [ -s "$dts" ]; then
        cp "$dts" "${out}.dts"
        echo "  INFO ${MODE}: kept decoded QEMU system-IOMMU DTS ${out}.dts"
    fi
    if [ -s "$err" ]; then
        cp "$err" "${out}.err"
        echo "  INFO ${MODE}: kept QEMU system-IOMMU stderr ${out}.err"
    fi
}

setup_system_iommu_mmio_binding_or_skip()
{
    local dtb dts

    if ! command -v dtc >/dev/null 2>&1; then
        echo "  UNSUPPORTED ${MODE}: dtc is required to inspect QEMU system-IOMMU bindings"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
    fi

    dtb="$(mktemp /tmp/rtmux-iommu-dtb.XXXXXX)"
    dts="$(mktemp /tmp/rtmux-iommu-dts.XXXXXX)"
    local net_device

    net_device="virtio-net-device,netdev=rtmuxnet,iommu_platform=on"
    net_device="${net_device},mac=52:54:00:12:34:56"
    if ! "${QEMU_HEADLESS_ENV[@]}" "${QEMU:-qemu-system-riscv64}" \
            -machine virt,graphics=off,iommu-sys=on,dumpdtb="$dtb" \
            -m 256M \
            -netdev user,id=rtmuxnet \
            -device "$net_device" \
            -nographic -serial none -monitor none -S \
            >/dev/null 2>"$dts.err"; then
        keep_system_iommu_dtb_artifacts "$dtb" "$dts" "$dts.err"
        rm -f "$dtb" "$dts" "$dts.err"
        echo "  UNSUPPORTED ${MODE}: cannot dump QEMU system-IOMMU DTB"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
    fi
    if ! dtc -I dtb -O dts "$dtb" >"$dts" 2>/dev/null; then
        keep_system_iommu_dtb_artifacts "$dtb" "$dts" "$dts.err"
        rm -f "$dtb" "$dts" "$dts.err"
        echo "  UNSUPPORTED ${MODE}: cannot decode QEMU system-IOMMU DTB"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
    fi
    keep_system_iommu_dtb_artifacts "$dtb" "$dts" "$dts.err"
    if system_iommu_mmio_binding_in_dts "$dts"; then
        rm -f "$dtb" "$dts" "$dts.err"
        return 0
    fi

    if grep -q 'iommu-map[[:space:]]*=' "$dts"; then
        echo "  UNSUPPORTED ${MODE}: QEMU exposes only PCI iommu-map, not MMIO virtio iommus binding"
    else
        echo "  UNSUPPORTED ${MODE}: QEMU system IOMMU is not bound to MMIO virtio devices"
    fi
    rm -f "$dtb" "$dts" "$dts.err"
    if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
        exit 2
    fi
    exit 0
}

patch_system_iommu_metadata_dts()
{
    local in_dts="$1"
    local out_dts="$2"
    local stream="$3"
    local phandle

    phandle="$(awk '
        /iommu@/ {
            in_iommu = 1
            in_compat = 0
            iommu_compat = 0
            phandle = ""
        }
        in_iommu && /compatible[[:space:]]*=/ {
            in_compat = 1
        }
        in_iommu && in_compat {
            n = split($0, compat, /"/)
            for (i = 2; i <= n; i += 2) {
                if (compat[i] == "riscv,iommu")
                    iommu_compat = 1
            }
            if ($0 ~ /;/)
                in_compat = 0
        }
        in_iommu && /(^|[[:space:]])(linux,)?phandle[[:space:]]*=/ {
            line = $0
            sub(/^.*(linux,)?phandle[[:space:]]*=[[:space:]]*</, "", line)
            split(line, cells, /[[:space:]>]/)
            phandle = cells[1]
        }
        in_iommu && /^[[:space:]]*};/ {
            if (iommu_compat && phandle != "" && found == "") {
                found = phandle
                exit
            }
            in_iommu = 0
        }
        END { if (found != "") print found }
    ' "$in_dts")"
    [ -n "$phandle" ] || return 1

    awk -v phandle="$phandle" -v stream="$stream" '
        /virtio_mmio@/ {
            in_virtio = 1
            wrote_iommus = 0
            skip_iommus = 0
        }
        in_virtio && /iommus[[:space:]]*=/ {
            if (!wrote_iommus) {
                printf "\t\tiommus = <%s %s>;\n", phandle, stream
                wrote_iommus = 1
            }
            skip_iommus = 1
            if ($0 ~ /;/)
                skip_iommus = 0
            next
        }
        in_virtio && skip_iommus {
            if ($0 ~ /;/)
                skip_iommus = 0
            next
        }
        in_virtio && /^[[:space:]]*};/ {
            if (!wrote_iommus)
                printf "\t\tiommus = <%s %s>;\n", phandle, stream
            in_virtio = 0
        }
        { print }
    ' "$in_dts" >"$out_dts"
}

setup_system_iommu_metadata_or_skip()
{
    local dtb dts patched_dtb patched_dts

    setup_system_iommu_or_skip
    if ! command -v dtc >/dev/null 2>&1; then
        echo "  UNSUPPORTED ${MODE}: dtc is required to build the IOMMU metadata DTB fixture"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
    fi

    dtb="$(mktemp /tmp/rtmux-iommu-dtb.XXXXXX)"
    dts="$(mktemp /tmp/rtmux-iommu-dts.XXXXXX)"
    patched_dtb="$(mktemp /tmp/rtmux-iommu-patched-dtb.XXXXXX)"
    patched_dts="$(mktemp /tmp/rtmux-iommu-patched-dts.XXXXXX)"
    IOMMU_METADATA_DTB="$patched_dtb"
    IOMMU_METADATA_DTS="$patched_dts"
    IOMMU_METADATA_ERR="$dts.err"

    local net_device
    net_device="virtio-net-device,netdev=rtmuxnet,iommu_platform=on"
    net_device="${net_device},mac=52:54:00:12:34:56"
    if ! "${QEMU_HEADLESS_ENV[@]}" "${QEMU:-qemu-system-riscv64}" \
            -machine virt,graphics=off,iommu-sys=on,dumpdtb="$dtb" \
            -m 256M \
            -netdev user,id=rtmuxnet \
            -device "$net_device" \
            -nographic -serial none -monitor none -S \
            >/dev/null 2>"$dts.err"; then
        keep_system_iommu_dtb_artifacts "$dtb" "$dts" "$dts.err"
        rm -f "$dtb" "$dts"
        echo "  UNSUPPORTED ${MODE}: cannot dump QEMU system-IOMMU DTB"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
    fi
    if ! dtc -I dtb -O dts "$dtb" >"$dts" 2>>"$dts.err" ||
       ! patch_system_iommu_metadata_dts "$dts" "$patched_dts" 42 ||
       ! dtc -I dts -O dtb "$patched_dts" -o "$patched_dtb" 2>>"$dts.err"; then
        keep_system_iommu_dtb_artifacts "$dtb" "$patched_dts" "$dts.err"
        rm -f "$dtb" "$dts"
        echo "  UNSUPPORTED ${MODE}: cannot build IOMMU metadata DTB fixture"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
    fi
    keep_system_iommu_dtb_artifacts "$patched_dtb" "$patched_dts" "$dts.err"
    rm -f "$dtb" "$dts"
    export EMU_QEMU_OPTS="${EMU_QEMU_OPTS:+$EMU_QEMU_OPTS }-dtb $patched_dtb"
}

# patch_fdt_cpu_status_dts IN OUT HART PLACEMENT: set cpu@HART status = "disabled"
# in a decoded QEMU DTS. PLACEMENT is "first" (status ahead of mmu-type, QEMU's
# own order) or "last" (after mmu-type, ahead of the cpu's child nodes). dtc
# needs a node's properties before its subnodes, so the status line is dropped
# from where QEMU put it and re-emitted at the requested spot.
patch_fdt_cpu_status_dts()
{
    local in_dts="$1" out_dts="$2" hart="$3" where="$4"

    awk -v hart="$hart" -v where="$where" '
        $0 ~ "^[[:space:]]*cpu@" hart "[[:space:]]*{" {
            in_cpu = 1
            in_child = 0
            emitted = 0
            found++
            print
            if (where == "first") {
                print "\t\t\tstatus = \"disabled\";"
                emitted = 1
            }
            next
        }
        in_cpu && !in_child && /^[[:space:]]*status[[:space:]]*=/ { next }
        in_cpu && !emitted && /\{[[:space:]]*$/ {
            print "\t\t\tstatus = \"disabled\";"
            emitted = 1
        }
        in_cpu && /\{[[:space:]]*$/ { in_child = 1 }
        in_cpu && /^\t\t};/ { in_cpu = 0 }
        { print }
        END { exit(found == 1 && emitted ? 0 : 1) }
    ' "$in_dts" >"$out_dts"
}

# setup_fdt_cpu_status_or_skip PLACEMENT BASE_OPTS: dump QEMU's DTB for CPUS
# harts, patch cpu@1 to disabled, and point the guest at it.
setup_fdt_cpu_status_or_skip()
{
    local where="$1" base="$2" ok=1 raw raw_dts

    if ! command -v dtc >/dev/null 2>&1; then
        echo "  UNSUPPORTED ${MODE}: dtc is required to build the cpu status DTB fixture"
        [ "$REQUIRE_SUPPORTED" -eq 1 ] && exit 2
        exit 0
    fi
    rm -f "$FDT_CPU_DTB" "$FDT_CPU_DTS"
    FDT_CPU_DTB="$(mktemp /tmp/rtmux-fdt-cpu-dtb.XXXXXX)"
    FDT_CPU_DTS="$(mktemp /tmp/rtmux-fdt-cpu-dts.XXXXXX)"
    raw="$FDT_CPU_DTB.raw"
    raw_dts="$FDT_CPU_DTS.raw"
    if ! "${QEMU_HEADLESS_ENV[@]}" "${QEMU:-qemu-system-riscv64}" \
            -machine "virt,graphics=off,dumpdtb=$raw" -m 256M -smp "${CPUS:-1}" \
            -nographic -serial none -monitor none -S >/dev/null 2>&1 ||
       ! dtc -I dtb -O dts "$raw" >"$raw_dts" 2>/dev/null ||
       ! patch_fdt_cpu_status_dts "$raw_dts" "$FDT_CPU_DTS" 1 "$where" ||
       ! dtc -I dts -O dtb "$FDT_CPU_DTS" -o "$FDT_CPU_DTB" 2>/dev/null; then
        ok=0
    fi
    rm -f "$raw" "$raw_dts"
    if [ "$ok" -eq 0 ]; then
        echo "  UNSUPPORTED ${MODE}: cannot build cpu status DTB fixture"
        [ "$REQUIRE_SUPPORTED" -eq 1 ] && exit 2
        exit 0
    fi
    export EMU_QEMU_OPTS="${base:+$base }-dtb $FDT_CPU_DTB"
}

# check_fdt_cpu_status LOG PLACEMENT: OpenSBI picks the boot hart by lottery, so
# the run lands in one of two cases. If cpu@1 is not the boot hart it must be
# reported withheld, never started, and the online count must drop by one. If it
# is the boot hart its status must be ignored (and said so) with every hart
# online. The first case is also compared across the two property orders.
check_fdt_cpu_status()
{
    local log="$1" where="$2" cpus="${CPUS:-1}" seen

    if grep -aq 'fdt: hart 1 is the boot hart, status=disabled ignored' "$log"; then
        echo "  INFO     ${MODE} (${where}) cpu@1 won the boot lottery; checking it stays online"
        require_log_patterns "$log" "cpu status (${where}, boot hart)" \
            "smp: ${cpus} harts online[[:space:]]*$" || return 1
        if grep -aq 'fdt: hart 1 withheld' "$log"; then
            echo "  FAIL     ${MODE} (${where}) withheld the boot hart"
            return 1
        fi
        return 0
    fi
    require_log_patterns "$log" "cpu status (${where})" \
        'fdt: hart 1 withheld \(status=disabled\)' \
        "smp: $((cpus - 1)) harts online[[:space:]]*$" || return 1
    if grep -aEq 'cpu_start_others: hart 1 (started|failed)' "$log"; then
        echo "  FAIL     ${MODE} (${where}) hart 1 was started despite status=disabled"
        grep -aEn 'cpu_start_others: hart 1' "$log"
        return 1
    fi
    seen="$(grep -aoE 'fdt: hart [0-9]+ withheld.*|smp: [0-9]+ harts online' "$log" |
            tr -d '\r' | sort)"
    if [ -n "$FDT_CPU_SEEN" ] && [ "$seen" != "$FDT_CPU_SEEN" ]; then
        echo "  FAIL     ${MODE} results differ between property orders"
        printf '%s\n--- vs ---\n%s\n' "$FDT_CPU_SEEN" "$seen"
        return 1
    fi
    FDT_CPU_SEEN="$seen"
}

mode_needs_lkl_ext4_seed()
{
    case "$MODE" in
        lkl-fsd-tree|lkl-fsd-tree-split|lkl-fsd-tree-isolated)
            return 0
            ;;
        lkl-multi|lkl-multi-crash|lkl-multi-stress|lkl-fsd-restart)
            return 0
            ;;
        shutdown-sync|shutdown-sync-negative|shutdown-sync-ignore)
            return 0
            ;;
    esac
    return 1
}

mode_needs_net_helpers()
{
    case "$MODE" in
        lkl-netd-http|lkl-netd-http-split|lkl-netd-http-isolated)
            return 0
            ;;
        lkl-multi|tracelogger-lkl-netd|tracelogger-lkl-netd-default)
            return 0
            ;;
    esac
    return 1
}

mode_needs_payload_helper()
{
    case "$MODE" in
        lkl-multi|lkl-multi-stress) return 0 ;;
    esac
    return 1
}

mode_allows_fs_diff()
{
    case "$MODE" in
        lkl-fsd-tree|lkl-multi|lkl-multi-stress) return 0 ;;
        shutdown-sync|shutdown-sync-negative|shutdown-sync-ignore) return 0 ;;
    esac
    return 1
}

mode_needs_lkl_fsd_archive()
{
    case "$MODE" in
        lkl-host-selftest|lkl-fsd-*|lkl-multi|lkl-multi-crash|lkl-multi-stress|tracelogger-lkl-fsd|shutdown-sync|shutdown-sync-negative|shutdown-sync-ignore)
            return 0
            ;;
    esac
    return 1
}

mode_needs_lkl_netd_archive()
{
    case "$MODE" in
        lkl-netd-*|lkl-multi|lkl-multi-crash|lkl-multi-stress|tracelogger-lkl-netd|tracelogger-lkl-netd-default)
            return 0
            ;;
    esac
    return 1
}

validate_lkl_personality_archive()
{
    local name="$1"
    local want_source="$2"
    local dir="$TOP_DIR/prebuilt/lkl-rv64/out-$name"
    local info="$dir/BUILD_INFO"
    local lib="$dir/liblkl-rtmux.a"
    local personality source ar_tool members

    if [ ! -f "$info" ]; then
        echo "qemu-regress: missing $info" >&2
        echo "qemu-regress: rebuild LKL with LKL_PERSONALITY=$name" >&2
        return 1
    fi
    personality="$(awk -F= '$1 == "personality" { print $2 }' "$info")"
    source="$(awk -F= '$1 == "source" { print $2 }' "$info")"
    if [ "$personality" != "$name" ] || [ "$source" != "$want_source" ]; then
        echo "qemu-regress: $dir is not the $name personality archive" >&2
        echo "qemu-regress: got personality=${personality:-unknown} source=${source:-unknown}" >&2
        echo "qemu-regress: want personality=$name source=$want_source" >&2
        echo "qemu-regress: rebuild with LKL_SRC=<lkl-tree> LKL_PERSONALITY=$name LKL_RTMUX_OUT=prebuilt/lkl-rv64/out-$name scripts/build-lkl-rv64.sh --build-from-source" >&2
        return 1
    fi
    if [ ! -f "$lib" ]; then
        echo "qemu-regress: missing $lib" >&2
        return 1
    fi
    ar_tool="$(resolve_tool "${LKL_AR:-}" riscv64-unknown-linux-gnu-ar \
        riscv64-linux-gnu-ar riscv-none-elf-ar llvm-ar ar)" || {
        echo "qemu-regress: need ar to verify $lib" >&2
        return 1
    }
    members="$("$ar_tool" t "$lib" | paste -sd+ -)"
    if [ "$members" != "$want_source" ]; then
        echo "qemu-regress: $dir archive members do not match BUILD_INFO" >&2
        echo "qemu-regress: got members=${members:-unknown}" >&2
        echo "qemu-regress: want members=$want_source" >&2
        return 1
    fi
    return 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --mode)
            MODE="$2"
            shift 2
            ;;
        --timeout)
            TIMEOUT="$2"
            TIMEOUT_SET=1
            shift 2
            ;;
        --repeat)
            REPEAT="$2"
            shift 2
            ;;
        --laps)
            LAPS="$2"
            shift 2
            ;;
        --keep-logs)
            KEEP_LOGS=1
            shift
            ;;
        --gdb-on-timeout)
            # Keep QEMU alive past the timeout with a live gdb stub, dump every
            # hart, then kill it. The log is kept so the dump sits beside it.
            GDB_ON_TIMEOUT=1
            KEEP_LOGS=1
            shift
            ;;
        --require-supported)
            REQUIRE_SUPPORTED=1
            shift
            ;;
        --stress)
            STRESS=1
            shift
            ;;
        --differential)
            DIFFERENTIAL=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        --)
            shift
            EXTRA_ARGS+=("$@")
            break
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

case "$LAPS" in
    "") ;;
    *[!0-9]* | 0)
        echo "qemu-regress: --laps needs a positive integer" >&2
        exit 2
        ;;
    *)
        if [ "$MODE" != "syscall-testing" ] && [ "$MODE" != "leak-gate" ]; then
            echo "qemu-regress: --laps is only valid with --mode syscall-testing or leak-gate" >&2
            exit 2
        fi
        ;;
esac

# leak-gate (opt-in) runs killspawn and then LAPS suite laps; default 3 laps.
if [ "$MODE" = "leak-gate" ]; then
    [ -n "$LAPS" ] || LAPS=3
    [ "$TIMEOUT_SET" -eq 1 ] || TIMEOUT=1800
fi

# --stress is a sustained-load variant of lkl-multi: it runs the cross-subsystem
# net->fs loop many times to pound the shared timer path under dual-LKL load.
# Model it as its own guest mode so it slots into the existing mode plumbing.
if [ "$STRESS" -eq 1 ]; then
    if [ "$MODE" != "lkl-multi" ]; then
        echo "qemu-regress: --stress is only valid with --mode lkl-multi" >&2
        exit 2
    fi
    MODE=lkl-multi-stress
fi

# PASS markers are fixed strings (grep -F) unless a mode sets PASS_GREP=-E for a
# regex (boot/pci-nvme-smoke's '^login:', sh-posix's anchored suite marker).
PASS_GREP=-F
case "$MODE" in
    syscall-testing|posix-cap-boundary|least-privilege-manifest|boot-manifest|loader-negative|rt-check|pi-inversion-xfail|rcu-lookup-measure|revoke-stress|smp-invariants|interactive-smoke|ipc-roundtrip|ipc-priority-donation|irq-notify|irq-notify-suppress|gpio-latency|fault-recovery|monotonic-time|rr-fairness|sporadic-budget|logger-restart|preempt-latency|mutex-wake|mutex-uncontended|ipc-contention|serial-unblock|shutdown-sync-wedge|leak-gate|spawn-storm)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        ;;
    debug-inject)
        # Forces CONFIG_DEBUG_INJECT=y, boots four harts, stalls hart 1 for
        # three seconds with interrupts off, and requires the guest to carry on
        # (scripts/qemu-regress.sh --mode debug-inject --gdb-on-timeout also
        # makes it a check of the hang dump).
        BEGIN_MARKER="RTMux debug-inject: BEGIN"
        PASS_MARKER="RTMux debug-inject: PASS"
        FAIL_MARKER="RTMux debug-inject: FAIL"
        MODE_ARG="rtmux.run=debug-inject"
        NEEDS_DEBUG_INJECT=1
        ;;
    hart-watchdog|hart-watchdog-console|hart-watchdog-idle)
        # Neighbour watchdog (Q18). hart-watchdog stalls hart 2 for three
        # seconds with interrupts off; hart-watchdog-idle only idles for a
        # minute; hart-watchdog-console also has the stalled hart hold the
        # console lock throughout, so the report must go out lock-free.
        # check_hartwatch_reports() counts the kernel's reports.
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        NEEDS_STALL_DETECTOR=1
        [ "$MODE" != "hart-watchdog-idle" ] && NEEDS_DEBUG_INJECT=1
        ;;
    syslog-wrap)
        # Q21: forces CONFIG_ROOTTASK_SYSLOG_SIZE=4096 so 200 numbered markers
        # wrap roottask's log ring; /sys/log must read back contiguously and
        # report EOVERFLOW for a window the ring has overwritten.
        BEGIN_MARKER="RTMux syslog-wrap: BEGIN"
        PASS_MARKER="RTMux syslog-wrap: PASS"
        FAIL_MARKER="RTMux syslog-wrap: FAIL"
        MODE_ARG="rtmux.run=syslog-wrap"
        NEEDS_SYSLOG_SMALL=1
        ;;
    roottask-wait-watch)
        # Roottask wait scan (Q20). slow-server holds two sync requests for 8 s
        # each; check_roottask_wait_reports() requires one "liveness:" line per
        # held sync, 5..7 s after it began, and none for RECEIVE.
        BEGIN_MARKER="RTMux roottask-wait-watch: BEGIN"
        PASS_MARKER="RTMux roottask-wait-watch: PASS"
        FAIL_MARKER="RTMux roottask-wait-watch: FAIL"
        MODE_ARG="rtmux.run=roottask-wait-watch"
        NEEDS_STALL_DETECTOR=1
        ;;
    hang-inject)
        # Hangs every hart but one with interrupts off, so the run never
        # finishes. Only meaningful with --gdb-on-timeout: the pass condition is
        # a complete dump with a register and backtrace block for every hart.
        BEGIN_MARKER="RTMux hang-inject: BEGIN"
        PASS_MARKER="RTMux hang-inject: PASS"
        FAIL_MARKER="RTMux hang-inject: FAIL"
        MODE_ARG="rtmux.run=hang-inject"
        NEEDS_DEBUG_INJECT=1
        if [ "$GDB_ON_TIMEOUT" -ne 1 ]; then
            echo "qemu-regress: --mode hang-inject needs --gdb-on-timeout" >&2
            exit 2
        fi
        ;;
    rt-check-instr)
        BEGIN_MARKER="RTMux rt-check: BEGIN"
        PASS_MARKER="RTMux rt-check: PASS"
        FAIL_MARKER="RTMux rt-check: FAIL"
        MODE_ARG="rtmux.run=rt-check"
        NEEDS_INSTRUMENTATION=1
        ;;
    trace-negative)
        # The trace payload cases (full-width user pointers, payload caps) only
        # run on an instrumented kernel; syscall-testing skips them on a stub.
        BEGIN_MARKER="RTMux trace-negative: BEGIN"
        PASS_MARKER="RTMux trace-negative: PASS"
        FAIL_MARKER="RTMux trace-negative: FAIL"
        MODE_ARG="rtmux.run=trace-negative"
        NEEDS_INSTRUMENTATION=1
        ;;
    tickless-idle)
        # Needs the instrumented build so the kernel emits the per-hart
        # [tickless] STIMER-count lines the oracle checks for a quiet hart.
        BEGIN_MARKER="RTMux tickless-idle: BEGIN"
        PASS_MARKER="RTMux tickless-idle: PASS"
        FAIL_MARKER="RTMux tickless-idle: FAIL"
        MODE_ARG="rtmux.run=tickless-idle"
        NEEDS_INSTRUMENTATION=1
        ;;
    sh-posix)
        # The init wrapper's "RTMux sh-posix: PASS" is derived from the suite's
        # exit status, which a bug that exits the script early can leave at 0 --
        # a silent false pass.  Require the suite's OWN completion marker, which
        # test.sh prints (unprefixed, at line start) only after every check ran
        # with no failures; the wrapper line keeps its "RTMux " prefix, so the
        # anchored regex ignores it.
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER='^sh-posix: PASS[[:space:]]*$'
        PASS_GREP=-E
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        ;;
    toolbox)
        # Same false-pass guard as sh-posix: require the suite's own unprefixed
        # completion marker, printed only after every applet check passed.
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER='^toolbox: PASS[[:space:]]*$'
        PASS_GREP=-E
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        ;;
    opts-pse51)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        ;;
    syscall-testing-disk)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        DISK_SUITE=1
        ;;
    lkl-host-selftest)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        NEEDS_LKL_SERVERS=1
        ;;
    lkl-contain)
        # Exercises the roottask MAP_PHYS containment gate directly (the
        # mechanism lkl-netd self-restricts with); needs no LKL server, no ext4,
        # no network — just a plain boot running the guest probe.
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    lkl-iommu-probe)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        setup_pci_iommu_or_skip
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    fdt-cpu-status)
        # Boots the irq-notify guest on a DTB with cpu@1 disabled; the host
        # drives both property orders (see the main loop) and checks the log.
        BEGIN_MARKER="RTMux irq-notify: BEGIN"
        PASS_MARKER="RTMux irq-notify: PASS"
        FAIL_MARKER="RTMux irq-notify: FAIL"
        MODE_ARG="rtmux.run=irq-notify"
        [ -n "${CPUS:-}" ] || CPUS=4
        export CPUS
        if [ "$CPUS" -lt 2 ]; then
            echo "qemu-regress: fdt-cpu-status needs CPUS >= 2" >&2
            exit 2
        fi
        ;;
    lkl-iommu-sys-probe)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        setup_system_iommu_or_skip
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    lkl-iommu-metadata)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        setup_system_iommu_metadata_or_skip
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    lkl-iommu-domain-preflight)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        setup_system_iommu_metadata_or_skip
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    lkl-netd-iommu-fail-closed)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        NEEDS_LKL_SERVERS=1
        setup_system_iommu_metadata_or_skip
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    lkl-iommu-domain|lkl-iommu-dma-deny|lkl-iommu-revoke)
        setup_system_iommu_or_skip
        setup_system_iommu_mmio_binding_or_skip
        echo "  UNSUPPORTED ${MODE}: RTMux system-IOMMU domain programming is not implemented"
        if [ "$REQUIRE_SUPPORTED" -eq 1 ]; then
            exit 2
        fi
        exit 0
        ;;
    tracelogger|tracelogger-kcall|tracelogger-irq|tracelogger-pi|tracelogger-lkl-fsd|tracelogger-lkl-netd|tracelogger-lkl-netd-default)
        # Drains the kernel instrumentation ring to userland (CTF-exporter first
        # increment); requires a CONFIG_INSTRUMENTATION=y build. Drop the "-T"
        # auto-trace flag so the boot dumper does not pre-own the singleton ring
        # buffer — the userland tracelogger allocates and owns it instead.
        export EMU_EXTRA_CMDLINE=""
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        NEEDS_INSTRUMENTATION=1
        if [ "$MODE" = "tracelogger-lkl-fsd" ] ||
           [ "$MODE" = "tracelogger-lkl-netd" ] ||
           [ "$MODE" = "tracelogger-lkl-netd-default" ]; then
            NEEDS_LKL_SERVERS=1
            [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=120
        else
            [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        fi
        ;;
    shutdown-sync|shutdown-sync-negative|shutdown-sync-ignore)
        # The guest leaves an un-fsynced sentinel on the LKL ext4 mount and
        # shuts down; the verdict is the host-side oracle (--differential).
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        NEEDS_LKL_SERVERS=1
        [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60
        ;;
    lkl-fsd-crash|lkl-fsd-tree|lkl-fsd-tree-split|lkl-fsd-tree-isolated|lkl-fsd-tricky|lkl-fsd-restart|lkl-netd-crash|lkl-netd-direct-denied|lkl-netd-http|lkl-netd-http-split|lkl-netd-http-isolated|lkl-netd-revoke|lkl-netd-restart|lkl-netd-stale|lkl-multi|lkl-multi-crash|lkl-multi-stress)
        BEGIN_MARKER="RTMux ${MODE}: BEGIN"
        PASS_MARKER="RTMux ${MODE}: PASS"
        FAIL_MARKER="RTMux ${MODE}: FAIL"
        MODE_ARG="rtmux.run=${MODE}"
        NEEDS_LKL_SERVERS=1
        case "$MODE" in
            # Single-server one-boot LKL modes are quick (60s).  The two-server
            # lkl-multi* modes and the *-restart modes (which boot two server
            # generations) keep the 180s default; the stress loop gets longer.
            lkl-multi|lkl-multi-crash|lkl-netd-restart|lkl-fsd-restart) : ;;
            lkl-multi-stress) [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=360 ;;
            *) [ "$TIMEOUT_SET" -eq 0 ] && TIMEOUT=60 ;;
        esac
        ;;
    version-skew)
        # Builds two trees with distinct build ids, boots one's kernel with the
        # other's initramfs, and requires exactly one "version skew:" line (and
        # none for a matching pair).  Self-contained: see check-version-skew.sh.
        if [ "$TIMEOUT_SET" -eq 1 ]; then
            exec "$SCRIPT_DIR/check-version-skew.sh" --timeout "$TIMEOUT"
        fi
        exec "$SCRIPT_DIR/check-version-skew.sh"
        ;;
    boot)
        BEGIN_MARKER='Mounting '
        PASS_MARKER='^login:'
        PASS_GREP=-E
        FAIL_MARKER='*** /usr unavailable'
        MODE_ARG=''
        ;;
    pci-nvme-smoke)
        BEGIN_MARKER='Probing for NVMe'
        PASS_MARKER='^login:'
        PASS_GREP=-E
        FAIL_MARKER='*** /usr unavailable'
        MODE_ARG=''
        NVME_SMOKE=1
        ;;
    *)
        echo "qemu-regress: unknown mode '$MODE'" >&2
        usage >&2
        exit 2
        ;;
esac

# rr-fairness pins its workers to CPU1, so it needs at least two harts. The
# default run is one vCPU, which has no CPU1 and would false-fail; bump the
# default when the caller did not set CPUS explicitly.
if { [ "$MODE" = "debug-inject" ] || [ "$MODE" = "hang-inject" ] ||
     [ "$MODE" = "hart-watchdog" ] || [ "$MODE" = "hart-watchdog-console" ] ||
     [ "$MODE" = "hart-watchdog-idle" ]; } && [ -z "${CPUS:-}" ]; then
    CPUS=4
    export CPUS
fi
if { [ "$MODE" = "rr-fairness" ] || [ "$MODE" = "sporadic-budget" ]; } && [ -z "${CPUS:-}" ]; then
    CPUS=2
    export CPUS
fi

# preempt-latency pins a waiter and hog to CPU0, a watchdog to CPU1, and the
# controller to CPU2, and the timer-wake gap it probes only appears with more
# than two harts. Default to four vCPUs when the caller did not set CPUS.
if [ "$MODE" = "preempt-latency" ] && [ -z "${CPUS:-}" ]; then
    CPUS=4
    export CPUS
fi

# mutex-wake puts the holder on cpu0, the high-priority waiter on cpu1, and the
# controller on cpu2, so it needs at least three harts. Default to four vCPUs
# when the caller did not set it. tracelogger-pi drives the same mutex-wake
# workload to generate priority-inheritance boosts, so it has the same floor.
if { [ "$MODE" = "mutex-wake" ] || [ "$MODE" = "tracelogger-pi" ]; } &&
   [ -z "${CPUS:-}" ]; then
    CPUS=4
    export CPUS
fi

# ipc-contention spreads the controller, server, low-priority pool, and high
# client across cpu0 through cpu3, so it needs at least four harts. Default to
# four vCPUs when the caller did not set it.
if [ "$MODE" = "ipc-contention" ] && [ -z "${CPUS:-}" ]; then
    CPUS=4
    export CPUS
fi

if [ "$MODE" = "smp-invariants" ] && [ -z "${CPUS:-}" ]; then
    CPUS=8
    export CPUS
fi

BAD_RE='FAULT|FATAL|CRASH|Kernel panic|panic:|lkl-host: panic|FREED-ACTIVE|__kernel_restore bad ptr|/usr unavailable|Undefined symbol|No such file or directory|Bad file descriptor'
if [ "$MODE" = "lkl-fsd-crash" ] || [ "$MODE" = "lkl-netd-crash" ] ||
   [ "$MODE" = "lkl-netd-direct-denied" ] ||
   [ "$MODE" = "lkl-netd-revoke" ] || [ "$MODE" = "lkl-multi-crash" ] ||
   [ "$MODE" = "lkl-netd-restart" ] || [ "$MODE" = "lkl-netd-stale" ] ||
   [ "$MODE" = "lkl-fsd-restart" ]; then
    BAD_RE='FATAL|CRASH|Kernel panic|panic:|lkl-host: panic|FREED-ACTIVE|__kernel_restore bad ptr|/usr unavailable|Undefined symbol|No such file or directory|Bad file descriptor'
fi

if [ "$NEEDS_LKL_SERVERS" -eq 1 ] || [ "$NEEDS_INSTRUMENTATION" -eq 1 ] ||
   [ "$NEEDS_DEBUG_INJECT" -eq 1 ] || [ "$NEEDS_STALL_DETECTOR" -eq 1 ] ||
   [ "$NEEDS_SYSLOG_SMALL" -eq 1 ]; then
    EMU_BUILD_DIR="${LKL_REGRESS_BUILD_DIR:-$TOP_DIR/build/lkl-regress}"
    IMAGE_DIR="$EMU_BUILD_DIR/images"
    NVME_IMG="$IMAGE_DIR/nvme.img"
    NVME_PRE_IMG="$IMAGE_DIR/nvme-pre.dat"
    NVME_P8_IMG="$IMAGE_DIR/nvme-p8.dat"
    set -e
    if [ -f .config ]; then
        HAD_CONFIG=1
    else
        RESTORE_CONFIG=1
        CONFIG_SAVE="$(mktemp /tmp/rtmux-config.XXXXXX)"
        AUTOCONF_SAVE="$(mktemp /tmp/rtmux-autoconf.XXXXXX)"
        if [ -f "$AUTOCONF_H" ]; then
            cp "$AUTOCONF_H" "$AUTOCONF_SAVE"
            HAD_AUTOCONF=1
        fi
        "${MAKE:-make}" defconfig
    fi

    if [ "$NEEDS_LKL_SERVERS" -eq 1 ] &&
       ! grep -q '^CONFIG_LKL_SERVERS=y$' .config; then
        if [ "$RESTORE_CONFIG" -eq 0 ]; then
            CONFIG_SAVE="$(mktemp /tmp/rtmux-config.XXXXXX)"
            AUTOCONF_SAVE="$(mktemp /tmp/rtmux-autoconf.XXXXXX)"
            cp .config "$CONFIG_SAVE"
            if [ -f "$AUTOCONF_H" ]; then
                cp "$AUTOCONF_H" "$AUTOCONF_SAVE"
                HAD_AUTOCONF=1
            fi
            RESTORE_CONFIG=1
        fi

        if grep -q '^CONFIG_LKL_SERVERS=' .config; then
            sed -i.bak 's/^CONFIG_LKL_SERVERS=.*/CONFIG_LKL_SERVERS=y/' .config
        elif grep -q '^# CONFIG_LKL_SERVERS is not set' .config; then
            sed -i.bak 's/^# CONFIG_LKL_SERVERS is not set/CONFIG_LKL_SERVERS=y/' .config
        else
            printf '\nCONFIG_LKL_SERVERS=y\n' >>.config
        fi
        rm -f .config.bak
    fi
    if [ "$NEEDS_INSTRUMENTATION" -eq 1 ] &&
       ! grep -q '^CONFIG_INSTRUMENTATION=y$' .config; then
        if [ "$RESTORE_CONFIG" -eq 0 ]; then
            CONFIG_SAVE="$(mktemp /tmp/rtmux-config.XXXXXX)"
            AUTOCONF_SAVE="$(mktemp /tmp/rtmux-autoconf.XXXXXX)"
            cp .config "$CONFIG_SAVE"
            if [ -f "$AUTOCONF_H" ]; then
                cp "$AUTOCONF_H" "$AUTOCONF_SAVE"
                HAD_AUTOCONF=1
            fi
            RESTORE_CONFIG=1
        fi

        if grep -q '^CONFIG_INSTRUMENTATION=' .config; then
            sed -i.bak 's/^CONFIG_INSTRUMENTATION=.*/CONFIG_INSTRUMENTATION=y/' .config
        elif grep -q '^# CONFIG_INSTRUMENTATION is not set' .config; then
            sed -i.bak 's/^# CONFIG_INSTRUMENTATION is not set/CONFIG_INSTRUMENTATION=y/' .config
        else
            printf '\nCONFIG_INSTRUMENTATION=y\n' >>.config
        fi
        rm -f .config.bak
    fi
    if [ "$NEEDS_DEBUG_INJECT" -eq 1 ] &&
       ! grep -q '^CONFIG_DEBUG_INJECT=y$' .config; then
        if [ "$RESTORE_CONFIG" -eq 0 ]; then
            CONFIG_SAVE="$(mktemp /tmp/rtmux-config.XXXXXX)"
            AUTOCONF_SAVE="$(mktemp /tmp/rtmux-autoconf.XXXXXX)"
            cp .config "$CONFIG_SAVE"
            if [ -f "$AUTOCONF_H" ]; then
                cp "$AUTOCONF_H" "$AUTOCONF_SAVE"
                HAD_AUTOCONF=1
            fi
            RESTORE_CONFIG=1
        fi

        if grep -q '^CONFIG_DEBUG_INJECT=' .config; then
            sed -i.bak 's/^CONFIG_DEBUG_INJECT=.*/CONFIG_DEBUG_INJECT=y/' .config
        elif grep -q '^# CONFIG_DEBUG_INJECT is not set' .config; then
            sed -i.bak 's/^# CONFIG_DEBUG_INJECT is not set/CONFIG_DEBUG_INJECT=y/' .config
        else
            printf '\nCONFIG_DEBUG_INJECT=y\n' >>.config
        fi
        rm -f .config.bak
    fi
    if [ "$NEEDS_STALL_DETECTOR" -eq 1 ] &&
       ! grep -q '^CONFIG_STALL_DETECTOR=y$' .config; then
        if [ "$RESTORE_CONFIG" -eq 0 ]; then
            CONFIG_SAVE="$(mktemp /tmp/rtmux-config.XXXXXX)"
            AUTOCONF_SAVE="$(mktemp /tmp/rtmux-autoconf.XXXXXX)"
            cp .config "$CONFIG_SAVE"
            if [ -f "$AUTOCONF_H" ]; then
                cp "$AUTOCONF_H" "$AUTOCONF_SAVE"
                HAD_AUTOCONF=1
            fi
            RESTORE_CONFIG=1
        fi

        if grep -q '^CONFIG_STALL_DETECTOR=' .config; then
            sed -i.bak 's/^CONFIG_STALL_DETECTOR=.*/CONFIG_STALL_DETECTOR=y/' .config
        elif grep -q '^# CONFIG_STALL_DETECTOR is not set' .config; then
            sed -i.bak 's/^# CONFIG_STALL_DETECTOR is not set/CONFIG_STALL_DETECTOR=y/' .config
        else
            printf '\nCONFIG_STALL_DETECTOR=y\n' >>.config
        fi
        rm -f .config.bak
    fi
    if [ "$NEEDS_SYSLOG_SMALL" -eq 1 ] &&
       ! grep -q '^CONFIG_ROOTTASK_SYSLOG_SIZE=4096$' .config; then
        if [ "$RESTORE_CONFIG" -eq 0 ]; then
            CONFIG_SAVE="$(mktemp /tmp/rtmux-config.XXXXXX)"
            AUTOCONF_SAVE="$(mktemp /tmp/rtmux-autoconf.XXXXXX)"
            cp .config "$CONFIG_SAVE"
            if [ -f "$AUTOCONF_H" ]; then
                cp "$AUTOCONF_H" "$AUTOCONF_SAVE"
                HAD_AUTOCONF=1
            fi
            RESTORE_CONFIG=1
        fi

        if grep -q '^CONFIG_ROOTTASK_SYSLOG_SIZE=' .config; then
            sed -i.bak 's/^CONFIG_ROOTTASK_SYSLOG_SIZE=.*/CONFIG_ROOTTASK_SYSLOG_SIZE=4096/' .config
        else
            printf '\nCONFIG_ROOTTASK_SYSLOG_SIZE=4096\n' >>.config
        fi
        rm -f .config.bak
    fi
    # Force regeneration: the sed edits above can leave .config no newer
    # than a stale autoconf.h, and make would then skip the rule.
    rm -f "$AUTOCONF_H"
    "${MAKE:-make}" "$AUTOCONF_H"
    # The default build/lkl-regress tree is shared by every forced-Kconfig mode
    # and the object rules there do not track headers or the config, so a tree
    # built for another option set or an older checkout silently yields a kernel
    # without the forced option (inject: "call refused: Function not
    # implemented"). Stamp the tree with a hash of the effective .config, the
    # commit, the tracked-file diff, and the contents of untracked (not ignored)
    # files, and start from nothing when the stamp differs: switching modes with
    # different options costs a full rebuild, repeating one costs none. A tree
    # named by LKL_REGRESS_BUILD_DIR is the caller's: it is never wiped here (the
    # Makefile's LKL smoke targets rebuild theirs from scratch themselves).
    if [ -z "${LKL_REGRESS_BUILD_DIR:-}" ] &&
       [ "${LKL_REGRESS_SKIP_BUILD:-0}" != 1 ]; then
        build_stamp="$({
            cat .config
            echo "lkl=$NEEDS_LKL_SERVERS instr=$NEEDS_INSTRUMENTATION"
            git -C "$TOP_DIR" rev-parse HEAD 2>/dev/null || echo nogit-$$-$RANDOM
            git -C "$TOP_DIR" diff HEAD 2>/dev/null
            (cd "$TOP_DIR" && git ls-files -z --others --exclude-standard 2>/dev/null |
                xargs -0 -r sha256sum 2>/dev/null)
        } | sha256sum | cut -d' ' -f1)"
        if [ "$(cat "$EMU_BUILD_DIR/.regress-stamp" 2>/dev/null)" != "$build_stamp" ]; then
            echo "  CLEAN    $EMU_BUILD_DIR (config or sources changed since its build)"
            rm -rf "$EMU_BUILD_DIR"
            mkdir -p "$EMU_BUILD_DIR"
            printf '%s\n' "$build_stamp" >"$EMU_BUILD_DIR/.regress-stamp"
        fi
    fi
    if mode_needs_lkl_ext4_seed; then
        LKL_EXT4_IMG="$EMU_BUILD_DIR/lkl-ext4.img"
        MKFS_EXT4_BIN="$(find_mkfs_ext4 || true)"
        if [ -z "$MKFS_EXT4_BIN" ]; then
            echo "qemu-regress: mkfs.ext4 not found; install e2fsprogs" >&2
            exit 2
        fi
        mkdir -p "$EMU_BUILD_DIR" "$IMAGE_DIR"
        rm -f "$LKL_EXT4_IMG"
        truncate -s 8M "$LKL_EXT4_IMG"
        rm -rf "$EMU_BUILD_DIR/lkl-ext4-root"
        mkdir -p "$EMU_BUILD_DIR/lkl-ext4-root"
        printf 'hello from lkl-fsd\n' >"$EMU_BUILD_DIR/lkl-ext4-root/hello.txt"
        "$MKFS_EXT4_BIN" -q -F -d "$EMU_BUILD_DIR/lkl-ext4-root" "$LKL_EXT4_IMG"
        rm -f "$NVME_IMG" "$NVME_PRE_IMG" "$NVME_P8_IMG"
    fi
    # lkl-fsd-tricky reverses the FS oracle: host e2fsprogs authors the ext4
    # (hardlink/sparse/big dir/perms) and lkl-fsd must read it back.
    if [ "$MODE" = "lkl-fsd-tricky" ]; then
        LKL_EXT4_IMG="$EMU_BUILD_DIR/lkl-tricky.img"
        mkdir -p "$EMU_BUILD_DIR" "$IMAGE_DIR"
        bash "$SCRIPT_DIR/lkl/author-tricky-ext4.sh" "$LKL_EXT4_IMG" 8388608
        rm -f "$NVME_IMG" "$NVME_PRE_IMG" "$NVME_P8_IMG"
    fi
    if mode_needs_lkl_fsd_archive; then
        validate_lkl_personality_archive fsd \
            'lkl.o+fs.o+iomem.o+jmp_buf.o+utils.o+virtio_blk.o+virtio.o' ||
            exit 1
    fi
    if mode_needs_lkl_netd_archive; then
        validate_lkl_personality_archive netd \
            'lkl.o+fs.o+iomem.o+net.o+jmp_buf.o+utils.o+virtio_blk.o+virtio.o+virtio_net.o' ||
            exit 1
    fi
    if [ "${LKL_REGRESS_SKIP_BUILD:-0}" = 1 ]; then
        test -f "$EMU_BUILD_DIR/initramfs.cpio" || {
            echo "qemu-regress: LKL_REGRESS_SKIP_BUILD=1 but $EMU_BUILD_DIR/initramfs.cpio is missing" >&2
            exit 2
        }
    else
        make_args=(OUT="$EMU_BUILD_DIR")
        [ "$NEEDS_LKL_SERVERS" -eq 1 ] &&
            make_args+=(CONFIG_LKL_SERVERS=y)
        [ "$NEEDS_INSTRUMENTATION" -eq 1 ] &&
            make_args+=(CONFIG_INSTRUMENTATION=y)
        if [ "$NEEDS_INSTRUMENTATION" -eq 1 ]; then
            "${MAKE:-make}" -B "${make_args[@]}" startup
            "${MAKE:-make}" "${make_args[@]}" initramfs
        else
            "${MAKE:-make}" "${make_args[@]}" startup initramfs
        fi
    fi
    set +e
fi

postprocess_trace_log()
{
    local log="$1"
    local ctf_dir="$log.ctf"
    local cfg_hash="unknown"
    local qemu_version="unknown"
    local git_sha="unknown"

    case "$MODE" in
        tracelogger|tracelogger-kcall|tracelogger-irq|tracelogger-pi|tracelogger-lkl-fsd|tracelogger-lkl-netd|tracelogger-lkl-netd-default) ;;
        *) return 0 ;;
    esac
    [ "$KEEP_LOGS" -eq 1 ] || return 0

    echo "  TRACE    ${log}.sched.csv"
    python3 scripts/rt-trace-sched.py "$log" >"${log}.sched.csv" || return 1

    if [ -f .config ]; then
        if command -v sha256sum >/dev/null 2>&1; then
            cfg_hash=$(sha256sum .config | cut -d' ' -f1)
        else
            cfg_hash=$(shasum -a 256 .config | cut -d' ' -f1)
        fi
        cfg_hash="sha256:$cfg_hash"
    fi
    qemu_version=$("${QEMU:-qemu-system-riscv64}" --version 2>/dev/null |
        sed -n '1s/^QEMU emulator version //p')
    qemu_version=${qemu_version:-unknown}
    git_sha=${RTMUX_GIT_SHA:-}
    if [ -z "$git_sha" ]; then
        git_sha=$(git rev-parse --short HEAD 2>/dev/null || printf unknown)
    fi
    # Seeded non-empty so `set -u` on bash 3.2 does not trip on the expansion.
    local budget_args
    budget_args=(--guest-log "$log")
    if [ "$MODE" = "tracelogger-lkl-fsd" ] ||
       [ "$MODE" = "tracelogger-lkl-netd" ] ||
       [ "$MODE" = "tracelogger-lkl-netd-default" ]; then
        # Generate the host-op wait CSV first so the budget step can turn its
        # rows into rt-budget blocks alongside the lkl-measure rows.
        echo "  TRACE    ${log}.lkl-host.csv"
        python3 scripts/rt-trace-lkl-host.py "$log" >"${log}.lkl-host.csv" ||
            return 1
        budget_args+=(--lkl-host "${log}.lkl-host.csv")
    elif [ "$MODE" = "tracelogger-kcall" ]; then
        # This mode exists to produce the per-kernel-call rows, so an empty
        # result is a failure, not an absence: without this the run stays green
        # when the kcall hook stops emitting.
        :
    else
        # tracelogger-irq and the plain CTF-exporter self-test legitimately have
        # nothing for the budget step to measure. tracelogger-pi lands here too
        # and does produce pi_boost rows, so it is allowed rather than required
        # to: its own gate is the boost-span block in docs/rt-budgets.md.
        budget_args+=(--allow-empty)
    fi
    # Every tracelogger mode gets a budget file: the LKL modes for their
    # lkl-measure rows, the rest for the per-kernel-call cost rows.
    # Platform contracts are chosen per row class by rt-trace-budget.py, not per
    # mode: one run can mix guest fs/net rows and kcall rows.  kcall
    # and pi_boost spans are timer-only and fixed in the script; the classes
    # below are named here, each with its own citation and no fallback, and a
    # row whose class is unknown makes the script fail.
    budget_args+=(--platform-fs "qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT"
                  --platform-net "qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED"
                  --platform-host "qemu-only PB-TIMER-RDTIME")
    echo "  TRACE    ${log}.rt-budget.md"
    python3 scripts/rt-trace-budget.py "${log}.sched.csv" \
        --command "scripts/qemu-regress.sh --mode $MODE --timeout $TIMEOUT --keep-logs" \
        --config-hash "$cfg_hash" \
        --target "qemu-system-riscv64 ${qemu_version} virt, ${CPUS:-1} vCPU, 256M, TCG multi-thread" \
        --opensbi "QEMU bundled default (-bios ${BIOS:-default})" \
        --kernel-git-sha "$git_sha" \
        "${budget_args[@]}" >"${log}.rt-budget.md" || return 1

    echo "  TRACE    ${log}.irq.csv"
    python3 scripts/rt-trace-irq.py "$log" >"${log}.irq.csv" || return 1

    rm -rf "$ctf_dir"
    echo "  TRACE    ${ctf_dir}"
    if ! python3 scripts/rt-trace-ctf.py "$log" -o "$ctf_dir" \
            >"${log}.ctf.log" 2>&1; then
        cat "${log}.ctf.log"
        return 1
    fi

    # Parse the real captured CTF with the same reader Trace Compass uses. The
    # exporter unit test only exercises a synthetic trace, so this is where a
    # malformation that only real event volume or shapes trigger gets caught.
    # Skip where babeltrace2 is absent; fail closed where it is present.
    if command -v babeltrace2 >/dev/null 2>&1; then
        echo "  TRACE    ${ctf_dir} (babeltrace2)"
        if ! babeltrace2 "$ctf_dir" >"${log}.ctf.bt.log" 2>&1; then
            echo "babeltrace2 failed to parse $ctf_dir" >&2
            cat "${log}.ctf.bt.log"
            return 1
        fi
        if [ ! -s "${log}.ctf.bt.log" ]; then
            echo "babeltrace2 parsed $ctf_dir but produced no events" >&2
            return 1
        fi
    fi
}

run_kernel()
{
    printf '%s\n' "${EMU_KERNEL:-${EMU_BUILD_DIR:-${BUILD_DIR:-$TOP_DIR/build}}/arch/riscv/rtmux-kernel}"
}

run_identity()
{
    local prefix="$1" kernel hash
    kernel=$(run_kernel)
    hash=$(sha256sum "$kernel" 2>/dev/null | cut -d' ' -f1)
    echo "${prefix}commit: $(git -C "$TOP_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "${prefix}dirty files: $(git -C "$TOP_DIR" status --porcelain 2>/dev/null | wc -l)"
    echo "${prefix}kernel sha256: ${hash:-unavailable}"
    echo "${prefix}cpus: ${CPUS:-default}"
}

# dump_hang_state LOG PORT: attach to the live gdb stub and write every hart's
# state to LOG's .dump sibling, headed by the build identity. A gdb failure is
# written into the dump as such: an incomplete dump is never passed off as a
# complete one.
dump_hang_state()
{
    local log="$1" port="$2"
    local dump="${log%.log}.dump"
    local kernel
    kernel=$(run_kernel)
    local gdb="${GDB:-gdb-multiarch}"

    {
        echo "# hang dump for ${MODE} after ${TIMEOUT}s"
        run_identity '# '
    } >"$dump"
    echo "  HANG     ${MODE}: no pass marker after ${TIMEOUT}s; dumping harts to ${dump}"
    if "$TIMEOUT_CMD" "${HANG_DUMP_TIMEOUT:-120}" "$gdb" -batch -ex "target remote :${port}" \
        -x "$TOP_DIR/scripts/gdb/dump-state.gdb" "$kernel" >>"$dump" 2>&1 &&
       grep -q "DUMP COMPLETE" "$dump"; then
        echo "# result: complete" >>"$dump"
        # User-mode return addresses (roottask is a fixed-VA image at
        # 0xc0000000) do not resolve in the kernel ELF; symbolize them here.
        local roottask="${EMU_BUILD_DIR:-${BUILD_DIR:-$TOP_DIR/build}}/servers/roottask/roottask"
        local a2l="${ADDR2LINE:-${CROSS_COMPILE:-riscv-none-elf-}addr2line}"
        if [ -f "$roottask" ] && command -v "$a2l" >/dev/null 2>&1; then
            echo "# roottask symbols for \$ra/\$sepc values at 0xc0000000+:" >>"$dump"
            grep -aoE '^\$(ra|sepc) = (0xc[0-9a-f]{7}|[0-9]{10})' "$dump" |
                while read -r _ _ v; do
                    case "$v" in 0x*) ;; *) v=$(printf '0x%x' "$v") ;; esac
                    printf '#   %s -> %s\n' "$v" \
                        "$("$a2l" -f -e "$roottask" "$v" 2>/dev/null | paste -sd' ')" >>"$dump"
                done
        fi
    else
        echo "# result: GDB FAILED or timed out; this dump is INCOMPLETE" >>"$dump"
        echo "  HANG     ${MODE}: gdb dump failed or incomplete (see ${dump})"
    fi
}

# check_hartwatch_reports LOG: neighbour-watchdog verdict. hart-watchdog needs
# exactly one report, naming the injected hart, at 2000..2500 ms of silence (the stall
# lasts 3 s, and the report must land within 2.5 s of the last progress);
# hart-watchdog-idle needs none. Reports may carry a "[n] " console prefix, so
# the match is not anchored.
check_hartwatch_reports()
{
    local log="$1" n line ms hart
    n="$(grep -c 'hartwatch: hart ' "$log" || true)"
    if [ "$MODE" = "hart-watchdog-idle" ]; then
        if [ "$n" -ne 0 ]; then
            echo "  FAIL     ${MODE}: $n unexpected hartwatch report(s) while idle"
            grep 'hartwatch: hart ' "$log" | head -n 5
            return 1
        fi
        return 0
    fi
    if [ "$n" -ne 1 ]; then
        echo "  FAIL     ${MODE}: want exactly one hartwatch report, got $n"
        grep 'hartwatch: hart ' "$log" | head -n 5
        return 1
    fi
    line="$(grep 'hartwatch: hart ' "$log")"
    hart="$(sed -n "s/.*RTMux ${MODE}: INJECT hart=\([0-9][0-9]*\).*/\1/p" "$log" | head -n 1)"
    ms="$(printf '%s\n' "$line" |
          sed -n "s/.*hartwatch: hart ${hart:-x} silent for \([0-9][0-9]*\) ms.*/\1/p")"
    if [ -z "$hart" ] || [ -z "$ms" ] || [ "$ms" -lt 2000 ] || [ "$ms" -gt 2500 ]; then
        echo "  FAIL     ${MODE}: report does not name injected hart '${hart}' at 2000..2500 ms: $line"
        return 1
    fi
    if [ "$MODE" = "hart-watchdog-console" ]; then
        # The report must land while the stalled hart still owns the console
        # lock: after the "held" line and before the "released" one.
        local held rep rel clean
        clean="$(tr -d '\r' <"$log")"
        held="$(printf '%s\n' "$clean" | grep -n 'inject: console lock held' | head -n 1 | cut -d: -f1)"
        rep="$(printf '%s\n' "$clean" | grep -n 'hartwatch: hart ' | head -n 1 | cut -d: -f1)"
        rel="$(printf '%s\n' "$clean" | grep -n 'inject: console lock released' | head -n 1 | cut -d: -f1)"
        if [ -z "$held" ] || [ -z "$rep" ] || [ -z "$rel" ] ||
           [ "$held" -ge "$rep" ] || [ "$rep" -ge "$rel" ]; then
            echo "  FAIL     ${MODE}: report not printed during the console-lock hold (held@${held:-none} report@${rep:-none} released@${rel:-none})"
            return 1
        fi
    fi
    return 0
}

# check_roottask_wait_reports LOG: roottask wait-scan verdict. Each held
# sync (the fixture prints "slow-server: WITHHOLD n" as it starts one) must
# produce exactly one "liveness: pid 1 tid <n> REPLY <ms>ms chid=<c>" line with
# the wait between 5000 and 7000 ms, and no liveness line may name a thread in
# RECV. Other states (an idle roottask worker parked on a condvar, say) are
# listed but do not decide the verdict. Lines may carry a "[n] " console
# prefix, so the match is not anchored.
check_roottask_wait_reports()
{
    local log="$1" held recv ms bad=0 n w l next
    held="$(grep -c 'slow-server: WITHHOLD ' "$log" || true)"
    recv="$(grep -c 'liveness: .* RECV ' "$log" || true)"
    if [ "$held" -ne 2 ]; then
        echo "  FAIL     ${MODE}: fixture held $held syncs, want 2"
        return 1
    fi
    # Each liveness line must fall between its own WITHHOLD line and the next
    # one (or the end of the log), exactly once.
    for n in 1 2; do
        w="$(grep -En "slow-server: WITHHOLD $n[[:space:]]*\$" "$log" | head -n 1 | cut -d: -f1)"
        next="$(grep -En "slow-server: WITHHOLD $((n + 1))[[:space:]]*\$" "$log" | head -n 1 | cut -d: -f1)"
        next="${next:-999999999}"
        l="$(awk -v a="$w" -v b="$next" \
            'NR > a && NR < b && /liveness: pid 1 tid [0-9]+ REPLY [0-9]+ms chid=[0-9]+/' "$log")"
        if [ "$(printf '%s\n' "$l" | grep -c .)" -ne 1 ]; then
            echo "  FAIL     ${MODE}: want one REPLY liveness line after WITHHOLD $n, got:"
            printf '%s\n' "$l"
            return 1
        fi
        ms="$(printf '%s\n' "$l" | sed -n 's/.* REPLY \([0-9]*\)ms .*/\1/p')"
        if [ "$ms" -lt 5000 ] || [ "$ms" -gt 7000 ]; then
            echo "  FAIL     ${MODE}: wait of ${ms} ms is outside 5000..7000"
            bad=1
        fi
    done
    if [ "$recv" -ne 0 ]; then
        echo "  FAIL     ${MODE}: $recv liveness line(s) name a receive-blocked thread"
        bad=1
    fi
    grep 'liveness: ' "$log" | grep -v ' REPLY ' | head -n 5 | sed 's/^/  note     /'
    return "$bad"
}

# check_hang_inject_dump DUMP: the injected hang must have left a complete dump
# with a register block and a backtrace for every hart the guest has.
check_hang_inject_dump()
{
    local dump="$1" want="${CPUS:-1}" got bt
    got="$(grep -ac '^=== hart ' "$dump")"
    bt="$(grep -ac '^#0 ' "$dump")"
    if grep -q '^# result: complete' "$dump" && [ "$got" -eq "$want" ] &&
       [ "$bt" -ge "$want" ] && grep -q "^DUMP COMPLETE: ${want} harts" "$dump"; then
        return 0
    fi
    echo "  FAIL     hang-inject: dump has $got hart blocks and $bt backtraces, want $want of each"
    return 1
}

run_one()
{
    local idx="$1"
    local log rc result
    local cmd
    local mounted_line login_line
    local pass_seen=0
    local pass_grace=20

    # Kept logs go to a stable, findable path so follow-on post-processing
    # (differential checks, artifact collection) can name them deterministically;
    # ephemeral runs stay in a throwaway mktemp file.
    if [ "$KEEP_LOGS" -eq 1 ]; then
        log="${QEMU_REGRESS_LOG_DIR:-$TOP_DIR/build/lkl-logs}/${MODE}-${idx}.log"
        mkdir -p "${log%/*}" || return 1
        : > "$log" || return 1
    else
        log="$(mktemp "/tmp/rtmux-${MODE}-${idx}.XXXXXX")"
    fi
    LAST_LOG="$log"
    echo "  QEMU     ${MODE} run ${idx}/${REPEAT} (log: ${log})"
    if [ -n "${QEMU_REGRESS_LOG_DIR:-}" ]; then
        run_identity '' >"${log%.log}.identity"
    fi

    rc=0
    if [ -n "$EMU_BUILD_DIR" ]; then
        cmd=("${QEMU_HEADLESS_ENV[@]}" BUILD_DIR="$EMU_BUILD_DIR")
        if [ -n "$LKL_EXT4_IMG" ]; then
            cmd+=(LKL_EXT4_IMG="$LKL_EXT4_IMG")
        fi
        cmd+=(./scripts/emu.sh)
    elif [ "$DISK_SUITE" -eq 1 ]; then
        cmd=("${QEMU_HEADLESS_ENV[@]}" EMU_ROMFS_TESTS=1 ./scripts/emu.sh)
    else
        cmd=("${QEMU_HEADLESS_ENV[@]}" ./scripts/emu.sh)
    fi
    if [ -n "$MODE_ARG" ]; then
        cmd+=("$MODE_ARG")
    fi
    if [ -n "$LAPS" ]; then
        cmd+=("rtmux.laps=${LAPS}")
    fi
    if [ "${#EXTRA_ARGS[@]}" -gt 0 ]; then
        cmd+=("${EXTRA_ARGS[@]}")
    fi
    local wrap_timeout="$TIMEOUT"
    local gdb_port=
    local ticks=0 hung=0 hang_ok=0
    if [ "$GDB_ON_TIMEOUT" -eq 1 ]; then
        # Unique per process and run, so concurrent harness loops do not collide.
        gdb_port=$((20000 + ($$ % 20000) + idx))
        cmd+=("-gdb-live=${gdb_port}")
        wrap_timeout=$((TIMEOUT + ${HANG_DUMP_TIMEOUT:-120} + 20)) # headroom for the dump
    fi
    # serial-unblock feeds one byte to the guest UART (QEMU's stdio) once the
    # guest prints "serial-unblock: WAITING"; every other mode reads nothing.
    local stdin_src=/dev/null serial_fifo= serial_fd= serial_sent=0
    if [ "$MODE" = "serial-unblock" ]; then
        serial_fifo="$(mktemp -u "/tmp/rtmux-serial-unblock-fifo.XXXXXX")"
        mkfifo "$serial_fifo" || return 1
        exec {serial_fd}<>"$serial_fifo"
        stdin_src="$serial_fifo"
    fi
    "$TIMEOUT_CMD" "$wrap_timeout" "${cmd[@]}" <"$stdin_src" >"$log" 2>&1 &
    local runner=$!
    local terminal=0
    while kill -0 "$runner" 2>/dev/null; do
        if grep -Fq "$FAIL_MARKER" "$log" || grep -Eq "$BAD_RE" "$log"; then
            terminal=1
            kill "$runner" 2>/dev/null || true
            break
        fi
        if [ -n "$serial_fd" ] && [ "$serial_sent" -eq 0 ] &&
           grep -Fq 'serial-unblock: WAITING' "$log"; then
            printf x >&"$serial_fd"
            serial_sent=1
        fi
        if [ "$pass_seen" -eq 0 ] &&
           grep -q "$PASS_GREP" "$PASS_MARKER" "$log"; then
            pass_seen=1
        fi
        if [ "$pass_seen" -eq 1 ]; then
            if [ "$pass_grace" -le 0 ]; then
                terminal=1
                kill "$runner" 2>/dev/null || true
                break
            fi
            pass_grace=$((pass_grace - 1))
            sleep 1
            continue
        fi
        if [ "$terminal" -eq 1 ]; then
            kill "$runner" 2>/dev/null || true
            break
        fi
        sleep 1
        ticks=$((ticks + 1))
        if [ "$GDB_ON_TIMEOUT" -eq 1 ] && [ "$ticks" -ge "$TIMEOUT" ]; then
            dump_hang_state "$log" "$gdb_port"
            kill "$runner" 2>/dev/null || true
            hung=1
            if [ "$MODE" = "hang-inject" ]; then
                if check_hang_inject_dump "${log%.log}.dump"; then
                    hang_ok=1
                    echo "RTMux hang-inject: PASS" >>"$log"
                fi
            fi
            break
        fi
    done
    wait "$runner" 2>/dev/null || rc=$?
    if [ -n "$serial_fd" ]; then
        exec {serial_fd}>&-
        rm -f "$serial_fifo"
    fi
    if [ "$hung" -eq 1 ] && [ "$hang_ok" -eq 1 ]; then
        rc=0 # the injected hang was the test, and its dump is complete
    elif [ "$hung" -eq 1 ]; then
        rc=124
    elif [ "$terminal" -eq 1 ] || [ "$pass_seen" -eq 1 ]; then
        rc=0
    fi

    # boot and pci-nvme-smoke never exit the guest (they idle at login:), so a
    # timeout (rc=124) is expected once the pass marker is in the log; tolerate it
    # for both and fall through to the login-in-log check below.
    if [ "$rc" -ne 0 ] &&
       { { [ "$MODE" != "boot" ] && [ "$NVME_SMOKE" -ne 1 ]; } ||
         [ "$rc" -ne 124 ]; }; then
        if [ "$rc" -eq 124 ]; then
            echo "  FAIL     ${MODE} timed out after ${TIMEOUT}s"
        else
            echo "  FAIL     ${MODE} exited rc=${rc}"
        fi
        tail -n 120 "$log"
        return 1
    fi

    if ! grep -Fq "$BEGIN_MARKER" "$log"; then
        echo "  FAIL     ${MODE} missing begin marker: ${BEGIN_MARKER}"
        tail -n 120 "$log"
        return 1
    fi

    if grep -Eq "$BAD_RE" "$log"; then
        echo "  FAIL     ${MODE} boot log contains crash/fault signature"
        grep -En "$BAD_RE" "$log" | tail -n 20
        tail -n 160 "$log"
        return 1
    fi

    if grep -Fq "$FAIL_MARKER" "$log"; then
        echo "  FAIL     ${MODE} reported guest failure"
        tail -n 160 "$log"
        return 1
    fi

    if ! grep -q "$PASS_GREP" "$PASS_MARKER" "$log"; then
        echo "  FAIL     ${MODE} missing pass marker: ${PASS_MARKER}"
        tail -n 160 "$log"
        return 1
    fi

    if [ -n "$LAPS" ]; then
        local lap
        for lap in $(seq 1 "$LAPS"); do
            if ! grep -Eq "^lap=${lap} PASS\r?$" "$log"; then
                echo "  FAIL     ${MODE} missing lap line: lap=${lap} PASS"
                tail -n 160 "$log"
                return 1
            fi
        done
    fi

    # lkl-multi-crash proves lkl-fsd survives an lkl-netd crash. The absence of
    # /net alone would also hold if lkl-netd merely exited cleanly, so require
    # the crash path's own marker before trusting the isolation result.
    if [ "$MODE" = "lkl-multi-crash" ] &&
       ! grep -Fq 'lkl-netd: intentional crash after start' "$log"; then
        echo "  FAIL     ${MODE} lkl-netd did not log its intentional crash"
        tail -n 160 "$log"
        return 1
    fi

    if [ "$MODE" = "lkl-netd-direct-denied" ] &&
       ! grep -Fq 'lkl-netd: lkl_start_kernel returned' "$log"; then
        echo "  FAIL     ${MODE} did not boot lkl-netd directly"
        tail -n 160 "$log"
        return 1
    fi

    # The *-restart modes pass only if the supervisor brought up a *restarted*
    # generation -- "gen 1 up" proves the full crash -> cap-reclaim -> respawn ->
    # republish cycle ran, not just that generation 0 came up.
    case "$MODE" in
        lkl-netd-restart | lkl-fsd-restart)
            if ! grep -Fq 'lkl-supervisor: gen 1 up' "$log"; then
                echo "  FAIL     ${MODE} supervisor did not bring up a restarted generation"
                tail -n 160 "$log"
                return 1
            fi
            ;;
    esac

    if [ "$MODE" = "boot" ] || [ "$NVME_SMOKE" -eq 1 ]; then
        if grep -q 'login:.*romfs:' "$log"; then
            echo "  FAIL     ${MODE} log interleaves login prompt with romfs mount output"
            grep -n 'login:.*romfs:' "$log" | tail -n 5
            return 1
        fi
        mounted_line="$(grep -n 'romfs: mounted romfs at /usr' "$log" |
                        head -n 1 | cut -d: -f1)"
        login_line="$(grep -n '^login:' "$log" | head -n 1 | cut -d: -f1)"
        if [ -z "$mounted_line" ] || [ -z "$login_line" ] ||
           [ "$login_line" -le "$mounted_line" ]; then
            echo "  FAIL     ${MODE} reached login before /usr mount completion was logged"
            tail -n 160 "$log"
            return 1
        fi
    fi

    # PCI/NVMe bring-up smoke: the boot reaches login with /usr mounted before it
    # (checked above), and every stage of PCI enumeration + NVMe driver attach
    # left its diagnostic in the log. Covers ECAM discovery, device attach + BAR0
    # mapping (sizing), device identity, block I/O (GPT parse), IRQ/MSI vector
    # allocation, and that mainfs= actually selected the NVMe p8 device. Each
    # pattern is a short substring that survives console interleaving — the SBI
    # console splits long writes, so a single line like "controller at <bdf> BAR0
    # 0x<addr>" can arrive as two, hence the separate "controller at" and "BAR0
    # 0x" checks rather than one combined regex.
    if [ "$NVME_SMOKE" -eq 1 ]; then
        for re in \
            'pci-host-ecam-generic' \
            'dev-nvme: controller at ' \
            'BAR0 0x' \
            'nvme: controller VID:DID' \
            'GPT OK, [0-9]+ partition' \
            'IRQ=[0-9]+ vector=[0-9]+' \
            'dev=/dev/nvme0n1p8'; do
            if ! grep -Eq "$re" "$log"; then
                echo "  FAIL     ${MODE} missing PCI/NVMe diagnostic: ${re}"
                tail -n 160 "$log"
                return 1
            fi
        done
    fi

    if [ "$MODE" = "lkl-netd-revoke" ]; then
        if ! grep -Eq 'lkl: virtio-net crash after bounced tx' "$log"; then
            echo "  FAIL     ${MODE} did not crash after virtio-net TX"
            tail -n 160 "$log"
            return 1
        fi
    fi

    if [ "$MODE" = "lkl-fsd-restart" ]; then
        if ! grep -Eq 'lkl: block crash after host copy' "$log"; then
            echo "  FAIL     ${MODE} did not crash after an LKL block copy"
            tail -n 160 "$log"
            return 1
        fi
    fi

    if [ "$MODE" = "hart-watchdog" ] || [ "$MODE" = "hart-watchdog-console" ] ||
       [ "$MODE" = "hart-watchdog-idle" ]; then
        check_hartwatch_reports "$log" || return 1
    fi

    if [ "$MODE" = "roottask-wait-watch" ]; then
        check_roottask_wait_reports "$log" || return 1
    fi

    if [ "$MODE" = "logger-restart" ]; then
        if ! grep -Eq '^logger-restart: PASS first=[0-9]+ second=[0-9]+' "$log"; then
            echo "  FAIL     ${MODE} did not prove logger respawn"
            tail -n 160 "$log"
            return 1
        fi
    fi

    if [ "$MODE" = "fault-recovery" ]; then
        require_log_patterns_in_order "$log" "fault recovery probes" \
            '^  PASS  fault-recovery illegal_syscall[[:space:]]*$' \
            '^fault-event event_id=illegal-syscall severity=deny subject=self cause=unsupported-private-call action=return-EPERM authority_delta=none[[:space:]]*$' \
            '^  PASS  fault-recovery invalid_capability[[:space:]]*$' \
            '^fault-event event_id=invalid-capability severity=deny subject=self cause=malformed-cap action=return-EBADF authority_delta=none[[:space:]]*$' \
            '^  PASS  fault-recovery user_page_fault[[:space:]]*$' \
            '^fault-event event_id=user-page-fault severity=thread-fatal subject=self cause=--fault-sigsegv action=reported-signal authority_delta=none[[:space:]]*$' \
            '^  PASS  fault-recovery readonly_write_fault[[:space:]]*$' || return 1
        require_log_patterns "$log" "fault recovery readonly event" \
            '^fault-event event_id=user-page-fault severity=thread-fatal subject=self cause=--fault-mprotect-ro action=reported-signal authority_delta=none[[:space:]]*$' || return 1
    fi

    if [ "$MODE" = "rcu-lookup-measure" ]; then
        require_log_patterns_in_order "$log" "RCU lookup measurements" \
            '^rcu-lookup-measure: measure-overhead samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: cap-attach-detach samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: msgsend-cap-roundtrip samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: msgsend-coid-roundtrip samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: connect-server-info-self samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: connect-server-info-pid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: connect-flags-self samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: connect-flags-pid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-current-thread samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-info-self samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-info-pid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-pid-tid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-set-self samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-set-pid-tid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: sched-get-set-pid-tid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: timer-info-pid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: timer-info-self samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: clock-id-self samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: clock-id-pid-tid samples=1000 batch=64 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-join-post-store-self samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-stats label=thread-join-post-store-self calls=1000 rcu-wait-ns=[1-9][0-9]* pin-wait-ns=[1-9][0-9]* active-scan-ns=[1-9][0-9]* object-free-ns=[1-9][0-9]* object-free-heap=[0-9][0-9]* object-free-cache=[0-9][0-9]* rcu-waited=[0-9][0-9]* rcu-skipped=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-object-free-ns label=thread-join-post-store-self object-free-heap-ns=[0-9][0-9]* object-free-cache-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-object-free-cache label=thread-join-post-store-self clear-ns=[0-9][0-9]* link-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-pin label=thread-join-post-store-self pin-waited=[0-9][0-9]* pin-skipped=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-ns label=thread-join-post-store-self rcu-waited-ns=[0-9][0-9]* rcu-skipped-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-detail label=thread-join-post-store-self enroll-ns=[0-9][0-9]* scan-ns=[0-9][0-9]* post-scan-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-peers label=thread-join-post-store-self busy-peer-sum=[0-9][0-9]* busy-peer-max=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-busy label=thread-join-post-store-self thread=[0-9][0-9]* msg=[0-9][0-9]* sys=[0-9][0-9]* channel=[0-9][0-9]* connect=[0-9][0-9]* interrupt=[0-9][0-9]* sync=[0-9][0-9]* sched=[0-9][0-9]* time=[0-9][0-9]* idle=[0-9][0-9]* dispatch=[0-9][0-9]* timer=[0-9][0-9]* fault=[0-9][0-9]* priv=[0-9][0-9]* heartbeat=[0-9][0-9]* unstamped=[0-9][0-9]* other=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-priv label=thread-join-post-store-self system=[0-9][0-9]* vm=[0-9][0-9]* ipc=[0-9][0-9]* process=[0-9][0-9]* thread=[0-9][0-9]* pa=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-priv-pa label=thread-join-post-store-self alloc=[0-9][0-9]* free=[0-9][0-9]* zero=[0-9][0-9]* info=[0-9][0-9]* xlat=[0-9][0-9]* pq=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-claim label=thread-join-post-store-self active=[0-9][0-9]* reserved=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-join-wake label=thread-join-post-store-self count=[0-9][0-9]* ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-join-post-ready label=thread-join-post-store-self count=[0-9][0-9]* ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-post-ready-detail label=thread-join-post-store-self vector-ns=[0-9][0-9]* cpu-ns=[0-9][0-9]* name-ns=[0-9][0-9]* notify-ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-post-ready-residual label=thread-join-post-store-self named-ns=[0-9][0-9]* residual-ns=[0-9][0-9]* notify-ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-join-post-store-pid samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-stats label=thread-join-post-store-pid calls=1000 rcu-wait-ns=[1-9][0-9]* pin-wait-ns=[1-9][0-9]* active-scan-ns=[1-9][0-9]* object-free-ns=[1-9][0-9]* object-free-heap=[0-9][0-9]* object-free-cache=[0-9][0-9]* rcu-waited=[0-9][0-9]* rcu-skipped=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-object-free-ns label=thread-join-post-store-pid object-free-heap-ns=[0-9][0-9]* object-free-cache-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-object-free-cache label=thread-join-post-store-pid clear-ns=[0-9][0-9]* link-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-pin label=thread-join-post-store-pid pin-waited=[0-9][0-9]* pin-skipped=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-ns label=thread-join-post-store-pid rcu-waited-ns=[0-9][0-9]* rcu-skipped-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-detail label=thread-join-post-store-pid enroll-ns=[0-9][0-9]* scan-ns=[0-9][0-9]* post-scan-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-peers label=thread-join-post-store-pid busy-peer-sum=[0-9][0-9]* busy-peer-max=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-busy label=thread-join-post-store-pid thread=[0-9][0-9]* msg=[0-9][0-9]* sys=[0-9][0-9]* channel=[0-9][0-9]* connect=[0-9][0-9]* interrupt=[0-9][0-9]* sync=[0-9][0-9]* sched=[0-9][0-9]* time=[0-9][0-9]* idle=[0-9][0-9]* dispatch=[0-9][0-9]* timer=[0-9][0-9]* fault=[0-9][0-9]* priv=[0-9][0-9]* heartbeat=[0-9][0-9]* unstamped=[0-9][0-9]* other=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-priv label=thread-join-post-store-pid system=[0-9][0-9]* vm=[0-9][0-9]* ipc=[0-9][0-9]* process=[0-9][0-9]* thread=[0-9][0-9]* pa=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-rcu-priv-pa label=thread-join-post-store-pid alloc=[0-9][0-9]* free=[0-9][0-9]* zero=[0-9][0-9]* info=[0-9][0-9]* xlat=[0-9][0-9]* pq=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-destroy-claim label=thread-join-post-store-pid active=[0-9][0-9]* reserved=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-join-wake label=thread-join-post-store-pid count=[0-9][0-9]* ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-join-post-ready label=thread-join-post-store-pid count=[0-9][0-9]* ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-post-ready-detail label=thread-join-post-store-pid vector-ns=[0-9][0-9]* cpu-ns=[0-9][0-9]* name-ns=[0-9][0-9]* notify-ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-destroy-post-ready-residual label=thread-join-post-store-pid named-ns=[0-9][0-9]* residual-ns=[0-9][0-9]* notify-ns=[0-9][0-9]*[[:space:]]*$' \
	            '^rcu-lookup-measure: thread-create-return-self samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-create-return-pid samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-create-self samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-create-pid samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-create-join-self samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' \
            '^rcu-lookup-measure: thread-create-join-pid samples=1000 batch=1 min-ns=[0-9][0-9]* p50-ns=[0-9][0-9]* p99-ns=[0-9][0-9]* max-ns=[0-9][0-9]*[[:space:]]*$' || return 1
    fi

    if [ "$MODE" = "revoke-stress" ]; then
        require_log_patterns "$log" "endpoint revoke stress measurements" \
            '^revoke-stress: endpoint-detach samples=' \
            '^revoke-stress: unrelated-ipc samples=' || return 1
    fi

    if [ "$MODE" = "smp-invariants" ]; then
        require_log_patterns_in_order "$log" "SMP invariant smoke" \
            '^lock-rank-stress: AB_workers=' \
            '^rcu-lookup-measure: measure-overhead samples=100 ' \
            '^revoke-stress: PASS[[:space:]]*$' \
            '^preempt-latency: PASS ' \
            '^ipc-contention: PASS ' || return 1
    fi

    if [ "$MODE" = "lkl-netd-iommu-fail-closed" ]; then
        require_log_patterns_in_order "$log" "LKL IOMMU fail-closed handoff" \
            '^lkl: virtio-mmio IOMMU phandle=[1-9][0-9]* stream=42[[:space:]]*$' \
            '^lkl: system IOMMU MMIO @ 0x[0-9a-f]+ irq [1-9][0-9]* caps=0x[0-9a-f]+ ddtp=0x[0-9a-f]+ mode=[01] delegated[[:space:]]*$' \
            '^lkl: virtio-mmio IOMMU domain range 0 paddr=0x[0-9a-f]+ size=0x[0-9a-f]+[[:space:]]*$' \
            '^lkl: virtio-mmio IOMMU domain allowlist ranges=[1-9][0-9]* bytes=[1-9][0-9]*[[:space:]]*$' \
            '^lkl: virtio-mmio IOMMU domain programmed stream=42 allow=0x[0-9a-f]+ deny=0x[0-9a-f]+[[:space:]]*$' \
            '^lkl: virtio-mmio IOMMU domain cleared stream=42[[:space:]]*$' \
            '^lkl: virtio-mmio IOMMU domain enforcement unavailable[[:space:]]*$' || return 1
    fi

    if [ "$MODE" = "lkl-iommu-domain-preflight" ]; then
        require_log_patterns_in_order "$log" "LKL synthetic IOMMU domain preflight" \
            '^lkl-contain: virtio-net IOMMU metadata preserved[[:space:]]*$' \
            '^lkl-iommu-probe: Bare device context installed for DID 42[[:space:]]*$' \
            '^lkl-iommu-probe: translated IOVA 0x400000 resp=0x[0-9a-f]+ deny=0x[0-9a-f]+[[:space:]]*$' \
            '^lkl-iommu-probe: IOTINVAL.GVMA post-clear resp=0x[0-9a-f]+[[:space:]]*$' || return 1
    fi

    if [ "$MODE" = "tickless-idle" ]; then
        # Two conditions over the [tickless] lines emitted DURING the probe's
        # quiet phase (bounded by the window markers so a not-yet-active AP at
        # boot or an incidental low count at shutdown is ignored):
        #   1. No hart may take 500 or more STIMER interrupts in any report
        #      window. The periodic tick is CONFIG_TIMER_HZ (1000/s here) and a
        #      hart that fails to disarm spins far higher (about 4000/s was seen
        #      on the reference host before SIE_STIE masking), so a 500 ceiling catches both a
        #      lost tickless transition on any single hart and a spinning hart.
        #   2. At least one hart must drop below 100/s, proving the tick is truly
        #      gone and not merely reduced.
        # Checking every hart, not just the first quiet one, is deliberate: a
        # weaker one-hart check passed even while three harts spun at 4000/s.
        if ! awk '
            /tickless-idle: quiet window .* begins/ { win = 1 }
            /tickless-idle: quiet window done/      { win = 0 }
            win && match($0, /\[tickless\] cpu=[0-9]+ stimer=[0-9]+\/s/) {
                cnt = $0; sub(/.*stimer=/, "", cnt); sub(/\/s.*/, "", cnt)
                seen = 1
                if (cnt + 0 >= 500) spun = 1
                if (cnt + 0 < 100) low = 1
            }
            END { exit (seen && low && !spun) ? 0 : 1 }
        ' "$log"; then
            echo "  FAIL     ${MODE} a hart spun (>=500/s) or none went tickless"
            tail -n 160 "$log"
            return 1
        fi
    fi

    if [ "$MODE" = "irq-notify" ]; then
        require_log_patterns "$log" "IRQ attempt outcomes" \
            '^irq-notify: seed=[0-9]+ ' \
            '^irq-notify: outcomes ok=100 early=0 timeout=0 attempts=100 suppress_one=0[[:space:]]*$' || return 1
    fi

    if [ "$MODE" = "tracelogger-irq" ]; then
        require_log_patterns "$log" "IRQ attempt outcomes" \
            '^irq-latency: seed=[0-9]+ ' \
            '^irq-latency: outcomes ok=50 early=0 timeout=0 attempts=50[[:space:]]*$' || return 1
    fi

    if [ "$MODE" = "irq-notify-suppress" ]; then
        # Exactly one timeout, and it is the suppressed attempt; the guest has
        # already checked the 1.0-1.1 s window and prints the elapsed time.
        require_log_patterns "$log" "suppressed IRQ attempt" \
            '^irq-notify: seed=[0-9]+ ' \
            '^irq-notify: suppress-one attempt=[0-9]+[[:space:]]*$' \
            'attempt path=(chan|ist) n=[0-9]+ .* outcome=timeout suppressed ' \
            '^irq-notify: outcomes ok=99 early=0 timeout=1 attempts=100 suppress_one=1[[:space:]]*$' \
            '^irq-notify: suppress-one timeout=1 elapsed_ns=1[01][0-9]{8} ' || return 1
    fi

    require_sync_log_oracle "$log" || return 1
    require_lkl_mode_oracle "$log" || return 1

    result="$(grep -E '=== Results:|RTMux .*: PASS|login:' "$log" | tail -n 1)"
    echo "  PASS     ${result:-$PASS_MARKER}"
    postprocess_trace_log "$log" || return 1
    # fdt-cpu-status inspects the log in the main loop, which removes it.
    if [ "$KEEP_LOGS" -eq 0 ] && [ "$MODE" != "fdt-cpu-status" ] &&
       { [ "$DIFFERENTIAL" -eq 0 ] || [ "$NET_LOG_DIFF" -eq 0 ]; }; then
        rm -f "$log"
    fi
}

if mode_needs_net_helpers; then
    python3 -m http.server 8000 --bind 127.0.0.1 >/tmp/rtmux-lkl-http.log 2>&1 &
    http_pid=$!
    python3 - <<'PY' >/tmp/rtmux-lkl-udp.log 2>&1 &
import socket

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 8001))
while True:
    data, addr = s.recvfrom(2048)
    s.sendto(b"udp:" + data, addr)
PY
    udp_pid=$!
    python3 - <<'PY' >/tmp/rtmux-lkl-dns.log 2>&1 &
import socket

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 8002))
while True:
    data, addr = s.recvfrom(512)
    name_end = data.find(b"\x00", 12) + 1
    q = data[12:name_end + 4]
    answer = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\x0a\x00\x02\x02"
    resp = data[:2] + b"\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00" + q + answer
    s.sendto(resp, addr)
PY
    dns_pid=$!
    sleep 1
    if ! kill -0 "$http_pid" 2>/dev/null; then
        echo "qemu-regress: failed to start HTTP helper on 127.0.0.1:8000" >&2
        cat /tmp/rtmux-lkl-http.log >&2
        exit 1
    fi
    if ! kill -0 "$udp_pid" 2>/dev/null; then
        echo "qemu-regress: failed to start UDP helper on 127.0.0.1:8001" >&2
        cat /tmp/rtmux-lkl-udp.log >&2
        exit 1
    fi
    if ! kill -0 "$dns_pid" 2>/dev/null; then
        echo "qemu-regress: failed to start DNS helper on 127.0.0.1:8002" >&2
        cat /tmp/rtmux-lkl-dns.log >&2
        exit 1
    fi
fi

# lkl-multi's cross-subsystem test (lkl-multi-test) fetches a fixed payload over
# TCP, so it needs a raw-TCP payload server the http.server helper does not
# provide. Serve exactly the 0..255 ramp the guest regenerates as its oracle.
# lkl-multi-stress runs the same test in a loop, so it needs the server too.
if mode_needs_payload_helper; then
    python3 - <<'PY' >/tmp/rtmux-lkl-payload.log 2>&1 &
import socket

payload = bytes(i & 0xff for i in range(8192))
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 8003))
s.listen(4)
while True:
    conn, _ = s.accept()
    conn.sendall(payload)
    conn.close()
PY
    payload_pid=$!
    sleep 1
    if ! kill -0 "$payload_pid" 2>/dev/null; then
        echo "qemu-regress: failed to start payload helper on 127.0.0.1:8003" >&2
        cat /tmp/rtmux-lkl-payload.log >&2
        exit 1
    fi
fi

# Rebuild the dedicated /usr image each run so it carries the just-built suite,
# not a stale cached copy. emu.sh regenerates it (with EMU_ROMFS_TESTS=1) when
# absent; the normal dev nvme.img is untouched. Derive the image dir exactly as
# emu.sh does (from BUILD_DIR) so a non-default OUT=/BUILD_DIR tree is honored.
if [ "$DISK_SUITE" -eq 1 ]; then
    disk_img_dir="${BUILD_DIR:-$TOP_DIR/build}/images"
    echo "  IMAGE    regenerating $disk_img_dir/nvme-tests.img with the suite on /usr"
    rm -f "$disk_img_dir/nvme-tests.img" \
          "$disk_img_dir/nvme-tests-pre.dat" \
          "$disk_img_dir/nvme-tests-p8.dat"
fi

# The differential oracle (opt-in) runs post-boot e2fsprogs checks for
# fs-producing modes, and a virtio-net log/stat oracle for lkl-netd-http.
if [ "$DIFFERENTIAL" -eq 1 ]; then
    case "$MODE" in
        lkl-netd-http) NET_LOG_DIFF=1 ;;
        *)
            mode_allows_fs_diff || {
                echo "qemu-regress: --differential has no oracle for mode '$MODE'" >&2
                exit 2
            }
            ;;
    esac
fi

i=1
FDT_CPU_BASE_OPTS="${EMU_QEMU_OPTS:-}"
while [ "$i" -le "$REPEAT" ]; do
    if [ "$MODE" = "fdt-cpu-status" ]; then
        FDT_CPU_SEEN=
        for placement in first last; do
            setup_fdt_cpu_status_or_skip "$placement" "$FDT_CPU_BASE_OPTS"
            run_one "$i" || exit 1
            check_fdt_cpu_status "$LAST_LOG" "status-$placement" || exit 1
            [ "$KEEP_LOGS" -eq 1 ] || rm -f "$LAST_LOG"
        done
        i=$((i + 1))
        continue
    fi
    run_one "$i" || exit 1
    if [ "$DIFFERENTIAL" -eq 1 ]; then
        if [ "$NET_LOG_DIFF" -eq 1 ]; then
            differential_net_check || exit 1
            [ "$KEEP_LOGS" -eq 1 ] || rm -f "$LAST_LOG"
        elif [ "$MODE" = "shutdown-sync" ] ||
             [ "$MODE" = "shutdown-sync-negative" ] ||
             [ "$MODE" = "shutdown-sync-ignore" ]; then
            differential_sync_check || exit 1
        else
            differential_fs_check || exit 1
        fi
    fi
    i=$((i + 1))
done
