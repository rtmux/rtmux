# Native RTMux on QEMU Raspberry Pi 3B

The AArch64 port boots the RTMux kernel, its EL0 roottask, native libc and
ELF dynamic linker, logger, pipe service and interactive toolbox shell on QEMU
`raspi3b`. Four Cortex-A53 CPUs participate in the scheduler. The image contains
no Linux kernel, LKL, Linux rootfs, BusyBox or hypervisor.

## Build and run

Requirements: GNU Make, Python 3, git, cpio, LLVM/Clang/LLD 18, and
`qemu-system-aarch64` with `raspi3b`. No AArch64 cross sysroot is needed.
`make defconfig` fetches the repository's Kconfiglib dependency if absent.

```sh
make defconfig DEFCONFIG=configs/pi3_defconfig
make -j8 pi3 OUT=build/pi3
make qemu-pi3 OUT=build/pi3
```

The console reaches `RTMux Pi 3: native services ready` and a `#` prompt.
For example:

```sh
ls /
cat /sys/board
ps
echo hello | cat
/bin/pi3-runtime-test
/bin/sh /bin/toolbox-test
```

Use Ctrl-C in the host terminal to stop QEMU. `shutdown` currently parks the
guest CPUs; it does not power off the emulator. Exiting the shell starts a new
shell under init.

`LLVM_PREFIX=/path/to/llvm/bin/` selects another tool installation;
`QEMU_AARCH64=/path/to/qemu-system-aarch64` selects the emulator. Without an
override the build finds the directory of `clang-18`, or uses unversioned LLVM
tools on PATH. Use separate output directories when changing architecture,
and rebuild after changing configuration or headers: the existing build system
does not track every header dependency. Configuration and `include/arch` are
shared within a worktree; do not build two architectures concurrently there.

The equivalent emulator invocation is:

```sh
qemu-system-aarch64 -M raspi3b -display none -monitor none -serial stdio \
  -kernel build/pi3/arch/aarch64/rtmux-kernel.bin \
  -device loader,file=build/pi3/initramfs.cpio,addr=0x08000000
```

QEMU loads the raw kernel at physical `0x80000` and the CPIO archive at
`0x08000000`. The kernel records the archive's build-time size, so use the
kernel and archive from the same build. `make pi3` orders archive creation
before kernel linking. The ELF with debug symbols is alongside the raw image
as `rtmux-kernel`.

## Automated acceptance

```sh
make -j8 check-pi3 OUT=build/pi3
# Recheck already-built images without rebuilding:
python3 scripts/run-pi3.py --check --out build/pi3
```

The runner drives the actual serial shell, waits for each command's exit status,
and checks its output. A banner alone cannot pass. Evidence is saved to
`build/pi3/pi3-check/serial.log` and `results.json`; the latter records the
command, image hashes, elapsed time, individual checks and final status.

Coverage:

- Real kernel startup, EL0 roottask, four online CPUs and an interactive shell.
- CPIO root filesystem, board identity, process listing and shell status.
- Dynamically linked external programs, pipe IPC and timer-backed sleep.
- Native runtime test: setjmp/longjmp, monotonic time, pthread creation,
  CPU affinity on all four CPUs, FP/SIMD register preservation across SVC/yield
  and timer interrupts, libc TLS isolation, joining exited threads, repeated
  spawn/wait with distinct exit codes, process exit while a peer thread is
  reply-blocked on an UNBLOCK channel, fatal EL0 fault isolation and logger IPC.
- The repository's existing toolbox regression script (70 checks), unmodified. Pi3's CPIO
  includes the same two account records expected by that script. CPIO and romfs
  also carry the RISC-V development image's shadow fixture, with mode 0600:
  `root` / `RTMux` and `rtmuxuser` / `rtmux`. These are public development-image
  credentials. The interactive test enters the shared getty before login.
- Further shell commands after the tests, proving the guest remains usable.
