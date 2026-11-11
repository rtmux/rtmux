#!/bin/sh
set -eu

for f in docs/lkl-domain.md docs/capability-namespace-feasibility.md \
         docs/vfs-contract.md docs/rt-budgets.md; do
    test -f "$f" || {
        echo "LKL gate: missing $f" >&2
        exit 1
    }
done

for s in lkl_init lkl_start_kernel lkl_cleanup lkl_syscall lkl_strerror \
         lkl_disk_add lkl_mount_dev lkl_host_ops lkl_dev_blk_ops \
         lkl_netdev_add; do
    grep -q "$s" docs/lkl-domain.md || {
        echo "LKL gate: docs/lkl-domain.md missing $s" >&2
        exit 1
    }
done

grep -q 'single serialized LKL entry' docs/lkl-domain.md || {
    echo "LKL gate: serialized entry rule missing" >&2
    exit 1
}

grep -q 'system-IOMMU MMIO binding' docs/lkl-domain.md || {
    echo "LKL gate: IOMMU skip note must cover PCI and system-IOMMU preflights" >&2
    exit 1
}

grep -q 'probes which legal' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must name DDT mode probing" >&2
    exit 1
}

grep -q 'lkl-iommu-sys-probe.*register and retained-DDT-mode probing' docs/lkl-domain.md &&
grep -q 'lkl-iommu-domain-preflight.*command-queue, debug-translation, Sv39x4 allow/deny, and post-clear fault probing' docs/lkl-domain.md || {
    echo "LKL gate: platform inventory must split sys and domain preflight claims" >&2
    exit 1
}

grep -q 'zero-payload `IOFENCE.C`' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must name the system-IOMMU MMIO preflight" >&2
    exit 1
}

grep -q 'temporary Bare/Bare/Off' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must name Bare device-context probing" >&2
    exit 1
}

grep -q 'debug translation request' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must name debug translation probing" >&2
    exit 1
}

grep -q 'same numeric value' docs/lkl-domain.md &&
grep -q 'metadata check, decodes' docs/lkl-domain.md &&
grep -q 'the delegated virtio stream from the returned handle' docs/lkl-domain.md &&
grep -q 'with the observed stream' docs/lkl-domain.md &&
grep -q 'passed as `--did`' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must not overclaim DID 42 binding" >&2
    exit 1
}

grep -q 'synthetic Sv39x4 second-stage context' docs/lkl-domain.md &&
grep -q '16 KiB-aligned root' docs/lkl-domain.md &&
grep -q 'neighboring unmapped page' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must name translated allow/deny probing" >&2
    exit 1
}

grep -q 'IOTINVAL.GVMA' docs/lkl-domain.md &&
grep -q 'stale-IOTLB invalidation proof' docs/lkl-domain.md &&
grep -q 'does not populate' docs/lkl-domain.md || {
    echo "LKL gate: optional LKL QEMU docs must name translated post-clear probing" >&2
    exit 1
}

awk '
    /^LKL_QEMU_MODES[[:space:]]*=/ { in_list = 1 }
    in_list && /^[[:space:]]*lkl-iommu-sys-probe:60[[:space:]]*\\[[:space:]]*$/ { sys = 1 }
    in_list && /^[[:space:]]*lkl-iommu-metadata:60[[:space:]]*\\[[:space:]]*$/ { metadata = 1 }
    in_list && /^[[:space:]]*lkl-iommu-domain-preflight:60[[:space:]]*\\[[:space:]]*$/ { domain = 1 }
    in_list && /^[[:space:]]*lkl-netd-iommu-fail-closed:60[[:space:]]*\\[[:space:]]*$/ { failclosed = 1 }
    in_list && /^[^[:space:]].*=/ && $0 !~ /^LKL_QEMU_MODES[[:space:]]*=/ { exit }
    END { exit (sys && metadata && domain && failclosed) ? 0 : 1 }
' Makefile || {
    echo "LKL gate: LKL_QEMU_MODES must include system-IOMMU metadata preflights" >&2
    exit 1
}

