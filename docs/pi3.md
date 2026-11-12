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

## Shared RISC-V coverage

The native coverage image builds and runs the **same** `syscall_testing` sources
and assertion contracts as RISC-V. Architecture-specific assembly probes select
AArch64 register operations; block tests select real RAM block devices instead
of requiring an NVMe controller that the Pi3 machine does not provide.

```sh
make defconfig DEFCONFIG=configs/pi3_coverage_defconfig
make -j4 check-pi3-coverage OUT=build/pi3-coverage
make check-pi3-profiles OUT=build/pi3-coverage
```

Use a fresh output directory when changing profiles or configuration. The
coverage configuration enables instrumentation and allocation-failure injection,
uses all four Pi3 CPUs and 256 MiB of managed RAM, and keeps LKL servers off.
The ordinary `pi3_defconfig` remains an uninstrumented 960 MiB configuration.
Do not run independent timing-sensitive QEMU suites concurrently on a busy
host: the suite includes bounded watchdog and CPU-time accuracy checks.

`check-pi3-coverage` runs three cold boots. Each runs the full shared suite; the
second also runs it from `/usr/bin/syscall_testing` on the native block
filesystem. The first runs the additional probes below. Every command must
return the expected exit status and complete its required output. The runner
checks assertion counts, rejects unexpected SKIPs, and retains serial logs,
image/config hashes, injection handshakes and trace-class counts in JSON.

| RISC-V native contract | Pi3 coverage |
|---|---|
| Timers, synchronization, printf, setjmp, signals, notifications | Shared suite, including FP/SIMD state across scheduling and syscall entry |
| Capability gates, trace policy, invalid user pointers | Shared suite, same denial and fault contracts |
| VFS, POSIX options, resource gauges | Shared suite against CPIO, shmem, pipes, RAM block and native romfs |
| Block permissions, malformed reads, filesystem-backed executables | Real `/dev/ram0` and `/dev/ram1`; full suite also loaded from `/usr` |
| Fault recovery, loader negatives, allocation failure, POSIX cap boundary | Existing dedicated probes; injection is enabled and must execute |
| SMP lock ranks, RCU, revoke, preemption, IPC contention | Existing probes on four Cortex-A53 CPUs |
| RT latency, jitter, IPC roundtrip, priority inheritance/donation | Existing native probes, using CNTVCT as the timebase |
| Tickless idle, monotonic clock, RR fairness, sporadic budget, mutexes | Existing scheduling and timer probes |
| IRQ notification, one deliberately suppressed IRQ, IRQ-to-IST trace | Actual BCM2835 compare-3 interrupt, 100 notifications and 50 traced alarms |
| Kernel trace rings and per-syscall cost events | Shared tracelogger; verifies thread/IPC and kernel-call event classes |
| Serial cancellation and replacement read | Shared serial-service test using PL011 IRQ delivery and host-injected input; driver reap restores console polling |
| Memory admission under exhaustion | Existing ENOMEM/canary probe, followed by a working shell |
| Namespace churn | Additional 32-round open/unlink/combined-stat regression (352 assertions) |
| Shell and toolbox | Existing shell POSIX script and 70-check toolbox script |
| Interactive terminal | Shared login, vi rendering/editing and shell job-control scenarios |
| POSIX profiles | `full`, `minimal`, `realtime`, and `lkl` option surfaces; Linux domains remain disabled in every image |
| Log-ring overflow | Separate 4 KiB ring image runs the shared `--sys-log-wrap` test, including full-ring reads, overwritten-window denial and rewind recovery |

The full SMP suite retains the upstream **Q32 `spawn_concurrent` quarantine**,
just as RISC-V does. It is reported as SKIP, never counted as a pass. The profile
named `lkl` selects an API option surface; it does not enable Linux services.

Recheck existing images without rebuilding:

