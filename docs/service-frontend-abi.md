# Capability-Scoped I/O Frontend ABI

This pattern is for user-mode services that publish a namespace path and answer
RTMux `_IO_*` messages through `dispatch` / `ioserv`. It is the shared shape of
`servers/lkl/server.c`, `servers/lkl-fsd`, `servers/lkl-netd`,
`userland/dev-*`, and `userland/romfs`.

## Dispatch And Publication

- Create one `dispatch_t` before acquiring device or domain resources.
- Attach service paths with `ioserv_attach()`. Directory personalities use
  `IOSERV_FLAG_DIR`; leaf devices use a normal server object.
- For LKL domains, call `rtmux_lkl_dispatch_begin()`, attach the personality
  paths, then call `rtmux_lkl_dispatch_serve()`. That detaches from the parent
  only after fallible setup is complete and runs the bounded dispatch pool.
- Native single-threaded frontends use `dispatch_context_alloc()`,
  `dispatch_block()`, and `dispatch_handler()` directly.

The namespace advertises reachability only. Authority comes from the opened
connection, process credentials, delegated hardware resources, and any
transferred endpoint caps, not from spelling a global path.

## Open State

`open` allocates one OCB per client handle and binds it with
`ioserv_open_bind()`. The OCB owns mutable per-open state such as flags, file
position, selected partition or inode, exported text buffer, or LKL fd.

Open handlers must reject unsupported create, type, access, and write-intent
combinations before binding an OCB. A stale or closed handle must fail through
the common I/O-service binding path, normally as `EBADF` or the server's precise
errno.

## I/O Framing

Handlers consume the fixed `_IO_*` header from `ioserv_context_t::msg`.
Additional request payload is copied with `ioserv_msgread()` /
`ioserv_msgreadv()` after validating size, offset, xtype, and access mode.
Replies use `IOSERV_PTR()`, `IOSERV_NPARTS()`, `msg_reply()`, or
`IOSERV_NOREPLY` according to the existing ioserv contract.

`read` and `write` must clamp or reject client sizes before copying. `pread` /
`pwrite` style requests use `_IO_XTYPE_OFFSET`; normal requests update the OCB
position only after a successful operation. `sync` is either a real backend
flush, as in `lkl-fsd`, or an explicit no-op for read-only/in-memory services,
as in `romfs`.

## Devctl And Native Extensions

POSIX-like control operations stay in `devctl`: flags, mount flags, statvfs, and
device-specific control records. Unknown commands fail closed with `ENOSYS`,
`ENOTTY`, or the local precise errno.

LKL fast paths use `_IO_NATIVE` messages only after checking the native header
version and cap-transfer count. The native operation still resolves authority
from the opened OCB and any transferred caps; it must not bypass open-time
access checks.

## Capability Delegation

Hardware-facing frontends obtain authority before publication. Examples include
PCI BAR/MSI claims in `dev-nvme`, delegated virtio/MMIO resources for LKL net,
and block-device fds for `lkl-fsd` / `romfs`. If setup fails, the service exits
without publishing a healthy path.

Delegated resources are owned by the serving process generation. On crash or
exit, roottask and the kernel reclaim process-owned channels, connections,
endpoint caps, and hardware/resource claims. A restarted generation must
reacquire them; it must not inherit stale client authority from the old
generation.

## Stats And Diagnostics

Long-lived services should expose a text stats/control path when the data is
needed for budgets or failure diagnosis. Existing examples are `/stats`,
`/net/stats`, `/ctl`, `/mounts`, `/sys/ioserv-accounting`, and device-specific
`devctl` records. Counters must be snapshots or monotonic totals; wrapped or
ambiguous accounting is worse than no counter.

I/O-service frontends classify reply errno values into the shared
`ioserv_reply_status_t` taxonomy:

| Status | Errno inputs | Disclosure rule |
| --- | --- | --- |
| `OK` | `EOK` | Operation succeeded. |
| `DENIED` | `EACCES`, `EPERM` | Capability, credential, or policy denial. Fail closed. |
| `UNAVAILABLE` | `ENOENT`, `ENODEV`, `ENXIO`, `EBADF`, `ESRCH`, `EPIPE` | Resource cannot be used or named by this caller. |
| `TIMEOUT` | `ETIMEDOUT` | Bounded wait expired. |
| `BAD_REQUEST` | `EINVAL`, `EBADMSG`, unknown errno | Malformed or unsupported request framing. |
| `IO_FAILURE` | `EIO`, `ENOSPC`, `EROFS` | Backend or media refused the requested I/O. |
| `FAULTED` | `EFAULT` | User-copy or peer-buffer fault. |
| `BUSY` | `EBUSY`, `EAGAIN` | Queue, endpoint, or backend is temporarily saturated. |

Information-flow rule: if the caller lacks authority to name a resource,
frontends must return `DENIED` or `UNAVAILABLE` without distinguishing "exists
but forbidden" from "missing" through a more specific status. Precise errno may
still be returned to an already-authorized open handle.

The canonical monotonic counter names are:

- `ioserv_sends`
- `ioserv_receives`
- `ioserv_denied_sends`
- `ioserv_timeouts`
- `ioserv_queue_full`
- `ioserv_max_depth`

The common dispatch path updates receive and reply counters with O(1) relaxed
atomics. `ioserv_max_depth` is zero unless a frontend with its own queue feeds a
depth sample. Accounting is non-authoritative: it must not decide access.
Services that expose a stats file can append `ioserv_reply_counters_format()`
output; `lkl-fsd` `/stats` and `lkl-netd` `/net/stats` do so. The formatter
returns `ENOSPC` instead of publishing a truncated counter block.

## Crash Cleanup

Restart policy belongs to a user-mode supervisor. A crash test must prove three
things:

- the health path drops or is explicitly detached before the new generation is
  accepted;
- old handles fail closed (`EBADF`, `EPIPE`, timeout, or a documented local
  errno);
- the replacement process republishes the health path after reacquiring its own
  resources.

See `docs/server-restart-contract.md` for service-specific restart eligibility
and gates. Do not add kernel respawn policy to satisfy this contract.