awk '
    /^LKL_QEMU_MODES[[:space:]]*=/ { in_list = 1 }
    in_list && /^[^[:space:]].*=/ && $0 !~ /^LKL_QEMU_MODES[[:space:]]*=/ { exit }
    in_list && /lkl-iommu-(domain|dma-deny|revoke):/ { bad = 1 }
    END { exit bad ? 1 : 0 }
' Makefile || {
    echo "LKL gate: live IOMMU enforcement modes must stay out of portable LKL_QEMU_MODES" >&2
    exit 1
}

grep -q 'RTMux must program it before claiming' docs/lkl-domain.md || {
    echo "LKL gate: IOMMU contract must not overclaim current DMA containment" >&2
    exit 1
}

grep -q 'build/lkl-logs/<mode>-iommu.{dtb,dts,err}' docs/lkl-domain.md || {
    echo "LKL gate: IOMMU docs must name kept DTB/DTS/stderr artifacts" >&2
    exit 1
}

for s in '"iommus"' 'cells[0]' 'cells[1]' '"iommu-phandle=%u"' \
         '"iommu-stream=%u"' '"fdt-phandle=%u"'; do
    grep -Fq "$s" arch/riscv/startup/arch-init.c || {
        echo "LKL gate: startup must preserve MMIO virtio IOMMU stream metadata $s" >&2
        exit 1
    }
done

for s in '"iommu-phandle="' '"iommu-stream="' \
         RTMUX_RESOURCE_IOMMU_PHANDLE_NONE \
         RTMUX_RESOURCE_IOMMU_STREAM_ENCODE \
         'h->iommu_phandle == RTMUX_RESOURCE_IOMMU_PHANDLE_NONE'; do
    grep -Fq "$s" servers/virtio-mmio/main.c || {
        echo "LKL gate: virtio-mmio must encode hwinfo IOMMU stream metadata $s" >&2
        exit 1
    }
done

for s in iommu_phandle RTMUX_RESOURCE_IOMMU_STREAM_NONE \
         RTMUX_RESOURCE_IOMMU_STREAM_DECODE \
         RTMUX_DEVICE_RISCV_IOMMU \
         '<sys/riscv-iommu.h>' RISCV_IOMMU_CAPABILITIES RISCV_IOMMU_DDTP \
         RISCV_IOMMU_VERSION_1_0 RISCV_IOMMU_MODE_MASK \
         'system IOMMU MMIO @ %#lx irq %d caps=0x%lx ddtp=0x%lx' \
         'virtio_mmio_require_iommu_domain' \
         'virtio-mmio IOMMU domain range %u paddr=%#lx size=%#lx' \
         'virtio-mmio IOMMU domain allowlist ranges=' \
         'virtio-mmio IOMMU domain programmed stream=%u' \
         'virtio-mmio IOMMU domain cleared stream=%u' \
         'IOMMU domain enforcement unavailable' 'return ENOTSUP'; do
    grep -Fq "$s" servers/lkl/virtio.c || {
        echo "LKL gate: LKL virtio transport must decode delegated IOMMU stream metadata $s" >&2
        exit 1
    }
done

for s in 'VIRTNET_IOMMU_RANGE_MAX' \
         'virtnet_require_iommu_domain' \
         'virtnet_add_dma_range' \
         'rtnd->rx_pool.buf[i].paddr' \
         'rtnd->tx_pool.buf[i].paddr'; do
    grep -Fq "$s" servers/lkl/host.c || {
        echo "LKL gate: LKL virtio IOMMU DMA allowlist path missing $s" >&2
        exit 1
    }
done

for s in RTMUX_DEVICE_RISCV_IOMMU '"system IOMMU"'; do
    grep -Fq "$s" servers/lkl-netd/main.c || {
        echo "LKL gate: lkl-netd must delegate system-IOMMU control handles $s" >&2
        exit 1
    }
done

