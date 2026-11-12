# Experimental AArch64 / Raspberry Pi 3B bring-up

This standalone diagnostic tests the architecture/platform layer and reuses
RTMux's memory, CPIO and ELF validation code. Its static EL0 fixture deliberately
does not run the scheduler, roottask or shell. The separate
[full native Pi3 port](pi3.md) now boots those components and has its own
`make check-pi3` acceptance gate. Neither target uses Linux or LKL.

The existing RISC-V `make`, `.config` and `make qemu` targets retain their
meaning. Pi3 builds have a separate output tree and private `include/arch`
symlink; they do not change the source-tree symlink or generate a configuration.

## Build and reproduce

Required tools: GNU Make, Python 3, Clang, LLD, LLVM objcopy, and a
`qemu-system-aarch64` with `raspi3b` support. No AArch64 Linux sysroot or rootfs
is needed. On Ubuntu the packages are `clang lld llvm qemu-system-arm`.

```sh
make pi3-bringup
make check-pi3-bringup
```

On a host where versioned LLVM tools are not on PATH:

```sh
make check-pi3-bringup \
  PI3_CLANG=clang-18 \
  PI3_LD=/usr/lib/llvm-18/bin/ld.lld \
  PI3_OBJCOPY=/usr/lib/llvm-18/bin/llvm-objcopy
```

`OUT=/absolute/path` selects a different output root. `QEMU_AARCH64=/path/to/qemu`
selects the emulator. To run interactively after building:

```sh
qemu-system-aarch64 -M raspi3b -accel tcg -display none -monitor none \
  -serial stdio -no-reboot \
  -kernel build/arch/aarch64/raspi3-bringup/kernel8.img
```

The test ends with `PI3 BRINGUP PASS` and parks the CPUs. Ctrl-C stops the
interactive emulator. The automated runner stops QEMU after checking every
stage; a timeout, early exit, exception or partial banner fails the gate.
This uses QEMU's raw-image `-kernel` boot mechanism, not a Linux kernel.

Outputs under `build/arch/aarch64/raspi3-bringup/`:

- `kernel8.img`: raw image loaded at physical `0x80000`.
- `rtmux-pi3-bringup.elf` and `.map`: symbols for debugging.
- `pi3-user.elf` and `fixture.cpio`: static EL0 test and its archive.
- `qemu.log` / `qemu.json`: serial evidence, emulator version, image SHA-256,
  command, completed stages and elapsed time.
- `qemu-no-timer.log` / `.json`: intentional timer-routing failure evidence.

The negative image is built separately under `build/pi3-negative/`. It must
reach the six pre-timer checks and then report `FAIL timer-irq-timeout`.
The runner's `--expect-failure timer-irq-timeout` option accepts only that
failure at that stage. The normal invocation rejects it. The CI workflow also
runs six host tests of serial-evidence validation.

## What is exercised

| Gate stage | Guest-side evidence |
|---|---|
| `boot-el1-uart` | QEMU EL2 entry lowered to EL1h; PL011 serial output |
| `rtmux-cpio-elf-aarch64` | Existing CPIO parser and ELF validator find the static AArch64 fixture and `.rtmux` identity; reject a RISC-V header and a truncated header |
| `mmu-cache-high-alias` | MMU, I/D cache enable bits and shared data through the high direct-map alias |
| `mmu-tlb-remap` | A used 4 KiB mapping changes physical page after break-before-make and TLB invalidation |
| `el1-unmapped-fault` | Exact fault PC, FAR, ESR class and level on an unmapped address |
| `el1-readonly-fault` | A write to kernel text raises a permission fault |
| `timer-irq-5-ticks` | At least five physical timer interrupts through BCM2836 local IRQ routing |
| `smp-4-cpus-el1` | Three secondary CPUs released through the Pi spin table reach EL1 and enable the shared page tables |
| `mailbox-ipi-roundtrip` | Each secondary receives and acknowledges 32 mailbox-0 interrupts |
| `el0-timer-preemption` | Five timer interrupts from a running EL0 fixture return through the lower-EL exception path with state preserved |
| `el0-svc-registers` | Saved x0–x30, SP, PC, NZCV and SVC return behavior; an unknown private test operation returns an error |
| `el0-memory-isolation` | EL0 cannot read kernel text, write its own text, execute writable data, read its high direct-map alias or read the stack guard page; user data remains writable |

The vector frame has compile-time offset assertions shared with assembly.
FP/SIMD instructions are disabled with `-mgeneral-regs-only` and CPACR_EL1;
floating-point context switching is not implemented. All synchronous test
faults require an explicitly armed expectation. Unanticipated exceptions print
the vector, ESR, FAR and PC, then stop with failure.

The gate was run locally with Clang/LLD 18 and QEMU
`8.1.50 (v8.1.0-2759-g8aba939e77-dirty)`. The workflow is configured for Ubuntu
24.04's packaged tools; its remote result must be checked after pushing.

## Platform assumptions

- Cortex-A53, AArch64 little-endian LP64, four CPUs; QEMU `raspi3b` defaults.
- Boot image at `0x80000`, MMU and caches initially off. QEMU raw-image boot
  through EL2 is tested. Assembly also handles EL1/EL3 entry, but those paths
  are not covered by the current gate.
- PL011 at `0x3f201000`; BCM2836 local control at `0x40000000`.
- Nonsecure physical generic timer (CNTP), local IRQ source bit 1. Its rate is
  read from CNTFRQ_EL0, not hardcoded to a CPU frequency.
- Mailbox 0 supplies test IPIs. Secondaries use the QEMU/Pi spin-table cells at
  `0xd8 + cpu * 8`, with cache clean, DSB and SEV on release.
- 4 KiB translation granule, 39-bit virtual addresses, TTBR0 identity mapping
  and TTBR1 direct-map aliases at `0xffffffc000000000`. Boot text is RX,
  rodata is R/XN, data is RW/XN, and MMIO is Device-nGnRnE/XN. The upper tables
  prohibit EL0 even where their lower aliases are user-accessible.
- RAM below `0x3c000000` is mapped; the default GPU-reserved region is excluded.
  The peripheral region at `0x3f000000` and local control are device mappings.
  This fixed bootstrap map is not a discovered physical-memory allocator.
- User fixture text/data/stack are at `0x200000`, `0x201000`, `0x204000`.
  Its loader accepts only the expected two-segment static executable layout.
  The private `SVC #0x5254` test protocol is not an RTMux syscall ABI.

## Relationship to the native port

This is an isolated architecture diagnostic. The native kernel, roottask and
user-space ABI are introduced separately. Its private SVC protocol and fixed
memory map do not define the native kernel ABI.

Physical Pi3 validation remains separate: firmware handoff, device tree,
reserved memory, GPIO alternate functions, UART clock, cache/coherency behavior
and interrupt timing. QEMU TCG does not establish real-time latency or
physical-board cache correctness. Linux co-residency and virtio communication
remain future work; both current Pi3 targets run without Linux.

## Hardware and emulator references

- [QEMU Raspberry Pi board documentation](https://www.qemu.org/docs/master/system/arm/raspi.html)
- [BCM2836 ARM-local control specification](https://datasheets.raspberrypi.com/bcm2836/bcm2836-peripherals.pdf), timer routing, interrupt sources and mailboxes
- [BCM2835 peripheral specification](https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf), PL011 register layout; Pi3 uses the `0x3f000000` peripheral base
- [QEMU v8.2 Pi boot implementation](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.0/hw/arm/raspi.c), raw boot and secondary spin table
- [QEMU v8.2 local interrupt controller](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.0/hw/intc/bcm2836_control.c)
