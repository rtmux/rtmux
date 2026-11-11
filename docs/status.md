# Status and Roadmap

Version 0.11. RTMux boots to a multi-user login on QEMU virt (up to 8 harts) and
on a SiFive HiFive Unmatched (FU740), where it sustains repeated process
spawn/exit. Claims about POSIX coverage, realtime budgets, and LKL behavior are
backed by generated evidence rather than this page:
[posix-compliance.md](posix-compliance.md), [rt-budgets.md](rt-budgets.md),
[lkl-domain.md](lkl-domain.md).

## What works

**Kernel**
- SMP boot via SBI HSM; no big kernel lock (fine-grained per-object locks).
- Synchronous IPC, channels and connections, capability table, RCU-sched.
- Process lifecycle across CPUs: spawn, run, exit, address-space teardown.
- POSIX signals delivered through notification-backed process control; every
  process has a system thread for notifications.
- Real pthread mutex/condvar/TLS via the RISC-V `tp` register.
- Runtime linker (`ld-rtmux.so`) that self-relocates with zero PLT imports.
- Kconfig-based configuration (`make menuconfig`).

**Userland and services**
- Multi-user login: toolbox `getty`/`login` with `crypt(3)` SHA-512 against
  `/etc/shadow`, uid/gid drop, and the shell from `/etc/passwd`.
- toolbox `sh` with scripts, builtins, redirection, pipelines, and
  `spawn`-based startup; dynamically linked applets (`ls`, `cat`, `ps`, `top`,
  `vi`, `mount`, `dmesg`, `shutdown`, `setconsole`, ...).
- Filesystem layout: cpio at `/`, `/usr` on NVMe partition 8 (`romfs`), `/etc`
  symlinked to `/usr/conf`, on-disk `levelN.sh` init stages, and a read-only
  `/sys` (`cmdline`, `version`, `board`).
- Path resolution through the full I/O-service protocol, including symlinks at
  the cpio layer.
- Drivers: interrupt-driven `dev-serial-8250` (QEMU), `dev-serial-sifive`
  (FU740 native UART), and read-only `dev-nvme` with GPT. Both UART drivers
  provide canonical-mode line discipline and `tcsetattr`.
- Stay-resident I/O services (`dev-serial-8250`, `dev-nvme` call
  `rtmux_detach()` after initialization).
- `shutdown` powers off; `shutdown -r` reboots.

**Hardware**
- QEMU virt: SMP with interrupt-driven console.
- HiFive Unmatched: bare-metal shell on the native UART, NVMe + `romfs`, SMP
  spawn/exit loops.

## Roadmap

- Root-cause the kernel-to-kernel `MsgRead` `EFAULT` on slab-allocated kernel
  memory (same MMU-replication bug family as fixed cases on thread stacks).
- Writable `romfs` (mounts are read-only today).
- Read-only `ext4` driver so RTMux can read Linux-formatted partitions without
  `mkromfs`.
- `time()` via the Unmatched's DA9063 RTC.
- `utmp`/`wtmp`/`lastlog` on an in-memory `tmpfs` so sessions appear in `who(1)`
  and `last(1)`.
