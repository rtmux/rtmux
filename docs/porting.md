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

`ARG_MAX` is 98,304 bytes (96 KiB) and is what `sysconf(_SC_ARG_MAX)` returns.
It counts every argument and environment string with its terminating NUL, plus
one pointer for each string and for the null pointer that ends each of the two
vectors. `posix_spawn`, `posix_spawnp` and the `exec*` functions fail with
`E2BIG` when the list is larger, before anything is sent, and roottask refuses
an oversized request from a client that skips the check. The arguments are
placed on the new program's stack, which is enlarged by their size, so they do
not reduce the stack the program asked for. A spawn within the limit can still
fail with `ENOMEM`: roottask copies a `posix_spawn` request into its fixed heap
while it starts the child. Large requests are admitted in turn against half of
that heap, and may wait there or fail with `EAGAIN` when too many queue, but
the heap is shared with everything else roottask allocates, so a request can
still meet a heap the rest of roottask has filled.

## Shell command lists

The toolbox shell parses command lists before expanding or executing them.
Nested conditions, case arms, function bodies and trailing redirections share
the grammar in `userland/utils/sh-parse.c`. Patterns carry a separate quote
mask so every non-NUL byte remains available as data. The guest `sh-posix` suite exercises
compound redirections and nested grammar. Its `gaps.sh` cases also run under dash.

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
caught signal whose handler was installed without `SA_RESTART` ends an
`open()` or a `read()` that a service is holding with `EINTR`. An `open()`
with `O_CREAT` or `O_TRUNC` is the exception: once sent it is not
interrupted, because the server may already have created or truncated the
file, and `EINTR` would say that nothing happened. A service that holds such an
open for good (a FIFO opened with `O_CREAT` while no reader comes) therefore
keeps the caller until it answers: open an existing FIFO without `O_CREAT`, or
end the wait with `pthread_cancel`, which still applies. Other blocked calls are not
yet interrupted; code that relies on that needs a different wakeup (a timeout
or a notification).
Stop and continue signals are treated as ignored: there is no job control.

## Memory

Mappings are populated eagerly: `mmap` allocates and maps every page up front
and there is no demand paging, so a large mapping costs its full size at call
time and fails with `ENOMEM` if physical memory is short. malloc grows its heap
by mapping 256 KiB chunks and maps directly any request that, with its 8-byte
header, does not fit a 128 KiB block (131,065 bytes or more).
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
