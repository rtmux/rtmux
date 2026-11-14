# Building, Running, and CI

## Prerequisites

- A RISC-V bare-metal cross toolchain, default `riscv-none-elf-` (xPack or
  equivalent); override with `make CROSS_COMPILE=foo-`.
- `lld` for shared-object links (`libc.so`, `ld-rtmux.so`), because xPack
  binutils ships `--disable-shared`; override `LDSHARED` for another linker.
- `qemu-system-riscv64` and OpenSBI firmware (`scripts/emu.sh` uses a system
  copy when present, otherwise QEMU's built-in one).
- `git` and `python3`. Kconfiglib is cloned on demand into `tools/kconfig/`.
- For the LKL servers: a `riscv64-linux-gnu-` cross toolchain (to build the
  archive from `lkl/linux`) and `e2fsprogs`.

## Build commands

```bash
make defconfig           # First time only: apply configs/defconfig
make                     # kernel + roottask + libc + userland + initramfs.cpio
make -Bj                 # Forced rebuild (only after a toolchain or flag change)
make startup             # Kernel binary only
make initramfs           # Repack initramfs.cpio after userland/lib/servers edits
make menuconfig          # Kconfig front end
make qemu                # Build and boot in QEMU
make clean | distclean   # distclean also removes build/images/nvme.img
V=1 make ...             # Verbose output
```

Changes under `userland/`, `lib/`, or `servers/` are not visible until
`make initramfs`. All products land under `build/` (override with `OUT=`).

## POSIX conformance profiles

The profile selects which POSIX option macros the public headers and `sysconf()`
advertise. Apply a profile with its config fragment:

```bash
DEFCONFIG=configs/profile-minimal.config make defconfig && make -Bj
```

| Profile | Fragment | Scope |
|---------|----------|-------|
| `minimal` | `configs/profile-minimal.config` | PSE51 mandatory realtime |
| `realtime` | `configs/profile-realtime.config` | PSE52 filesystem + PSE53 process |
| `lkl` | `configs/profile-lkl.config` | Measured floor needed by the LKL servers |
| `full` (default) | `configs/profile-full.config` | Every advertised option |

`minimal` is a subset of `realtime` and `lkl`; both are subsets of `full`;
`realtime` and `lkl` are incomparable. `scripts/posix_profile.py` is the
registry. See [posix-compliance.md](posix-compliance.md) and
[posix-capability-boundary.md](posix-capability-boundary.md).

## LKL servers

`CONFIG_LKL_SERVERS=y` (only valid with the `lkl` or `full` profile) builds
`lkl-fsd` and `lkl-netd`. The RV64 archives come either from a prebuilt tarball
(`scripts/build-lkl-rv64.sh --extract`) or from source:

```bash
make LKL_SRC=/abs/path/to/lkl-tree CONFIG_LKL_SERVERS=y
```

See [lkl-domain.md](lkl-domain.md) for the archive build and personalities.

## Running in QEMU

```bash
./scripts/emu.sh                 # 1 CPU
./scripts/emu.sh -v | -vv        # DEBUG / TRACE verbosity
./scripts/emu.sh -gdb            # wait for GDB on :1234
./scripts/emu.sh -mainfs=/dev/X  # override the /usr backing device
CPUS=8 ./scripts/emu.sh          # SMP
```

The script runs the virt machine with 256 MB RAM, the initramfs as initrd, and
auto-creates `build/images/nvme.img` (192 MiB, 8 GPT partitions, p8 = `/usr`
romfs). Boot lands at `login:`; accounts are `root`/`RTMux` and
`rtmuxuser`/`rtmux`.

## Build output

```
build/arch/riscv/rtmux-kernel          Kernel (ELF, static)
build/servers/roottask/roottask        Process/memory/path manager
build/lib/libc/libc.so                 C library
build/lib/ldso/ld-rtmux.so             Runtime linker
build/servers/{pci,logger}/            PCIe server, system logger
build/userland/toolbox                 toolbox applets
build/userland/{dev-*,romfs,pipe}/     Drivers and I/O services
build/tests/userland/syscall_testing   Conformance suite
build/initramfs.cpio                   Initramfs mounted at /
build/images/nvme.img                  NVMe backing image
```

## Build system

Per-component Makefiles, every output under `$(OUT)`:

- `Makefile` (top level) drives `arch/riscv`, `servers/*`, `lib/*`, `userland`.
- `kernel/objects.mk` lists kernel sources; add new kernel `.c` files there.
- `mk/toolchain.mk` selects the arch, toolchain, and flags; `mk/kconfig.mk`
  handles Kconfig and `autoconf.h`; `mk/recurse.mk` provides jobserver-friendly
  recursive targets; `mk/def.mk` is a stable include shim.
- `-ffixed-tp` applies to the kernel only (kernel `tp` = per-CPU pointer;
  userland `tp` = TLS).

## Verification

`make check` runs the static gates, boots RTMux, and runs the conformance
suites. Narrower entry points: `make check-rt`, `make check-formal`,
`make check-lkl`, `make check-lkl-qemu`, `make check-lkl-differential`,
`make posix-profile-boot-probe`, `make lkl-profile-smoke`. The testing entry
points are `tests/userland/syscall_testing` (boot and exec it) and
`scripts/qemu-regress.sh --mode <mode>`.

## Continuous integration

GitHub Actions workflows in `.github/workflows/`. Independent jobs run in
parallel, and a job that needs another (an LKL gate needs the built archives)
waits for it. Toolchains, apt packages, Kconfiglib, the opam switch, and the
built LKL archives are cached.

| Workflow | Covers |
|----------|--------|
| `build-rtmux.yml` | `static` host gates (run with `make -j`, including the advertised option surface of every POSIX profile); `formal` (CBMC, Spin, Z3); `litmus` (herd7); `host` tests; `runtime` QEMU gates, one leg each; a build and QEMU boot probe per POSIX profile |
| `build-lkl.yml` | RV64 `lkl-fsd`/`lkl-netd` archives built from `lkl/linux`; also reused by `lkl-qemu.yml` |
| `lkl-qemu.yml` | RTMux+LKL on QEMU, one leg per gate: containment and libc floor, smoke modes, differential oracles, traced latency windows, LKL profile boot |

`make check-ci-coverage` fails when a test target in the Makefile is run by no
workflow and has no reasoned waiver in `scripts/check-ci-coverage.py`.
