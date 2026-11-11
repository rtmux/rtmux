# Platform Boundary Contracts

This file separates what RTMux assumes about the hardware platform from what
the QEMU `virt` test fixture happens to provide.  Budget rows in
`docs/rt-budgets.md` and claim rows in `docs/formal-claims.md` cite a row of
the table below through a `platform:` field, so a reader can tell whether a
number or a claim rests on production silicon or on a permissive emulator.

## Classes

| class | meaning |
| --- | --- |
| `production` | Exercised on real hardware (SiFive HiFive Unmatched, FU740) with the evidence named in the row. |
| `qemu-only` | Holds on QEMU/TCG; real hardware may differ or has not been shown to agree. |
| `fixture` | A test scaffold that does not model the production mechanism (for example a patched device tree whose emulated device enforces nothing). |

A class is `production` only when the evidence column names a hardware run.  The
checker enforces the shape of that statement only, and makes no trust or
security claim: a `production` row's evidence must contain a word naming the
hardware (Unmatched, FU740, board) and a documented file, and its gate must be a
real procedure naming a documented file (`none`, `TBD` and similar placeholders
are rejected).  A documented file is a regular file (not a directory) inside the
checkout, with no `..` component, reached without crossing a symbolic link.  Evidence that
mentions QEMU, TCG, emulation, simulation or synthetic data is rejected as an
obvious slip.  No wording can prove a hardware run took place, and prose that
denies one ("no run yet") is not detected; review of the cited file is what
decides whether a row is `production`.
README prose about a hardware run counts as evidence for the behavior it
describes and for nothing nearby.  Absence of a hardware artifact keeps a row
at `qemu-only`.

The `hardware gate` column names the script, run, or board procedure whose
result would let the row move to `production`, or `none` when no such gate
exists yet.  A gate only counts if it can produce hardware evidence: a script
that validates synthetic logs (`make check-fu740-smoke-scripts`, however it is
spelled or invoked) is a parser check, not a gate result.

## Citing a row

A budget block carries a field line, and a table row carries a cell, of the
form

```text
platform: <claim-class> PB-ID[, PB-ID...]
```

`claim-class` is `production`, `qemu-only`, or `fixture`, and states how the
row is to be read.  A claim-class of `production` is rejected by
`make check-platform-boundary-contracts` if any cited row is not class
`production`; a row with a hardware gate stays `qemu-only` until that gate has
produced hardware evidence.  Claim rows with no hardware dependence write
`platform: n/a` (formal claims only).  Every `rt-budget` block and every
formal-claim row must carry the field; `make check-rt-budgets` and
`make check-formal-claims` enforce presence and syntax, and the contract check
enforces that each id exists.

## Contracts