for s in '#define RISCV_IOMMU_CAPABILITIES 0x00U' \
         '#define RISCV_IOMMU_DDTP         0x10U' \
         '#define RISCV_IOMMU_CQB          0x18U' \
         '#define RISCV_IOMMU_CQH          0x20U' \
         '#define RISCV_IOMMU_CQT          0x24U' \
         '#define RISCV_IOMMU_CQCSR        0x48U' \
         '#define RISCV_IOMMU_TR_REQ_IOVA  0x258U' \
         '#define RISCV_IOMMU_TR_REQ_CTL   0x260U' \
         '#define RISCV_IOMMU_TR_RESPONSE  0x268U' \
         '#define RISCV_IOMMU_CMD_IOTINVAL 1U' \
         '#define RISCV_IOMMU_CMD_IOFENCE  2U' \
         '#define RISCV_IOMMU_CMD_IODIR    3U' \
         '#define RISCV_IOMMU_DID_SHIFT    40U' \
         '#define RISCV_IOMMU_CMD_IOTINVAL_FUNC_GVMA 1U' \
         '#define RISCV_IOMMU_CMD_IOTINVAL_AV (1ULL << 10)' \
         '#define RISCV_IOMMU_CMD_IOTINVAL_GV (1ULL << 33)' \
         '#define RISCV_IOMMU_CMD_IODIR_DV (1ULL << 33)' \
         '#define RISCV_IOMMU_DC_TC_V      1ULL' \
         '#define RISCV_IOMMU_CAP_DBG      (1ULL << 31)' \
         '#define RISCV_IOMMU_CAP_SV39X4   (1ULL << 17)' \
         '#define RISCV_IOMMU_CQCSR_CQEN (1U << 0)' \
         '#define RISCV_IOMMU_CQCSR_CQON (1U << 16)' \
         '#define RISCV_IOMMU_CQCSR_BUSY (1U << 17)' \
         '#define RISCV_IOMMU_CQCSR_ERR  ((1U << 8) | (1U << 9) | (1U << 10))' \
         '#define RISCV_IOMMU_CMD_FUNC_SHIFT 7U' \
         '#define RISCV_IOMMU_DDTP_PPN_MASK (((1ULL << 54) - 1ULL) & ~0x3ffULL)' \
         '#define RISCV_IOMMU_DDTP_MODE_OFF 0U' \
         '#define RISCV_IOMMU_DDTP_MODE_BARE 1U' \
         '#define RISCV_IOMMU_IOHGATP_MODE_SV39X4 8ULL' \
         '#define RISCV_IOMMU_PTE_V        1ULL' \
         '#define RISCV_IOMMU_PTE_R        (1ULL << 1)' \
         '#define RISCV_IOMMU_PTE_W        (1ULL << 2)' \
         '#define RISCV_IOMMU_PTE_U        (1ULL << 4)' \
         '#define RISCV_IOMMU_PTE_A        (1ULL << 6)' \
         '#define RISCV_IOMMU_PTE_D        (1ULL << 7)' \
         '#define RISCV_IOMMU_TR_REQ_GO    1ULL' \
         '#define RISCV_IOMMU_TR_REQ_NW    (1ULL << 3)' \
         '#define RISCV_IOMMU_TR_RESPONSE_FAULT 1ULL' \
         '#define RISCV_IOMMU_TR_RESPONSE_PPN_MASK' \
         '#define RISCV_IOMMU_DDTP_BUSY    (1ULL << 4)' \
         '#define RISCV_IOMMU_DDTP_MODE_1LVL 2U' \
         '#define RISCV_IOMMU_DDTP_MODE_2LVL 3U' \
         '#define RISCV_IOMMU_DDTP_MODE_3LVL 4U' \
         '#define RISCV_IOMMU_VERSION_1_0  0x10U' \
         '#define RISCV_IOMMU_CQ_BYTES   4096U' \
         '#define RISCV_IOMMU_DDT_BYTES  4096U' \
         '#define RISCV_IOMMU_ROOT_BYTES (4U * 4096U)' \
         'struct riscv_iommu_cmd' 'struct riscv_iommu_dc' \
         'riscv_iommu_iodir_inval_ddt_cmd' \
         'riscv_iommu_cmd_word(RISCV_IOMMU_CMD_IODIR, 0) |' \
         'RISCV_IOMMU_CMD_IODIR_DV |' \
         '((uint64_t) did << RISCV_IOMMU_DID_SHIFT)' \
         'riscv_iommu_iofence_cmd' \
         'riscv_iommu_cmd_word(RISCV_IOMMU_CMD_IOFENCE, 0)' \
         'riscv_iommu_iotinval_gvma_cmd' \
         'RISCV_IOMMU_CMD_IOTINVAL_FUNC_GVMA' \
         'RISCV_IOMMU_CMD_IOTINVAL_AV |' \
         'RISCV_IOMMU_CMD_IOTINVAL_GV' \
         '.dword1 = iova >> 2' \
         'riscv_iommu_pte' \
         'return ((pa >> 12) << 10) | flags;'; do
    grep -Fq "$s" internal/include/sys/riscv-iommu.h || {
        echo "LKL gate: shared RISC-V IOMMU header missing $s" >&2
        exit 1
    }
