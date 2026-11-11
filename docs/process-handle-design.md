# RTMux Process Handle Design

Process handles are procfs files that pin a process generation.  The first
target is `/proc/<pid>/wait`; `/proc/<pid>/ctl` can follow when signaling and
control operations move off ambient PID lookup.

## Interface

Opening `/proc/<pid>/wait` resolves `(pid, generation)` in roottask and stores
that pair in the open control block.  The open file does not keep the process
runnable or unreaped forever; it keeps enough generation state to reject later
operations that would otherwise hit a recycled PID.

Reads return wait status after exit.  Operations after the process has exited
and the retained status has been consumed fail with the documented stale-handle
error instead of re-resolving `pid`.  Normal exits report the POSIX exit code;
hardware user faults are preserved as signaled wait status, including
`WIFSIGNALED`/`SIGSEGV`.

## Current Scaffold

`/proc/<pid>/wait` exists as a read-only procfs file and is advertised from
`/proc/<pid>`.  Opening it pins the full RTMux PID value, including generation.
Reads report `live` while that exact generation is running, `exited <status>`
while roottask still retains `WAITINFO`, and `stale` after the generation no
longer resolves.

## Child Creation

The race-free creation primitive returns both the child PID and an already-open
process handle.  RTMux userland creates children with `posix_spawn()`; realtime
paths should use the handle-returning primitive so a parent never needs
`spawn -> lookup pid -> open procfs`.  `rtmux_spawn_waitfd()` is the libc entry
point for that path: it performs `posix_spawn()`, immediately opens the
matching `/proc/<pid>/wait` handle, and returns both values to the caller.
`SPAWN_NOZOMBIE` is rejected because there is no retained wait status for such
children.

## Lifetime

| State | Handle behavior |
| --- | --- |
| live process | operations target the matching `(pid, generation)` |
| exited, unreaped | wait status remains readable |
| reaped | stale-handle operations fail cleanly |
| PID reused | stale handle never targets the new generation |

The module seam is roottask procfs: libc wrappers can build `waitpid()` and
future pidfd-shaped calls on top without teaching each caller PID-generation
rules.
