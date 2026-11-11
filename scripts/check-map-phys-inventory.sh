#!/bin/sh
set -eu

scan_map_phys()
{
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git grep -n 'MAP_PHYS' -- \
            ':!docs/*' \
            ':!Makefile' ':!scripts/check-lkl-domain.sh' \
            ':!scripts/qemu-regress.sh' \
            ':!scripts/check-map-phys-inventory.sh' || true
        return
    fi

    find . \
        \( -path './.git' -o -path './build' -o -path './externals' \
           -o -path './tools/kconfig' \) -prune -o \
        -type f -print |
    sed 's#^\./##' |
    grep -Ev '(^|/)[^/]*\.(diff|patch)$' |
    grep -Ev '^(docs/|Makefile$|scripts/check-lkl-domain\.sh$|scripts/qemu-regress\.sh$|scripts/check-map-phys-inventory\.sh$)' |
    xargs grep -n 'MAP_PHYS' /dev/null 2>/dev/null || true
}

bad=$(
    scan_map_phys |
    grep -Ev '^include/sys/mman.h:' |
    grep -Ev '^internal/include/sys/procmsg.h:' |
    grep -Ev '^kernel/include/private/priv-calls.h:' |
    grep -Ev '^kernel/mm/core.c:' |
    grep -Ev '^lib/libc/services/rtmux-phys-contain.c:' |
    grep -Ev '^servers/lkl/virtio\.(c|h):' |
    grep -Ev '^servers/pci/(client|dw-atu|ecam)\.c:' |
    grep -Ev '^servers/virtio-mmio/main\.c:' |
    grep -Ev '^servers/roottask/(include/root-process\.h|mem/.*|path/cpiofs\.c|path/devmem\.c|proc/detach\.c|proc/kext-wrappers\.c|sys/loader-elf\.c):' |
    grep -Ev '^tests/userland/capgate\.c:' |
    grep -Ev '^tests/userland/rt-latency/(irq-latency|irq-notify|lkl-contain-test|lkl-iommu-probe|preempt-latency|tracelogger)\.c:' |
    grep -Ev '^userland/dev-nvme/main\.c:' |
    grep -Ev '^userland/dev-serial-(8250|sifive)/main\.c:' || true
)

if test -n "$bad"; then
    echo "MAP_PHYS inventory changed; update docs/lkl-domain.md and this gate" >&2
    echo "$bad" >&2
    exit 1
fi

if ! awk '
/^int main\(int argc, char \*\*argv\)/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /hdl = pci_device_attach\(/ && !attach) attach = NR
    if ($0 ~ /rtmux_phys_contain\(\)/ && !seal) seal = NR
    if ($0 ~ /bringup\(&ctrl, hdl\)/ && !bringup) bringup = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(attach && seal && bringup && attach < seal && seal < bringup) }
' userland/dev-nvme/main.c; then
    echo "MAP_PHYS inventory: dev-nvme must seal after PCI attach and before BAR map" >&2
    exit 1
fi

if ! awk '
/^static int setup_msix\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /MAP_PHYS/ && !map) map = NR
    if ($0 ~ /dev_ptr->msix_tab = \(msix_table_t \*\) tmp/ && !cache) cache = NR
    if ($0 ~ /dev_ptr->msix_tab = NULL/ && !clear) clear = NR
    if ($0 ~ /munmap\(m_tab1, BaseReg->Size\)/ && !unmap) unmap = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(map && cache && clear && unmap && map < cache && cache < clear && clear < unmap) }
' servers/pci/client.c; then
    echo "MAP_PHYS inventory: pci MSI-X helper must clear cached BAR pointer before unmap" >&2
    exit 1
fi

if ! awk '
/^static int mem_read_write\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /if \(!obp\)/ && !raw) raw = NR
    if ($0 ~ /proc_rlock_adp\(ctp->info.pid\)/ && !lock) lock = NR
    if ($0 ~ /root_proc\(ctp->info.pid\)->phys_contained/ && !seal) seal = NR
    if ($0 ~ /flags \|= MAP_PHYS/ && !mapflag) mapflag = NR
    if ($0 ~ /root_vm_ops.mmap/ && !mmap) mmap = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(raw && lock && seal && mapflag && mmap && raw < lock &&
              lock < seal && seal < mapflag && mapflag < mmap) }
