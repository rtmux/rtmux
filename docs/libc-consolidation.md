# libc / roottask Seam Consolidation

This is the §10 task-2 tracking artifact. It maps each libc group to one of
three roles and records the single owner of each cross-cutting facility:

- C11/POSIX body — OS-neutral POSIX implementation; should compile under
  `-std=c11` (see the C11 build-mode row below).
- RTMux seam — the thin syscall/channel/TLS glue that is deliberately
  RISC-V/RTMux-specific; kept behind internal seam headers.
- Deleted residue — QNX-shaped code removed in the §10 trim.

A §10 task-2 sub-item closes only when it has an owner note here,
a focused patch (or a "no change needed, already single-owner" note backed by
evidence), and the shared check list passes:
`make -C lib/libc CFLAGS+=-std=c11`, `make -Bj`, `make initramfs`,
`make check`, `make check-posix-matrix`,
`scripts/qemu-regress.sh --mode syscall-testing --timeout 180`.

## Single-owner map

| Facility | Owner (definition) | Role | Status |
| --- | --- | --- | --- |
| `errno` | libc owner: `__get_errno_ptr()` in `lib/libc/rtmux/tls.c:20`; `<errno.h>` is `#define errno (*__get_errno_ptr())` over `LIBC_TLS()->__errptr` | RTMux TLS seam | Single owner within libc. A separate early-boot `int errno` + fallback `__get_errno_ptr()` lives in `arch/riscv/startup/entry.c:187-209` for the pre-TLS startup path; it is not a libc duplicate but the seam should stay documented. |
| `environ` | `char **environ` in `lib/libc/stdlib/getenv.c:11`; every other site is `extern` | C11/POSIX body | Single owner verified. |
| stdio streams | `__sF[3]` plus `stdin`/`stdout`/`stderr` (and BSD `__std*p` aliases) in `lib/libc/stdio/findfp.c:36-47` | C11/POSIX body | Single owner verified. |
| `malloc` | exactly one public `malloc()` in `lib/libc/alloc/malloc.c:18`; allocator internals in `lib/libc/alloc/malloc-core.c` | C11/POSIX body | Single owner verified. (`lib/libc/ioserv/link-alloc.c` is not a second allocator — it allocates I/O-service link records via `calloc()`.) |
| pthread TCB / TLS | `struct _thread_local_storage` in `include/sys/storage.h:19`; accessor `__tls()` / `LIBC_TLS()` in `lib/libc/rtmux/tls.c:15` | RTMux TLS seam | Single owner verified. |
| spawn attrs | opaque `posix_spawnattr_t` = `uintptr_t` (`include/spawn.h:29`); internal `_posix_spawnattr_t` defined once in `lib/libc/spawn/posix-spawnattr.h:80`; handle codec `set_attrp`/`get_attrp` and the storage owner `_posix_spawnattr_t_realloc` (`posix-spawnattr-support.c:145`) | C11/POSIX body | Single owner verified. |
| spawn file actions | opaque `posix_spawn_file_actions_t` = `uintptr_t` (`include/spawn.h:33`); internal `_posix_spawn_file_actions_t` defined once in `lib/libc/spawn/posix-spawn-file-actions.h:50`; handle codec `get_factp`/validation `valid_factp` in `posix-spawn-file-actions-support.c` | C11/POSIX body | Single owner verified. |

Method: each row was confirmed by grepping for non-`extern` definitions of the
storage/accessor across `lib/libc/`, `include/`, and `internal/`; each has
exactly one definition. These groups are already consolidated in code — the
remaining work per sub-item is the check-list pass (gated on the C11 flip
below), not a consolidation patch.

## fd / path operations and direct-fd I/O — DONE

Single owner: there is no separate libc fd→connection table. A file descriptor
*is* the kernel connection id (coid). Already-open fd operations dispatch
through that fd/coid using the appropriate connection syscall — `read()`/
`pread()` use `msg_send(fd, …)`, `write()`/`pwrite()` use vectored
`msg_sendv(fd, …)`, `fstat()` uses `msg_sendnc(fd, …)`, `close()` sends
`_IO_CLOSE` then detaches — but in every case the fd is passed straight through
as the `__coid` of the kernel MsgSend (`msg_send(int __coid, …)`,
`internal/include/sys/rtmux-core.h:500`; `dup.i.info.coid = fd` in
`lib/libc/posix/fcntl.c:50` makes the identity explicit). A few `fcntl()`
commands act on connection state rather than the data channel
(`F_GETFD`/`F_SETFD` via `connect_flags()`, `F_GETFL`/`F_SETFL` via `_devctl()`,
`F_DUPFD` sends `_IO_DUP` to the new fd), but all still key off the fd/coid.

