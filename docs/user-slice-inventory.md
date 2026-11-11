# User-pointer inventory (`user_slice`)

Every kernel site that validates a caller-supplied pointer, and whether it goes
through the typed boundary in `kernel/include/user-slice.h`
(`user_slice_init` / `user_slice_copyin` / `user_slice_copyout`, or the one-shot
`user_slice_get` / `user_slice_put`). Source of the list:

    grep -rn 'WR_VERIFY_PTR\|RD_VERIFY_PTR\|WR_PROBE_INT\|RD_PROBE_INT' kernel

The rule set is `user_slice_check()`: a zero length is accepted and copies
nothing; otherwise NULL, wraparound, any byte at or above `boundary_addr`, and
`len > max` are refused (`EFAULT`, `EMSGSIZE` for the length). The copy reads
the slice only and runs under the xfer fault handler, so a page unmapped by a
sibling hart returns `EFAULT`. `make check-user-slice` proves the rules and the
"copy length equals validated length" property (`tests/cbmc/user-slice.c`);
`tests/userland/user-ptr-negative.c` exercises them on QEMU.

Status `converted` means the old macro pair is gone from that site. `max` is the
fixed object size unless noted. Not converted: listed at the end; the pass
condition "no unconverted site" is **not yet met**.

## Converted

| subsystem | site | dir | max |
| --- | --- | --- | --- |
| ipc | `channel.c` `ksys_channel_create` event | in | `sizeof(struct sigevent)` |
| ipc | `channel.c` `ksys_channel_create` cred | in | `sizeof(struct _cred_info)` |
| ipc | `channel.c` `ksys_channel_create` cap_out | out | `sizeof(rtmux_cap_t)` |
| ipc | `connect.c` `ksys_connect_server_info` info | out | `sizeof(struct _server_info)` |
| ipc | `connect.c` `ksys_connect_client_info` info | out | `sizeof(client_info_t)` |
| ipc | `message.c` `ksys_msg_info` info | out | `sizeof(struct _msg_info)` |
| ipc | `message.c` `ksys_msg_deliver_event` event | in | `sizeof(struct sigevent)` |
| ipc | `message.c` `ksys_msg_verify_event` event | in | `sizeof(struct sigevent)` |
| ipc | `message.c` `ksys_msg_keydata` newkey | out | `sizeof(uint32_t)` |
| ipc | `message.c` `ksys_msg_keydata` iov list | in | `KEYDATA_MAX_PARTS * sizeof(iov_t)` |
| ipc | `message.c` `ksys_msg_keydata` iov bodies | in | `KEYDATA_MAX_BYTES - total` (checked before the copy) |
| sync | none: the sync path has no `*_VERIFY_PTR`/`*_PROBE_INT` site; it uses `xfer_atomic_or_user` (Q36) | | |
| time | `clock.c` `ksys_clock_time` old (two sites), new | out, in | `sizeof(uint64_t)` |
| time | `clock.c` `ksys_clock_period` old, new | out, in | `sizeof(struct _clockperiod)` |
| time | `timer.c` `ksys_timer_create` event | in | `sizeof(struct sigevent)` |
| time | `timer.c` `ksys_timer_settime` itime, oitime | in, out | `sizeof(struct _itimer)` |
| time | `timer.c` `ksys_timer_timeout` ntime, event | in | `sizeof(uint64_t)`, `sizeof(struct sigevent)` |
| time | `timer.c` `ksys_timer_timeout` otime | out | `sizeof(uint64_t)`; validated only, pointer kept for the special-return writeback |
| sched | `policy.c` `ksys_sched_get` param | out | `sizeof(struct sched_param)` |
| sched | `policy.c` `ksys_sched_set` param (sporadic, and `sched_priority` only otherwise) | in | `sizeof(struct sched_param)`, `sizeof(int)` |
| sched | `policy.c` `ksys_sched_info` info | out | `sizeof(struct _sched_info)` |
| sched | `thread.c` `ksys_thread_create` attr | in | `sizeof(struct _thread_attr)`; copied in the caller's context into the new thread's `args.wa.kattr`, which `thread_specret` validates and reads (never the user pointer, which may name another address space) |
| sched | `thread.c` `ksys_thread_join` status | out | `sizeof(void *)`; initialized under `vec_slock` after the last refusal, copied out after the target is reaped |
| sched | `thread.c` `kcall_op_thread_ctl` `RUNMASK_GET_AND_SET_INHERIT` | in, out | header + `2 * num` words, `sizeof` the kernel copy; copied out before `set_runmask` |
| sched | `thread.c` `kcall_op_thread_ctl` `RUNMASK_GET_AND_SET` | in, out | `sizeof(unsigned)` |
| sched | `thread.c` `kcall_op_thread_ctl` `ALIGN_FAULT` | in, out | `sizeof(int)`; the flag is applied before the copy-out |
| sched | `thread.c` `kcall_op_thread_ctl` `NAME` header, new name | in | `sizeof(struct _thread_name)`; `RTMUX_FLG_THREAD_NAME_MAX` bytes |
| sched | `thread.c` `kcall_op_thread_ctl` `NAME` previous name | out | `name_buf_len` (any non-negative `int`); a prefix is copied out before the name is replaced |
| sched | `thread.c` `kcall_op_thread_ctl` `THREAD_DESTROY_STATS` | in, out | `sizeof(struct _thread_destroy_stats)`; a `reset` snapshot is destructive, so a copy-out failure loses it |
| trace | `trace/instr/trace.c` `RTMUX_TRACE_ALLOCBUFFER` paddr_out | out | `sizeof(paddr_t)`; one slice, written twice (null pre-clear, then the buffer paddr). Reached only with `RTMUX_PRIV_CAP_TRACE`, so the userland cases cannot exercise it unprivileged |

## Not converted

| subsystem | site | why |
| --- | --- | --- |
| ipc | `fastmsg.c` `RD_PROBE_INT(kap->rmsg)` (net-connection path) | the pointer is handed to `remote_ipc_vthread` as a vtid table; needs a bounded copy-in of `rparts` entries |
| ipc | `fastmsg.c` `ksys_msg_receivev` info (`WR_VERIFY_PTR` + two probes) | the IPC hot path writes receive info in place via `STUFF_RCVINFO`; converting means a staged copy-out and an `ipc-roundtrip` measurement of its own |
| ext | none left of the macro pairs; `kernel/ext` copies already use `xfer_memcpy` directly (`as-memcpy.c`, `process.c`, `query.c`) with their own range checks | audit for the explicit-range check, not a macro conversion |
| thread | `lifecycle.c` commented-out `RD_VERIFY_PTR` | dead comment, no site |

The `RD_VERIFY_PTR`, `WR_VERIFY_PTR`, `RD_PROBE_INT` and `WR_PROBE_INT` macros stay
in `kernel/include/core/macros.h` until the last site above moves.

## Residuals

- Output slices are copied out at the end of the call, as the old stores were. A
  sibling hart unmapping the output page mid-call now yields `EFAULT` after side
  effects (for example `timer_settime` has re-armed the timer, `channel_create`
  has published the channel) instead of a kernel fault. Early validation of the
  first and last byte rejects the deliberate case before any side effect.
- The CBMC model proves the range predicate; the "copy extent equals validated
  extent" property is structural (copy helpers take only the slice) and is
  modeled, not proven against the C function.
