# `/sys/kstats` — kernel resource gauges

`/sys/kstats` (mode 0444, served by roottask) exports counts of kernel
resources so a test can detect leaks. It is deliberately separate from
`docs/rt-budgets.md`, which `make check-rt-budgets` parses.

## Format

Plain text, one `key=value` per line, `value` an unsigned decimal integer.
Every key appears exactly once. If the kernel call fails the file holds a single
`soul.error=<errno>` line (plus `pa.free_bytes`) instead of the pool keys. The file is a snapshot taken at `open()`;
`lseek(fd, 0, SEEK_SET)` takes a fresh snapshot on the same descriptor, so a
test can resample without opening another connection (which would itself
change connection counts).

## Source and authority

The kernel exports the pool counters through the `SOUL_STATS` private call
(`kernel/ext/query.c:kext_soul_stats`), gated by `RTMUX_PRIV_CAP_SYSTEM` and
held only by roottask. It returns aggregate counts under `soul_slock`: no object
ids and no pointers, so it is not an enumerate-global-object path. Pools are
named by `KSTAT_SOUL_LIST` in `kernel/include/private/kstats.h`, not by
`soul_t.type` (many pools share type 0 and two share `LIMITS_CHANNEL`).

## Keys

Each kernel object pool `<name>` produces three live-count gauges.