done

grep -Fq '<sys/riscv-iommu.h>' tests/userland/rt-latency/lkl-iommu-probe.c || {
    echo "LKL gate: system-IOMMU probe must use shared RISC-V IOMMU constants" >&2
    exit 1
}

for s in '#define RISCV_IOMMU_PT_BYTES     (9U * 4096U)' \
         '#define RISCV_IOMMU_TEST_IOVA    0x400000ULL' \
         '#define RISCV_IOMMU_TEST_DID     42U' \
         '#define RISCV_IOMMU_MAX_TEST_IOVA' \
         mmio_read64 mmio_write64 mem_offset wait_tr_done tr_busy \
         debug_translate \
         'tr_busy = true' \
         'RISCV_IOMMU_TR_RESPONSE_PPN_MASK) !=' \
         '!tr_busy && ddt_programmed' '!tr_busy && cq_enabled' \
         'parse_u64_arg' '"--did"' '"--iova"' \
         'explicit_domain_arg = true' \
         'RISCV_IOMMU_MAX_TEST_IOVA' \
         'lkl-iommu-probe: --did/--iova require --domain' \
         'sizeof(struct riscv_iommu_dc)' \
         'dc[test_did]' \
         'root_off =' 'RISCV_IOMMU_ROOT_BYTES - 1U' \
         '~(uint64_t) (RISCV_IOMMU_ROOT_BYTES - 1U)' \
         'root_pa = pt_pa + (off_t) root_off' \
         'RISCV_IOMMU_IOHGATP_MODE_SV39X4 << 60' \
         'test_iova + 4096' \
         'did << RISCV_IOMMU_DID_SHIFT' \
         'riscv_iommu_pte' \
         'riscv_iommu_iodir_inval_ddt_cmd(test_did)' \
         'riscv_iommu_iofence_cmd()' \
         'riscv_iommu_iotinval_gvma_cmd(test_iova)' \
         '"IODIR.INVAL_DDT"' '"IOFENCE.C"' \
         '"translated IODIR.INVAL_DDT"' '"translated IOFENCE.C"' \
         '"IOTINVAL.GVMA"' '"post-clear IOFENCE.C"' \
         'debug_translate(mmio, test_did' \
         'IOTINVAL.GVMA post-clear resp=' \
         'DDT modes mask=' \
         'Bare device context installed for DID %u' 'IODIR.INVAL_DDT fetched' \
         'debug translation resp=' 'translated IOVA %#lx resp=%#lx deny=%#lx' \
         'IOFENCE.C completed'; do
    grep -Fq "$s" tests/userland/rt-latency/lkl-iommu-probe.c || {
        echo "LKL gate: system-IOMMU probe missing $s" >&2
        exit 1
    }
done