' servers/roottask/path/devmem.c; then
    echo "MAP_PHYS inventory: /dev/mem I/O must check phys_contained before roottask MAP_PHYS" >&2
    exit 1
fi

if grep -q 'rtmux_hwres_claim_range(' servers/lkl/virtio.c; then
    echo "MAP_PHYS inventory: LKL virtio MMIO must not self-claim hwres" >&2
    exit 1
fi
if grep -q 'hwinfo_find_device' servers/lkl-netd/main.c; then
    echo "MAP_PHYS inventory: LKL net launcher must not self-discover hwinfo" >&2
    exit 1
fi

if grep -q 'hwinfo_find_device' servers/lkl/virtio.c; then
    echo "MAP_PHYS inventory: LKL virtio transport must query service records" >&2
    exit 1
fi

if ! awk '
/MAP_PHYS/ {
    total++
    if ($0 ~ /MAP_PHYS \| MAP_SHARED/) mmio++
    else if ($0 ~ /MAP_PHYS \| MAP_ANON \| MAP_SHARED/) anon++
    else other++
}
END { exit !(total == 4 && mmio == 1 && anon == 3 && !other) }
' tests/userland/rt-latency/lkl-iommu-probe.c; then
    echo "MAP_PHYS inventory: lkl-iommu-probe must contain exactly one delegated MMIO map and three anonymous command-table maps" >&2
    exit 1
fi

if ! awk '
/^static int probe_system_iommu\(bool domain, uint32_t test_did,/ {
    in_fn = 1; depth = 0; opened = 0
}
in_fn {
    if ($0 ~ /rtmux_delegate_device\(RTMUX_DEVICE_RISCV_IOMMU/ && !delegate)
        delegate = NR
    if ($0 ~ /h\.kind != RTMUX_RESOURCE_MMIO/ && !validate)
        validate = NR
    if ($0 ~ /MAP_PHYS \| MAP_SHARED/) {
        map_mmio_count++
        if (!map_mmio) map_mmio = NR
    }
    if ($0 ~ /MAP_PHYS \| MAP_ANON \| MAP_SHARED/) {
        map_anon_count++
        map_anon = NR
    }
    if ($0 ~ /MAP_PHYS/ &&
        $0 !~ /MAP_PHYS \| MAP_SHARED/ &&
        $0 !~ /MAP_PHYS \| MAP_ANON \| MAP_SHARED/)
        other_map = NR
    if ($0 ~ /rtmux_hwres_release_range\(h\.base, h\.length, RTMUX_HWRES_MEMORY\)/ &&
        !release)
        release = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(delegate && validate && map_mmio && map_anon && release &&
              delegate < validate && validate < map_mmio &&
              map_mmio < map_anon && map_anon < release &&
              map_mmio_count == 1 && map_anon_count == 3 && !other_map) }
' tests/userland/rt-latency/lkl-iommu-probe.c; then
    echo "MAP_PHYS inventory: lkl-iommu-probe must map only delegated IOMMU MMIO plus anonymous command tables" >&2
    exit 1
fi

if ! awk '
/^int virtio_mmio_discover\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /virtio_mmio_delegated/ && !delegated) delegated = NR
    if ($0 ~ /rtmux_phys_contain\(\)/ && !seal) seal = NR
    if ($0 ~ /virtio_mmio_map_claimed\(vio, &cand\[i\]\)/ && !map) map = NR
    if ($0 ~ /virtio_mmio_release_candidates\(cand, count\)/ && map && !release) release = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(delegated && seal && map && release && delegated < seal &&
              seal < map && map < release) }
' servers/lkl/virtio.c; then
    echo "MAP_PHYS inventory: LKL virtio MMIO must require delegation, seal, map delegated range, then release leftovers" >&2
    exit 1
fi

