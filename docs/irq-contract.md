# RTMux IRQ Contract

RTMux handles device interrupts through user-mode Interrupt Service Threads
(ISTs).  Linux developers can think "threaded IRQ handler" for orientation, but
RTMux ISTs are ordinary user-mode threads attached with `interrupt_attach_thread()`.

## Hard-IRQ Path

The kernel trap path may:

- claim the interrupt from the controller;
- mask the source when the controller requires it;
- record pending state;
- wake the attached IST or deliver the attached event.

It must not allocate memory, call libc, block, call `msg_send()`, drain device
queues, or run arbitrary driver policy.

## IST Path

The IST owns device-level work:

- call `interrupt_wait()` in a loop;
- drain the device completion source at thread level;
- notify other driver threads with notifications, mutexes, condition variables, or
  file/device APIs as needed;
- call `__interrupt_unmask()` only after the device is drained far enough that the
  same interrupt will not immediately refire forever.

Because the interrupt remains masked until `__interrupt_unmask()`, an IST must not
sleep for an unbounded time before unmasking.  The latency budget belongs in
`docs/rt-budgets.md`.

## Attach / Unmask Checklist

Every `interrupt_attach_thread()` user must satisfy this order:

1. Initialize the shared state the IST or control thread will touch.
2. Program or clear the device source so stale status is not left pending.
3. Attach the IST.
4. Drain or acknowledge any source state that can already be visible.
5. Unmask only after the source can be handled by initialized state.
6. After every `interrupt_wait()`, drain or acknowledge before unmasking again.

Audited drivers:

| Driver | State initialized before source can fire | First unmask rule | Shared queue rule |
| --- | --- | --- | --- |
| `userland/dev-nvme/nvme-ist.c` | Admin/I/O CQ rings, per-queue gates, controller vector, and IST attach sentinel are initialized in `nvme_ist_start()` before client service switches to interrupt mode. | No initial unmask; the IST waits, drains both CQs until quiet, then unmasks. | One outstanding command per queue; submitter and IST synchronize with each gate mutex/condvar. |
| `userland/dev-serial-8250/main.c` | UART MMIO, RX buffer, I/O-service dispatch, side-channel connection, and notification handler are ready before the IST starts. | IST drains RX once after attach and before first unmask. | IST-only RX producer, dispatch-thread consumer through `serial-rxbuf`; notification wakes the consumer. |
| `userland/dev-serial-sifive/main.c` | UART MMIO, RX buffer, I/O-service dispatch, side-channel connection, and notification handler are ready before the IST starts. | IST drains RX once after attach and before first unmask. | IST-only RX producer, dispatch-thread consumer through `serial-rxbuf`; notification wakes the consumer. |
| `servers/pci/dwmsi.c` | DBI mapping, target PA, MSI address registers, status clear, mask, and enable registers are programmed before the demux IST starts. | No initial unmask; the demux IST waits, drains/acks all pending MSI bits, then unmasks. | Single demux IST owns MSI status drain; vector allocation bitmap is protected by `dwmsi_mtx`. |
| `servers/lkl/virtio.c` | Virtio MMIO mapping, IRQ vector, netdev lock/condvar, and LKL netdev state exist before the IST reports `irq_ready`. | `interrupt_wait(...UNMASK...)` arms the source; after wake the IST ACKs virtio interrupt status before broadcasting waiters. | Netdev state is protected by `rtnd->lock`; LKL consumers wake through the condition variable. |

## Current Driver Pattern

Canonical examples:

- `userland/dev-nvme/nvme-ist.c`: attach, raise priority, wait, drain
  completion queues, unmask.
- `userland/dev-serial-8250/main.c` and `userland/dev-serial-sifive/main.c`: serial
  ISTs drain UART status and unmask.
- `servers/pci/dwmsi.c`: MSI demultiplexing in user space.

## Notification/Event Tradeoff

Use direct IST wakeup when one driver thread owns the device.  Use notifications when
completion must fan out to another I/O-service thread or subsystem.  Use
`__interrupt_attach_event()` only for simple event delivery where the hard-IRQ
constraints above are still satisfied.