if grep -Fq 'revoke IOFENCE.C' tests/userland/rt-latency/lkl-iommu-probe.c; then
    echo "LKL gate: system-IOMMU probe must not call post-clear probing revoke" >&2
    exit 1
fi

grep -Fq 'IOMMU metadata lost' tests/userland/rt-latency/lkl-contain-test.c || {
    echo "LKL gate: lkl-contain must check IOMMU stream metadata preservation" >&2
    exit 1
}

grep -Fq -- '--virtio-iommu-metadata' tests/userland/rt-latency/lkl-contain-test.c || {
    echo "LKL gate: lkl-contain must expose the boot-DTB IOMMU metadata fixture" >&2
    exit 1
}

grep -Fq 'lkl-netd-iommu-fail-closed' userland/init || {
    echo "LKL gate: init must verify LKL virtio fail-closed IOMMU handoff" >&2
    exit 1
}

grep -Fq 'lkl-iommu-domain-preflight' userland/init || {
    echo "LKL gate: init must verify synthetic IOMMU domain preflight" >&2
    exit 1
}
grep -Fq -- '--virtio-iommu-domain-preflight' tests/userland/rt-latency/lkl-contain-test.c &&
grep -Fq -- '--virtio-iommu-domain-preflight' userland/init || {
    echo "LKL gate: synthetic IOMMU domain preflight must use observed stream helper" >&2
    exit 1
}
for s in 'RTMUX_RESOURCE_IOMMU_STREAM_DECODE' \
         'stream != 42' \
         'snprintf(did, sizeof(did), "%u", stream)' \
         '"/bin/lkl-iommu-probe"' '"--domain"' '"--did"' '"--iova"' '"0x400000"'; do
    grep -Fq "$s" tests/userland/rt-latency/lkl-contain-test.c || {
        echo "LKL gate: synthetic IOMMU domain preflight missing $s" >&2
        exit 1
    }
done

if grep -Fq '/bin/lkl-iommu-probe --domain --did 42 --iova 0x400000' userland/init; then
    echo "LKL gate: synthetic IOMMU domain preflight must not duplicate fixture DID in init" >&2
    exit 1
fi

grep -Fq 'LKL IOMMU fail-closed handoff' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require LKL IOMMU fail-closed evidence" >&2
    exit 1
}

grep -Fq 'IOMMU domain allowlist ranges=' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require IOMMU DMA allowlist evidence" >&2
    exit 1
}

grep -Fq 'IOMMU domain range 0 paddr=' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require IOMMU DMA range evidence" >&2
    exit 1
}

grep -Fq 'IOMMU domain programmed stream=42' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require IOMMU domain programming evidence" >&2
    exit 1
}

grep -Fq 'IOMMU domain cleared stream=42' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require IOMMU domain cleanup evidence" >&2
    exit 1
}

grep -Fq 'LKL synthetic IOMMU domain preflight' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require synthetic IOMMU domain evidence" >&2
    exit 1
}

grep -Fq 'virtio-net IOMMU metadata preserved' scripts/qemu-regress.sh || {
    echo "LKL gate: synthetic IOMMU domain preflight must require metadata evidence" >&2
    exit 1
}

grep -Fq 'caps=0x[0-9a-f]+ ddtp=0x[0-9a-f]+ mode=[01]' scripts/qemu-regress.sh || {
    echo "LKL gate: qemu-regress must require delegated system-IOMMU evidence" >&2
    exit 1
}

for s in iommu-phandle-without-stream iommu-stream-without-phandle; do
    grep -Fq "$s" tests/userland/rt-latency/lkl-contain-test.c || {
        echo "LKL gate: lkl-contain must reject inconsistent IOMMU metadata $s" >&2
        exit 1
    }
done

grep -q 'keep_system_iommu_dtb_artifacts "$dtb" "$dts" "$dts.err"' \
    scripts/qemu-regress.sh || {
    echo "LKL gate: IOMMU preflight must keep DTB/DTS artifacts" >&2
    exit 1
}