if ! awk '
/^static int delegate_netd_child\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /"--delegated-virtio"/ && !child) child = NR
    if ($0 ~ /posix_spawn\(&pid, "\/sbin\/lkl-netd"/ && !spawn) spawn = NR
    if ($0 ~ /request_virtio_mmio_delegation\(pid\)/ && !claim) claim = NR
    if ($0 ~ /wait_child\(pid\)/ && claim && !wait) wait = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(child && spawn && claim && wait && child < spawn &&
              spawn < claim && claim < wait) }
' servers/lkl-netd/main.c; then
    echo "MAP_PHYS inventory: lkl-netd must spawn delegated child, ask virtio-mmio service, then wait" >&2
    exit 1
fi

if ! awk '
/^int main\(int argc, char \*\*argv\)/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /"--serve-net"/ && !serve) serve = NR
    if ($0 ~ /delegate_netd_child\("--serve-net"\)/ && serve && !delegate) delegate = NR
    if ($0 ~ /"--delegated-virtio"/ && !child) child = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(serve && delegate && child && serve < delegate && delegate < child) }
' servers/lkl-netd/main.c; then
    echo "MAP_PHYS inventory: public lkl-netd --serve-net must enter the delegated launcher before child parsing" >&2
    exit 1
fi

if ! awk '
/^static int delegate_device\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /hwinfo_find_device/ && !bad) bad = NR
    if ($0 ~ /rtmux_hwres_claim_range_for_pid/ && !claim) claim = NR
    if ($0 ~ /release_delegated_handles\(handles, found\)/ && !rollback) rollback = NR
    if ($0 ~ /handles\[found\] = \*dev/ && !copy) copy = NR
    if ($0 ~ /handles\[found\]\.owner_pid = claim_pid/ && !owner) owner = NR
    if ($0 ~ /handles\[found\]\.generation = next_generation\+\+/ && !gen) gen = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(!bad && claim && copy && owner && gen &&
              claim < rollback && rollback < copy &&
              copy < owner && owner < gen) }
' servers/virtio-mmio/main.c; then
    echo "MAP_PHYS inventory: virtio-mmio service must claim registered typed handles for child pid and roll back partial claims" >&2
    exit 1
fi

if ! awk '
    /release_delegated_handles\(/ { in_func = 1 }
    in_func && /rtmux_hwres_release_range_for_pid/ { release = 1 }
    in_func && /owner_pid/ { owner = 1 }
    in_func && /^}/ { exit (release && owner) ? 0 : 1 }
    END { if (!in_func) exit 1 }
' servers/virtio-mmio/main.c; then
    echo "MAP_PHYS inventory: virtio-mmio rollback must release handles for their owner pid" >&2
    exit 1
fi

if ! awk '
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
' servers/virtio-mmio/main.c; then
    echo "MAP_PHYS inventory: virtio-mmio service must release claimed handles if delegation recording fails" >&2
    exit 1
fi

if ! awk '
/^static int find_boot_devices\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /RTMUX_DEVICE_NS16550/ && !ns16550) ns16550 = NR
    if ($0 ~ /RTMUX_DEVICE_SIFIVE_UART/ && !sifive) sifive = NR
    if ($0 ~ /RTMUX_DEVICE_PCI_ECAM/ && !ecam) ecam = NR
    if ($0 ~ /RTMUX_DEVICE_PCI_DBI/ && !dbi) dbi = NR
    if ($0 ~ /RTMUX_DEVICE_RISCV_IOMMU/ && !iommu) iommu = NR
    if ($0 ~ /hwinfo_find_device\("virtio"/ && !scan) scan = NR
    if ($0 ~ /VIRTIO_MMIO_MAP_MAX/ && !cap) cap = NR
    if ($0 ~ /\.kind = RTMUX_RESOURCE_MMIO/ && !kind) kind = NR
    if ($0 ~ /\.device_id = \(_device_id\)/ && !devid) devid = NR
    if ($0 ~ /\.generation = found \+ 1/ && !gen) gen = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(ns16550 && sifive && ecam && dbi && iommu && scan && cap && kind && devid && gen &&
              kind < devid && devid < gen) }
' servers/virtio-mmio/main.c; then
    echo "MAP_PHYS inventory: virtio-mmio boot registrar must publish typed records" >&2
    exit 1
fi

