# Boot Capability Manifest

RTMux derives bootstrap service privilege from the static roottask manifest in
`servers/roottask/sys/bootstrap-manifest.c`. `docs/loader-policy.md` records the
executable-image validation boundary. The loader grants privilege only when the
opened executable is on the read-only boot cpio, its resolved absolute path
matches an entry, and its image hash matches that entry. Unlisted images start
with `priv_caps == 0`.

The manifest below is policy only (which path gets which caps). The per-image
hashes are not hand-edited: they are **generated** at build time from the shipped
binaries into `boot-manifest.hashes` at the boot-cpio root, and roottask loads
that file (via cpiofs, fail-closed) at boot for the provenance check. See
`docs/boot-manifest-hash-generation.md`. So the "image hash" column reads
`generated` rather than a literal.

| service | exec path | reserved endpoint/channel | granted caps | dependencies | lifecycle owner | image hash | withheld-capability audit |
| --- | --- | --- | --- | --- | --- | --- | --- |
| roottask | bootstrap grant, no exec-path match | bootstrap process namespace | ALL | kernel-created initial process | kernel bootstrap | not path-hash matched | withholds no private caps while coresrv/namesrv remain monolithic roottask code |
| coresrv | monolithic roottask component, no exec-path match | process, vm, path, and bootstrap IPC services | PROCESS, VM, PTE, PA, IPC, NOTIFICATION, THREAD, CRED | roottask boot image | roottask process | not path-hash matched | withholds SYSTEM, DEBUG, TIME, TRACE, SCHED, IRQ when split into a spawned service |
| namesrv | monolithic roottask component, no exec-path match | namespace IPC service channel | IPC | roottask boot image | roottask process | not path-hash matched | withholds SYSTEM, VM, PTE, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED, IRQ when split into a spawned service |
| init | /sbin/init | init script process | none | boot cpio image | kernel bootstrap and roottask spawn | generated | withholds all private caps |
| logger | /sbin/logger | logging service channel | none | boot cpio image | user-mode init | generated | withholds all private caps |
| virtio-mmio | /sbin/virtio-mmio | VirtIO-MMIO registry channel | none | boot cpio image, VirtIO-MMIO hardware | user-mode init | generated | withholds all private caps |
| romfs | /bin/romfs | mounted romfs path service | none | boot cpio image, block fd | user-mode init | generated | withholds all private caps |
| pipe | /bin/pipe | pipe path service | none | boot cpio image | QEMU regression and interactive init modes | generated | withholds all private caps |
| waitfor | /bin/waitfor | readiness probe process | none | boot cpio image | user-mode init | generated | withholds all private caps |
| getty | /sbin/getty | console login process | none | boot cpio image | user-mode init | generated | withholds all private caps |
| dev-nvme | /bin/dev-nvme | NVMe IRQ endpoint | IRQ | boot cpio image, PCI/NVMe hardware | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| dev-serial-8250 | /bin/dev-serial-8250 | 8250 UART IRQ endpoint | IRQ | boot cpio image, UART hardware | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| dev-serial-pl011 | /bin/dev-serial-pl011 | PL011 UART IRQ endpoint | IRQ | boot cpio image, UART hardware | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| dev-serial-sifive | /bin/dev-serial-sifive | SiFive UART IRQ endpoint | IRQ | boot cpio image, UART hardware | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| pci-server | /sbin/pci-server | PCI IRQ endpoint | IRQ | boot cpio image, PCI hardware | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| lkl-fsd | /sbin/lkl-fsd | LKL filesystem IRQ endpoint | IRQ | boot cpio image, LKL block service | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| lkl-netd | /sbin/lkl-netd | LKL network IRQ endpoint | IRQ | boot cpio image, LKL network service | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| inject | /bin/inject | debug fault injection (CONFIG_DEBUG_INJECT builds only) | INJECT | boot cpio image, `DEBUG_INJECT` private call | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED, IRQ |
| tracelogger | /bin/tracelogger | trace drain control path | TRACE | boot cpio image, trace ring | user-mode init | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, SCHED, IRQ |
| irq-latency | /bin/irq-latency | IRQ latency endpoint and trace ring | IRQ, TRACE | boot cpio image, IRQ fixture | QEMU regression mode | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, SCHED |
| irq-notify | /bin/irq-notify | IRQ notification endpoint | IRQ | boot cpio image, IRQ fixture | QEMU regression mode | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| gpio-latency | /bin/gpio-latency | GPIO IRQ endpoint | IRQ | boot cpio image, GPIO fixture | QEMU regression mode | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |
| preempt-latency | /bin/preempt-latency | preemption IRQ fixture endpoint | IRQ | boot cpio image, preemption fixture | QEMU regression mode | generated | withholds SYSTEM, VM, PTE, IPC, CRED, DEBUG, PROCESS, PA, NOTIFICATION, THREAD, TIME, TRACE, SCHED |

Run `make check-boot-manifest` after changing this table, the bootstrap grant
loader path, or any privileged boot service.
