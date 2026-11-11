# POSIX Over Capabilities Boundary

RTMux keeps the kernel ABI capability-native. POSIX is a user-mode contract built
by libc and servers on top of endpoint, process, memory, and namespace
capabilities. Kernel calls must not grow a global path namespace, uid shortcut,
or guessed integer authority to make POSIX convenient.

## Mapping

| POSIX concept | RTMux authority |
| --- | --- |
| File descriptor | Process-local endpoint capability or fd-vector entry. The integer is only a local handle. |
| `dup`, `dup2`, `fcntl(F_DUPFD)` | Derived fd for the same open object, with no rights amplification. |
| `open` | Libc asks the namespace service to resolve a path using the process namespace grants. Success returns an fd/cap. |
| `openat`, `fstatat`, `faccessat` | Relative lookup through a directory fd capability; `AT_FDCWD` uses process cwd state. |
| cwd | Process state: directory capability plus display label, updated by `chdir`. |
| Absolute path | Lookup through the process root namespace capability, not a kernel global namespace. |
| `chroot` | Monotonic narrowing of the process root; cached cwd/dirfd path labels are invalidated. |
| `pipe` | Pair of endpoint-backed fds. |
| `posix_spawn` file actions | Explicit fd inheritance, close, dup, and open actions applied by roottask/libc. |
| Environment | Process image state copied at spawn/exec. It carries data, not authority. |
| Signals, wait, job control | Libc/proc-server policy over process handles and notification endpoints. |

## Namespace Composition

A process namespace is a composed view of explicitly granted roots. Grants are
monotonic: a child can inherit, close, or narrow a grant, but cannot widen it
without an explicit parent or supervisor transfer.

Required grant classes:

| Grant | Purpose |
| --- | --- |
| cwd/root directory | Relative and absolute POSIX path lookup. |
| read-only directory | `/bin`, `/etc`, `/sys`, and immutable device views. |
| writable directory | Scratch state such as `/dev/shmem` or supervised service-private trees. |
| curated executable tree | Spawn/exec inputs; no ambient whole-root execution. |
| optional network tree | `/net/...` only for processes granted network service endpoints. |

Forbidden paths fail closed with normal POSIX errors such as `ENOENT`, `EACCES`,
`EROFS`, or `EBADF` depending on where resolution or rights validation fails.
The kernel remains unaware of POSIX path strings.

## Child Inheritance

`posix_spawn` is the boundary where POSIX process state is made explicit:

- fd 0/1/2 are inherited only if present and not close-on-exec;
- file actions close, duplicate, or open named fds before the child commits;
- cwd/root namespace state is inherited after any monotonic narrowing;
- environment and argv are copied as data;
- wait authority is returned as process/wait state, not as pid ownership.

## Regression Gate

`scripts/qemu-regress.sh --mode posix-cap-boundary --timeout 180` boots RTMux and
runs `syscall_testing --posix-cap-boundary`. The test covers fd 0/1/2, `openat`,
`dup2`, `pipe`, `posix_spawn` plus wait, cwd-relative lookup, and a child with a
narrowed root that can open permitted paths while paths outside that root fail
closed.