if ! awk '
/\/sbin\/virtio-mmio$/ && !svc { svc = NR }
/\/sbin\/virtio-mmio --register-boot/ && !reg { reg = NR }
/\/sbin\/pci-server/ && !pci { pci = NR }
END { exit !(svc && reg && pci && svc < reg && reg < pci) }
' userland/init; then
    echo "MAP_PHYS inventory: init must register boot MMIO records before pci-server" >&2
    exit 1
fi

if ! awk '
/^static int query_delegation\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /delegations\[i\]\.handle\.owner_pid != pid/ && !pid) pid = NR
    if ($0 ~ /RTMUX_RESOURCE_HANDLE_F_REVOKED/ && !revoked) revoked = NR
    if ($0 ~ /found > max_handles/ && !size) size = NR
    if ($0 ~ /return EMSGSIZE/ && size && !emsgsize) emsgsize = NR
    if ($0 ~ /handles\[out\+\+\] = delegations\[i\]\.handle/ && !copy) copy = NR
    if ($0 ~ /delegation_count = dst/ && !consume) consume = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(pid && revoked && size && emsgsize && copy && consume &&
              pid < revoked && revoked < size && size < emsgsize &&
              emsgsize < copy && copy < consume) }
' servers/virtio-mmio/main.c; then
    echo "MAP_PHYS inventory: virtio-mmio query must size-check, return, and consume caller-delegated typed handles" >&2
    exit 1
fi

if ! awk '
/^static int virtio_mmio_query\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /VIRTIO_MMIO_SERVICE_PATH/ && !path) path = NR
    if ($0 ~ /IOM_VIRTIO_MMIO_QUERY/ && !query) query = NR
    if ($0 ~ /msg\.handles\[i\]\.kind != RTMUX_RESOURCE_MMIO/ && !kind) kind = NR
    if ($0 ~ /msg\.handles\[i\]\.owner_pid != getpid\(\)/ && !owner) owner = NR
    if ($0 ~ /msg\.handles\[i\]\.generation == 0/ && !gen) gen = NR
    if ($0 ~ /cand\[i\]\.claimed = true/ && !claimed) claimed = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(path && query && kind && owner && gen && claimed &&
              path < query && query < kind && kind < owner &&
              owner < gen && gen < claimed) }
' servers/lkl/virtio.c; then
    echo "MAP_PHYS inventory: LKL virtio transport must query typed delegated records" >&2
    exit 1
fi

if ! awk '
/cmd == HWRES_REQ_CREATE \|\| cmd == HWRES_REQ_CLAIM/ && !cmds { cmds = NR }
/pid == ctp->info.pid && root_proc\(pid\)->phys_contained/ && cmds && !seal { seal = NR }
/return proc_status\(ctp, EPERM\)/ && seal && !deny { deny = NR }
END { exit !(cmds && seal && deny && cmds < seal && seal < deny) }
' servers/roottask/sys/hwres-cmd.c; then
    echo "MAP_PHYS inventory: contained callers must not mint or claim hwres" >&2
    exit 1
fi

if ! awk '
/^static int pci_io_msg\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /client_pid = ctp->info.pid/ && !pid) pid = NR
    if ($0 ~ /PCI_attach_device\(/ && !attach) attach = NR
    if ($0 ~ /ocb, client_pid, &local_errno/ && attach && !pass) pass = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(pid && attach && pass && pid < attach && attach < pass) }
' servers/pci/main.c; then
    echo "MAP_PHYS inventory: pci attach must pass client pid as resource owner" >&2
    exit 1
fi

if ! awk '
/^static void fill_bar_handles\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /\.kind = RTMUX_RESOURCE_PCI_BAR/ && !kind) kind = NR
    if ($0 ~ /\.base = PCI_MEM_ADDR\(bar->Address\)/ && !base) base = NR
    if ($0 ~ /\.owner_pid = owner_pid/ && !owner) owner = NR
    if ($0 ~ /\.device_id = dev->ClassCode/ && !devid) devid = NR
    if ($0 ~ /\.generation =/ && !gen) gen = NR
    if ($0 ~ /\.reserved = i/ && !bar) bar = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(kind && base && owner && devid && gen && bar) }
' servers/pci/main.c; then
    echo "MAP_PHYS inventory: pci attach must return typed BAR handles" >&2
    exit 1