```sh
python3 scripts/check-pi3-coverage.py --out build/pi3-coverage --repeat 3
# Other profiles are built separately and the original .config is restored:
python3 scripts/check-pi3-profiles.py --out build/pi3-profile-matrix
# Compare equivalent successful RISC-V/Pi3 logs by assertion identity and count:
python3 scripts/compare-native-coverage.py riscv-serial.log pi3-serial.log \
  --out native-parity.json
# Use an ordinary, uninstrumented image for terminal rendering assertions:
RTMUX_TTY_PI3_OUT=build/pi3 python3 scripts/vi-test.py
RTMUX_TTY_PI3_OUT=build/pi3 python3 scripts/tty-jobctl-test.py
```

Coverage evidence is in `OUT/pi3-coverage/coverage.json` and
`OUT/pi3-coverage/boot-N/{serial.log,results.json}`. The profile target writes
`OUT/pi3-profiles/profiles.json` and a separate log/config/image tree per profile.
The CI workflow runs architecture diagnostics, shared ABI/kernel/driver/host
checks, the three-boot gate, the profile matrix, and interactive terminal tests.
Local execution is not a
claim that the remote GitHub workflow has run.

The local acceptance run passed three cold boots and four full-suite executions
(including romfs), each with the same 1,398 passing assertion identities as the
four-CPU RV64 reference. The other profiles passed 1,365 (`minimal`), 1,385
(`realtime`) and 1,376 (`lkl`) assertions; the 4 KiB log-ring fixture passed six.
Supplemental teardown stress passed 100,000 detach iterations with debug queue
checks enabled. The ordinary image passed ten native smoke checks, 52 shared
login/vi checks and 12 job-control checks. Local evidence is indexed in
`build/pi3-parity-v3/validation/summary.json`; reports retain separate image
hashes and record the final account-fixture correction. These results establish
the existing coverage baseline within the scope below.

The separate [architecture diagnostic](pi3-bringup.md) still checks MMU
permissions, TLB remapping, timer routing, mailbox IPIs and fault handling with
a small static fixture. `make check-pi3-bringup` also requires a timer-disabled
negative image to fail at the expected stage. This diagnostic is separate from
the full native runtime acceptance above.

## Implementation

The kernel executes at `0xffffffc000080000` in EL1; userspace executes at EL0
with a separate TTBR0 page table. Startup handles QEMU's EL2 entry, creates the
high mapping, publishes the syspage, and releases secondary CPUs through the
Pi spin table. The BCM2836 local controller routes the architectural virtual
timer and mailbox-0 IPIs. PL011 supplies the boot console and the native
`dev-serial-pl011` user-space serial service. The BCM2835 peripheral controller routes timer compare-3 and
PL011 IRQs to CPU0, with masking, unmasking, notification and IST delivery.
Console polling yields to an attached PL011 IST and resumes after it detaches;
console buffer writes serialize kernel diagnostics with user output.

The syscall ABI passes six arguments in x0–x5, the call number in x8 and the
result in x0 through `svc #0`. Exception entry preserves x0–x30, SP, PC, PSTATE,
TLS, all 32 SIMD registers, FPSR and FPCR. Kernel C uses `-mgeneral-regs-only`.
Native libc startup, atomics, setjmp and the eager AArch64 dynamic relocator
complete the path from the kernel ELF loader to ordinary user programs.

Common-code defects encountered during native validation are fixed:
zero-bias dynamic-loader address arithmetic no longer adds offsets to a NULL
pointer, and stack reclamation preserves a normal exiting thread's joinability.
Joinability is exercised by joining workers after they have returned. Idle
selection now preserves the IRQ mask through run-queue unlocks; otherwise a
timer could interrupt between selecting and installing the active thread,
stranding a RUNNING thread outside the queues. AArch64 checks this invariant
on each idle selection and masks interrupts through the final return to EL0.
Process termination also bypasses the IPC UNBLOCK acknowledgment while keeping
transfer-claim and channel-lock coordination. Otherwise the last exiting thread
can remain reply-blocked on a request (such as `_PROC_EXIT`) that the server
will never answer, leaving the parent's `waitpid()` blocked. The runtime test
holds a child's IPC request unanswered while its sibling exits, then verifies
that process teardown and `waitpid()` complete; this is repeated 16 times.

