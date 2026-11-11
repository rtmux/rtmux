# Manager Removal Plan

RTMux should use seL4-style names for concrete mechanisms and authorities
rather than generic `*mgr` names.  A `manager` name is allowed only while it
marks historical private implementation code that is being collapsed.

## Naming Map

| Current spelling | Replacement direction | Rule |
| --- | --- | --- |
| file-service shorthand | `ioserv`, then `file_protocol` internals | POSIX file I/O enters through `ioserv_*`; hidden backend names are being collapsed from filenames inward. |
| `iomgr` | `iomsg` | `_IO_MSG` dispatch uses protocol ids, not a manager namespace. |
| `pathmgr` | `namespace`, `path`, `vfs`, or `path_*` | Use `path_*` for pathname algorithms, `namespace_*` for authority-free presentation, and `vfs_*` only for file-protocol routing. |
| `procmgr` | `process`, `proc`, `spawn`, `session`, `wait` | Split by POSIX process operation.  Do not keep a broad process-manager API. |
| `memmgr` | `vm`, `address_space`, `memobj`, `page`, `memory` | Split virtual-memory, address-space, and memory-object responsibilities. |
| `sysmgr` | `roottask`, `sysconf`, `console`, or `boot` | Name the actual roottask service surface, not a generic system manager. |
| `netmgr` | removed | Network behavior belongs in LKL domains or explicit drivers, not a manager shim. |

## Removal Order

1. Hide or remove exported manager surfaces.
   Done for the former public manager surface; backend `ioserv_backend_*` and
   remaining `_ioserv_*` helper symbols are hidden from `libc.so`.

2. Rename private message classifications.
   `iomgr` becomes `iomsg`; `_IO_MSG` carries protocol ids such as PCI and the
   native RTMux file-message subset.

3. Collapse file-service implementation names.
   Keep POSIX file I/O and LKL behavior behind `ioserv_*`.  Libc-internal
   files now live under `lib/libc/ioserv/`; remaining `_ioserv_*` helper
   identifiers should collapse to `ioserv` or file-protocol names without
   changing wire messages.
   The private backend header is now `sys/ioserv-private.h`; no in-tree
   code includes the old public file-service header.

4. Split roottask modules by authority.
   Replace broad `pathmgr`, `procmgr`, `memmgr`, and `sysmgr` names with the
   resource they actually own: namespace presentation, process lifecycle,
   address spaces, memory objects, sysconf, console, or boot.
   The private pathname registration header is now `sys/path-registry.h`; no
   in-tree code includes `sys/pathmgr.h`.

5. Delete manager compatibility shims once no POSIX/LKL caller remains.
   A shim survives only when it is required by generated POSIX conformance,
   realtime tests, or LKL QEMU gates.

## Gates

Every removal step must pass:

- `make -Bj`
- `make check-indent`
- `make check-posix-matrix`
- `make check-lkl`
- `make check`
- `make check-lkl-qemu` when the change touches file I/O, drivers, PCI, or LKL

Do not use `make modpkg` for these cleanup gates.