RTMux does not have Linux softirqs or kernel workqueues.  Do not add identifiers
named `softirq_*`, `workqueue_*`, or `kthread_*` for driver follow-up work
unless RTMux implements those exact semantics.

## Unified async wait

A server thread blocks once on a channel with MsgReceive (or the notification
variant) and wakes on any source that lands on that channel send_queue: client
messages, pulses, and timer sigevents, all priority-ordered by pril_add. An IRQ
can join that same queue: interrupt_attach_event registers the IRQ as a
SIGEV_NOTIFICATION sigevent (embedded by value in the interrupt object), and on
fire the trap path calls sigevent_deliver_inline, which delivers a notification
onto the target channel send_queue exactly like a pulse. So one MsgReceive loop
can service messages, pulses, timers, and a channel-routed IRQ together.

This is an ergonomics and fan-out option, not a replacement for the dedicated
low-latency path. interrupt_wait with STATE_INTR stays the default for hard-RT
IRQs: a channel-routed IRQ cannot preempt a handler already running in a
single-threaded server, it waits its turn in the receive loop, so it carries the
receive-loop latency rather than the direct IST wake latency. Choose the
channel-routed form only when the server accepts that latency in exchange for one
unified wait. The authority is identical on both paths: routing an IRQ onto a
channel uses the same delegated IRQ authority (the IOPRIV check on the attaching
thread) as interrupt_wait, with no new ambient path to a channel.

Priority order is preserved: a delivered IRQ notification enters send_queue at
its sigevent priority through pril_add, so a high-priority IRQ is dequeued ahead
of a lower-priority queued pulse or message and does not sit behind it.

Fixture and measurement (section 15 P3): tests/userland/rt-latency/irq-notify.c
exercises this with a genuine hardware interrupt rather than a synthetic call. It
uses the QEMU virt goldfish-rtc alarm as an isolated, self-triggerable IRQ source
(IRQ 11, MMIO 0x101000), delegated to the probe by the virtio-mmio resource
server as RTMUX_DEVICE_GOLDFISH_RTC through the same delegated-capability path a
UART driver uses. The RTC is isolated from the OpenSBI firmware console and the
kernel reads it only once at boot for wall-clock time, so a userland owner of its
alarm IRQ does not collide with anything. The probe attaches the alarm two ways
and asserts three things: the SIGEV_NOTIFICATION channel path delivers the IRQ as
a notification carrying the exact code and value it was attached with (the
unified receive works with a real IRQ); a higher-priority IRQ notification is
dequeued ahead of a lower-priority pulse already queued on the same channel
(priority order honored); and the dedicated interrupt_wait path wakes on the same
IRQ. It runs as its own qemu-regress mode (scripts/qemu-regress.sh --mode
irq-notify) rather than as an rt-check stage, so its blocking IRQ waits carry
their own isolated timeout and cannot stall the shared rt-check gate.

Both wake latencies are reported but not gated. Under QEMU/TCG the absolute value
is inflated by host descheduling and is noise-dominated (it varies run to run and
the two paths land within the same noise band), the same caveat as every other
section 18 measurement block. The durable, design-level fact, not the QEMU
number, is that the channel-routed path carries one extra receive-loop hop over
the direct IST wake; a clean absolute comparison shares the real-hardware
measurement gate with the physical-board GPIO fixture. The probe runs at a
realtime priority so its own scheduling delay does not pollute the measurement
and so an earlier rt-check stage's leftover threads cannot starve it.

## Notification delivery allocation contract

Every pulse, IRQ sigevent, and timer sigevent that finds no waiting receiver is
queued on the channel send_queue as a notification entry. The entry storage and
the behavior under memory pressure are audited here, because a hard-RT wakeup
must not depend on a delivery-time allocation that can fail.

Implemented behavior (§15 P2): IRQ and timer SIGEV_NOTIFICATION delivery is now
allocation-free after attach. The notification pool (a soul list) carries a
reserved-soul floor, soul_t.reserved. A SIGEV_NOTIFICATION source grows the pool
and bumps the floor by one at attach or create (object_reserve); attach or create
fails if the pool cannot grow, so the failure is reported to the caller once
rather than lost at fire time. At delivery the source draws from the floor
(object_alloc_reserved), which never grows the heap and bypasses the per-process
ceiling, so it cannot fail under memory pressure. Non-realtime allocation keeps
(total - used) above the floor, growing rather than dipping in, and object_free
never returns a soul to the heap while doing so would drop free below the floor,
so the reserved souls stay available for the realtime sources.