| Key | Unit | Kind |
| --- | --- | --- |
| `soul.<name>.used` | objects in use | live count: returns to baseline when the objects are freed |
| `soul.<name>.reserved` | free objects held back for allocation-free realtime delivery | live count: tracks attached realtime sources |
| `soul.<name>.total` | objects allocated, free or in use | retained capacity: may grow and plateau, does not shrink |
| `thread.live` | threads | live count of threads that are in some process's thread vector (summed over the process table, each vector under its `vec_slock`); a subset of `soul.thread.used`. The difference is threads whose soul nobody owns: a reap in flight (settles) or a leak. Server dispatch pools add a thread when requests overlap and shed one only past their high-water mark, so `soul.thread.used` itself rises by a few under load and stays; leak tests gate on `soul.thread.used - thread.live`. Not atomic with the pool counters: a thread created or reaped during the read shows as +-1 that settles |
| `timeout.live` | timers | live count of the threads in `thread.live` that own a timeout timer (`thread_t.timeout`, allocated on a thread's first timed wait, freed with the thread); a subset of `soul.timer.used`. Which pool threads have timed out yet depends on scheduling, so `soul.timer.used` moves by +-1 or 2 with no leak; leak tests gate on `soul.timer.used - timeout.live`, which still counts a timer that outlived its thread and every explicit timer |
| `prio.reply_depth` | hops | constant: `CONFIG_PRIO_REPLY_DEPTH`, the most REPLY or SEND hops one priority-propagation walk takes |
| `prio.reply_depth_overflow` | walks | monotonic: priority-propagation walks that reached an IPC hop (REPLY or SEND) past `CONFIG_PRIO_REPLY_DEPTH` and stopped before applying it (`prio_reply_depth_overflow`, `prio_chain_walk` in `kernel/sync/wait.c`), leaving the rest of the chain unboosted at the priority it had; see docs/pi-pc-design.md "REPLY propagation". A rising value means server chains deeper than the bound are being cut off there |
| `timer.<hart>.intr` | interrupts | monotonic: supervisor timer interrupts (`RISCV_INT_STIMER`) hart `<hart>` has taken since boot (`stimer_intr_count`, one relaxed counter per hart, bumped in `arch/riscv/trap-handler.c`). One key per online hart, `<hart>` = 0 .. harts-1. Read per hart without a cross-hart snapshot, so compare deltas on one key, not keys against each other. `nanosleep_1ns_storm` bounds the sleeper hart's delta with it |
| `pa.free_bytes` | bytes | live count: free physical memory from `pa_free_size()` |
| `pa.largest_free_run` | bytes | the longest run of contiguous free physical memory, reserved or not, a multiple of the page size: the largest contiguous allocation that can succeed. It is read apart from `pa.free_bytes` (which leaves reserved memory out), so the two are not one snapshot; a value far below `pa.free_bytes` means physical memory is fragmented. Roottask's loader contexts are contiguous (20 KiB for an ordinary spawn, up to 512 KiB for one with large arguments or a long descriptor map), so a spawn that needs a new one larger than this fails with `EAGAIN` and a `spawn: no loader context` line in the log. Computed only for a `/sys/kstats` read, under the physical allocator's lock (interrupts off), by a walk of its free runs of eight pages or more that visits at most 256 of them: exact unless free memory is split into more such runs than that, and then a lower bound |
| `vm.rollback_pages` | pages | Number of pages roottask successfully unmapped while rolling back a partially completed map. |
| `roottask.heap_used` | bytes | how much of roottask's fixed malloc arena (`CONFIG_ROOTTASK_HEAP_SIZE`, `servers/roottask/sys/heap.c`) lies below the bump pointer: backing the allocator has taken and not given back. A size-class block (a request of up to 32,760 bytes, which with its 8-byte header fills a 32 KiB block) is cached for its class and never merged, so this climbs to the peak demand of each class and stays; a mapped block (any larger request, such as the buffers of a spawn with large arguments) is a whole number of pages that goes back to the arena when freed, merges with its neighbours and lowers this when it was at the top. When the arena cannot supply a mapped block, the request is served from memory the size classes already hold if they have room: the unused tail of their current chunk, or a free block of its class or a larger one; that block then stays with its class. A value that keeps rising across `--laps` at the same load is a roottask leak. A value at the arena size does not by itself mean failure, since a free extent below the bump pointer can still serve a request; a request that needs new backing fails with `ENOMEM` when neither the tail nor any free extent holds it (it is larger than `roottask.heap_largest`), and roottask then logs a `nomem:` line with the arena state, while `pa.free_bytes` may still be high |
| `roottask.heap_size` | bytes | the size of that arena |
| `roottask.heap_largest` | bytes | the largest block a request for new backing could still get: the tail above the bump pointer or the largest free extent below it |
| `roottask.loader_context_bytes` | bytes | monotonic: memory roottask has mapped for loader contexts since boot (`servers/roottask/proc/loader-context.c`). A context is kept for reuse and never returned, so this only grows; it is bounded, because each size class may have only so many contexts (64 of 20 KiB, 8 of 40 KiB, 8 of 80 KiB, 4 of 160 KiB, 2 of 320 KiB, 2 of 512 KiB, in all 4,653,056 bytes) and a spawn whose class is used up waits for a context instead of mapping another |
| `cpiofs.ocbs` | OCBs | live count: open control blocks of the boot cpio service (`servers/roottask/path/cpiofs.c`), one per open file and one per mapping of a file; returns to baseline once the processes that opened or mapped files are gone and the mapped-file cleaner has retired their mappings. At `CPIOFS_MAX_OCBS` (2048) opens and mappings of boot files fail with `ENOMEM` and roottask logs an `OCB limit reached` line |
| `cpiofs.ocb_mappings` | OCBs | live count: the part of `cpiofs.ocbs` held for mappings rather than client opens |
| `cpiofs.ocb_peak` | OCBs | monotonic: the most OCBs that have been allocated at once since boot |
| `mmapfd.zombies` | objects | live count: mapped-file objects whose last mapping is gone and which wait for the mapped-file cleaner (`servers/roottask/mem/vm-fd.c`) to close their file; each holds an open handle of the server behind the file, a cpiofs OCB for a boot file. Past 50 the cleaner's next pass is forced, so while the cleaner keeps up this stays near 50 under a spawn storm and falls to 0 within a second or two after it; a value far above that means the cleaner is not getting to run |
| `ioserv.disconnects` | notices | monotonic: client-disconnect notifications roottask's own I/O service has handled (`_ioserv_disconnect_handler`). Not a system counter: it is roottask's process-local libc counter, so other servers' disconnects are not included |

The `cpiofs.*` and `mmapfd.zombies` values are read one at a time without a
lock, so one read is not a consistent snapshot of them: a count may be a moment
older than the one next to it.

Pools: `notification`, `sync`, `timer`, `thread`, `connect`, `channel`,
`process`, `global_channel`, `interrupt`, `syncevent`, `fpu`, `credential`,
`limits`, `vthread`, `debug`, `breakpt`, `ssinfo`, `threadname`.

A leak test samples `used` after two warm-up reads (so lazily grown pools have
settled) and requires it to return to its baseline.

## `/sys/log` — roottask log ring

Not a gauge file, but it lives beside `/sys/kstats`. `/sys/log` (mode 0400, so
`EACCES` for a non-root opener, and for a non-root `stat()` too, since `stat` goes through
the same open path and `/sys` has no separate metadata gate; this is intended) streams roottask's log ring
(`CONFIG_ROOTTASK_SYSLOG_SIZE`, 1 MiB by default). The ring is larger than
roottask's arena, so it is read by absolute 64-bit byte sequence instead of
being copied at `open()`. Open fixes the window the ring holds at that moment,
oldest byte first, newest last; `st_size` is the window length; a read returns
at most 4096 bytes. If roottask logs enough to overwrite part of the window
before it is read, the read fails with `EOVERFLOW`; `lseek(fd, 0, SEEK_SET)`
takes a fresh window. A root caller can add a line with the `_SYS_CMD_LOG_MARK`
message (`struct _sys_cmd_log_mark`, text of at most 128 bytes, logged
through a literal `"%s"`, control bytes such as newline replaced by `?` so one mark stays one
line; a non-root sender gets `EPERM`). Tests: `vfs.c` `sys_log_read`, and `sys_log_wrap`
under `scripts/qemu-regress.sh --mode syslog-wrap`.