fi

if ! awk '
/^pci_device_t \*PCI_attach_device\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /pid_t resource_owner_pid/ && !arg) arg = NR
    if ($0 ~ /addup_resources\(dev_ptr, flags, show_pci,/ && !add) add = NR
    if ($0 ~ /resource_owner_pid\)\) !=/ && add && !pass) pass = NR
    if ($0 ~ /dev_ptr->ResourceOwner = resource_owner_pid/ && !save) save = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(arg && add && pass && save && arg < add && add < pass && pass < save) }
' servers/pci/attach.c; then
    echo "MAP_PHYS inventory: pci attach must save owner pid for BAR resources" >&2
    exit 1
fi

if ! awk '
/^int addup_resources\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /pid_t resource_owner_pid/ && !arg) arg = NR
    if ($0 ~ /pci_alloc_resource\(&alloc_io_reg, 0, a_bus,/ && !io) io = NR
    if ($0 ~ /pci_alloc_resource\(&alloc_mem_reg, mem_align, a_bus,/ && !mem) mem = NR
    if ($0 ~ /pci_alloc_resource\(&alloc_mem_reg, pref_align, a_bus,/ && !pref) pref = NR
    if ($0 ~ /pci_alloc_resource\(&alloc_mem_reg, rom_align, a_bus,/ && !rom) rom = NR
    if ($0 ~ /resource_owner_pid\)\) !=/ && !seen_owner) seen_owner = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
END { exit !(arg && io && mem && pref && rom && seen_owner) }
' servers/pci/client.c; then
    echo "MAP_PHYS inventory: pci BAR allocation must use attach owner pid" >&2
    exit 1
fi

if ! awk '
/^int pci_alloc_resource\(/ { in_alloc = 1; depth = 0; opened = 0 }
in_alloc {
    if ($0 ~ /pid_t owner_pid/ && !alloc_arg) alloc_arg = NR
    if ($0 ~ /hwres_claim_for_pid\(&rreq, 1, owner_pid\)/ && !claim) claim = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_alloc = 0
}
/^int pci_free_resource\(/ { in_free = 1; depth2 = 0; opened2 = 0 }
in_free {
    if ($0 ~ /pid_t owner_pid/ && !free_arg) free_arg = NR
    if ($0 ~ /hwres_release_for_pid\(&rreq, 1, owner_pid\)/ && !release) release = NR
    opens2 = gsub(/{/, "{")
    closes2 = gsub(/}/, "}")
    if (opens2) opened2 = 1
    if (opened2) depth2 += opens2 - closes2
    if (opened2 && depth2 == 0) in_free = 0
}
END { exit !(alloc_arg && claim && free_arg && release &&
              alloc_arg < claim && free_arg < release) }
' servers/pci/resource.c; then
    echo "MAP_PHYS inventory: pci BAR hwres must claim/release for owner pid" >&2
    exit 1
fi

if ! awk '
/^static int validate_bar0_handle\(/ { in_fn = 1; depth = 0; opened = 0 }
in_fn {
    if ($0 ~ /bar->bar_num != 0/ && !bar) bar = NR
    if ($0 ~ /handle->kind != RTMUX_RESOURCE_PCI_BAR/ && !kind) kind = NR
    if ($0 ~ /handle->owner_pid != getpid\(\)/ && !owner) owner = NR
    if ($0 ~ /handle->device_id/ && /NVME_CCODE_CLASS_SUBCLASS/ && !class) class = NR
    if ($0 ~ /handle->generation == 0/ && !gen) gen = NR
    if ($0 ~ /RTMUX_RESOURCE_HANDLE_F_REVOKED/ && !revoke) revoke = NR
    if ($0 ~ /handle->reserved != 0/ && !reserved) reserved = NR
    opens = gsub(/{/, "{")
    closes = gsub(/}/, "}")
    if (opens) opened = 1
    if (opened) depth += opens - closes
    if (opened && depth == 0) in_fn = 0
}
/validate_bar0_handle\(&bars\[0\]\)/ && !validate { validate = NR }
/MAP_PHYS/ && !map { map = NR }
END {
    exit !(bar && kind && owner && class && gen && revoke && reserved &&
           validate && map && validate < map)
}
' userland/dev-nvme/main.c; then
    echo "MAP_PHYS inventory: dev-nvme BAR0 must validate typed PCI BAR handle before map" >&2
    exit 1
fi

helper_users=$(
    git grep -n 'roottask_mmap_phys_user(' -- servers include 2>/dev/null |
    grep -Ev '^include/priv\.h:' |
    grep -Ev '^servers/roottask/proc/kext-wrappers\.c:' |
    grep -Ev '^servers/roottask/path/cpiofs\.c:' || true
)
if test -n "$helper_users"; then
    echo "MAP_PHYS inventory: bootstrap phys-map helper gained a caller" >&2
    echo "$helper_users" >&2
    exit 1
fi

if ! awk '
/roottask_mmap_phys_user\(\(paddr_t\) cpio_phys_start, cpio_size,/ { call = NR }
/PROT_READ, 0, &va\)/ && call && !prot { prot = NR }
END { exit !(call && prot && call < prot) }
' servers/roottask/path/cpiofs.c; then
    echo "MAP_PHYS inventory: cpiofs bootstrap map must stay ramdisk read-only" >&2
    exit 1
fi

if ! awk '
/root_vm_ops.mmap\(0, NULL, \(phdr->p_vaddr\),/ { call = NR }
/MAP_PHYS \+ MAP_SHARED \+ MAP_SYSRAM/ && call && !flags { flags = NR }
END { exit !(call && flags && call < flags) }
' servers/roottask/sys/loader-elf.c; then
    echo "MAP_PHYS inventory: loader physical maps must remain MAP_SYSRAM" >&2
    exit 1
fi

for driver in userland/dev-serial-8250/main.c userland/dev-serial-sifive/main.c; do
    if grep -q 'hwinfo_find_device\|rtmux_hwres_claim_range(' "$driver"; then
        echo "MAP_PHYS inventory: $driver must not self-discover or self-claim MMIO" >&2
        exit 1
    fi
    if ! awk '
    /rtmux_delegate_device\(/ && !delegate { delegate = NR }
    /MAP_PHYS/ && !map { map = NR }
    /rtmux_hwres_release_range\(/ && !release { release = NR }
    END {
        exit !(delegate && map && release && delegate < map && map < release)
    }
    ' "$driver"; then
        echo "MAP_PHYS inventory: $driver must map only typed delegated MMIO and release on map failure" >&2
        exit 1
    fi
done

if ! awk '
/delegate_boot_mmio\(RTMUX_DEVICE_PCI_ECAM/ && !claim { claim = NR }
/claim_from_handle\(&dev->ecam_claim/ && !claim_record { claim_record = NR }
/dev->ecam_vaddr =/ && !map_start { map_start = NR }
map_start && /MAP_PHYS/ && !map { map = NR }
/hwres_release_range\(&dev->ecam_claim\)/ && map && !release { release = NR }
/delegate_boot_mmio\(RTMUX_DEVICE_PCI_DBI/ && !dbi_claim { dbi_claim = NR }
/claim_from_handle\(&dev->dbi_claim/ && !dbi_record { dbi_record = NR }
/dev->dbi_claimed = 1/ && !dbi_mark { dbi_mark = NR }
/pci_dw_atu_init\(dev->dbi_base/ && !dbi_map { dbi_map = NR }
END {
    exit !(claim && claim_record && map && release &&
           claim < claim_record && claim_record < map && map < release &&
           dbi_claim && dbi_record && dbi_mark && dbi_map &&
           dbi_claim < dbi_record && dbi_record < dbi_mark &&
           dbi_mark < dbi_map)
}
' servers/pci/ecam.c; then
    echo "MAP_PHYS inventory: pci ECAM/DBI maps must follow typed boot handles" >&2
    exit 1
fi

if grep -q 'MAP_PHYS' servers/pci/enum.c ||
   ! grep -q -- '-P raw PCIEX mapping is unsupported' servers/pci/main.c; then
    echo "MAP_PHYS inventory: legacy pci -P PCIEX raw map must stay removed" >&2
    exit 1
fi

echo "map-phys inventory: ok"
