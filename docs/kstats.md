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
| `timer.<hart>.intr` | interrupts | monotonic: supervisor timer interrupts (`RISCV_INT_STIMER`) hart `<hart>` has taken since boot (`stimer_intr_count`, one relaxed counter per hart, bumped in `arch/riscv/trap-handler.c`). One key per online hart, `<hart>` = 0 .. harts-1. Read per hart without a cross-hart snapshot, so compare deltas on one key, not keys against each other. `nanosleep_1ns_storm` bounds the sleeper hart's delta with it |
| `pa.free_bytes` | bytes | live count: free physical memory from `pa_free_size()` |
| `roottask.heap_used` | bytes | high-water mark: how much of roottask's fixed malloc arena (`CONFIG_ROOTTASK_HEAP_SIZE`, `servers/roottask/sys/heap.c`) has been handed out. The arena only grows and the allocator keeps freed blocks in their size class, so this climbs to the peak demand of each size and stays; a value that keeps rising across `--laps` is a roottask leak, and one that reaches the arena size makes roottask allocations fail with `ENOMEM` while `pa.free_bytes` is still high |
| `ioserv.disconnects` | notices | monotonic: client-disconnect notifications roottask's own I/O service has handled (`_ioserv_disconnect_handler`). Not a system counter: it is roottask's process-local libc counter, so other servers' disconnects are not included |

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