bad=$(find kernel lib userland servers -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name Makefile \) \
    -exec grep -nE 'lkl_(init|start_kernel|cleanup|syscall|sys_[a-z0-9_]+|disk_add|mount_dev)|lkl_host_ops|lkl_dev_blk_ops' {} + |
    grep -vE 'servers/lkl(/|-fsd/|-netd/)|docs/' || true)
if test -n "$bad"; then
    echo "LKL gate: direct LKL entry outside LKL domain prototype" >&2
    echo "$bad" >&2
    exit 1
fi

bad=$(find servers/lkl servers/lkl-fsd servers/lkl-netd -type f \
    \( -name '*.c' -o -name '*.h' \) \
    ! -path 'servers/lkl/virtio.c' \
    -exec grep -n 'MAP_PHYS' {} + || true)
if test -n "$bad"; then
    echo "LKL gate: MAP_PHYS must stay behind servers/lkl/virtio.c" >&2
    echo "$bad" >&2
    exit 1
fi

for f in servers/lkl-fsd/main.c servers/lkl-netd/main.c; do
    grep -Fq 'rtmux_phys_contain()' "$f" || {
        echo "LKL gate: $f must seal physical maps with rtmux_phys_contain" >&2
        exit 1
    }
done

for s in rtmux_hwres_release_range \
         VIRTIO_MMIO_MAP_MAX VIRTIO_DMA_ALLOC_MAX VIRTIO_DMA_POOL_MAX; do
    grep -q "$s" servers/lkl/virtio.c servers/lkl/virtio.h || {
        echo "LKL gate: virtio transport missing $s" >&2
        exit 1
    }
done

if grep -q 'rtmux_hwres_claim_range(' servers/lkl/virtio.c; then
    echo "LKL gate: virtio transport must not self-claim MMIO" >&2
    exit 1
fi

if grep -q 'hwinfo_find_device' servers/lkl-netd/main.c; then
    echo "LKL gate: lkl-netd must not self-discover virtio MMIO" >&2
    exit 1
fi

for s in VIRTIO_MMIO_SERVICE_PATH IOM_VIRTIO_MMIO_DELEGATE --delegated-virtio; do
    grep -q -- "$s" servers/lkl-netd/main.c || {
        echo "LKL gate: lkl-netd missing delegated MMIO launcher $s" >&2
        exit 1
    }
done

for s in 'delegate_netd_child("--serve-net")' \
         'delegate_netd_child("--serve-net-crash-on-ping")'; do
    grep -Fq "$s" servers/lkl-netd/main.c || {
        echo "LKL gate: lkl-netd public net modes must delegate before serving" >&2
        exit 1
    }
done

grep -q '\.virtio_devices = RTMUX_LKL_VIRTIO_DEVS' servers/lkl/host.c || {
    echo "LKL gate: LKL host shim must gate virtio device-table reachability" >&2
    exit 1
}

if grep -q 'hwinfo_find_device' servers/lkl/virtio.c; then
    echo "LKL gate: virtio transport must get MMIO records from the service" >&2
    exit 1
fi

awk '
    /release_delegated_handles\(/ { in_func = 1 }
    in_func && /rtmux_hwres_release_range_for_pid/ { release = 1 }
    in_func && /owner_pid/ { owner = 1 }
    in_func && /^}/ { exit (release && owner) ? 0 : 1 }
    END { if (!in_func) exit 1 }
' servers/virtio-mmio/main.c || {
    echo "LKL gate: virtio-mmio rollback must release handles for their owner pid" >&2
    exit 1
}

awk '
/^static int delegate_device\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /hwinfo_find_device/ && !bad) bad = NR
    if ($0 ~ /rtmux_hwres_claim_range_for_pid/ && !claim) claim = NR
    if ($0 ~ /release_delegated_handles\(handles, found\)/ && !rollback) rollback = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(!bad && claim && rollback && claim < rollback) }
' servers/virtio-mmio/main.c || {
    echo "LKL gate: virtio-mmio service must delegate registered records and roll back partial claims" >&2
    exit 1
}

