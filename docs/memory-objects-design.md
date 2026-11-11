# RTMux Memory Objects As Files Design

RTMux grows file-backed memory objects without replacing POSIX `mmap()` in one
step.  The first target was explicit metadata for typed memory and `/dev/mem`;
the current target adds private memory-object handles for mapping authority.

## Current Baseline

`/dev/mem`, `/dev/shmem`, and `/dev/tymem` are I/O-service files.  The
first explicit metadata surface is `DEVCTL_MEM_OBJECT_INFO`: callers can query
whether a descriptor is physical, typed, privilege-gated, resource-database
backed, and what cacheability policy the object advertises.  This keeps policy
on the file object before the mapping path consumes the descriptor.

For now `/dev/mem` reports a privileged physical aperture over the known
physical address space.  The next tightening step is to replace that single
range with an allow-list from platform/resource metadata.

The file-backed mapping path also preserves descriptor authority after the
mapping is established: a read-only shared mapping is tagged read-only, and
`mprotect()` cannot later add `PROT_WRITE`.

Current revocation coverage also checks stale user mappings after unmap.  The
VFS suite spawns a helper that maps `/dev/zero`, unmaps it, and then reads the
old address; the parent must observe `WIFSIGNALED`/`SIGSEGV`.  This is coverage
for today's PTE removal path, not yet a replacement for the future
handle-backed memory-object revoke contract.

Handle-backed memory objects now sit beside POSIX fd mappings.  A process can
derive a private memory-object handle from an already-open concrete memory
object with `memobj_handle_from_fd()`, map it with `mmap_handle()`, and revoke it
with `memobj_handle_close()`.  Handles carry explicit map rights:
`RTMUX_CAP_MEM_MAP_READ`, `RTMUX_CAP_MEM_MAP_WRITE`, and
`RTMUX_CAP_MEM_MAP_EXEC`.  Stale handles fail with `EBADF`; handles whose rights
do not cover the requested `PROT_*` bits fail before a mapping is installed.

The current handle mapping path is validated for `/dev/shmem` shared-memory
objects.  Typed-memory descriptors can mint and revoke handles, but typed-memory
page allocation remains on the existing fd `mmap()` path until its allocator is
hardened for handle-backed entry.  Raw `/dev/mem` physical aperture mappings
remain fd/privilege gated; the handle model should not become ambient physical
memory authority until platform/resource metadata can provide an explicit
allow-list of ranges, cacheability, and DMA rights.

PTE changes that remove or restrict mappings now bind revocation to arch TLB
completion.  The PTE state machine marks pending TLB work while serialized,
then the kernel drops the PTE serialization lock before running the RISC-V
completion helper.  That helper performs local `sfence.vma`, sends
`IPI_TLB_FLUSH` to peer harts, and waits until each peer increments its
completion counter after executing `sfence.vma`.

## Object Types

| Type | Backing | Required metadata |
| --- | --- | --- |
| anonymous memory | roottask memory service | size, owner, permissions |
| boot-image slice | boot/initramfs archive | offset, size, immutable flag |
| typed memory | physical memory class | class name, cacheability, allowed operations |
| device range | physical device aperture | physical address, size, cacheability, required privilege |

## Interface

Opening a memory object returns a file descriptor that can be passed to
`mmap()`.  The file object carries permission and cacheability metadata so the
mapping path does not infer policy from raw physical addresses.  Code that needs
to delegate mapping authority without keeping fd authority can derive a
memory-object handle:

```c
uint64_t h = memobj_handle_from_fd(fd, RTMUX_CAP_MEM_MAP_READ);
void *p = mmap_handle(NULL, len, PROT_READ, MAP_SHARED, h, 0);
memobj_handle_close(h);
```

This API is RTMux-specific; POSIX `mmap()`, `munmap()`, `mprotect()`, fork/spawn
inheritance, and errno behavior for fd mappings remain unchanged.

`/dev/mem` is restricted to explicitly permitted ranges.  It is not a generic
ambient authority over all physical memory.

## Realtime Constraint

No hard-IRQ, scheduler, or trap-context path may depend on server paging or
unbounded memory-object RPC.  Memory-object operations remain roottask/I/O-service
policy paths; mapped access is the hot path.
