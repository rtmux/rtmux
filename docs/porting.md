# Porting to RTMux

RTMux is a capability microkernel with a POSIX userland built on top. This guide
is for someone bringing an existing POSIX program: what to use instead of the
interfaces that are deliberately absent, and what behaves differently. The
claimed profiles and per-interface status are generated in
`docs/posix-compliance.md`; this page covers what that table does not say.

## Process creation

There is no `fork`. Start a child with `posix_spawn`, or `posix_spawnp` for a
`PATH` search; `exec*` replace the calling process. `posix_spawn` file actions
(`adddup2`, `addclose`, `addopen`) set up the child's descriptors, and the
spawn attributes set its scheduling policy, process group, and signal masks.
Descriptors without `FD_CLOEXEC` are inherited. libc lists them and roottask
validates the list afterwards, so a thread in another part of the program that
closes one of them during the call can make the spawn fail with `EBADF`. Create
pipes shared with children close-on-exec and keep them open across the spawn.

## Profiles and what is claimed

The matrix in `docs/posix-compliance.md` is the source of truth. An interface
not listed there is not claimed; an unclaimed interface fails closed with the
documented errno, or is absent from libc entirely (see the omission table).

## Dynamic linking

Programs link against `libc.so` and start through `ld-rtmux.so`. A set-id image
runs the loader in secure mode and ignores `LD_*` overrides (`issetugid()`
reports it; it finds the auxiliary vector through `environ`, so a start file
must set `environ` to the initial envp before calling `__rtmux_syschan_init()`,
as `crt0.S` does). The loader identifies a library already loaded by name, and
by device and inode only when the filesystem reports a non-zero inode (the
initramfs reports 0 for every file). `dlsym` is provided by the loader library, not libc, and shared
objects must carry a hash table the loader understands.

## Filesystem differences

Names resolve through roottask's namespace and servers, not a single kernel
filesystem. The initramfs is mounted at `/`; do not assume a writable `/tmp`.
`/dev/shmem` is the writable scratch area. Raw block devices under
`/dev/nvme*` are root-only. `/sys/kstats` and `/sys/version` are read-only
status files; `/sys/log` (mode 0400, root only) is roottask's log ring.

## Signals

Handlers do not interrupt the target thread. libc runs them on a per-process
system thread, so a handler runs concurrently with the main thread, at the
process's starting scheduling priority. `SA_SIGINFO` handlers receive
`si_signo`, `si_code`, and `si_pid`, with no interrupted-context argument. A
caught signal does not yet interrupt a blocked call with `EINTR`; code that
relies on that needs a different wakeup (a timeout or a notification).
Stop and continue signals are treated as ignored: there is no job control.

## Memory

Mappings are populated eagerly: `mmap` allocates and maps every page up front
and there is no demand paging, so a large mapping costs its full size at call
time and fails with `ENOMEM` if physical memory is short. malloc grows its heap
by mapping 256 KiB chunks and maps requests of 128 KiB or more directly.
malloc is not async-signal-safe.

## Omitted interfaces

These interfaces are absent from libc by design. The gate
`make check-porting-contract` compiles a probe for each and requires a link
failure, so adding one without updating this table fails the build.

| interface | alternative | expected outcome | fixture |
| --- | --- | --- | --- |
| `fork` | `posix_spawn` or `posix_spawnp` | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `brk` | `malloc`, or `mmap` for a large region | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `sbrk` | `malloc`, or `mmap` for a large region | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `link` | `symlink`, or copy the file | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `regcomp` | a bundled regex library | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `regexec` | a bundled regex library | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `regerror` | a bundled regex library | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `regfree` | a bundled regex library | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `system` | `posix_spawn` of the shell with `-c` | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `popen` | `pipe` plus `posix_spawn` with `adddup2` | undefined reference at link time | `scripts/check-porting-contract.sh` |
| `pclose` | `waitpid` on the spawned child | undefined reference at link time | `scripts/check-porting-contract.sh` |