awk '
/^static int virtio_io_msg\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /remember_delegation/ && !remember) remember = NR
    if ($0 ~ /release_delegated_handles\(rep\.handles, claimed\)/ && !rollback) rollback = NR
    if (rollback && $0 ~ /claimed = 0/ && !zero) zero = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(remember && rollback && zero && remember < rollback && rollback < zero) }
' servers/virtio-mmio/main.c || {
    echo "LKL gate: virtio-mmio service must release claimed handles if delegation recording fails" >&2
    exit 1
}

awk '
/^static int query_delegation\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /found > max_handles/ && !size) size = NR
    if ($0 ~ /return EMSGSIZE/ && size && !emsgsize) emsgsize = NR
    if ($0 ~ /delegation_count = dst/ && !consume) consume = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(size && emsgsize && consume && emsgsize < consume) }
' servers/virtio-mmio/main.c || {
    echo "LKL gate: virtio-mmio query must reject undersized replies before consuming records" >&2
    exit 1
}

for s in IOM_VIRTIO_MMIO_REGISTER store_device_records find_boot_devices \
         '--register-boot'; do
    grep -q -- "$s" servers/virtio-mmio/main.c userland/init || {
        echo "LKL gate: virtio-mmio missing boot record registration $s" >&2
        exit 1
    }
done

for s in remember_delegation query_delegation 'delegation_count = dst'; do
    grep -q "$s" servers/virtio-mmio/main.c || {
        echo "LKL gate: virtio-mmio service missing one-shot delegated query $s" >&2
        exit 1
    }
done

awk '
/^int rtmux_lkl_admission_lock\(/ { in_lock = 1; depth = 0; opened = 0 }
in_lock {
    if ($0 ~ /pthread_mutex_trylock\(&admission_lock\)/ && !trylock) trylock = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_lock = 0
}
/thread_pool_attr_t attr =/ { in_pool = 1; depth2 = 0; opened2 = 0 }
in_pool {
    if ($0 ~ /\.lo_water = 1,/) lo = NR
    if ($0 ~ /\.hi_water = 2,/) hi = NR
    if ($0 ~ /\.maximum = 2,/) max = NR
    opens2 = gsub(/{/, "{")
    closes2 = gsub(/}/, "}")
    if (opens2) opened2 = 1
    if (opened2) depth2 += opens2 - closes2
    if (opened2 && depth2 == 0) in_pool = 0
}
END { exit !(trylock && lo && hi && max) }
' servers/lkl/server.c || {
    echo "LKL gate: admission must stay trylock-based with a two-thread pool" >&2
    exit 1
}

grep -q '^#define VIRTIO_DMA_ALLOC_MAX (64U \* 1024U)$' servers/lkl/virtio.h || {
    echo "LKL gate: virtio DMA allocation ceiling changed" >&2
    exit 1
}

grep -q '^#define VIRTIO_MMIO_MAP_MAX 0x1000U$' servers/lkl/virtio.h || {
    echo "LKL gate: virtio MMIO map ceiling changed" >&2
    exit 1
}

grep -q '^#define VIRTIO_DMA_POOL_MAX (256U \* 1024U)$' servers/lkl/virtio.h || {
    echo "LKL gate: virtio DMA pool ceiling changed" >&2
    exit 1
}

for s in rtmux_lkl_host_stats_net_bounce_rx \
         rtmux_lkl_host_stats_net_bounce_tx \
         net_bounce_rx_bytes \
         net_bounce_tx_bytes; do
    grep -q "$s" servers/lkl/host.c servers/lkl/domain.h \
              servers/lkl-netd/main.c scripts/qemu-regress.sh || {
        echo "LKL gate: net bounce copy accounting missing $s" >&2
        exit 1
    }
done

for s in rtmux_lkl_host_stats_lkl_block_read \
         rtmux_lkl_host_stats_lkl_block_write \
         lkl_block_read_bytes \
         lkl_block_write_bytes; do
    grep -q "$s" servers/lkl/host.c servers/lkl/domain.h \
              servers/lkl-fsd/main.c scripts/qemu-regress.sh || {
        echo "LKL gate: LKL block copy accounting missing $s" >&2
        exit 1
    }