The fd/path operation logic has clear, non-duplicated owners:
`lib/libc/ioserv/` (the `_IO_*` message dispatch + connection handlers);
pathname `open()` in `lib/libc/posix/open.c` → `_vopen()` → `_connect()`
(`lib/libc/support/`), which builds the `_IO_CONNECT` path-resolution request;
`lib/libc/rtmux/openfd.c` (typed-open / openfd / `/proc/<pid>/fd`); and
`lib/libc/rtmux/atfd.c` (the descriptor→absolute-path registry backing `*at`).

Direct-fd I/O bypasses roottask on the traced fast path: because the fd is the
coid, an already-open fd's I/O message goes straight to its I/O server via the
kernel MsgSend rendezvous. Roottask (the path server) participates only in
pathname resolution — `open()`, and pathname `stat()` via
`_connect_combine(path, …)` — never in the `read()`/`write()`/`fstat()`
(fd-metadata) fast path. Codex-verified: no roottask hop exists in the
already-open fd operations. This is the QNX-shape direct I/O-service fd path the
task targets, already the implemented behavior.

## getdents framing — DONE

One framing, shared by all directory backends. `include/dirent.h` defines the
single wire record: `struct dirent` with a `d_reclen` length prefix (plus the
optional QNX-style `dirent_extra`/`dirent_extra_stat` trailer). Every backend
emits that exact layout — the base name payload is
`offsetof(struct dirent, d_name) + name_len + 1`, and `d_reclen` is rounded up
to an 8-byte boundary (and, for pseudo-filesystems like `namedsem`, may be
extended to carry a `dirent_extra_stat` trailer):

- native: `servers/roottask/path/cpiofs.c:417-425` (and the other path FS
  emitters — sysfs, procfs, capfs, namedsem — use the same `struct dirent`).
- romfs: `userland/romfs/main.c:277-285`. (`struct romfs_dirent` in
  `userland/romfs/fs.h:111` is romfs's *on-disk* format, translated into the
  shared `struct dirent` on emit — not a second wire framing.)
- LKL: `servers/lkl-fsd/main.c:482+` re-frames Linux dirents into the shared
  `struct dirent` and defensively re-validates `d_reclen` (8-byte-aligned,
  non-zero, in-bounds) rather than trusting the LKL fs value;
  `servers/lkl-netd/main.c:578-604` emits `/net` directory records in the same
  framing.

The single client consumer is `lib/libc/posix/iodir.c` (`readdir` advances by
`d->d_reclen`). No backend ships its own directory wire record, so native,
romfs, and both LKL domains are already unified on one getdents framing.

## C11 build-mode flip — DONE

Goal (TODO §10 task-2): libc compiles under `-std=c11` outside the RISC-V
register-asm seam; public headers compile under `-std=c11`.

Result: libc builds fully under strict `-std=c11` — 0 errors, 0 warnings across
all 308 objects, `libc.a`/`libc.so` link. The default is flipped to c11 so any
reintroduced GNU-ism breaks the build; verify with `make -C lib/libc CSTD=c11`
(the `CSTD` variable keeps the toolchain target flags, unlike a `CFLAGS+=`
override, which clobbers `-march`/`-mabi`).

How: the only strict-c11 blocker was the GNU `asm` keyword. All bare
`asm`/`asm volatile` in the RISC-V asm code was converted to the
language-neutral `__asm__` spelling — identical to `asm` under GNU mode (so the
kernel, which includes `arch/riscv/include/cpu-inline.h`, is unchanged), but
accepted under strict `-std=c11`. That covered the 34 CSR/fence shims in
`cpu-inline.h`, the register-variable syscall bindings in
`lib/libc/riscv/syscalls.c` (`register long a0 __asm__("a0")` still binds
a0-a5/a7 correctly), and one comment in `setjmp.c`. The POSIX bodies needed no
change. Reviewed by codex + a code-reviewer: every syscall register binding,
constraint, and clobber preserved; kernel inline-asm semantics unchanged.

Because the register-asm seam also compiles under c11 via `__asm__`, libc no
longer requires GNU language mode at all. This unblocks formally closing the
single-owner rows above (their per-sub-item check list gated on the c11 build).

## To investigate

- spawn attrs / file actions single owner (`lib/libc/spawn/`,
  `posix_spawnattr_t` / `posix_spawn_file_actions_t`).
- fd/path operations single owner + direct-fd I/O bypassing roottask on the
  traced fast path (`lib/libc/ioserv/`, `lib/libc/rtmux/`).
- typed request structs shared client/server, floor-checked before variable
  payload, forged sender fields ignored (identity from kernel `scoid`/pid/cred).
- one getdents framing shared by native, romfs, and LKL.