| id | assumption | class | evidence | hardware gate |
| --- | --- | --- | --- | --- |
| PB-TIMER-RDTIME | The timebase frequency comes from the device tree and `rdtime` (user-readable via `scounteren`) is a monotonic, cross-hart-consistent clock; latency rows are measured in this clock. | qemu-only | `arch/riscv/startup/init-cpu.c`, `arch/riscv/cpu-misc.c`; every `rt-budget` block names a QEMU/TCG target, where the timebase is host virtual time | `scripts/fu740-irqoff-budget.sh` (records `irqoff-timer` metadata on the board) |
| PB-TIMER-SBI-SET | A one-shot tick is armed through the SBI `sbi_set_timer` call into the platform firmware, with no Sstc `stimecmp`; expiry error is bounded by firmware, not by RTMux. | qemu-only | `arch/riscv/cpu-misc.c`; the `timer expiry error` blocks in `docs/rt-budgets.md` use QEMU's bundled OpenSBI | `scripts/fu740-irqoff-budget.sh` |
| PB-INTC-PLIC | External interrupts are claimed and completed through a PLIC whose S-mode context per hart is derived from the device tree. | qemu-only | `arch/riscv/plic.c`, `arch/riscv/startup/board.c`; the `hard-irq claim to ist dispatch` block is a QEMU goldfish-rtc alarm; the README reports an Unmatched boot but no interrupt-delivery or latency artifact | `scripts/fu740-gpio-latency.sh` (board runner plus logic analyzer) |
| PB-TLB-SFENCE | Page-table changes are made visible with a local `sfence.vma` and a ticketed cross-hart shootdown; a missing flush is observable. | qemu-only | `arch/riscv/cpu-pte.c`, `arch/riscv/include/cpu-inline.h`; TCG does not model a stale TLB the way silicon does, so a missing flush is not demonstrated either way | none |
| PB-ICACHE-FENCEI | Code written by a loader becomes executable on every hart through a local `fence.i` plus an SBI remote `fence.i` (`cpu_icache_sync_all`) at the writable-to-executable transition. | production | `README.md` (Recent fixes): about 640 consecutive spawns clean on the Unmatched where the loop faulted every few runs; `arch/riscv/include/sbi.h`, `kernel/ext/as-memcpy.c` | FU740 spawn loop described in `README.md` (no checked-in script) |
| PB-DCACHE-MAINT | No data-cache maintenance (no Zicbom `cbo.*`) is issued anywhere; memory shared with a device is assumed coherent. | qemu-only | no `cbo.` instruction in the tree; QEMU has no data cache model | none |
| PB-DMA-COHERENT | Bus-master DMA (virtio queues, NVMe queues and PRPs) sees CPU stores after a `fence rw,rw`, and the CPU sees device stores, with no bounce or cache operation. | qemu-only | `userland/dev-nvme/nvme-ist.c`, `servers/lkl/virtio.c`; exercised only against QEMU virtio and NVMe devices | none |
| PB-IOMMU-RISCV | A RISC-V IOMMU 1.0 (DDTP, Sv39x4 device contexts) fronts delegated DMA devices and enforces the programmed domain. | fixture | `internal/include/sys/riscv-iommu.h`, `tests/userland/rt-latency/lkl-iommu-probe.c`, `docs/lkl-domain.md`; the QEMU fixture is a patched device tree and its device does not enforce the domain for the live MMIO path; the FU740 has no IOMMU | none |
| PB-IOMMU-ABSENT | When a virtio-mmio IOMMU stream is described, the LKL net path refuses before posting RX descriptors or setting `DRIVER_OK` while domain enforcement is unavailable. | qemu-only | `lkl-netd-iommu-fail-closed` mode in `scripts/qemu-regress.sh`, `docs/lkl-domain.md` | none |
| PB-IOMMU-UNDESCRIBED | With no IOMMU stream described, the LKL net path proceeds without establishing a per-device DMA translation boundary; the device must be treated as trusted. | qemu-only | `virtio_mmio_require_iommu_domain` in `servers/lkl/virtio.c`, its caller in `servers/lkl/host.c`, `lkl-netd-http` mode in `scripts/qemu-regress.sh`, trusted-device policy in `docs/lkl-domain.md` | none |
| PB-MMIO-WIDTH | Device registers are accessed with `volatile` loads and stores at the register's native width and natural alignment; no sub-width or unaligned access is relied on. | qemu-only | `arch/riscv/plic.c`, the userland serial and NVMe drivers; QEMU devices accept mismatched widths that real SiFive peripherals may fault or ignore | none |
| PB-MMIO-ORDER | Most device paths rely on `volatile` and `fence rw,rw`; LKL virtio-net queue selection, publication, and notification use explicit memory and I/O fences. Other device-register ordering and posted-write completion remain unspecified. | qemu-only | `arch/riscv/include/cpu-inline.h`, `userland/dev-nvme/nvme-ist.c`, `servers/lkl/virtio.c`, `servers/lkl/host.c`; TCG executes MMIO in program order, so a missing I/O fence is not observable | none |
