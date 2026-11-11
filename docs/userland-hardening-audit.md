# Userland Hardening Audit

This is the §10 ledger for userland hardening.  It records bug lessons from
comparable microkernel userlands without copying their code, and keeps the
gate: POSIX.1, native realtime, or LKL-supporting behavior only.

| Item | Status | Evidence |
| --- | --- | --- |
| Generated-header dependency tracking | Done | Commit `b53bc69a`: userland and roottask compile rules emit `.d` files with `-MMD -MP`; generated `autoconf.h` / `rtmux-version.h` edits rebuild dependent objects without a clean. |
| Common I/O-service defaults | Already satisfied | `lib/libc/ioserv/` owns dispatch defaults for connect, read/write, stat, dup, close, notify, pathconf, and native-message plumbing.  No per-server wrapper was added. |
| `_IO_DUP` fd lifetime | Done | Commit `076183a1`: `test_iodup_fd_lifetime` proves shared open-file offset and survives partial close across cpiofs, romfs-over-NVMe, and `/dev/zero`; LKL can reuse the helper from a §11 boot mode when needed. |
| Block-backed suite execution | Done | Commit `4bf380c3`: `scripts/qemu-regress.sh --mode syscall-testing-disk` boots `/sbin/init`, mounts `/usr` from the NVMe romfs image, and runs `/usr/bin/syscall_testing`. |
| PCI/NVMe smoke | Done | Commit `b8951ea9`: `pci-nvme-smoke` asserts ECAM discovery, BAR0 mapping, VID:DID read, GPT parse, IRQ/MSI vector allocation, and `mainfs=` selecting `nvme0n1p8`. |
| Sysinfo through pci-server | Deferred, YAGNI | No user utility maps PCI config space directly.  The pci-server protocol has no read-only inventory op; using attach for inspection would reinitialize live devices and fails on driver-owned NVMe.  Add a read-only query op only when a real PCI inventory consumer exists. |
| Getty/login terminal edges | Done | Commit `bd1cb343`: `scripts/vi-test.py` drives wrong-password, unknown-user, retry-limit, prompt, CR/LF, and terminal-recovery paths. |
| Shell/editor TTY smoke | Done | Commit `c0aa647c`: `make tty-smoke` drives login, `sh`, and `vi` over serial, including editing, backspace/DEL, read-only save failure, and prompt recovery. |

## Review Notes

- The PCI/NVMe smoke proves vector allocation, not end-to-end MSI-X completion
  delivery; current `dev-nvme` still runs completions in polling mode and logs
  that honestly.
- The sysinfo item stays out until it has a consumer.  Adding a read-only PCI
  query op would be new server surface, not a simplification.
- The LKL leg of `_IO_DUP` is not duplicated here; §11 LKL modes are the right
  boot context for it.
