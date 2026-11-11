# Server DMA buffer isolation

High-risk server input buffers are the DMA-visible rings that a device or a
remote peer writes: the NVMe queues in userland/dev-nvme and the virtio rings in
the LKL host backend. This note records how those buffers are page-isolated and,
just as important, where the userspace VM cannot help so no false protection is
claimed.

## Primitive: userspace mprotect is real

A user-mode server can narrow a mapping's permissions after it is filled.
mprotect (lib/libc/posix/mman/mprotect.c) reaches the roottask address-space
manager (servers/roottask/mem/as/mprotect.c, as_mprotect), which issues a
permission-only PTE update (PTE_OP_PROT) that preserves the physical address and
rewrites only the permission bits. A normal MAP_ANON mapping can therefore be
mapped writable, filled, and then flipped read-only for the rest of its life.
The tests/userland syscall_testing suite proves the enforcement is real: the
mprotect-read-only-rejects-write case maps a page writable, fills it, flips it to
PROT_READ, and confirms a later write faults with SIGSEGV.

## What the page table cannot do: stop device DMA

A device writes DMA-target memory by physical address. That path does not walk
the CPU page table, so dropping host write permission on a DMA buffer does not
stop the device from writing it. mprotect on a DMA ring guards against a stray
host CPU write, not against the device. This is a hardware property, not a
missing VM primitive, and it bounds what page isolation can claim for these
buffers.

## NVMe rings (userland/dev-nvme)

Every ring is one mmap of a single 4 KiB page (page-aligned by construction,
PROT_NOCACHE for DMA coherency, never executable). The isolation splits by who
writes each ring after setup:

- Completion queues (admin ACQ, I/O IOCQ) are host-read-only for their whole
  operational life. The driver only reads completion entries and rings the
  completion-queue-head doorbell in BAR0 MMIO; the controller DMA-writes the
  entries. After the one-time zero at setup, both queues are mprotect'd to
  PROT_READ plus PROT_NOCACHE (dropping host write permission but keeping the
  cache attribute), so a stray CPU write into a completion ring faults instead
  of silently corrupting completion state. See nvme_setup_admin_queues and
  nvme_create_io_queues.
- Submission queues (admin ASQ, I/O IOSQ) stay writable because the driver
  writes command entries into them on the submission path. Read-only is not
  applicable here.

Caveat: if a future controller-reset path reuses a live queue page and re-zeroes
a completion queue in place, it must first mprotect the page writable again or
reallocate it, otherwise the re-zero faults.

## Virtio rings (servers/lkl host backend)

The virtio rings (servers/lkl/virtio.c) are page-aligned, PROT_NOCACHE, never
executable DMA allocations. Modern split rings use separate 4 KiB pages for the
descriptor, available, and used rings; legacy rings use one combined contiguous
allocation, rounded up to a page, with those three regions at offsets. They are
left mapped writable and are not flipped read-only during operation: the LKL
virtio core drives their layout and both the host and the device touch them
throughout a queue's life, so there is no filled-then-parsed window to lock down
without racing the virtio state machine. The protection here is limited to the
existing page alignment and the no-execute mapping. The lkl-netd control text
buffers are small fixed struct members, not DMA-visible, and are out of scope.