One reserved soul per source suffices because delivery coalesces to at most one
queued entry per source. notification_add_reserved scans the whole send_queue for
the source's (code, value, id) identity instead of only the priority-bucket tail,
so a source queued behind a same-priority peer still folds onto its existing entry
rather than allocating a second. A resolved-priority change reinserts the entry at
the new priority; the entry is never reallocated for a repeat fire.

| Producer | Attach-time storage | Delivery context | Queue entry storage | Bound | Overflow |
| --- | --- | --- | --- | --- | --- |
| IRQ via SIGEV_NOTIFICATION (interrupt_attach_event) | sigevent copied by value into the interrupt object; one reserved notification soul (nresv) | trap path, sigevent_deliver_inline then notification_deliver | drawn from the reserve at delivery (object_alloc_reserved), or coalesced by count | one queued entry per source, count to _NOTIFICATION_COUNT_MAX | allocation-free; at the count ceiling the fire folds (count saturates), never dropped, never a second allocation |
| Timer via SIGEV_NOTIFICATION (timer expiry) | sigevent by value in the timer object; one reserved notification soul (nresv) | expiry drains a per-CPU deferred (event, thread, reserved) buffer after releasing timer_wheel_slock, then notification_deliver | drawn from the reserve at delivery, or coalesced by count | one queued entry per source | same as IRQ |
| Async server pulse (MsgSendPulse / notification send) | none reserved | the sending thread context | allocated per send from the notification pool | per-process notification pool limit | send returns an error to the caller (not a hard-RT wakeup source) |

Teardown releases the reservation only after any in-flight delivery of that source
has finished, so lowering the floor cannot strand a live source's reserved slot.
The IRQ detach path calls intrevent_flush (waits for in-flight IRQ handlers)
before object_unreserve. The timer path defers each fire by value into a per-CPU
buffer, so timer_free calls timer_fire_deferred_flush (waits until no CPU has an
undrained fire batch) before object_unreserve; the destroyed timer is already off
the wheel and cannot be recorded again, so the wait is bounded by the in-progress
drains.

At the coalesce ceiling the entry saturates (the fire folds into the existing
count) rather than being overwritten, so an unhandled event is never silently
replaced; queue-full is deterministic. Only the async pulse path keeps allocating
at send time, and that path already reports failure to the calling server, so it
is not a hard-RT wakeup source.

## Tracepoints

Interrupt latency claims require two trace events:

- hard-IRQ claim/mask/record entry, emitted from the trap-side interrupt path;
- IST dispatch observation, emitted when `interrupt_wait()` returns to the
  attached user-mode IST.

Current kernel hook names are `_TRACE_IRQ_CLAIM()` and
`_TRACE_IST_DISPATCH()`.  With `CONFIG_INSTRUMENTATION=y`, they emit INT-class
`irq_claim` and `ist_dispatch` records; host tools name those records in the CTF
export and scheduler summary.  The dispatch record carries IST pid/tid, so
current correlation is per attached IST rather than per IRQ id on that endpoint.

The measured interval is trap entry to IST dispatch.  Driver drain time is a
separate IST budget and must be measured around the driver's drain loop and
`__interrupt_unmask()`.

### Blocked-IST slow path coverage

`_TRACE_IST_DISPATCH()` fires from `ksys_interrupt_wait()` on the three paths
where it is reachable in IST thread context:

- the `RTMUX_FLG_THR_INTR_PENDING` fast path (IRQ already pending when
  `interrupt_wait()` is entered);
- the race-close re-check that happens after the `STATE_INTR` commit but before
  `block()`;
- the normal blocked return after an IRQ wakes a `STATE_INTR` thread and the IST
  resumes from `block()`.

The scheduler-side hook in `mark_running_default()` also observes `STATE_INTR`
threads as they transition to running.  The syscall-side blocked return is the
contract endpoint for host trace correlation, so no queue, worker thread, or
resident trace buffer is added.
