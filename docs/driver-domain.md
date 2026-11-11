# Native Driver-Domain Contract

This is the §10 import seam for native driver domains.  It is a contract for
future vendor/HAL code; the current in-tree drivers remain local RTMux code.

## Import Boundary

- Vendor code lives under its own subtree and includes one local OSAL header
  before any vendor header.
- Warning suppressions are scoped to that subtree's Makefile.  RTMux code keeps
  the normal warning-clean build.
- Vendor headers must not redefine RTMux compiler, type, endian, atomic, or
  assert macros.  Guard those names in the OSAL header before including vendor
  headers.
- Shared helpers live once in RTMux code, not inside each driver: power-of-two
  math, bounded rings, and 32-bit atomic load/store wrappers.

## Authority

- MMIO, IRQ, DMA, block fd, and namespace binds must arrive as explicit
  delegated resources.  A driver must not self-scan ambient physical memory or
  publish a device node before its delegated resources are valid.
- IRQ handling follows `docs/irq-contract.md`: kernel hard-IRQ paths only claim,
  mask, and record; user-mode ISTs acknowledge device state, drain bounded work,
  wake waiters, and unmask.
- Revoke order is mask IRQ, stop DMA or bus mastering, reset device state where
  possible, then release mappings and namespace binds.

## Bring-Up State

Every native driver records its clock, power, reset, IRQ, DMA, and publish steps
as an explicit checked sequence.  A failed step unwinds earlier resources and
must not leave a usable device node behind.

Current drivers satisfy the contract without a vendor seam:

| Driver | Contract evidence |
| --- | --- |
| `userland/dev-nvme` | PCI resources come from `pci-server`; BAR0 is mapped only from the typed PCI BAR handle returned by attach, BAR/IRQ details are logged and smoked by `pci-nvme-smoke`; polling mode is explicit until interrupt completions land. |
| `userland/dev-serial-8250` / `dev-serial-sifive` | MMIO records are delegated through `/dev/virtio-mmio`; IRQ setup and IST order are recorded in `docs/irq-contract.md` / `docs/driver-review-checklist.md`. |
| `servers/pci` | Maps ECAM/DBI only from typed boot MMIO records and delegates narrowed BAR/IRQ data to device drivers. |

## Deferred Gates

Add these only with the first real imported vendor subtree:

- a vendor-header redefinition probe;
- a Makefile check that third-party warning flags do not leak to RTMux objects;
- the smallest driver smoke that fails on wrong clock, power, reset, IRQ, or DMA
  order before publication.