done

grep -q 'lkl-gate' userland/init || {
    echo "LKL gate: rt-check does not run lkl-gate" >&2
    exit 1
}

grep -q 'lkl-netd-direct-denied' userland/init || {
    echo "LKL gate: init must keep the legacy lkl-netd mode name covered" >&2
    exit 1
}

awk '
    /^check-lkl-iommu-preflight:/ { in_target = 1; next }
    in_target && /^[^[:space:]].*:/ { exit }
    in_target && /lkl-iommu-probe --timeout 60/ { pci = 1 }
    in_target && /lkl-iommu-sys-probe --timeout 60/ { sys = 1 }
    in_target && /lkl-iommu-metadata --timeout 60/ { metadata = 1 }
    in_target && /lkl-iommu-domain-preflight --timeout 60/ { domain = 1 }
    END { exit (pci && sys && metadata && domain) ? 0 : 1 }
' Makefile || {
    echo "LKL gate: standalone IOMMU preflight target must run all probes" >&2
    exit 1
}

awk '
    /^check-lkl-iommu-live:/ { in_target = 1; next }
    in_target && /^[^[:space:]].*:/ { exit }
    in_target && /lkl-iommu-domain --timeout 180 --require-supported/ {
        domain = 1
    }
    in_target && /lkl-iommu-dma-deny --timeout 180 --require-supported/ {
        deny = 1
    }
    in_target && /lkl-iommu-revoke --timeout 180 --require-supported/ {
        revoke = 1
    }
    END { exit (domain && deny && revoke) ? 0 : 1 }
' Makefile || {
    echo "LKL gate: live IOMMU target must require supported enforcement modes" >&2
    exit 1
}

awk '
    /^check-lkl-containment:/ {
        if ($0 ~ /check-lkl-iommu-preflight/) found = 1
    }
    /^check-lkl-containment:/ { in_target = 1; next }
    in_target && /^[^[:space:]].*:/ { exit }
    in_target && /lkl-netd-iommu-fail-closed --timeout 60/ { failclosed = 1 }
    END { exit (found && failclosed) ? 0 : 1 }
' Makefile || {
    echo "LKL gate: containment target must include IOMMU fail-closed handoff" >&2
    exit 1
}

awk '
    /lkl-iommu-domain\|lkl-iommu-dma-deny\|lkl-iommu-revoke\)/ {
        in_case = 1
        if ($0 ~ /lkl-iommu-domain/) mode = "domain"
        if ($0 ~ /lkl-iommu-dma-deny/) mode = mode " dma"
        if ($0 ~ /lkl-iommu-revoke/) mode = mode " revoke"
        sys = binding = gap = 0
        next
    }
    in_case && /^[[:space:]]*;;/ {
        split(mode, modes)
        for (i in modes) {
            ok[modes[i]] = ok[modes[i]] || (sys && binding && gap)
        }
        in_case = 0
        mode = ""
    }
    in_case && /setup_system_iommu_or_skip/ { sys = 1 }
    in_case && /setup_system_iommu_mmio_binding_or_skip/ { binding = 1 }
    in_case && /RTMux system-IOMMU domain programming is not implemented/ {
        gap = 1
    }
    END { exit (ok["domain"] && ok["dma"] && ok["revoke"]) ? 0 : 1 }
' scripts/qemu-regress.sh || {
    echo "LKL gate: live IOMMU modes must stay binding-gated until programming lands" >&2
    exit 1
}

if grep -v 'lkl-netd-direct-denied' userland/init |
   grep -qE '/sbin/lkl-netd --crash-after-start'; then
    echo "LKL gate: init LKL net modes must use delegated lkl-netd launch" >&2
    exit 1
fi

if grep -q 'rtmux.run=lkl-[^*]' userland/init; then
    echo "LKL gate: userland/init must parse rtmux.run generically" >&2
    exit 1
fi