Namespace stress also exposed two shared I/O-service cleanup defects. Deferred
opens now replay with a separate context while the original combined request
finishes its stat/close operations. Failed combined operations roll back the
new binding before the client can reuse its connection ID. The focused guest
regression reproduces the old failure, and a host test of the real dispatcher
checks eight success/error/cleanup outcomes, including a negative old-code run.

Extended IPC teardown stress exposed shared SMP hand-off defects. Deferred
message errors now recover the original errno and control bits before replay.
Receive completion and abort publish queue ownership under the channel lock,
and abort rechecks the pending block transition after acquiring that lock.
Block-and-ready modifies the dispatch queue whose lock it holds. Last-link
connection cleanup serializes server-vector removal with attachment. Host
regressions execute these paths and reject the former error, ownership and
dispatch behavior; the extended guest run uses 10,000 detach iterations per
channel instead of the shared suite's normal 100.

Tracing and peripheral masking preserve the caller's IRQ state, including when
invoked from scheduler critical sections or hard IRQ context. The trace append
host regression exercises 16 buffer/control paths and rejects the former
unconditional-enable behavior. Raw SMP spin unlocks publish with release
ordering. Trace programs reset their class selection before starting a new
capture, so a preceding all-class capture cannot crowd IRQ records out of the
ring. Startup reports the architectural counter frequency for the existing
CSV/CTF trace exporters.

The shared `/sys/log` marker test reads the recent 64 KiB when the ring is full,
so concurrent loader cleanup can append diagnostics without invalidating its
very oldest byte before the first read. It retains the marker content, ordering,
read-size, rewind and permission assertions without retrying failures. The
separate small-ring wrap test exercises full-ring reads and `EOVERFLOW`.

The short-burst CPU-time calibration runs its control and spin blocks on one
CPU, then restores the caller's affinity. Its original accuracy threshold is
unchanged. Separate two-thread and yield/steal tests retain migration and SMP
billing coverage; every calibration round reports its error and tolerance.

## Scope and remaining limitations

This is a QEMU Pi 3B native runtime port. Physical Raspberry Pi boot has not
been validated. Its SD-card firmware/boot packaging, cache maintenance on real
hardware and device drivers still need board testing. There is no Pi SD, USB,
Ethernet or GPU driver in this port. CPIO is the read-only boot filesystem,
`/dev/shmem` provides native shared memory, and a RAM block service supplies the
read-only romfs mounted at `/usr`.

The present common process manager starts `LC_EXEC_SWAP` but has no completion
path for the image swap. A successful `exec()` can hang; init therefore stays
resident and uses normal spawn/wait to run and restart the shell. This port does
not claim complete POSIX conformance or execution of arbitrary Linux/AArch64
binaries. Shared syscall coverage does not imply support for the RISC-V board's
NVMe/PCI, FU740 GPIO or RISC-V IOMMU hardware, eight-CPU scaling, or real-board
latency guarantees. Hardware debugger breakpoints,
complete ET_REL relocation support, PMU counters and static ELF TLS remain
outside this port's verified coverage. Spawn/wait and filesystem-backed program
loading are covered.

The TLS distinction matters when comparing with RV64: libc's per-thread
`errno`, RTMux TLS and pthread keys are covered, but compiler-generated
`__thread` / `_Thread_local` objects require the ELF TLS ABI and loader/thread
integration. Both architectures currently share `lib/ldso/machine/tls.h`'s
global `_ldso_tcb_ptr`; this is not complete per-thread ELF TLS. Successful
`exec()` and standard ELF TLS need new shared positive tests on both targets,
in addition to passing the existing RV64 conformance baseline. Passing that
baseline alone must not be described as completing those two features.

The later RT-kernel + Linux multikernel/virtio design is a separate milestone;
this configuration runs only RTMux.
