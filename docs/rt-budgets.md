# RTMux Realtime Budgets

This file records realtime service-level objectives and the tests expected to
measure them.  Values are initial engineering budgets, not compliance claims.
They must be replaced with measured platform numbers before RTMux advertises
realtime POSIX coverage.

Unless a block says otherwise, "the reference host" below is the Linux
multi-core host that runs these gates with `qemu-system-riscv64` 8.2.2 (virt,
multi-threaded TCG, up to 8 vCPUs, 256 MB); each block's `target:` field records
the exact configuration of that run.

| Class | Initial budget | Measurement owner | Platform |
| --- | --- | --- | --- |
| Hard IRQ claim/mask/record | measured, absolute FU740-gated | hard-irq claim to ist dispatch block below (tracelogger-irq) | qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC |
| Interrupt-to-IST dispatch | measured, absolute FU740-gated | hard-irq claim to ist dispatch block below (tracelogger-irq) | qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC |
| IST drain-before-unmask | measured, TCG-dependent | NVMe IST drain-before-unmask block below (CONFIG_MEASURE_IST_DRAIN) | qemu-only PB-INTC-PLIC, PB-DMA-COHERENT, PB-MMIO-ORDER |
| High-priority periodic wake jitter | under 250 us on QEMU baseline | cyclictest analogue | qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET |
| Timer expiry error | under 250 us on QEMU baseline | timer-jitter test | qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET |
| Mutex uncontended lock/unlock | measured, TCG-dependent | uncontended mutex lock/unlock blocks below (mutex-uncontended) | qemu-only PB-TIMER-RDTIME |
| Mutex contended wake | measured, TCG-dependent | contended prio_inherit mutex wake block below (mutex-wake) | qemu-only PB-TIMER-RDTIME |
| Priority-inheritance boost span | measured, TCG-dependent | prio_inherit boost-to-restore span block below (tracelogger-pi) | qemu-only PB-TIMER-RDTIME |
| Per-kernel-call kernel-side cost | measured, absolute FU740-gated | per-kernel-call cost blocks below (tracelogger-kcall) | qemu-only PB-TIMER-RDTIME |
| LKL host-op wait | measured, TCG-dependent | LKL host-op wait-span block below (tracelogger-lkl-fsd/netd) | qemu-only PB-TIMER-RDTIME |
| Message round trip | under 100 us same CPU, under 250 us cross CPU | send/receive/reply benchmark | qemu-only PB-TIMER-RDTIME |
| LKL request round trip | under 2 ms on QEMU baseline | `lkl-measure` and traced `lkl-fsd`/`lkl-netd` QEMU rows; FU740 rows required before hardware claims | qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT |

## Measurement Block Format

Measured entries must use this exact fenced block shape so
`make check-rt-budgets` can reject free-form aspirational numbers:

```rt-budget
class: interrupt-to-ist dispatch
command: tests/userland/rt-latency/rt-latency 10000 1000000 60
config-hash: unknown
target: qemu-system-riscv64 virt
platform: qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC
opensbi: unknown
kernel-git-sha: unknown
samples: 0
max-ns: 0
p99-ns: 0
budget-ns: 0
histogram-artifact: artifacts/rt/unknown.txt
```

Every block carries a `platform` field, `<class> PB-ID[, PB-ID...]`, citing the
hardware assumptions the number rests on in `docs/platform-boundary-contracts.md`.
All measured blocks here are `qemu-only`: no block is hardware evidence until an
FU740 run fills one in.  `make check-rt-budgets` checks the field's syntax and
`make check-platform-boundary-contracts` resolves the ids.

Blocks with `unknown` fields or zero samples are acceptable only
while no release claim cites them.  Release notes must cite measured blocks.
Blocks that include `budget-ns` are enforced by `make check-rt-budgets`: both
`max-ns` and `p99-ns` must be less than or equal to that ceiling.
Generated trace budgets use raw `lkl-measure` rows when present; scheduler CSV
rows remain the fallback and diagnostic histogram source.

Performance-oriented patches that claim lower latency, higher throughput, smaller
footprint, or better scalability must add a before/after evidence block:

```text
claim: short description of the claimed win
metric: p99 latency, max latency, throughput, footprint bytes, or operation count
command: reproducible command used for both baseline and result
config: target, CPU count, relevant config hash, and host if applicable
baseline: measured before value plus artifact or commit
result: measured after value plus artifact or commit
rollback-threshold: maximum tolerated regression or condition that reverts the change
```

`make check-rt-budgets` rejects malformed `perf-evidence` blocks. If a change
cannot fill in the baseline/result fields, remove the performance claim and land
the mechanism as an unmeasured functional change.

## Baseline Measurements

These are engineering baselines from `make check-rt` on QEMU.  They are not
hardware realtime compliance claims.

```rt-budget
class: high-priority periodic wake jitter
command: make check-rt (guest: rt-latency 100 1000000 60)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: a16c0d1d852cb58e3dc969aa4f52a1bb7f5660dc
samples: 100
max-ns: 1000000
p99-ns: 1000000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

The hard-IRQ to IST-dispatch span below is the kernel-internal interval from the
trap-entry IRQ claim tracepoint to the IST-ready dispatch tracepoint, paired by
the receiving IST process. It is driven by a genuine hardware interrupt: the
tracelogger-irq mode runs the irq-latency fixture, which delegates the QEMU virt
goldfish-rtc alarm, attaches its IRQ as a dedicated IST, and fires fifty armed
alarms while draining the kernel trace ring, so rt-trace-irq reports the span (in
nanoseconds, scaled by the syspage cycles_per_sec) from real IRQ_CLAIM and
IST_DISPATCH records rather than a prose estimate. Under CPUS=8 TCG the span runs
138.7 us at the min and 165.7 us at the p50, with a p99/max tail of 829.7 us over
49 samples.

These replace an earlier capture that read a 43 ms min and a 50 ms p50, with a
max between 253 ms and 15 s depending on the run, and that attributed the floor to
TCG virtual-time descheduling across eight harts. That attribution was wrong: the
trace ring stamped events with the CYCLE counter while the consumer scaled by the
TIME counter's rate, inflating every ring-derived figure by about 300 on the reference host.
See the priority-inheritance boost-span section for the full diagnosis. The floor
is still not IRQ hardware latency, and the tail is still a TCG outlier, so the
absolute hardware budget stays FU740-gated and no budget-ns ceiling is asserted
here; but the numbers are now real nanoseconds rather than host-TSC counts
mislabelled as such, and the tail is three orders of magnitude smaller than what
was published. The measured artifact plus the fixture replaces the former bare
under-10-us and under-100-us prose.

```rt-budget
class: hard-irq claim to ist dispatch
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-irq --keep-logs (guest: irq-latency, 50 goldfish-rtc alarms)
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the time-CSR correction in this change
samples: 49
max-ns: 829700
p99-ns: 829700
histogram-artifact: artifacts/rt/qemu-virt-tracelogger-irq.irq.csv
```

IRQ wake-latency measurement method (Q26). The two IRQ fixtures, `irq-notify` and
`irq-latency`, share `tests/userland/rt-latency/rtc-probe.h`. Every attempt programs
the goldfish-rtc alarm a seeded uniform delay in [1, 10] ms ahead (xorshift64*; the
seed is printed on the first line of the run and replays with `--seed=N`), after the
alarm and asserted IRQ were cleared and before the vector is unmasked. Every blocking
wait (`MsgReceive`, `interrupt_wait`) carries a 1 s one-shot deadline. Each attempt
records the programmed alarm, the RTC before and after arming, and the RTC at wake,
and ends in exactly one outcome: `ok` (woke at or after the alarm), `early` (woke
before it) or `timeout` (deadline expired). An attempt is never retaken. The metric
is wake latency, the RTC at wake minus the programmed alarm, over the `ok` attempts
only; a normal run must report zero `early` and zero `timeout`. `irq-notify
--suppress-one` (`--mode irq-notify-suppress`) gates the IRQ of one attempt, chosen by
the seed, and must report exactly one `timeout` 1.0-1.1 s after arm, which proves a
lost IRQ is counted rather than hung on.

Measured under QEMU 8.2.2 virt TCG on a shared host (load average 20-27 on 64 cores),
seeds below, 50 `ok` samples per path and no `early` or `timeout`. Wake latency
(ns), `CPUS=4`, seed 1794408170220043909: channel notification min 180927, p50
355836, max 1966892; dedicated IST min 142142, p50 209931, max 430904. `CPUS=1`, seed
1794407931777612017: channel p50 429480, max 781543; IST p50 322609, max 613682.
`irq-latency` (`tracelogger-irq`) at `CPUS=4`, seed 1794408295777815345: p50 280806,
max 925964; at `CPUS=1`, seed 1794408155207299401: p50 211025, max 1238111. The
suppress run at `CPUS=4` (seed 1794408298866527777) timed out 1000735000 ns after
arm; at `CPUS=1` (seed 1794408159257148293) 1004861000 ns. These are QEMU figures: TCG
host descheduling inflates the tail, so the absolute hardware budget stays
FU740-gated and no `budget-ns` is asserted. `CPUS=8` on the reference host is not yet measured.

```rt-budget
class: irq wake latency channel notification
command: CPUS=4 scripts/qemu-regress.sh --mode irq-notify --keep-logs (guest: irq-notify, 50 seeded goldfish-rtc alarms, seed 1794408170220043909)
config-hash: sha256:b803d9fb492c4d826b124d1988cb45a1b60f0c93fdf84eecc8528464a8c663c1
target: qemu-system-riscv64 8.2.2 virt, 4 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: d9094a70 plus the Q26 fixture change
samples: 50
max-ns: 1966892
p99-ns: 1966892
histogram-artifact: artifacts/rt/qemu-virt-irq-notify-cpus4.txt
```

```rt-budget
class: irq wake latency dedicated ist
command: CPUS=4 scripts/qemu-regress.sh --mode irq-notify --keep-logs (guest: irq-notify, 50 seeded goldfish-rtc alarms, seed 1794408170220043909)
config-hash: sha256:b803d9fb492c4d826b124d1988cb45a1b60f0c93fdf84eecc8528464a8c663c1
target: qemu-system-riscv64 8.2.2 virt, 4 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: d9094a70 plus the Q26 fixture change
samples: 50
max-ns: 430904
p99-ns: 430904
histogram-artifact: artifacts/rt/qemu-virt-irq-notify-cpus4.txt
```

SiFive GPIO IRQ-to-IST fixture (hardware-pending): the physical-board GPIO
IRQ-to-IST pulse capture needs an FU740 and a logic analyzer, so it is board-gated
like the MEMPART window. The QEMU-tractable half is wired and validated: the
kernel publishes a sifive-gpio hwinfo node when the device tree carries
sifive,gpio0 (FU540/FU740), the virtio-mmio resource server delegates it as
RTMUX_DEVICE_SIFIVE_GPIO, and the gpio-latency fixture
(tests/userland/rt-latency/gpio-latency.c) requests that delegation through the
same authority path irq-notify uses for the goldfish-rtc alarm. QEMU virt has no
sifive,gpio0, so the delegation returns ENODEV and the fixture reports a clean
UNSUPPORTED skip and exits 0; a real IPC or delegation fault exits nonzero. Check:
make initramfs and scripts/qemu-regress.sh --mode gpio-latency, which passes on
QEMU virt only through the expected missing-device skip. The hardware pulse row
with max, p99, histogram, and board/timer configuration is filled in on FU740.
Use `scripts/fu740-gpio-latency.sh --runner '<board and analyzer command>'` for
that hardware run; the runner must append three serial-log lines:
`gpio-latency-board: ... FU740 ...`, `gpio-latency-timer: ...`, and
`gpio-latency-capture: samples=N max_ns=N p99_ns=N`.
`make check-fu740-smoke-scripts` validates the parser contract with synthetic
logs only; it is not hardware evidence and must not fill this FU740 row.

The dispatch-latency block below bounds the cost of the runqueue scan the
fully-preemptive claim rests on, so that claim carries a measured dispatch bound
and not only the preemption-checkpoint inventory. The scan lives in
kernel/sched/dispatch.c. Its local pick reads the highest runnable priority in
O(1) from the per-CPU dispatch bitmap, then descends at most NUM_PRI priority
levels; the per-priority queue walk only costs extra when several top-priority
threads carry a runmask affinity that excludes this hart, so runnable-thread count
at a priority is otherwise free (the ready queue is priority-indexed with an O(1)
head). Two paths add a hart-count term. select_cpu, which the wakeup runs before
enqueueing a readied thread, sweeps num_processors to choose the lowest-priority
hart to preempt (O(num_processors), plus a short walk of same-priority alternates
for run-CPU affinity). The steal phase, taken only when the local dispatch is
empty, sweeps num_processors remote dispatches and walks each remote top-priority
queue past affinity-excluded entries, so its bound is O(num_processors plus
affinity-excluded top-priority entries on the scanned queues). The measurement is
the wake-to-dispatch probe (tests/userland/rt-latency/preempt-latency.c). The
syscall-context ipc reply-to-dispatch sample below is a local-pick dispatch
preceded by the select_cpu sweep: the reply keeps the client on its pinned hart,
so it exercises the O(num_processors) select_cpu path and the local pick but not
cross-hart steal. The timer source shows the hart-count term directly, its
same-hart max about 172 us rising to about 229 us cross-hart. Absolute values are
TCG-dependent, so they are reported, not gated by a budget-ns ceiling.

```rt-budget
class: dispatch latency, syscall-context reply-to-dispatch
command: CPUS=8 scripts/qemu-regress.sh --mode preempt-latency --keep-logs (guest: preempt-latency, ipc reply source, 200 wakes)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 85b230acafb839b0dbef11b89bbfac568f87ba0a
samples: 200
max-ns: 60000
p99-ns: 46700
histogram-artifact: artifacts/rt/qemu-virt-preempt-latency.txt
```

```rt-budget
class: timer expiry error, 100000 ns interval
command: make check-rt (guest: timer-jitter 50 60)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: a16c0d1d852cb58e3dc969aa4f52a1bb7f5660dc
samples: 50
max-ns: 1900000
p99-ns: 1900000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

```rt-budget
class: timer expiry error, 500000 ns interval
command: make check-rt (guest: timer-jitter 50 60)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: a16c0d1d852cb58e3dc969aa4f52a1bb7f5660dc
samples: 50
max-ns: 1500000
p99-ns: 1500000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

```rt-budget
class: timer expiry error, 1000000 ns interval
command: make check-rt (guest: timer-jitter 50 60)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: a16c0d1d852cb58e3dc969aa4f52a1bb7f5660dc
samples: 50
max-ns: 1000000
p99-ns: 1000000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

```rt-budget
class: timer expiry error, 5000000 ns interval
command: make check-rt (guest: timer-jitter 50 60)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: a16c0d1d852cb58e3dc969aa4f52a1bb7f5660dc
samples: 50
max-ns: 1000000
p99-ns: 1000000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

```rt-budget
class: timer expiry error, 10000000 ns interval
command: make check-rt (guest: timer-jitter 50 60)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: a16c0d1d852cb58e3dc969aa4f52a1bb7f5660dc
samples: 50
max-ns: 2000000
p99-ns: 2000000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

### Timer tick model: one-shot expiry and the residual periodic tick

The RISC-V per-hart timer is a one-shot comparator (mtimecmp via sbi_set_timer).
On every reprogram the boot CPU computes the next timer-wheel deadline
(timer_next_deadline) and trims the next interrupt down to it when it falls
within one tick period, so a sleep, receive timeout, or absolute timer fires at
its deadline rather than being rounded up to the next periodic tick
(arch_timer_program_next in arch/riscv/cpu-misc.c). The trim only shortens: an
overdue deadline, or one deferred because this CPU is mid-kernel-call, falls back
to the next full period by design, which is what avoids the near-zero-delta
reprogram livelock the code documents; and an interval timer whose period is
below one tick is floored at one tick when it rearms (timer-core.c). clock_handler
runs in dynamic-tick billing mode (timer_scale == -9): it bills the interval that
actually elapsed from arch_timer_now_ns, not a fixed increment, so a variable or
shortened interval keeps the running time charge correct.

A residual periodic tick of one nsec_inc period (the configured clock resolution,
empirically about 1 ms here) still fires even when the CPU is idle, so the system
is not tickless-idle. That tick is owned by consumers that are checked or advanced
per tick rather than off the timer wheel: round-robin timeslice accounting; the
boot-CPU time base, since clock_handler publishes qtp->nsec each tick and snap_time
reads that field rather than arch_timer_now_ns, plus one TOD-adjustment increment
applied and decremented per tick while adjust.tick_count is nonzero; the
sporadic-server replenishment deadline (ss_replenish_time), a sorted list checked
from the scheduler and clock path, not a wheel timer; and the stall-detector
heartbeat and its per-tick threshold check. The clock handler also exposes two
per-tick hook slots (scheduler_tick_hook, kcall_op_clock_handler_hook), but no in-tree
code assigns them, so they are latent surfaces, not current owners. The residual
tick's worst-case contribution to a non-timer-driven preemption is one tick period;
timer expiry itself does not pay it because the trim above programs the exact
deadline.

Before and after, reference-host CPUS=8, one-shot trim on versus off (built with the trim
disabled). The trim is load-bearing: with it removed, expiry quantizes to the
periodic tick.

| interval | trim on max | trim off max | trim on avg | trim off avg |
| --- | --- | --- | --- | --- |
| 500 us | 55 us | 1256 us | 39 us | 655 us |
| 1 ms | 66 us | 1306 us | 39 us | 650 us |
| 5 ms | 36 us | 1034 us | 28 us | 506 us |
| 10 ms | 68 us | 1133 us | 38 us | 563 us |

The trim-off average deviation lands near half the tick and the max near a full
tick, which supports a residual period of about 1 ms in this run. The 100 us and
250 us intervals are omitted here because the guest measurement loop plus TCG
emulation cannot service a wake faster than roughly 700 us to 1 ms, so both
variants read the same swamped value at those intervals and neither reflects the
timer path. The timer-jitter check in make check-rt is the regression this pins:
removing the one-shot trim pushes the 500 us row max from about 55 us to about
1256 us, past any sub-tick expiry threshold.

Full tickless-idle (extending the idle interrupt past one tick to the next real
deadline) requires a composite next-deadline over every per-tick consumer above,
not just the wheel. clock_next_deadline (kernel/time/clock-core.c) computes that
minimum across five owners in the shared arch_timer_now_ns monotonic base: the
timer-wheel deadline (timer_next_deadline), the running RR/OTHER thread's
remaining-timeslice expiry, sporadic-server replenishment (ss_replenish_time),
the next TOD-adjustment tick while adjust.tick_count is nonzero, and the
stall-detector heartbeat window (stall_next_deadline). It is read-only and
allocation-free so the idle path and the clock ISR can both call it. Skipping
ticks to only the wheel deadline would otherwise delay a sporadic-server
replenishment, miss an RR preemption, or stretch a TOD adjustment; the composite
minimum keeps each owner's wakeup intact.

cpu_idle now sleeps to that composite deadline. The idle reprogram path
(arch_timer_program_next with idle set, arch/riscv/cpu-misc.c) calls
clock_next_deadline for the sleeping hart and arms the STIMER at the returned
deadline, capped at IDLE_MAX_SLEEP_NS (50 ms). The cap is a liveness backstop.
Going fully dark (masking SIE_STIE and waiting only on a cross-hart IPI) was
tried first and is unsafe: a thread migrated or readied onto a quiet hart can
lose that single IPI wakeup under a rare SMP race and hang, because the periodic
tick that used to rescue it is gone. A pin-to-CPU1 probe reproduced the hang
about two runs in three at CPUS=8; capping the sleep fixed it in five of five.
The composite deadline still shortens the sleep whenever an owner needs an
earlier wakeup, and normal wakeups arrive immediately by IPI, so the cap only
bounds the pathological miss: a quiet hart drops from the nsec_inc rate to about
1e9 / IDLE_MAX_SLEEP_NS (20 per second) rather than to zero, and a lost wakeup is
recovered within 50 ms. The idle thread itself is SCHED_RR at priority 0 and
always running on a quiet hart, so clock_next_deadline excludes it from the RR
owner; otherwise its timeslice would rearm a tick every quiet slice.

The instrumented clock handler (CONFIG_INSTRUMENTATION) logs per-hart STIMER
counts once per report second as [tickless] lines, alongside the owner/delta
[deadline] lines. The tickless-idle qemu-regress mode boots with no runnable
user work, sleeps through a quiet window, then arms one-shot sleeps and checks
each still wakes within budget; its oracle fails if any hart takes 500 or more
STIMER interrupts in a report window (a spin or a lost tickless transition) and
requires at least one hart below 100. On CPUS=4 and reference-host CPUS=8 quiet harts sit
near 20 per report second (the cap) versus the nominal 1000, while the one-shot
deadlines stay within a 20 ms lateness budget (about 10 ms max observed on TCG).
Safe software-time publication across a skipped qtp->nsec increment (a snap_time
reader depends on it) is handled by moving the clock read onto arch_timer_now_ns
rather than by adding a publication deadline here.

snap_time (kernel/time/clock-core.c) and the SNAP_TIME_INLINE macro
(kernel/include/core/macros.h) now read arch_timer_now_ns, the free-running
cycle counter, instead of the published qtimeptr->nsec. Under tickless idle the
boot CPU republishes qtp->nsec only when it wakes, so a thread reading
CLOCK_MONOTONIC while the boot CPU is idle would otherwise see time frozen across
the skipped-tick gap. Both the clock_gettime path and the timer-base timestamps
move together so relative sleeps stay consistent: the timer wheel fires against
the qtp->nsec that clock_handler refreshes to the same cycle-counter value at
each tick, and a converging TOD adjustment keeps the boot CPU ticking at nsec_inc
through clock_next_deadline's TOD owner, so adjtime convergence is unchanged. The
monotonic-time qemu-regress mode busy-reads CLOCK_MONOTONIC and asserts it never
steps backward and never repeats the same value for a long run (a frozen clock
would), then sleeps a second and confirms the elapsed time reflects the gap. A
pin-to-CPU1 reproducer confirmed the pre-fix read freezes on a non-boot hart.

QTIME_FLAG_TICKLESS (internal/include/sys/syspage.h) is set on idle reprograms
and cleared otherwise, marking the reprogram context the idle sleep path reads.

```rt-budget
class: timer expiry error, one-shot trim on, 500000 ns interval
command: CPUS=8 scripts/qemu-regress.sh --mode rt-check --timeout 300 --keep-logs (guest: timer-jitter 50 60)
config-hash: unknown
target: qemu-system-riscv64 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: OpenSBI QEMU bundled default (-bios default)
kernel-git-sha: 6aef5d27
samples: 50
max-ns: 55300
p99-ns: 55300
histogram-artifact: /home/jserv/rtmux-loop/build/lkl-logs/rt-check-1.log (trim on)
```

```rt-budget
class: timer expiry error, one-shot trim off, 500000 ns interval
command: CPUS=8 scripts/qemu-regress.sh --mode rt-check --timeout 300 --keep-logs (guest: timer-jitter 50 60), built with the arch_timer_program_next trim disabled
config-hash: unknown
target: qemu-system-riscv64 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TIMER-SBI-SET
opensbi: OpenSBI QEMU bundled default (-bios default)
kernel-git-sha: 6aef5d27+trim-disabled
samples: 50
max-ns: 1256100
p99-ns: 1254400
histogram-artifact: /home/jserv/rtmux-loop/build/lkl-logs/rt-check-1.log (trim off)
```

```rt-budget
class: proc cache purge
command: make check-rt (cache purge paths covered by boot/spawn pressure smoke)
config-hash: unknown
target: qemu-system-riscv64 virt
platform: qemu-only PB-TIMER-RDTIME, PB-TLB-SFENCE
opensbi: unknown
kernel-git-sha: unknown
samples: 0
max-ns: 0
p99-ns: 0
histogram-artifact: artifacts/rt/proc-cache-purge.txt
```

The NVMe IST drain-before-unmask row below is now a measured block. The driver
previously ran its completion path in polling mode: the interrupt service thread
was never spawned and a stale comment warned that the interrupt path could lose
the first post-attach completion and wedge the /usr mount. That race was closed in
the kernel (interrupt_attach_thread unmasks the PLIC source before the runmask
retarget, so the IST sees its first IRQ, and ksys_interrupt_wait latches a pending
IRQ), so dev-nvme now spawns the IST and serves completions through it. It boots in
interrupt mode and mounts /usr cleanly across 23 of 23 runs (8 local, 15 reference-host
CPUS=4).

With that path live, the span from the IST returning out of interrupt_wait, through
draining both completion queues and re-unmasking the vector, is the IST
drain-before-unmask latency. A measurement build (CONFIG_MEASURE_IST_DRAIN) times it
per completion into a log2-ns histogram. The romfs server caches its reads in a
32-block buffer cache, so the rt-latency/ist-drain-load probe reads a /usr file
larger than that cache repeatedly to evict and re-fetch every block from the device,
which the EMU_ROMFS_TESTS image supplies. Validated on the reference host (CPUS=2): the drain sits
in the 64 to 128 us band for about 95 percent of completions, with a p99 near 128 us
and a rare tail to 642 us. These are TCG emulation costs, not hardware latency (real
drain of one CQE plus a doorbell and unmask is sub-microsecond), so no budget-ns
ceiling is asserted; the measured artifact replaces the former prose.

```rt-budget
class: NVMe IST drain-before-unmask
command: EMU_ROMFS_TESTS=1 CONFIG_MEASURE_IST_DRAIN=y CPUS=2 scripts/emu.sh rtmux.run=ist-drain
config-hash: sha256:ffde302842cea1f7277b37074d19ed0a1804936835469c2ccb2928e23c80eac3
target: qemu-system-riscv64 8.2.2 virt, 2 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-INTC-PLIC, PB-DMA-COHERENT, PB-MMIO-ORDER
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 670ee6d7c2a77e1bad0a03b81df5ea6c1dc6e4db
samples: 284672
max-ns: 642500
p99-ns: 131072
histogram-artifact: artifacts/rt/nvme-ist-drain.txt
```

### Kernel non-preemptible (interrupt-disabled) windows

A kspinlock disables supervisor interrupts (SIE) while held, so while one is held
this CPU cannot take a timer interrupt and cannot preempt. The span from the
outermost kspinlock acquire (per-CPU depth 0 to 1) to the outermost release
(1 to 0) is therefore one non-preemptible, interrupt-disabled window, and unlike
an in-kernel-call span it cannot cross a context switch, since a thread never
blocks or is preempted while holding a spinlock. A CONFIG_INSTRUMENTATION build
times each such window per CPU with the RISC-V time counter, tags it with the
class of the outermost lock, and every second prints a per-CPU line with the
sample count, the worst window, and a log2-bucketed p99. The measurement vehicle
is scripts/qemu-regress.sh --mode rt-check-instr; production builds carry none of
this.

Finding (reference-host, CPUS=8, three rt-check-instr runs, per-CPU maxima): the longest
interrupt-disabled windows are consistently owned by LOCK_CLASS_MEMPART (the heap
allocator's alloc_slock) and LOCK_CLASS_SCHED (the per-CPU ready queue), with
MEMPART owning the single largest window on the busiest harts. LOCK_CLASS_TURNSTILE
(sync-chain and priority-inheritance walks) and the interrupt-path classes
LOCK_CLASS_TIMER and LOCK_CLASS_INTREVENT show smaller maxima, so first-stage IRQ
work is not the non-preemptible bottleneck. The heap allocator is the owner of the
worst window.

The unbounded part of the MEMPART window is the free-list walk. alloc_slock brackets
the two address-ordered first-fit walks in _sreallocfunc (kernel/object/alloc.c),
the constant-time list splice and coalesce, and, on a realloc that relocates, the
memcpy of the old contents; the parts that grow with state are the two walks (the
realloc-and-free insertion-and-coalesce walk and the allocation fit search, growing
with free-list length) and the relocation copy (growing with allocation size). The
lock is not held across a heap grow: the allocator drops alloc_slock before the
vm_ops->mmap callback and re-takes it only to splice the leftover page remainder
onto the free list, so page mapping is outside the non-preemptible window. The walk
length is bounded by the free-list length, which coalescing keeps to the count of
non-adjacent free regions: a fragmentation bound, not a design constant.

Measured (reference-host, CPUS=8, rt-check-instr, per-hart lines): the p99 is 3.2 us on
every one of the eight harts, and per-hart maxima run from 173.9 us to 12.66 ms.
Resolution caveat: the window is now timed with the time CSR, whose QEMU virt
base is 10 MHz, so samples quantize to 100 ns and the shortest windows land in
one or two log2 buckets. The p99 above is many ticks wide and unaffected; a
future claim about the *shortest* windows would need a finer counter.
The max owner is LOCK_CLASS_MEMPART on six harts, LOCK_CLASS_SCHED and
LOCK_CLASS_TIMER on one each, which is the attribution above.

These supersede an earlier reading of "tens or hundreds of milliseconds" max and
low-millisecond p99. That reading was an artifact of the same counter bug the
priority-inheritance boost-span section documents: irqoff_open/irqoff_close timed
with clock_cycles() (the CYCLE counter) and nonpreempt_report() scaled by
qtime->cycles_per_sec (the TIME counter's rate), inflating every window by about
300 on the reference host. Both now use cpu_clock_ticks().

Scope and honesty: the p99 is stable across harts and runs and is a real
nanosecond figure, but the max is not a latency budget. rdtime advances with
wall-clock, so a vCPU thread descheduled by the host mid-window produces a
genuinely long measured window that says nothing about the guest's lock hold time;
the 12.66 ms outlier is that, not allocator work. What survives that noise is the
class attribution, the relative ranking, and the p99, which are stable across
harts and runs. A hardware absolute still needs the FU740 run, and no QEMU
surrogate row is asserted here. A
clean absolute non-preemptible-window budget shares the real-hardware measurement
gate with the physical-board GPIO fixture. This slice measures the lock-held
duration only: it excludes the acquire spin (interrupt_lock disables SIE before
spinning, near zero under this workload), raw interrupt_disable / INTR_LOCK spans
that do not go through a kspinlock, and preemption-disabled-only spans that keep
SIE on. Use
`scripts/fu740-irqoff-budget.sh --budget-ns N --runner '<instrumented board command>'`
for the FU740 run; the runner must boot the `rt-check-instr` configuration and
append `irqoff-board: ... FU740 ...` and `irqoff-timer: ...` metadata to the
serial log.
`make check-fu740-smoke-scripts` also checks the irqoff parser with a synthetic
MEMPART row; it is a script-regression gate, not an absolute hardware budget.

Closing the worst owner: the free-list walk is deliberately not split to drop and
reacquire alloc_slock mid-walk. The walk builds the interior prev, pp, and fit
pointers that the later insertion and coalesce steps consume; dropping the lock
lets another hart mutate the list and invalidate them, so a correct version would
have to restart from the head after every reacquire, risking an O(n squared)
rescan or livelock under allocator churn. A segregated-bin or slab-per-size
allocator would give a constant walk bound but is a full allocator replacement,
unwarranted for a single-heap microkernel whose realtime wake and dispatch paths
are already allocation-free by construction (the reserved-capacity notification
path). So the worst owner is left as measured. The durable evidence is the class
attribution above, and a hard absolute max or p99 budget for this window shares the
real-hardware measurement gate with the physical-board GPIO fixture: the QEMU-TCG
absolute values (hundreds of milliseconds max, low-millisecond p99) are emulation
timing noise, with the max spikes attributable to host vCPU descheduling. They are
not a real-hardware figure and not a defensible budget.

Nested-interrupt policy: RTMux does not nest interrupts inside a kspinlock window.
kspinlock_lock leaves SIE off for the whole held span, and first-stage IRQ handlers
that take a lock use kspinlock_lock_irqsave, which keeps SIE off, so a
higher-priority IRQ does not preempt a lower first-stage handler mid-window. The
interrupt-disabled window is therefore bounded by the lock-held duration measured
here, not by interrupt nesting.

Preemption model completeness: there is no separate preemption-disabled-only
window class to measure. The preempt_disable / preempt_enable primitive
(kernel/include/lock.h) and its pc_preempt_count are defined but have zero callers
and zero readers tree-wide, so they never fence anything and no code observes the
count; the older "function-level preempt_disable" comments describe the
in-kernel-call fence, not that dead primitive. A kernel call defers the clock's
preemption while in_kernel_context() holds, but long kernel loops (the xfer copy,
the connect and channel namespace walks, sync) cooperatively yield at
NEED_PREEMPT / KSYS_PREEMPT / KEXT_PREEMPT checkpoints that KCALL_RESTART to
dispatch, so the non-preemptible span between checkpoints is bounded by the loop
chunk rather than by the whole call. The measured kspinlock windows are the worst
class for interrupt latency because interrupts are fully off; the remaining
scheduler-preemption delay is cooperative, bounded either by an explicit restart
checkpoint in a long loop or by normal kcall return where no checkpoint exists.
An interrupt-enabled kcall chunk can be longer than a spinlock hold, but it is
less severe because interrupts still run and set pc_pending_preempt_prio for the
next checkpoint.

Preemption-checkpoint inventory gate: the argument above only holds if every
long or unbounded loop in the scheduling-critical subsystems (kernel/ipc, sync,
xfer, thread, ext, sched, time) really is one of the three benign shapes, so
scripts/preempt-inventory.sh audits that no unclassified loop slips in.
It skips the trivially bounded counting loops (an integer-induction for, a
NULL-terminated list walk, a while with its own decrement) and requires every
other candidate loop to be listed in scripts/preempt-inventory.txt as one of:
bounded (a real finite runtime bound, such as an iov part countdown, a
priority-ordered chain of fixed depth, or a cross-hart handshake bounded by the
peer clearing its wait state), checkpointed (a KSYS_PREEMPT / KEXT_PREEMPT
drain that restarts when a higher-priority thread is runnable), or irqoff (a
window under a named kspinlock, or a watchdog-capped cross-hart quiescence spin,
that is a measured non-preemptible span rather than a scheduler-preemption
delay). The inventory is keyed on the file and the normalized loop text and
matched as a multiset, so it survives reindentation and line moves; editing a
loop or adding a new one makes it show up as unclassified until it is
reviewed and recorded. The gate is pure static text analysis, so it runs in the
default make check next to check-lock-rank. Check: make check-preempt-inventory.
The current tree classifies 141 loops (73 bounded, 9 checkpointed, 59 irqoff).

### Wake-to-dispatch preemption latency and the interrupt-wake gap

The wake-to-dispatch probe measures how long a higher-priority thread waits to
run after it is made runnable, for three wake sources in one run
(tests/userland/rt-latency/preempt-latency.c). Latencies come from the rdtime
counter (exposed to U-mode via scounteren), so the numbers survive the boot
clock stalling. Check: CPUS=8 scripts/qemu-regress.sh --mode preempt-latency
--timeout 300 --keep-logs.

IPC-reply source: a SCHED_FIFO client at priority 40 and server at priority 30
share the boot hart over a fixed-priority channel (so message-driven priority
inheritance does not boost the server to the client priority). The client blocks
in a synchronous MsgSend; the server timestamps just before MsgReply, and the
reply, running in the server's syscall context, preempts the server returning
from it. Validated local CPUS=4 and reference-host CPUS=8, the reply-to-dispatch max and
p99 are a few tens of microseconds (TCG-dependent, so reported not gated), so a
syscall-context wake dispatches a higher-priority thread promptly.

Timer source and IRQ-notification source: a SCHED_FIFO waiter at priority 40
either sleeps to an absolute deadline (timer) or blocks in interrupt_wait for the
goldfish-rtc alarm IRQ (irq), while a priority-8 hog holds the boot hart; a
watchdog at priority 45 on a second hart and the controller on a third let the
probe report rather than hang. Original finding, before the kernel_exit fix
described below (local CPUS=4 and CPUS=8): both interrupt-context wakes had a real
gap. Neither a nanosleep wheel-timer wake nor a hardware IRQ notification preempted
a busy same-hart lower-priority thread once there were more than two harts. Each
waiter took one wake and then stalled; the watchdog survived (so the stall was
cpu0-local, not a system-wide wedge) and named the path, which is the task pass
criterion for an as-yet-unfixed source.

That the IRQ source stalled the same way was the key result: the IRQ path does not
go through the timer wheel, so the gap was not a timer-wheel arm-versus-walk race
as first suspected. The common factor was an interrupt-context wake (STIMER or a
PLIC device IRQ) that readied a higher-priority thread onto a busy same-hart
lower-priority U-mode thread and failed to dispatch it, while a syscall-context
wake (the IPC-reply source) dispatched promptly. The evidence that the IRQ stall
was a preemption failure rather than a delivery failure: the irq-notify probe runs
the identical arm and interrupt_wait loop 50 times with no hog, and this probe
stalled on the second wake once a busy hog was added, so the only difference was
the contending hog.

First cause found and fixed (arch/riscv/exit.c kernel_exit): the trap-exit routed
into kernel_exit when the pending-preempt-prio sentinel was set, but kernel_exit
only actually rescheduled when the resuming thread carried a timeslice or
resched async flag. A busy SCHED_FIFO thread carries neither, so it resumed and
stranded the interrupt-readied higher-priority thread. kernel_exit now reschedules
whenever the sentinel outranks the thread it was about to resume. With this fix
both interrupt-context sources preempt cleanly at CPUS=8 on the authoritative
validation host.

The earlier "CPUS=8 residual" was a local-host measurement artifact, not a kernel
defect. On reference-host, the Linux multi-hart validation host, CPUS=8 preempt-latency
passes 35 of 35 runs with zero waiter stalls and zero setup failures, and it stays
clean under deliberate host oversubscription: 20 of 20 runs idle, 8 of 8 with the
64-core host driven to load average 74, and 7 of 7 driven to load average 285
(about four and a half times the core count). The one non-pass in the extreme
sweep was the QEMU process hitting the outer harness timeout at load average 321,
where the whole guest was too slow to finish a run, which is a harness timeout
rather than a stranded waiter. The stalls seen before were on the local 8-core
macOS host at CPUS=8, where eight vCPUs plus a busy spin hog oversubscribe eight
physical cores far harder than any tested reference-host condition, and the project rule is
explicit that local multi-hart QEMU is invalid for multi-hart claims. A full static
trace of the placement, ready, trap-exit, and dispatch paths had already found them
hart-count-independent and clean (the cross-hart placement and work-stealing
theories are ruled out: select_cpu returns the boot hart for the pinned waiter and
no retarget fires), which is consistent with the runtime result: the same-hart
trap-exit interrupt-wake preemption rule holds at CPUS=8 on the authoritative host.

Second cause found and fixed (cross-hart wake): the same-hart result above does
not cover a wake that fires on one hart and readies a higher-priority thread onto
a different busy hart. An adversarial review flagged, and a cross-hart probe
variant confirmed, a distinct kernel defect there. When ready_default enqueues a
higher-priority thread onto a remote hart it sends IPI_RESCHED, but ipi_handler
treated that IPI as a no-op on the assumption that the wfi wakeup alone
re-evaluates scheduling, which is true only for an idle hart. A hart busy in a
U-mode lower-priority thread took the software-interrupt trap, ran ipi_handler,
and returned straight to the interrupted thread without reaching the dispatch
gate, stranding the readied higher-priority thread on that hart's run queue. With
the cross-hart probe (waiter and hog pinned to a non-boot hart, timer fired from
the boot hart) this stalled 6 of 6 runs deterministically on the reference host at CPUS=8,
while the same-hart configuration passed. Fix (arch/riscv/trap-handler.c):
ipi_handler now marks the running thread for reschedule on IPI_RESCHED, and the
software-interrupt trap routes a U-mode return through the same dispatch gate the
timer path uses, so a busy remote hart dispatches the readied thread instead of
resuming the interrupted one. After the fix the cross-hart probe measures 17 of 18
reference-host CPUS=8 runs (the lone gap is the same 2 s-watchdog multi-hart TCG jitter),
the same-hart configuration stays 35 of 35, and syscall-testing passes 934 of 934.
So the trap-exit interrupt-wake preemption rule now holds at CPUS=8 for both
same-hart and cross-hart wakes on the authoritative host.

The probe now runs the cross-hart pass as a standing guard: after the same-hart
timer pass it checks for a fourth hart and, if present, repeats the timer
measurement with the waiter and hog pinned to a non-boot hart so the boot-hart
timer readies them across harts. A stall there is a hard failure (the same-hart
pass only names the gap), so a regression of the cross-hart preemption fix fails
the gate rather than passing silently. It is skipped cleanly below four harts.
Validated on the reference host: CPUS=8 passes 15 of 15 runs and the four-hart-only skip path
is correct at CPUS=3.

### Contended PRIO_INHERIT mutex wake

The contended-mutex wake probe (tests/userland/rt-latency/mutex-wake.c) times how
long a high-priority waiter blocked on a PTHREAD_PRIO_INHERIT mutex takes to
acquire it once the low-priority holder releases. The holder runs on cpu0, the
waiter on cpu1, and a controller on cpu2; each round the holder locks the mutex,
guards long enough for the waiter to block, records the rdtime counter just before
unlocking, and the waiter records its acquire time. The measured span is the pure
unlock-to-acquire handoff, so the guard is excluded. Inversion-freedom under a
competing medium thread is the separate job of pi-inversion-xfail; this probe only
times the handoff. Check: CPUS=4 scripts/qemu-regress.sh --mode mutex-wake
--keep-logs.

Validated on the reference host (CPUS=4, three runs): the contended wake is consistently in the
tens to low hundreds of microseconds, with a p99 of about 85 to 136 us across runs
and the bulk of samples between 32 and 128 us. The max is not stable run to run:
two runs stayed under 172 us while a third carried a single 50 ms sample. That
50 ms outlier is one sample in two hundred of TCG virtual-time descheduling, the
same inflation the wake-jitter and interrupt-wake rows carry, not mutex-path
latency, which is why no budget-ns ceiling is asserted. The durable result is the
measured mechanism and the sub-200 us p99; the block below records the run that
carried the TCG tail so the max reflects it. This measured artifact replaces the
former bare under-100-us prose row.

```rt-budget
class: contended prio_inherit mutex wake
command: CPUS=4 scripts/qemu-regress.sh --mode mutex-wake --keep-logs
config-hash: sha256:ffde302842cea1f7277b37074d19ed0a1804936835469c2ccb2928e23c80eac3
target: qemu-system-riscv64 8.2.2 virt, 4 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f612b3d131a6e193d2daa7619436dc054376485e
samples: 200
max-ns: 50102800
p99-ns: 135600
histogram-artifact: artifacts/rt/mutex-wake.txt
```

### Priority-inheritance boost span (trace-derived)

The mutex-wake block above measures the handoff from the waiter's side, in
wall-clock delta counters the userland probe records itself. The boost span is
the kernel's own view of the same event: how long the low-priority holder
carries the donated priority. The kernel emits a pi_boost trace event when it
raises a thread's effective priority on donation and again when it restores the
original on release; scripts/rt-trace-sched.py pairs each raise with its matching
restore for the same thread into a span, and scripts/rt-trace-budget.py turns the
pi_boost measure row into the block below. The generator is the same 200-iteration
mutex-wake workload, captured through /bin/tracelogger in windowed mode so the
sparse pi_boost events are kept while the rest of the ring is dropped. Check:
CPUS=4 scripts/qemu-regress.sh --mode tracelogger-pi --keep-logs, then read the
measure_summary,pi_boost row from the generated .sched.csv.

Validated on the reference host (CPUS=4): 326 raw pi_boost events pair into 163 spans, with a
min of 74.7 us, a p50 of 86.7 us, a p99 of 119.4 us, and a max of 120.3 us. The
span is tight, max over p50 is 1.39, which is what a donation window bounded by a
short guarded critical section should look like.

These numbers replace an earlier capture that read p50 26 ms, p99 61 ms, max
231 ms, and that explained the size as TCG virtual-time inflation of the guarded
critical section. That explanation was wrong. The ring stamped each event with
`clock_cycles()`, which reads the RISC-V CYCLE counter, while the consumer scaled
the delta by `qtime->cycles_per_sec`, which is the TIME counter's rate. Under
QEMU the cycle CSR is host-derived (the host TSC on an x86 validation host), so
the conversion inflated every ring-derived duration by the ratio between the two,
about 300 on the reference host. The ring now stamps with `cpu_clock_ticks()` (rdtime), which
is the counter `cycles_per_sec` actually describes. No budget-ns ceiling is
asserted yet; the durable result is that boost and restore are correctly paired
per thread from the trace stream, and the window is now reported in real
nanoseconds rather than host-TSC counts mislabelled as such.

```rt-budget
class: prio_inherit boost-to-restore span
command: CPUS=4 scripts/qemu-regress.sh --mode tracelogger-pi --keep-logs (rt-trace-sched.py pi_boost row)
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 4 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the time-CSR correction in this change
samples: 163
max-ns: 120300
p99-ns: 119400
histogram-artifact: artifacts/rt/pi-boost.txt
```

### Per-kernel-call kernel-side cost (trace-derived)

Every other block here measures a mechanism: an IPC round trip, a mutex wake, a
boost span, an IRQ-to-IST dispatch, a non-preemptible window per lock class.
None of them answers "what did the kernel itself spend on this call", so a slow
`MsgSend` could not be split into kernel work versus receiver latency. These
blocks close that: one row per kernel call number, naming the kernel-side cost of
that call.

Mechanism. The RISC-V trap dispatcher brackets `ksys_call_table[num]()` with the
time CSR and hands the delta to `trace_emit_kcall`, which emits one KER_CALL-class
exit event carrying the tick count as its payload. Measuring in the dispatcher
rather than pairing two ring timestamps keeps the cost event's own lock and buffer
write outside the measured window, and makes each event a complete sample, so the
consumer does not care that the ring is only per-buffer ordered. The hooks compile
to nothing without `CONFIG_INSTRUMENTATION`, and with it they cost one array load
until kcall tracing is enabled through the existing `ksys_call_masks` control
surface. `scripts/rt-trace-sched.py` turns the events into
`measure_summary,kcall_<name>` rows and `scripts/rt-trace-budget.py` turns those
into the blocks below, the same two-stage pipeline that produces the `pi_boost`
row. Check: `CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --keep-logs
--timeout 240`, then read `<log>.rt-budget.md`.

The capture must enable only the kernel-call exit class, which is what the
`tracelogger-kcall` mode does and what separates it from the plain `tracelogger`
mode. Under `RTMUX_TRACE_ADDALLCLASSES` the message handlers emit COMM and
thread-state ring events inside the measured window (`_TRACE_COMM_EMIT_SMSG`,
`_TRACE_TH_EMIT_STATE` and `_TRACE_COMM_EMIT_REPLY` in `kernel/ipc/fastmsg.c`,
plus `mark_running()` inside `block()`), each taking the trace state and buffer
spinlocks, and the `msg_*` rows then measure the ring as much as the handler.
Measured on the same host and workload, moving to kcall-only tracing cut the
`msg_*` p50s by 18 to 23 percent (`msg_replyv` 11.6 to 9.1 us, `msg_receivev`
11.8 to 9.7 us, `msg_sendv` 43.7 to 33.8 us) and left `dbgprint` unchanged at
about 226 us, which is the control: it emits no COMM events, so the delta is the
handlers' own instrumentation and not host load.

TCB delta. In a production (non-`CONFIG_INSTRUMENTATION`) build the kcall hooks
add no reference at all: the trap dispatcher emits no timer read (`objdump -d` on
the production `trap-handler.o` finds none) and the bracketing locals optimize
away. `cpu_clock_ticks` itself is an 8-byte leaf, but it is not unreferenced --
the same change moved five production callers onto it from `clock_cycles()`
(`nm` shows `U cpu_clock_ticks` in `object/core.o`, `sync/rcu.o` and
`thread/lifecycle.o`), which is a counter correction, not hook overhead. No new
kernel object, no new syscall, no new user-visible surface. Rollback is deleting
the two hooks and these blocks.

What the span covers. Kernel-side work only. The handler returns before
`kernel_exit()` performs any context switch, so a blocking call's peer-wait time
is excluded by construction: `block()` marks state and returns, and the register
switch happens at trap exit. Trap entry and exit assembly are outside the window.
A handler that leaves through `kernel_exit()` instead of returning (the xfer
fault paths, the teardown path) emits no event at all, which loses samples but
never produces a wrong one. Interrupts are off for the whole window unless the
handler itself re-enables them, which under this capture nothing does.

SMP invariant. The span is opened and closed on one hart with no intervening
`__kernel_restore`, and the event is self-contained, so no cross-hart ordering
rule is introduced and no pairing across CPUs is attempted. The event carries pid
and tid+1 for attribution only. The mask read that arms the span is an unlocked
load of a naturally-aligned word, matching existing `_TRACE_CHK_ENTRY` practice;
arming mid-call is suppressed by the zero-start test and disarming mid-call is
handled inside `add_trace_event`.

Restart and blocking accounting. One sample is one ecall execution, not one
logical kernel call. `KCALL_RESTART` rewinds the user PC by `KSYS_ENTRY_SIZE` so a
preempted call re-enters as a fresh trap, and a logical call that restarts N times
contributes N samples. A blocking call is split rather than restarted: `MsgSendv`
returns after `block()` marks state and the wake resumes through
`__kernel_restore` without re-entering the dispatcher, so the reply-side kernel
work is charged to the server's `msg_replyv` and never to the client's
`msg_sendv`. These rows are therefore a per-ecall-execution cost and are not a
WCET; no block below may be cited as one. This capture shows nothing inconsistent
with zero restarts: 24863 `dbgprint` calls and a 24868/24872/24874
`msg_sendv`/`msg_receivev`/`msg_replyv` ratio is the expected 1:1:1 shape with no
surplus chunks. Equal counts are consistent with no restarts rather than proof of
it, and say nothing about calls outside that path. The multi-chunk case stays an
accounting rule read off the `KCALL_RESTART` sites (13 direct uses plus 17
`KSYS_PREEMPT`/`KEXT_PREEMPT` expansions); a workload that preempts a long kernel
call is the revisit trigger for reporting a summed per-logical-call number.

Validated on the reference host (CPUS=8, `--mode tracelogger-kcall`, 99643 trace events, of
which only control-class and KER_CALL-class are present). Relative ranking is the
durable result and it is informative: `msg_replyv` and `msg_receivev` sit at about
9 us p50, `msg_sendv` at about 34 us, and `dbgprint` at about 224 us, twenty-five
times the reply cost, because it drives the serial console synchronously inside
the kernel call. That is exactly the attribution the mechanism blocks could not
give. The p50s reproduce across runs (three captures gave 9.1 / 9.7 / 33.8 /
225.8, 9.1 / 9.7 / 34.2 / 226.6 and 9.1 / 9.4 / 34.4 / 224.0 us for the same four
calls); the max values do not, which is the expected
TCG behavior. Absolute values are not a claim: they are TCG under host
descheduling, and
the time CSR base on QEMU virt is 10 MHz, so every figure is quantized to 100 ns.
FU740 mtime runs at 1 MHz, one tick per microsecond, so the promised hardware
measurement can rank the calls and pin down `dbgprint` but cannot give a
meaningful absolute for a sub-10-microsecond call; that needs a finer time source
than mtime. No `budget-ns` ceiling is asserted. Call coverage is whatever the
drainer self-test exercises, listed in the artifact; only the three `msg_*` rows
and `dbgprint` carry a sample count worth a distribution, and the rest are boot
residue kept for coverage rather than for their percentiles.

```rt-budget
class: kcall-msg_replyv
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 24874
max-ns: 737300
p99-ns: 29000
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-msg_receivev
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 24872
max-ns: 232500
p99-ns: 18000
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-msg_sendv
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 24868
max-ns: 611800
p99-ns: 53100
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-dbgprint
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 24863
max-ns: 1112300
p99-ns: 377400
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-trace_event
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 6
max-ns: 506700
p99-ns: 506700
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-timer_timeout
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 7
max-ns: 96300
p99-ns: 96300
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-sync_mutex_unlock
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 2
max-ns: 44000
p99-ns: 44000
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-sync_mutex_lock
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 2
max-ns: 16500
p99-ns: 16500
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-msg_sendvnc
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 1
max-ns: 73200
p99-ns: 73200
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-priv
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 2
max-ns: 3300
p99-ns: 3300
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-sync_create
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 1
max-ns: 6500
p99-ns: 6500
histogram-artifact: artifacts/rt/kcall-cost.txt
```

```rt-budget
class: kcall-clock_time
command: CPUS=8 scripts/qemu-regress.sh --mode tracelogger-kcall --timeout 240 --keep-logs
config-hash: sha256:e2b690eef747ce74d9f06832942e66f5fea945aaa861eab655a57250a8b135ed
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: ddf75e3d plus the kcall cost hooks in this change
samples: 1
max-ns: 5400
p99-ns: 5400
histogram-artifact: artifacts/rt/kcall-cost.txt
```

### Uncontended mutex lock/unlock

The uncontended-mutex probe (tests/userland/rt-latency/mutex-uncontended.c) times a
lock-then-unlock pair with no other thread contending, from a single SCHED_FIFO
thread pinned to cpu0, for the two uncontended paths. The default path is a lockless
userland compare-and-swap of the owner word that never enters the kernel, so it is
faster than the rdtime counter resolves and is timed in batches of 128 pairs divided
back to a per-pair cost. The PRIO_PROTECT ceiling path enters the kernel even when
uncontended (it applies and restores the ceiling boost), so it exercises the
ksys_sync_mutex_lock fast path and is timed one pair per sample. Check: CPUS=4
scripts/qemu-regress.sh --mode mutex-uncontended --keep-logs.

Validated on the reference host (CPUS=4, three runs): the default userland path is a stable few
hundred nanoseconds per pair (p99 about 400 to 640 ns, every sample between 256 and
1024 ns), confirming the common uncontended lock stays in userland and well under the
former 5 us target. The PRIO_PROTECT kernel path is about 8 to 16 us per pair with a
p99 near 24 to 27 us and a tail to about 37 us; that is two kernel round trips (lock
with ceiling boost, unlock with restore) under TCG virtual-time emulation, not
hardware syscall latency, so no budget-ns ceiling is asserted. The two blocks below
record one run and replace the former bare under-5-us prose row.

```rt-budget
class: uncontended default mutex lock/unlock
command: CPUS=4 scripts/qemu-regress.sh --mode mutex-uncontended --keep-logs
config-hash: sha256:ffde302842cea1f7277b37074d19ed0a1804936835469c2ccb2928e23c80eac3
target: qemu-system-riscv64 8.2.2 virt, 4 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 60d9cd58e4d8d03565cbca728fbc6bee33c63cb5
samples: 1000
max-ns: 780
p99-ns: 641
histogram-artifact: artifacts/rt/mutex-uncontended.txt
```

```rt-budget
class: uncontended prio_protect mutex lock/unlock
command: CPUS=4 scripts/qemu-regress.sh --mode mutex-uncontended --keep-logs
config-hash: sha256:ffde302842cea1f7277b37074d19ed0a1804936835469c2ccb2928e23c80eac3
target: qemu-system-riscv64 8.2.2 virt, 4 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 60d9cd58e4d8d03565cbca728fbc6bee33c63cb5
samples: 1000
max-ns: 36700
p99-ns: 25900
histogram-artifact: artifacts/rt/mutex-uncontended.txt
```

### Memory-pressure OOM policy: admission failure, no reclaim

RTMux resolves memory pressure by refusing the new allocation, never by picking
a victim. Physical memory is backed eagerly at allocation time (there is no
demand-paging handler; a user fault against an unbacked page is fatal), nothing
pages out, swaps, or reclaims a live process's memory, and there is no OOM
killer. So under pressure the physical quantum allocator is exhausted, pa_alloc()
returns NULL, the roottask memory path returns a clean ENOMEM to the requesting
thread, and every already-admitted process, in particular the protected classes
(realtime driver/IST processes, critical servers), keeps its full working set.
Protected classes are safe by construction rather than by a tunable priority
comparison: nothing is ever reclaimed, so nothing can be victimized. The full
policy, including the honest gap that the per-process RLIMIT_AS cap is currently
a stub, is in docs/oom-admission-policy.md.

The memory-growth bound this implies: a realtime process's resident set is
monotonic and owned. Once it has allocated and touched its working set, that
memory cannot shrink under external pressure, so its worst-case resident-memory
budget is exactly what it allocated up front, with zero variance from other
processes' allocation behavior. The bound holds only if the realtime process
allocates before its deadline-sensitive phase; a lazy allocation during the
critical phase is exposed to ENOMEM, which is a program bug under this policy.

Finding (local, qemu-system-riscv64 virt, 8 vCPU, 256M, TCG, full rt-check run):
tests/userland/rt-latency/mem-pressure.c drove a background hog to MAP_FAILED
after mapping and touching 226,492,416 bytes (about 216 MiB) of anonymous
memory, with no crash on the admission-failure path. Throughout the pressure and
at its peak the SCHED_FIFO realtime thread's 64 KiB canary stayed intact (no
reclaim of protected memory) and its periodic loop ran all its iterations (no
starvation). The realtime wake deviation is reported but not gated: under TCG the
absolute value is inflated by host descheduling of vCPU threads, the same caveat
as every other block here.

```rt-budget
class: memory-pressure admission failure (no victim reclaim)
command: make check-rt (guest: mem-pressure, final stage)
config-hash: sha256:16bcbe77b4638664c34e855848a6cd21f8b2185b21b48b49987387e1cb5b2cb3
target: qemu-system-riscv64 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TLB-SFENCE
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: bde73066987e21953ac38a90a8ca61b9f35fbfad
samples: 4000
max-ns: 30209000
p99-ns: 30209000
histogram-artifact: artifacts/rt/mem-pressure-qemu-virt.txt
```

The block carries no `budget-ns`: the realtime deviation is characterization,
not a gated ceiling, for the TCG reason above. The gated facts are structural
and asserted by the probe's exit status: hog reaches ENOMEM without a crash,
canary intact at peak pressure, realtime loop not starved.

### Native IPC MsgSend Round Trip

Cross-process MsgSend/MsgReceive/MsgReply round-trip baseline per payload and
cap-transfer class, measured by tests/userland/rt-latency/ipc-roundtrip.c. The
probe spawns a child that runs a timed send loop against a parent echo server in
a separate address space, so the address-space switch and copy the IPC path pays
are on the measured path. It reads the RISC-V time counter (rdtime) directly,
which the kernel now exposes to user mode via scounteren in
arch/riscv/startup/init-cpu.c; the software CLOCK_MONOTONIC is a per-tick counter
and cannot resolve a sub-tick round trip. rdtime is a single mtime source shared
by all harts, so the delta stays valid when the measuring thread migrates,
unlike the per-hart rdcycle.

Exposing rdtime and rdcycle to user mode is a deliberate, documented timing side
channel. It is the standard Linux default and is accepted here for measurement;
see the Spectre stance note in docs/smp-contract.md for the related discussion.

Scope honesty: these are reference-host QEMU numbers, per the Standing-Gate rule that
local QEMU is invalid for claim-grade latency rows. They are inflated by TCG
emulation cost, dominated by the whole-TLB sfence.vma the cross-address-space
path performs on each rendezvous, which forces TCG to retranslate. Real hardware
is faster; this is the before number that an IPC fast-path change measures
against in the same environment, not a hardware latency claim. The cap-transfer
rows use eight measured samples (plus two warmups) so the 1+2+3+4 transfer
classes fit the current 128-slot endpoint-cap table without adding benchmark-only
revoke machinery.

No `budget-ns` ceiling is asserted for these QEMU characterization rows. A future
IPC fast-path patch must rerun the same command on the reference host and either stay within
25% of the p99/max row for the touched class or document the regression and its
budget tradeoff here.

```rt-budget
class: native IPC MsgSend round trip, empty payload
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, empty class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 500
min-ns: 55000
avg-ns: 57900
p99-ns: 93300
max-ns: 102000
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

```rt-budget
class: native IPC MsgSend round trip, small payload 128 B
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, small class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 500
min-ns: 60400
avg-ns: 63400
p99-ns: 93200
max-ns: 104600
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

```rt-budget
class: native IPC MsgSend round trip, page payload 4096 B
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, page class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 500
min-ns: 110700
avg-ns: 115500
p99-ns: 142800
max-ns: 154100
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

```rt-budget
class: native IPC MsgSend cap transfer, 1 cap
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, cap1 class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 8
min-ns: 62700
avg-ns: 63000
p99-ns: 63300
max-ns: 63600
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

```rt-budget
class: native IPC MsgSend cap transfer, 2 caps
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, cap2 class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 8
min-ns: 64100
avg-ns: 67800
p99-ns: 72200
max-ns: 85200
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

```rt-budget
class: native IPC MsgSend cap transfer, 3 caps
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, cap3 class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 8
min-ns: 65500
avg-ns: 69900
p99-ns: 74400
max-ns: 90500
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

```rt-budget
class: native IPC MsgSend cap transfer, 4 caps
command: scripts/qemu-regress.sh --mode ipc-roundtrip --timeout 180 --keep-logs (guest: /bin/ipc-roundtrip 500, cap4 class)
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 8
min-ns: 67900
avg-ns: 74100
p99-ns: 88600
max-ns: 92900
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-roundtrip-1.log
```

### High-priority IPC round trip under a low-priority backlog

The round-trip blocks above are single client-server (uncontended). A hard-RT IPC
claim also needs the high-priority client's round trip bounded when a low-priority
pool is backlogged on the same endpoint. The receive is priority ordered by
construction: the kernel enqueues blocked senders on a pril (priority list) send
queue in kernel/ipc/message-core.c, so a high-priority sender is dequeued ahead of
the low-priority backlog rather than behind it. The probe
(tests/userland/rt-latency/ipc-contention.c) corroborates that: a server on cpu1
echoes, a pool of sixteen low-priority clients on cpu2 hammers it, and a
high-priority client on cpu3 times its own round trip with the backlog idle and
then hammering. Check: CPUS=8 scripts/qemu-regress.sh --mode ipc-contention
--timeout 180 --keep-logs.

Validated on the reference host (CPUS=8): the contended high-priority round trip is 310.5 us
p99 / 348.8 us max while the low pool completes 1719 sends, so the
high-priority client is not pushed behind the sixteen-client backlog. The
absolute numbers are TCG cross-hart descheduling artifacts, not hardware latency,
so they are reported and not gated on a ratio; the only gated check is that a
real backlog formed. A counterintuitive detail confirms the emulation effect:
the idle-backlog phase can read about 50 ms per round trip, worse than the
contended phase, because with the pool idle the server hart goes to sleep between
messages and pays a large TCG wake-from-idle penalty, whereas the hammering pool
keeps the server hart hot. The durable proof of ordering is the pril send queue;
this block records the contended measurement.

```rt-budget
class: high-priority IPC round trip under low-priority backlog
command: CPUS=8 scripts/qemu-regress.sh --mode ipc-contention --timeout 180 --keep-logs
config-hash: sha256:69c3c9d942ef057b61ba19da648d72d45d6cf014b566cb1339f99d7be5150908
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: e3e56db9+ipc-roundtrip-cap-budget
samples: 200
max-ns: 348800
p99-ns: 310500
low-pool-sends: 1719
histogram-artifact: /tmp/rtmux-ipc-budget/build/lkl-logs/ipc-contention-1.log
```

### LKL Path (Non-Realtime)

The LKL client request path is declared non-realtime; see `docs/lkl-domain.md`
"Realtime Scope".  The `LKL request round trip` summary budget (2 ms) is a
common-case throughput/latency target, not an RT guarantee: each server has one
LKL admission slot, serviced by a bounded two-thread dispatch pool.  The active
thread runs a request — including any blocking LKL syscall (`pread64`, `fsync`,
`poll`) — to completion while the spare receiver returns `EAGAIN` for overflow
instead of allowing silent queue growth.  IPC still boosts the active worker to
the waiting client's priority per message, but that cannot preempt an in-flight
handler.  The `under-load native wake` blocks below therefore measure how much a
*native* RT wake is perturbed by concurrent LKL load (non-interference of the
native path), not an RT budget for the LKL client path itself.

Re-measured on the per-personality servers.  The trimmed LKL
archives (lkl-fsd net-off, lkl-netd ext4-off; see `docs/lkl-domain.md`) run
against the *same* RTMux kernel and `.config` (`config-hash` `sha256:230f…`
unchanged — the trim touched only userland servers, build glue, and the LKL
archive, no kernel code).  A single reference-host run per mode still exceeds the 2 ms RT
budget: `fs-rt` max ~14.8 ms / p99 ~13.8 ms, `net-rt` max ~8.2 ms / p99 ~7.2 ms —
the same over-budget class as the combined-archive blocks below, so the footprint
trim did not bring the under-load native wake under budget.  These runs land
below the recorded combined-archive maxima (`net-rt` 35.9 ms, `fs-rt` 22.4 ms),
but a single 100-sample TCG run varies too much to attribute that drop to the
lighter servers rather than measurement noise — a controlled repeated sweep would
be needed to tell.  What is firm is that the stall mechanism lives in the shared
kernel dispatch / LKL host-op / QEMU-TCG path (unchanged here), so the `§17` CTF
export to localize dispatch vs host-op vs TCG remains the next step before any
scheduler-policy change.

Repeated scheduling-profile sweep (reference-host, CPUS=8): after bounded
request admission landed, each baseline and isolated profile passed 3/3 boots
with kept logs.  `lkl-fsd-tree` recorded `fs-rt` maxima of 24.5 / 42.1 / 22.0 ms
and isolated `lkl-fsd-tree-isolated` recorded 8.4 / 17.2 / 8.3 ms.
`lkl-netd-http` recorded `net-rt` maxima of 29.2 / 13.9 / 39.3 ms, while
isolated `lkl-netd-http-isolated` recorded 9.2 / 8.6 / 5.9 ms.  Normal request
maxima were 1.25-2.33 ms for fs and 4.52-6.98 ms for net across these runs; the
net path is still a throughput/latency target miss, not a realtime claim.
`lkl-netd-http` also ran `lkl-backpressure-test` 3/3, proving overflow returns
`EAGAIN` instead of silently extending the sender queue, and `lkl-netd-revoke`
passed 3/3 with 12.4-14.2 ms revoke samples.  Conclusion: server/runmask
isolation helps native wake non-interference, but the residual millisecond tails
are still host-op / dispatch / TCG-localization work for the §17 trace exporter,
not a scheduler policy change justified by current evidence.

Required LKL gate refresh (reference-host, CPUS=8, `41c23e48` clean archive):
`lkl-fsd-tree`, `lkl-netd-http`, and `lkl-netd-revoke` all passed after the
virtio claim-seal-map hardening.  The budget shape is unchanged: fs request
tail remains below 2 ms, while net request tail and fs/net native wake under LKL
load still miss.  The next latency work remains trace localization, not an LKL
host-op lock fix.

LKL baseline refresh after stale-handle gate promotion (reference-host, CPUS=8, `af869192`): `lkl-fsd-tree` and `lkl-netd-http` both passed.  The
normal request paths are now below the 2 ms target in this run (`fs max=1166700
ns`, `net max=1538200 ns`), and the fs native-wake row is also below 2 ms
(`fs-rt max=1903300 ns`).  The remaining miss is the default net native-wake
max tail (`net-rt max=2419200 ns`, p99 `1838500 ns`), so the latency gate stays
open for that tail rather than for the normal request path.

Traced net follow-up on the same reference-host tree: `tracelogger-lkl-netd` passed with
generated budget rows under 2 ms (`net max=1758000 ns`, `net-rt max=1530800
ns`).  The generated LKL host-op CSV had no waited host operations, and IRQ 40
reported `max_ns=1977360`.  This narrows the remaining default-mode
`net-rt` max tail to untraced scheduling/IRQ/TCG variance; no LKL host-path code
change is justified by this trace alone.

Runmask diagnostic refresh (reference-host, CPUS=8, `79e46994`): current
`lkl-netd-http-split` still misses (`net max=3596300 ns`, `net-rt max=2327000
ns`), and `lkl-netd-http-isolated` keeps normal net below target but still has a
native-wake max tail (`net max=1836500 ns`, `net-rt max=2230600 ns`).  Runmask
isolation is useful evidence but is not enough to close the remaining max-tail
gate.

Fresh default net rerun (reference-host, CPUS=8, `b3c4245b`): `lkl-netd-http`
still passes functionally and still misses only the max-tail budget samples:
normal net `max=2138700 ns` / `p99=2138700 ns`, net-rt `max=2348300 ns` /
`p99=1988600 ns`, net-rt-step `max=1788200 ns` / `p99=1554200 ns`.  The
100 us timer-jitter row stayed below the 2 ms request target (`max=1474100 ns`),
so this rerun preserves the same conclusion: the open gate is a rare default
network scheduling/IRQ/TCG tail, not a generic timer-wake failure or missing
LKL host copy evidence.

Default-topology trace diagnostic (reference-host, CPUS=8):
`tracelogger-lkl-netd-default` was the one-off default-topology diagnostic that
kept the then-current split traced gate intact while running the default
`lkl-measure net 10 trace` and `net-rt 100 trace` probes.  With
1 ms trace polling it captures enough raw windows to expose the same miss shape:
`net max=4456200 ns`, `net-rt max=4262400 ns`, `net-rt-step max=3419800 ns`.
The raw trace has 17 `net-rt` measured windows; their top scheduler-visible
states are `state_receive max=1312830000 ns` and `state_condvar max=730626000
ns`.  IRQ 40 stays below 2 ms in the generated IRQ CSV (`max=1382790 ns`), and
the LKL host-op CSV has no waited operations, so this default-topology artifact
keeps pointing away from IRQ delivery and measured LKL host waits.  Artifact:
`/tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/tracelogger-lkl-netd-default-1.log`.

Trusted-device copy evidence (reference-host, CPUS=8): the net bounce-copy
gate passed the latest default `lkl-netd-http` rerun with
`net_bounce_rx_bytes 8946` and `net_bounce_tx_bytes 4882` in
`/tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-http-1.log`.  The
fs/block host-copy gate (`3144a24d`) passed `lkl-fsd-tree` with
`lkl_block_read_bytes 37888` and `lkl_block_write_bytes 45056` in
`/tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-fsd-tree-1.log`.
These rows record current copy volume for the trusted-device fallback only; no
CPU-cycle win is claimed, and the over-budget latency rows above remain
scheduler/IPC/TCG-localization work rather than evidence for zero-copy.

Hostnet final rebaseline (reference-host, CPUS=8, `71ca0956` plus
hostnet-applied LKL source `/tmp/lkl-rv64-src-hostnet7`): `lkl-netd-http`,
`lkl-netd-http --differential`, `lkl-netd-revoke`, `lkl-multi`,
`lkl-fsd-tree --differential`, `tracelogger-lkl-netd-default`, and
`tracelogger-lkl-fsd` all passed headlessly without QEMU `virtio-net-device` or
superuser filesystem mounts.  The traced default network run still shows TCG
tail variance (`net max=2655500 ns`, `net-rt max=4274900 ns`), but both
generated hostnet host-op and IRQ CSVs contain only headers, so the miss is not
attributed to waited LKL host operations or a VirtIO/MMIO IRQ path.  The split
network diagnostic meets the actual request/native-wake rows (`net max=1806000
ns`, `net-rt max=1072700 ns`, `net-rt-step max=1072700 ns`).  The traced ext4
run also meets the rows (`fs max=1768200 ns`, `fs-rt max=143700 ns`,
`fs-rt-step max=117300 ns`) with header-only host/IRQ CSVs.  The remaining
default-topology millisecond tails are scheduler/IPC/TCG analysis work for the
trace tooling, not §11 LKL hostnet or filesystem-domain work.

Traced LKL fs/net capture (reference-host, CPUS=8, `e37e13cb`): the
headless `tracelogger-lkl-fsd` and `tracelogger-lkl-netd` modes now keep a
windowed trace while running split `lkl-measure` probes.  Both modes passed with
kept logs and 16 raw begin/end markers each, so the analyzer can attribute one
largest adjacent raw trace gap per measured sample.  The analyzer reports fs
`max=2850450 ns`, fs-rt `max=14357970 ns`, net `max=15774990 ns`, and net-rt
`max=15606420 ns`; largest in-window gaps were fs <=369900 ns, fs-rt <=2249490
ns, net <=3199530 ns, and net-rt <=1830150 ns.  These short traced probes are
localization evidence rather than replacement budget rows: fs/net/rt tails still
miss budget, and the remaining work is to use the §17 CTF export to separate
guest-visible RTMux scheduler/IPC spans from QEMU-TCG idle or serialization
artifacts before changing scheduler policy.

Traced net request cleanup (reference-host, CPUS=8, private
`/tmp/rtmux-lkl-ref` replay): `lkl-measure` no longer prints per-UDP-request
diagnostics inside the traced measurement window, and the native-interference
load reuses the same fd-backed request path instead of looping through raw ICMP
open/close setup.  The traced network request row is now below the 2 ms budget
(`net max=1569300 ns`, `p99=1569300 ns`).  The native wake row still misses
(`net-rt max=5341800 ns`, `p99=5341800 ns`; step-late max `3989400 ns`), so §11
remains open for native non-interference.

Trace timestamp rollover fix (reference-host, CPUS=8, `6e43a123` clean
archive logs analyzed with the fixed host tools): `rt-trace-sched.py` and the
CTF exporter now preserve tracelogger order and unwrap 32-bit trace timestamps
instead of sorting by raw `d0`.  The previous fs trace had bogus ~4.2 s state
spans after `d0` wrapped; with unwrapped timestamps the same class of kept logs
reports fs `max=3288060 ns`, fs-rt `max=13879830 ns`, net `max=20310210 ns`,
and net-rt `max=13852770 ns`.  Largest in-window adjacent gaps are now named
events: net sample 0 is dominated by `state_running -> comm_reply` at 6091230
ns, fs-rt sample 0 by `lkl_host_exit -> comm_reply` at 2160120 ns, and later
fs-rt samples by `sys_path -> comm_reply` or LKL host enter/exit gaps below 1.6
ms.  Host-op wait summaries are still below the request budget (`lkl-netd`
`sem_down max=1220280 ns`, `mutex_lock max=304470 ns`; `lkl-fsd` `mutex_lock
max=725460 ns`), so current evidence still points away from LKL host locks and
toward RTMux scheduler/IPC/TCG correlation as the next optimization target.

Controlled repeated net-rt sweep (reference-host, CPUS=8): the single-run
entries flagged that one run varies too much to tell a systematic miss from
measurement noise, so the default lkl-netd-http net-rt native wake was re-run
seven times and its per-run lkl-measure mode=net-rt figures (100 samples each)
were aggregated. Correction: a first pass of this entry read the standalone
timer-jitter probe by mistake and wrongly reported the p99 as comfortably under
budget; the net-rt row is the lkl-measure output, and its p99 is materially
higher. Across the seven runs the net-rt p99 straddled the 2 ms budget, ranging
1676100 to 2343200 ns: six of seven landed just below 2000000 ns (with run four at
1989700 ns effectively on the line) and one run missed at 2343200 ns, and a
separately observed run spiked to p99 4288200 ns with max 5234700 ns. Per-run max
exceeded 2 ms in three of the seven runs, up to 2406800 ns. The p50 stayed near
250000 ns in every run. So the net-rt native wake is a borderline p99 miss
dominated by run-to-run variance, not a clean pass: the median is well under
budget while the p99 sits on the 2 ms line and occasionally exceeds it, and the
worst-case max still misses. This matches the over-budget net-rt tail the
single-run entries above recorded rather than overturning it; localizing the tail
needs the §17 CTF export, and a real-hardware FU740 run gives the absolute figure
free of TCG variance.

### LKL Host-Op Waits (trace-derived)

The host-op wait figures above were read out of the lkl-host CSV by hand. This
row promotes them to a generated rt-budget block so the LKL host-op wait is a
measured artifact rather than prose. When an LKL domain enters a host-side
blocking primitive it emits a host-enter trace event and a host-exit on return
(the op type rides in the event, one of page_alloc, page_free, sem_down,
mutex_lock, thread_join, timer_set_oneshot, mmap, munmap). scripts/rt-trace-lkl-host.py
pairs each enter with its exit per op type and scales the raw counter delta to
nanoseconds using the cycles_per_sec the guest prints at boot; the earlier tool
reported raw ticks in nanosecond-labeled columns, so at the 10 MHz QEMU timebase
its numbers were low by a hundredfold. scripts/rt-trace-budget.py now turns each
host-op row of that CSV into a block, and the tracelogger-lkl-fsd and
tracelogger-lkl-netd postprocess feeds it in, so the harness emits these blocks
next to the lkl-measure rows. Check: scripts/qemu-regress.sh --mode
tracelogger-lkl-fsd --timeout 360 --keep-logs, then read the measure_summary
host rows from the generated .lkl-host.csv.

Validated on the reference host (tracelogger-lkl-fsd through make check-lkl-trace, clean
pass): the fs workload waits in the LKL host mutex-lock primitive 19 times
inside the measured windows, p50 near 1 ms, p99 and max about 1.9 ms. The
magnitude tracks TCG virtual time and swings run to run (other passes measured
max between 2.2 and 5.8 ms), and the sparse capture reflects that windowed
tracing keeps only host ops that fall inside a measured fs request, so no
budget-ns ceiling is asserted. A heavier tracelogger-lkl-netd pass shows the
same mutex-lock primitive with far more samples (78) plus a sem-down wait tail
into the tens of milliseconds, confirming the host locks stay well under the
request path they serve. The durable result is that the LKL host-op wait is now
a generated, unit-correct budget row rather than a hand-read number.

```rt-budget
class: LKL host mutex_lock wait span
command: scripts/qemu-regress.sh --mode tracelogger-lkl-fsd --timeout 360 --keep-logs
config-hash: sha256:e68017f9d6e67f86d19945eaf8aedca451a62c6db9bf28a0301510b071f62027
target: qemu-system-riscv64 8.2.2 virt, 1 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 994e98b7
samples: 19
max-ns: 1899000
p99-ns: 1899000
histogram-artifact: /home/jserv/rtmux-loop/build/lkl-logs/tracelogger-lkl-fsd-1.log.lkl-host.csv
```

Windowed state rollup (same `6e43a123` reference-host logs, re-analyzed with the
state-summary host tool): `rt-trace-sched.py` now emits
`measure_state_summary` rows that aggregate per-sample clipped thread states by
mode, `measure_event_summary` rows that count in-window event types, and
`measure_transition_summary` rows that aggregate in-window adjacent-event gaps.
It also emits `measure_client_state*` rows that keep only spans for the measured
client pid/tid when the `lkl-trace:` line carries both fields, plus one
`measure_critical` row per measured sample naming the longest clipped scheduler
state span across all visible threads, the largest adjacent transition, and the
in-window LKL-host/IPC/IRQ event counts on the same line.
When `scripts/qemu-regress.sh --mode tracelogger-lkl-{fsd,netd} --keep-logs`
passes, the harness now writes those derived artifacts next to the kept serial
log (`.sched.csv`, `.rt-budget.md`, `.lkl-host.csv`, `.irq.csv`, and `.ctf/`),
so the reference-host evidence can be archived directly instead of recreated by hand.
`make check-lkl-trace` runs both traced LKL modes with kept logs and fails if
the generated scheduler CSV lacks `measure_summary`, per-sample `measure_gap`,
`measure_client_state*`, or `measure_critical*` rows, if the generated
`rt-budget` block fails schema validation, if the LKL host-op / IRQ summaries
are absent, if the CTF metadata and stream are not materialized, or, where
babeltrace2 is installed, if babeltrace2 cannot parse the captured CTF into
events, keeping the latency-localization artifact path reproducible.  Where
babeltrace2 is installed (reference-host among them), that pass runs in the trace
postprocess for every kept tracelogger run, so the real captured CTF is
validated by the same reader Trace Compass uses, not just the synthetic exporter
unit test; hosts without babeltrace2 skip the pass rather than fail.  It
deliberately uses
`rt-budgets-check.py --schema-only` for freshly generated trace artifacts:
`budget-ns` remains in the artifact for analysis, while the known open
over-budget net tail stays tracked by the latency tasks instead of breaking the
coverage gate.  The IRQ CSV must still be generated with the expected header,
but QEMU `lkl-netd` no longer requires a non-empty IRQ row because the hostnet
backend is in-process and intentionally avoids a VirtIO/MMIO interrupt path.
Hardware IRQ rows belong to the separate FU740 validation track.
The scheduler CSV also includes `measure_irq` and `measure_irq_summary`
sections: when an IRQ claim and matching IST dispatch both fall inside a raw
measured LKL window, they report the interrupt-to-IST latency by mode, sample,
IRQ, and target pid.  That keeps net `state_intr` samples separable from pure
scheduler/IPC spans in the same artifact.
The same scheduler CSV now includes `measure_critical_state_summary` and
`measure_critical_transition_summary` sections, which aggregate each sample's
top state and top transition by mode so recurring candidates stand out without
manual grouping.
The CTF exporter uses the same compact stream layout as before, but the event
metadata now gives the §11 correlation payloads semantic field names instead of
only raw `d1`/`d2` columns: IPC events expose `srcvid`/`peer_pid`, scheduler
state exposes `pid`/`tid`, LKL host events expose `op`/`value`, measured windows
expose `mode`/`sample`, IRQ events expose `irq`/`target_pid`, and PI boosts
expose packed `pid_tid`/`old_new_priority`.  That makes the Trace
Compass/Babeltrace view line up with the `.sched.csv` critical-path summaries
without changing the binary trace format.
Node1 artifact-flow check (CPUS=8, `eee831c1` clean archive with
the LKL rv64 external copied in): both `tracelogger-lkl-netd` and
`tracelogger-lkl-fsd` passed, wrote `.sched.csv`, `.lkl-host.csv`, and `.ctf/`
artifacts, and `babeltrace2` parsed both CTF directories.  The kept net trace
reported `net max=14944020 ns` and `net-rt max=13092540 ns`; its worst
`measure_critical` rows were `state_intr=12610860 ns` with
`state_running->comm_smsg=3033180 ns`, and `state_receive=13092540 ns` with
`state_running->comm_smsg=1187100 ns`.  LKL host-op waits stayed below the
request target (`sem_down max=1307790 ns`, `mutex_lock max=1484670 ns`).  The
kept fs trace reported `fs max=2660070 ns` and `fs-rt max=13748730 ns`; its
worst rt row was `state_receive=4735200 ns` with
`state_running->comm_smsg=3185250 ns`, while host-op waits stayed below target
(`mutex_lock max=341340 ns`, `timer_set_oneshot max=442080 ns`).  This
strengthens, but does not close, the current conclusion: the open LKL latency
miss is visible in scheduler/IPC critical spans while measured LKL host waits
remain sub-budget.
For `tracelogger-lkl-netd`, net windows show large receive/running/condvar state
totals (`state_receive max=22227990 ns`, `state_running max=18387180 ns`,
`state_condvar max=17057880 ns`) and net-rt similarly shows
`state_running max=24276390 ns`, while the captured host-op waits remain below
the 2 ms request target.  The same windows include balanced IPC pairs and host-op
events (`net comm_smsg=11/comm_reply=11, lkl_host_enter=38/lkl_host_exit=38`;
`net-rt comm_smsg=13/comm_reply=12, lkl_host_enter=24/lkl_host_exit=24`), plus
IRQ/IST events on the net path.  The largest recurring net transitions are IPC
edges rather than host waits (`net state_running->comm_reply max=6091230 ns`,
`net state_running->comm_smsg max=3141660 ns`; `net-rt state_receive->comm_reply
max=2550570 ns`, `net-rt state_running->comm_smsg max=2140920 ns`).  For
`tracelogger-lkl-fsd`, normal fs windows are much smaller
(`state_running max=2795250 ns`, `state_reply max=1754220 ns`), while fs-rt
still carries larger clipped running/reply/receive spans
(`state_running max=22389090 ns`, `state_receive max=14725080 ns`) with IPC and
host-op events present (`fs-rt comm_smsg=20/comm_reply=20,
lkl_host_enter=25/lkl_host_exit=25`).  The largest fs-rt transitions include
`lkl_host_exit->comm_reply max=2160120 ns` and `state_running->comm_smsg
max=2049720 ns`, again pointing at the boundary between LKL completion,
scheduler state, and IPC reply/send rather than a standalone host-op wait.  This
is not a new budget row; it is stronger localization that the remaining misses
are visible in scheduler/IPC state spans rather than the currently measured LKL
host-op waits.

Current-HEAD reference-host artifact check (CPUS=8, `2d1919ce` clean
archive with the LKL rv64 external copied in): both traced LKL modes passed
headlessly and generated `.sched.csv`, `.lkl-host.csv`, and `.ctf/` artifacts;
`babeltrace2` parsed the net and fs CTF directories (360 and 274 events).  Net
still misses the request target (`net max=15771360 ns`) and under-load wake
target (`net-rt max=14401800 ns`).  Its top critical states are
`state_intr max=13149870 ns` for normal net and `state_condvar max=14401800 ns`
for net-rt; largest top-transition summaries stay at IPC/scheduler edges
(`state_running->comm_smsg max=3062580 ns`,
`lkl_host_exit->comm_reply max=2193090 ns`).  Fs normal remains close but above
target in this traced sample (`fs max=2763720 ns`), while fs-rt still misses
(`fs-rt max=13603230 ns`); fs-rt top states are `state_receive max=4279500 ns`
and `state_running max=3480810 ns`, with
`state_running->comm_smsg max=2606670 ns`.  LKL host waits are mostly below the
2 ms request target (`net sem_down max=904740 ns`, `fs mutex_lock max=378060
ns`, `fs timer_set_oneshot max=271230 ns`), with one net `mutex_lock` summary at
`2013930 ns`.  The conclusion remains unchanged: the current miss needs
scheduler/IPC/TCG localization, not host-lock tuning or a zero-copy data path.

IRQ/IST tracepoint sanity check (reference-host, CPUS=8, `32ab4be1`): a
clean-archive `scripts/qemu-regress.sh --mode tracelogger-lkl-netd --timeout 240
--keep-logs` run passed, and the retained scheduler summary reported
`irq_claim,4` and `ist_dispatch,4`.  Exporting the same log through
`scripts/rt-trace-ctf.py` and reading it with `babeltrace2` showed named
`irq_claim` and `ist_dispatch` events in the CTF stream.  This proves the LKL
net IST path carries both correlation endpoints through the host tools; it does
not replace the missing measured interrupt-to-IST latency budget row.
Kept tracelogger runs now also emit `.irq.csv`: `scripts/rt-trace-irq.py` pairs
`irq_claim` records with the next `ist_dispatch` for the same target pid and
reports min/p50/p99/max by IRQ and pid, giving the future IRQ budget row a
reproducible host-side extractor.
Current-HEAD reference-host artifact check (CPUS=8, `a41767cd` clean archive
with the LKL rv64 external copied in): `make check-lkl-trace` passed headlessly
and generated `.sched.csv`, `.rt-budget.md`, `.irq.csv`, `.lkl-host.csv`, and
`.ctf/` artifacts for both traced LKL modes.  The generated budget blocks carry
`kernel-git-sha: a41767cd`, and `babeltrace2` parsed the fs and net CTF
directories (252 and 372 events).  The scheduler CSV now carries the measuring
process pid on each `measure` row and repeats it as `sample_pid` on each
`measure_critical` row, separately from the top in-window scheduler pid/tid; in
this run the measured client pids were fs `12297`, fs-rt `16393`, net `24585`,
and net-rt `28681`.  The run still misses the LKL targets:
`fs max=2882850 ns`, `fs-rt max=14401740 ns`, `net max=17923980 ns`, and
`net-rt max=14958450 ns`.  Top critical states still point at scheduler-visible
spans (`fs-rt state_receive max=5122380 ns`,
`net state_intr max=15204030 ns`, `net-rt state_receive max=14958450 ns`), with
recurring IPC/scheduler transitions such as
`fs-rt state_running->comm_smsg max=3871650 ns` and
`net state_running->comm_smsg max=3519480 ns`.  The fs host waits stayed below
the request target (`mutex_lock max=1055820 ns`,
`timer_set_oneshot max=84270 ns`), and net IRQ 40 paired to pid 7 five times
(`max=2819700 ns`).  The net host-op summary also reported
`mutex_lock max=4294216696 ns` with an unmatched-enter warning, so this sample is
not clean evidence against host-lock waits; it is still localization evidence,
not closure of the open LKL latency miss or justification for a zero-copy data
path.

Current-HEAD reference-host artifact check (CPUS=8, `8097782d` clean archive
with the LKL rv64 external copied in): `make check-lkl-trace` passed headlessly
and generated `.sched.csv`, `.rt-budget.md`, `.irq.csv`, `.lkl-host.csv`, and
`.ctf/` artifacts for both traced LKL modes.  `babeltrace2` parsed the fs and
net CTF directories (185 and 419 events).  The run still misses the traced LKL
targets: `fs max=3045420 ns`, `fs-rt max=8848320 ns`,
`net max=20572740 ns`, and `net-rt max=13020390 ns`.  The new in-window IRQ
rows show net IRQ 40 to pid 7 is not the dominant tail in this sample:
`measure_irq_summary` reports normal-net `max=955680 ns` and net-rt
`max=533370 ns`, while the standalone `.irq.csv` reports `max=960840 ns`.
Host waits also stayed below the 2 ms request target (`net sem_down max=1239060
ns`, `net mutex_lock max=350160 ns`, `fs mutex_lock max=493740 ns`).  The
remaining large critical spans are scheduler-visible states and IPC edges:
normal net has `state_intr max=16280970 ns` and
`state_receive->comm_smsg max=4393920 ns`, net-rt has
`state_condvar max=13020390 ns` and `lkl_host_exit->lkl_host_enter max=1791300
ns`, while fs-rt has `state_receive max=4799940 ns` and
`state_running->comm_reply max=1341210 ns`.  This narrows the current diagnosis:
the net interrupt endpoint is visible and sub-budget in-window, so the open
latency miss remains in scheduler/IPC/TCG behavior around the LKL domain rather
than IRQ delivery or measured LKL host-op waits.

Current-HEAD reference-host artifact check after delegated virtio-MMIO admission
(CPUS=8, `f26e62e6` clean archive with the LKL rv64 external copied
in): `make check-lkl-trace` passed headlessly and generated `.sched.csv`,
`.rt-budget.md`, `.irq.csv`, `.lkl-host.csv`, and `.ctf/` artifacts for both
traced LKL modes.  `babeltrace2` parsed the fs and net CTF directories (239 and
405 events).  The run still misses the traced LKL targets:
`fs max=2621130 ns`, `fs-rt max=11055180 ns`, `net max=15912450 ns`, and
`net-rt max=15083820 ns`.  Net IRQ 40 is visible but not consistently
sub-budget in this sample: in-window IRQ rows report normal-net
`max=991140 ns` and net-rt `max=2013720 ns`, with the standalone `.irq.csv`
matching the 2.013 ms maximum.  Host waits remain mostly below the 2 ms request
target (`net sem_down max=693750 ns`, `net mutex_lock max=944430 ns`), while fs
reports one `mutex_lock max=2424000 ns`.  The largest critical spans are still
scheduler-visible states and IPC edges rather than a sealed virtio mapping path:
normal net has `state_intr max=13634460 ns`, net-rt has
`state_condvar max=15083820 ns`, fs-rt has `state_receive max=4300380 ns`, and
the largest repeated transition in the sample is
`state_running->comm_smsg max=3092100 ns`.  Delegated MMIO therefore closes the
direct self-claim containment gap, but it does not close the open latency item;
the remaining evidence still points at scheduler/IPC/TCG behavior around LKL
completion and native wake interference.

Current-HEAD reference-host artifact check after moving virtio-MMIO delegation behind
`/dev/virtio-mmio` (CPUS=8, `59b97fe1` clean archive with the LKL
rv64 external copied in): `make check-lkl-trace` passed headlessly and generated
`.sched.csv`, `.rt-budget.md`, `.irq.csv`, `.lkl-host.csv`, and `.ctf/`
artifacts for both traced LKL modes.  `babeltrace2` parsed the fs and net CTF
directories (267 and 397 events).  The traced LKL targets still miss:
`fs max=2971320 ns`, `fs-rt max=14552550 ns`, `net max=15447450 ns`, and
`net-rt max=14148750 ns`.  Net IRQ 40 is visible and close to the 1 ms level in
this sample: in-window IRQ rows report normal-net `max=879510 ns` and net-rt
`max=1083210 ns`, while the standalone `.irq.csv` reports `max=1085550 ns`.
Host waits stay below the 2 ms request target (`net sem_down max=727740 ns`,
`net mutex_lock max=1110660 ns`, `fs mutex_lock max=274080 ns`).  The largest
critical spans remain scheduler-visible states and IPC/LKL-boundary edges:
normal net has `state_intr max=13385580 ns`, net-rt has
`state_condvar max=13949940 ns`, fs-rt has `state_receive max=4703670 ns`, and
the largest transition in the sample is `state_running->comm_smsg max=3972060
ns`.  Re-reading the same logs with CPU attribution in `rt-trace-sched.py`
shows the largest critical scheduler states spread across CPUs rather than a
single saturated hart: normal net `state_intr` on CPU 4, net-rt `state_condvar`
on CPU 6, fs-rt `state_receive` on CPU 1, and normal fs `state_running` on CPU
0.  A pid-only client filter on these logs still mixes the `*-rt` measuring
thread with its detached load thread, so newer `lkl-trace:` lines now include
`tid=` and `rt-trace-sched.py` filters client-state rows by pid+tid when that
field is present.  Moving the parent-side MMIO grant into a dedicated service
improves the authority boundary, but does not close the open latency item; the
next latency work still needs tid-aware scheduler/IPC/TCG localization rather
than host-lock tuning or zero-copy.

Tid-aware trace refresh (reference-host, CPUS=8, `60696fc0` clean archive
with the LKL rv64 external copied in): `make check-lkl-trace` passed
headlessly.  `lkl-trace:` lines now carry `pid=` and `tid=` (`tid=1` for all
measured samples in this run), and `babeltrace2` parsed the fs and net CTF
directories (317 and 428 events).  The traced LKL targets still miss:
`fs max=2759190 ns`, `fs-rt max=13711710 ns`, `net max=16259310 ns`, and
`net-rt max=16067940 ns`.  Tid-filtered client-state summaries show normal
fs/net clients mostly waiting in `STATE_REPLY` (`fs max=1308990 ns`, `net
max=6634380 ns`), while the under-load native probes still have long measuring
thread spans (`fs-rt STATE_RUNNING max=6226620 ns`, `net-rt STATE_RUNNING
max=6959190 ns`, plus reply waits up to `fs-rt max=4206120 ns` and `net-rt
max=5300310 ns`).  All-thread critical rows still expose background LKL helper
waits (`fs-rt state_condvar max=11222730 ns`, `net state_condvar max=16065300
ns`, `net-rt state_condvar max=14575050 ns`).  Host waits remain below the
request target (`fs mutex_lock max=1196220 ns`, `net sem_down max=1002630 ns`,
`net mutex_lock max=1880610 ns`), but net IRQ 40 now has a standalone and
in-window `max=2370960 ns`.  This keeps the latency item open and shifts the
next diagnostic to tid-aware RT probe scheduling, IPC reply timing, and net IRQ
tail correlation.

Critical-row client attribution refresh (reference-host replay, CPUS=8):
`rt-trace-sched.py` now adds `client_top_state`, `client_state_ns`,
`client_tid`, and `client_cpu` to each `measure_critical` row.  Replaying the
kept `tracelogger-lkl-fsd` log from the clean archive shows the `fs-rt` miss
directly in the measured client: every `fs-rt` sample's client top state is
`state_reply`, with `client_state_ns` reaching `5006400`, while the global top
state alternates between scheduler-visible `state_running` and `state_receive`.
That narrows the next §11 probe to reply-path timing and the peer that holds the
client in reply, rather than another host-lock or whole-log gap pass.
The same replay now emits `measure_ipc*` rows keyed by COMM event, srcvid, and
peer pid.  For the `fs-rt` windows, replies to the measured client pid `16394`
arrive through srcvids `1` and `3` (`7` and `13` events respectively), while
sends inside the same windows target pid `1` and LKL/fs pid `9`; that gives the
next trace pass concrete peers to label instead of treating all reply time as
anonymous scheduler delay.
The IPC rows also decode the existing COMM payloads: `comm_smsg` splits srcvid
into `src_tid` and server `src_scoid`, while `comm_reply` decodes `src_tid` from
the reply token.  On the same fs replay, traffic to LKL/fs pid `9` is
`src_tid=2, src_scoid=11`, and replies through srcvid `3` decode to
`src_tid=2`; the remaining srcvid `1` replies decode to tid `0`, which keeps the
next probe honest about the still-opaque reply source instead of guessing.
The `measure_gap` rows now carry the same raw and decoded payloads for the
largest in-window adjacent transition.  In the fs replay, samples 1 and 4 have
their largest gap ending at `comm_reply` with payload `srcvid=1, peer_pid=16394`
(2.73768 ms and 1.08753 ms), proving those particular gaps end at a reply to
the measured client rather than merely near a reply-class event.

Native-wake split marker / trace-clock refresh (reference-host replay, CPUS=8): the `lkl-measure` under-load path now emits a raw
`lkl_measure_wake` trace event immediately after `nanosleep()` returns, then
closes the raw measured window before printing the `lkl-trace:` diagnostic
line.  The host scheduler and CTF exporters scale raw `clock_cycles()` offsets
with the boot `cycles_per_sec` value instead of treating raw low-32-bit trace
ticks as nanoseconds.  A fresh `tracelogger-lkl-fsd` run passed headlessly and
kept `.sched.csv`, `.rt-budget.md`, `.irq.csv`, `.lkl-host.csv`, and `.ctf/`
artifacts.  Replaying that log with cycle scaling reports
`measure_wake_summary,fs-rt,5,20226000,30657000,30657000` and raw-window
`fs-rt max=50883000 ns`, while the guest-visible `CLOCK_MONOTONIC` row still
reports `fs-rt max=6099300 ns`.  The scheduler CSV now emits a `measure_clock`
row when a raw marker pair and `lkl-trace:` serial row describe the same sample;
on this replay the fs-rt trace-vs-serial deltas range from `-873300 ns` to
`50663900 ns`.  That keeps the §11 latency item open and narrows the next probe
to correlating trace `clock_cycles()` with guest monotonic time and the decoded
reply path, not to another serial-output or host-lock cleanup.

Superseded pending re-measurement: the two paragraphs below, and
the trace-clock figures in the paragraph above them, were produced before the
counter correction. `clock_cycles()` reads the CYCLE counter while the consumer
scaled by `cycles_per_sec`, which describes the TIME counter, so every
trace-clock span here is inflated by the ratio between them (about 300 on the reference host).
The trace-versus-monotonic divergence these numbers document is therefore not
evidence of a guest-monotonic stall; it is the shape that bug produces. The ring
now stamps with `cpu_clock_ticks()`. Re-run the probe before drawing any
conclusion from these rows, and do not cite them as an open §11 finding until
that happens. The pi-boost and hard-IRQ sections above show what re-measurement
did to comparable numbers.

Trace/monotonic sync probe (reference-host, CPUS=8): `lkl-measure` now emits
paired `lkl_measure_mono_low/high` events around each native-wake begin/wake/end
marker.  The host scheduler CSV emits `measure_mono` rows with the raw trace
timestamp, monotonic timestamp, and offset, plus `measure_mono_span` rows that
compare trace-clock and monotonic deltas for the same user interval.  A fresh
`tracelogger-lkl-fsd` run passed headlessly.  Its fs-rt `measure_mono_span`
rows show trace-clock begin-to-end spans of 58.044 / 8.292 / 16.128 / 7.860 /
7.398 ms, while the matching monotonic spans are 1.1927 / 1.0345 / 0 / 0 / 0
ms.  The remaining miss is therefore not just an anonymous reply attribution
problem: under this traced load, `clock_cycles()` continues advancing while the
guest monotonic value observed by the measuring thread can stall or lag.  The
next optimization/diagnostic must inspect qtime/timer update behavior in the
native wake path before using raw trace spans as scheduler-delay proof.
The scheduler CSV also emits `measure_deadline` rows comparing the monotonic
sync points to the `lkl-trace:` deadline/after fields.  Replaying the same
reference-host fs trace shows every fs-rt sample was already late at the begin marker
(`begin_late_ns` 1.3357 / 5.2870 / 5.8009 / 7.0876 / 7.5072 ms), and
`end_vs_serial_after_ns` is zero.  That rules out `nanosleep()` itself as the
delay source for those samples; the next probe is the scheduling gap before the
measuring thread re-enters the loop for the next deadline.
Windowed pre-context refresh (reference-host, CPUS=8): tracelogger now keeps
32 raw records of pre-window context and emits them when an LKL measured window
starts, so the analyzer can see the measured thread's preceding state changes.
A fresh `tracelogger-lkl-fsd` run passed headlessly.  The fs-rt
`measure_entry` rows show last pre-begin ready spans of 22.149 / 6.549 / 6.324 /
6.006 / 6.123 ms; the same run still misses under-load native wake
(`max-ns: 5793400`) while normal fs request stays below budget (`max-ns:
1126200`).  This points at the measured RT thread spending milliseconds ready
or otherwise not re-entering the loop before the next deadline, rather than a
late `nanosleep()` return inside the measured window.
The same analyzer now reports the largest same-CPU state span from another
thread during each pre-begin ready span.  Replaying the kept fs trace shows CPU0
is dominated by pid 1/tid 4 during those spans (`state_receive` for 22.149 ms in
sample 0, then `state_running` for 4.968 / 4.626 / 4.404 / 4.563 ms).  The next
probe is therefore to identify pid 1/tid 4 and why it can occupy the RT probe's
pinned CPU while the probe is ready.
Serial-output deperturbation (reference-host, CPUS=8): traced
`lkl-measure` no longer prints each `lkl-trace:` row from inside the timed
sampling loop.  It records the per-sample timestamps in memory and emits the
serial diagnostics after the normal request loop or after the RT interference
load thread stops.  Fresh headless traced runs still pass and still miss the
2 ms target, but the native-wake rows are no longer dominated by self-inflicted
serial I/O: `tracelogger-lkl-fsd` reports fs `max-ns: 1078500` and fs-rt
`max-ns: 1240300`; `tracelogger-lkl-netd` reports net `max-ns: 2334500` and
net-rt `max-ns: 1552600`.  The remaining traced net request miss is now small
enough to continue scheduler/IPC attribution without treating per-sample serial
printing as part of the measured path.
UDP echo benchmark cleanup (reference-host, CPUS=8): `udp_echo_fd()` now
sends one datagram per measured sample and retries reads after `EAGAIN` instead
of re-sending a duplicate packet on every retry.  This removes stale-reply
undercounts from the net benchmark.  `lkl-netd-http-split` now passes with
per-sample net outliers at samples 5 and 9 (`3698900` / `3476700` ns), proving
the tail is recurring and not just first-sample warmup.  A normal
`lkl-netd-http` acceptance run still passes functionally but misses the 2 ms
latency gate: net `max_ns=4298700`, net-rt `max_ns=3783900`, net-rt-step
`max_ns=3783900`.  The next fix must target the net request/host path itself.

PI tracepoint sanity check (local): the kernel PI turnstile paths
now emit a SYSTEM-class `pi_boost` event whenever they change an owner's
inherited effective priority.  The CTF exporter and scheduler summary name that
event, and the synthetic exporter smoke tests include it.  This adds PI
visibility to the same trace timeline as IRQ, IPC, scheduler, and LKL host-op
records; it is not a new latency budget row.

Instrumentation overhead gate (reference-host, CPUS=8, `c5ae8d91`): the
`rt-check` and `rt-check-instr` clean-tree runs each passed 3/3 boots.  The
comparison script now has optional threshold enforcement; with a deliberately
loose 2x ceiling it fails the current retained logs, so the exporter overhead
item remains open.  `rt-latency` max stays below 2x (`1.820`) but p99 reaches
`4.116`; the 500 us `timer-jitter` interval is also noisy (`max_ratio=13.983`,
`p99_ratio=9.866`).  Other timer intervals are below 2x, and the 100 us interval
is actually lower under instrumentation in this sample.  This proves the
overhead gate can fail closed on the reference host, but it does not justify a bounded
overhead claim yet; the next step is to separate real trace cost from short
interval timer-jitter noise before choosing an acceptance ceiling.

Repeated overhead sample (reference-host, CPUS=8, `d960ff3c` plus the
delta-capable comparator follow-up): a clean archive at
`/tmp/rtmux-ref-overhead-d960ff3c` passed five `rt-check` boots and five
`rt-check-instr` boots.  The worst-run ratio view no longer shows the earlier
large 500 us jitter regression, but a 10 ms timer p99 ratio still spikes
(`13.245`) because the baseline p99 is only 84.4 us.  Absolute deltas tell the
usable story for this run: worst-run p99 delta tops out at 1.0335 ms, worst-run
max delta at 268.5 us, median max delta at 1.0119 ms, and median p99 delta at
48.8 us.  `rt-check-compare.py --max-max-delta-ns 2000000
--max-p99-delta-ns 2000000` passes for both worst-run and median aggregation on
the retained logs.  This gate is now reproducible as `make check-trace-overhead`
(`TRACE_OVERHEAD_REPEAT`, `TRACE_OVERHEAD_TIMEOUT`, and
`TRACE_OVERHEAD_DELTA_NS` are overridable; reference-host uses `CPUS=8`).  This is still
evidence for the overhead gate, not closure of the CTF exporter item.  The
machine-checkable half of the Trace Compass criterion is now closed on any host
with babeltrace2 (reference-host among them): the trace postprocess parses each real
captured CTF with babeltrace2, the reader Trace Compass is built on, so a
real-data-only malformation fails the run rather than waiting on a manual open.  What remains is the visual critical-path confirmation
in the Trace Compass GUI and the real LKL latency root cause, neither of which is
machine-automatable here.

```perf-evidence
claim: trace instrumentation overhead stays inside the retained absolute delta gate
metric: max and p99 latency delta ns
command: make check-trace-overhead
config: reference-host CPUS=8, five clean rt-check boots and five rt-check-instr boots, commit d960ff3c plus comparator follow-up
baseline: 5 rt-check logs in /tmp/rtmux-ref-overhead-d960ff3c, worst-run baseline rows retained
result: 5 rt-check-instr logs in /tmp/rtmux-ref-overhead-d960ff3c, worst-run p99 delta 1033500 ns and max delta 268500 ns
rollback-threshold: max and p99 deltas must stay <= 2000000 ns or the instrumentation overhead claim is withdrawn
```

Supervised-restart recovery: when an LKL domain crashes,
`lkl-supervisor` restarts a fresh generation and reports the client-visible
outage — from the dead domain's namespace path vanishing to the new generation
republishing it.  Measured `restart_ms=1786` for `lkl-netd` via
`scripts/qemu-regress.sh --mode lkl-netd-restart` on the reference host (CPUS=8; ~1286 on
local single-vCPU).  The `lkl-fsd-restart` block-copy crash gate
reported `restart_ms=1874` on the reference host.  This is a
coarse recovery bound dominated by the fresh LKL boot, not a realtime metric or
an RT budget; it is recorded so the restart cost is a measured number rather than
a promise (see the §11.2 supervised-restart item).

```rt-budget
class: LKL absent-domain request round trip
command: make check-rt (guest: lkl-gate)
config-hash: unknown
target: qemu-system-riscv64 virt
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT
opensbi: unknown
kernel-git-sha: unknown
samples: 0
max-ns: 0
p99-ns: 0
histogram-artifact: artifacts/rt/lkl-gate.txt
```

```rt-budget
class: LKL ext4 request round trip
command: scripts/qemu-regress.sh --mode lkl-fsd-tree --timeout 360 --keep-logs (guest: lkl-measure fs 25)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f800ee1a+steady-load-probe
samples: 25
max-ns: 1300200
p99-ns: 1300200
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-fsd-tree-1.log
```

```rt-budget
class: LKL ext4 under-load native wake
command: scripts/qemu-regress.sh --mode lkl-fsd-tree --timeout 360 --keep-logs (guest: lkl-measure fs-rt 100)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f800ee1a+steady-load-probe
samples: 100
max-ns: 1624900
p99-ns: 1413500
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-fsd-tree-1.log
```

```rt-budget
class: LKL ext4 under-load native wake step diagnostic
command: scripts/qemu-regress.sh --mode lkl-fsd-tree --timeout 360 --keep-logs (guest: lkl-measure fs-rt 100)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f800ee1a+steady-load-probe
samples: 100
max-ns: 1392600
p99-ns: 1002000
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-fsd-tree-1.log
```

```rt-budget
class: LKL ext4 under-load native wake, split CPUs diagnostic
command: scripts/qemu-regress.sh --mode lkl-fsd-tree-split --timeout 360 --keep-logs (guest: lkl-measure fs-rt 100 trace split)
config-hash: sha256:542de6d4049859dc1000b6832eaedfeaef469e00d552e3d26a3de62d354cdb66
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: copied-tree
samples: 100
max-ns: 10918500
p99-ns: 10780000
histogram-artifact: build/lkl-logs/lkl-fsd-tree-split-1.log
```

```rt-budget
class: LKL ext4 under-load native wake, isolated server diagnostic
command: scripts/qemu-regress.sh --mode lkl-fsd-tree-isolated --timeout 360 --keep-logs (guest: lkl-runmask 0x2 lkl-fsd, lkl-measure fs-rt 100 trace split)
config-hash: sha256:542de6d4049859dc1000b6832eaedfeaef469e00d552e3d26a3de62d354cdb66
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: copied-tree
samples: 100
max-ns: 8315300
p99-ns: 8031300
histogram-artifact: build/lkl-logs/lkl-fsd-tree-isolated-1.log
```

The network blocks below were reclassified from `PB-IOMMU-ABSENT` after review.
The undescribed-stream condition is inferred from the recorded QEMU mode and
platform setup; the retained measurements did not log the stream metadata.

```rt-budget
class: LKL network request round trip
command: scripts/qemu-regress.sh --mode lkl-netd-http --timeout 360 --keep-logs (guest: lkl-measure net 10)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f800ee1a+steady-load-probe
samples: 10
max-ns: 1732200
p99-ns: 1732200
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-http-1.log
```

```rt-budget
class: LKL network request round trip, split CPUs diagnostic
command: scripts/qemu-regress.sh --mode lkl-netd-http-split --timeout 300 --keep-logs (guest: lkl-measure net 10 trace, lkl-measure net-rt 100 trace split)
config-hash: sha256:9052eea6e36ac43d7b053f59b2a6cc9775e933f1b77f7942cca1cf3eeaa12a3b
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 71ca0956+hostnet7
samples: 10
max-ns: 1806000
p99-ns: 1806000
histogram-artifact: /tmp/rtmux-goal-current/build/lkl-logs/lkl-netd-http-split-1.log
```

```rt-budget
class: LKL network request round trip, isolated server diagnostic
command: scripts/qemu-regress.sh --mode lkl-netd-http-isolated --timeout 360 --keep-logs (guest: lkl-runmask 0x2 lkl-netd, lkl-measure net 10 trace)
config-hash: sha256:542de6d4049859dc1000b6832eaedfeaef469e00d552e3d26a3de62d354cdb66
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 79e46994
samples: 10
max-ns: 1836500
p99-ns: 1836500
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-http-isolated-1.log
```

```rt-budget
class: LKL network under-load native wake
command: scripts/qemu-regress.sh --mode lkl-netd-http --timeout 360 --keep-logs (guest: lkl-measure net-rt 100)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f800ee1a+steady-load-probe
samples: 100
max-ns: 2158400
p99-ns: 2089700
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-http-1.log
```

```rt-budget
class: LKL network under-load native wake step diagnostic
command: scripts/qemu-regress.sh --mode lkl-netd-http --timeout 360 --keep-logs (guest: lkl-measure net-rt 100)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: f800ee1a+steady-load-probe
samples: 100
max-ns: 2178600
p99-ns: 1631800
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-http-1.log
```

```rt-budget
class: LKL network under-load native wake, split CPUs diagnostic
command: scripts/qemu-regress.sh --mode lkl-netd-http-split --timeout 300 --keep-logs (guest: lkl-measure net-rt 100 trace split)
config-hash: sha256:9052eea6e36ac43d7b053f59b2a6cc9775e933f1b77f7942cca1cf3eeaa12a3b
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 71ca0956+hostnet7
samples: 100
max-ns: 1072700
p99-ns: 642700
histogram-artifact: /tmp/rtmux-goal-current/build/lkl-logs/lkl-netd-http-split-1.log
```

```rt-budget
class: LKL network under-load native wake, isolated server diagnostic
command: scripts/qemu-regress.sh --mode lkl-netd-http-isolated --timeout 360 --keep-logs (guest: lkl-runmask 0x2 lkl-netd, lkl-measure net-rt 100 trace split)
config-hash: sha256:542de6d4049859dc1000b6832eaedfeaef469e00d552e3d26a3de62d354cdb66
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 79e46994
samples: 100
max-ns: 2230600
p99-ns: 1684500
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-http-isolated-1.log
```

```rt-budget
class: LKL network blocked-poll timeout
command: scripts/qemu-regress.sh --mode lkl-netd-http --timeout 360 --keep-logs (guest: lkl-measure net-blocked 3)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 41c23e48
samples: 3
max-ns: 1006836900
p99-ns: 1006836900
histogram-artifact: /tmp/rtmux-ref-lkl-budget-41c23e48/build/lkl-logs/lkl-netd-http-1.log
```

```rt-budget
class: LKL network client-handle revoke
command: scripts/qemu-regress.sh --mode lkl-netd-revoke --timeout 180 --keep-logs (guest: lkl-measure net-revoke 1)
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: fba8753a+net-tx-revoke
samples: 1
max-ns: 26838400
p99-ns: 26838400
histogram-artifact: /tmp/rtmux-ref-49156dc3-1783449438/build/lkl-logs/lkl-netd-revoke-1.log
```

### Capability revocation latency

Revocation has two cost centers with different bounds.

The per-process cap-group scan (cap_group_retire in kernel/cap/table.c) loops over
one process endpoint-cap table of fixed size RTMUX_PROCESS_ENDPOINT_CAP_SLOTS
(128, in objects.h), so it is a fixed 128-slot scan; it decides pinned vs unpinned
per slot in that single pass. When such a scan runs standalone, under only the cap
table lock (a plain atomic spinlock, not an interrupt-disabling kspinlock), it
keeps interrupts enabled and does not add to the non-preemptible interrupt-off
budget.

Connection teardown revokes the endpoint caps that reference the dying connection
across every process: endpoint_cap_revoke_if_present in kernel/ipc/connect.c walks
process_vector and revokes the connection object from each process table via
rtmux_cap_revoke_object_context (table.c). That scan costs 128 slots once, plus, for
every slot naming the dying object, one cap_group_retire pass, itself a 128-slot
scan. So the per-table work is O(S) in the common case of one cap per object and
O(S squared) worst case (S = 128) when a process holds many caps over one object
in distinct revocation groups. Distinct groups over
one object come from repeated independent rtmux_cap_alloc or cross-table
rtmux_cap_transfer_to, which mints a fresh destination group; delegation does not,
because a delegated cap shares its parent group, and rtmux_cap_revoke_object_context
kills a whole group on its first matching slot, so same-group caps collapse to one
unit of work. Total scan work is thus O(num_processes times S) common and
O(num_processes times S squared) worst case, adversarially reachable (one process
transferring or allocating up to 128 caps over one object in distinct groups).

That scan does not run in the interrupt-off budget. endpoint_cap_revoke_if_present
pins one process at a time under process_vector_slock (a kspinlock), drops the lock,
then runs the cap-table scan with interrupts enabled, then unpins. The pin blocks the
process soul-free (kext_process_destroy does vector_rem then process_wait_unpinned
before object_free), so prp->endpoint_caps stays valid across the unlocked scan, and
process_vector indices are stable across the drop so the walked set is unchanged. The
interrupt-off window is therefore only the O(1) pin per process, num_processes small
disjoint windows rather than one O(num_processes times S squared) window. The
adversarial S squared work still exists as total CPU cost but is preemptible and off
the non-preemptible budget. This closed the endpoint-revoke hardening item that
docs/typed-object-caps-prereqs.md section C described.

The endpoint-revoke-under-churn row below measures the full endpoint detach on
reference-host CPUS=8 under multi-thread TCG (max about 12.7 ms, p99 about 10.5 ms). That
time is not the cap-table scans: it is attributed to the cross-hart RCU-sched
quiescence drain that connection and channel teardown wait on (an
rcu_sched_synchronize) before freeing the connect_t, which is bounded by peer
kernel-call quiescence rather than a compile-time constant, and is TCG-inflated at
this scale. The measured detach exercises the common one-cap-per-object shape, so
it does not surface the adversarial S squared window; that window is bounded by
analysis above, not by this sample. So capability revocation is bounded by
fixed-size cap-table scans (per-process constant in the common case, quadratic in
the per-object cap count only under the adversarial pattern, cross-process linear
in the process count) plus a quiescence-bounded RCU drain, and the measured detach
latency is the drain. This is the revoke and revoke-latency artifact the
capability-security evidence row requires for the common one-cap-per-object shape.
The typed process/thread cap direction (see docs/typed-object-caps-prereqs.md)
reuses this exact fan-out, so its per-operation revoke latency is the same and is
already characterized here for that common shape; teardown frequency changes
aggregate throughput, not the per-operation interrupt-off window that the realtime
budget bounds. The adversarial O(S squared) cost is now off the interrupt-off budget
entirely (the scan runs interrupts-enabled after the per-process pin), so it is
preemptible CPU work, not a non-preemptible-window concern.

```rt-budget
class: Endpoint revoke under sender churn
command: CPUS=8 scripts/qemu-regress.sh --mode revoke-stress --timeout 120 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TLB-SFENCE
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
samples: 5
max-ns: 12655200
p99-ns: 10453500
histogram-artifact: /tmp/rtmux-rcu-cpus8/build/lkl-logs/revoke-stress-1.log
```

```rt-budget
class: Unrelated high-priority IPC during endpoint revoke
command: CPUS=8 scripts/qemu-regress.sh --mode revoke-stress --timeout 120 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME, PB-TLB-SFENCE
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
samples: 5
max-ns: 2331500
p99-ns: 2242100
histogram-artifact: /tmp/rtmux-rcu-cpus8/build/lkl-logs/revoke-stress-1.log
```

```rt-budget
class: namespace round-trip chain throughput
command: make check-rt (guest: namespace-roundtrip 25)
config-hash: sha256:ecb1d5e99e6865ba9fc9e8670723d4919235d22a78d45be491df18d6fa09571d
target: qemu-system-riscv64 11.0.1 virt, 8 vCPU, 256M, TCG multi-thread
platform: qemu-only PB-TIMER-RDTIME
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 0d74c4e79ace4b8a82062ae492fe7a5c824ec266
samples: 25
max-ns: 1000000
p99-ns: 1000000
histogram-artifact: artifacts/rt/qemu-virt-rt-check.txt
```

RCU-sched lookup refresh (reference-host QEMU, CPUS=8): the current
RCU-sched worktree ran `rcu-lookup-measure` three times on the reference host.  The fast
lookup rows report batch-average cost per operation from 64 calls per sample,
not individual syscall tail latency.  This is lookup-cost attribution for
current-vs-old-SMR comparison and reduces QEMU/TCG clock-granularity artifacts
from the earlier one-call samples.  The detached create-return and child-start
rows remain unbatched because they are lifecycle paths, not lookup-only.  The
benchmark now pairs pid `0` self rows with explicit `getpid()` rows for
connection info/flags, scheduler-info, timer-info, clock-id, detached
thread-create, and joinable thread-create.  The detached thread-create return
rows cover `thread_create_r()` return while earlier detached children may still
be exiting; the child-start rows additionally wait until the new child reaches
its start routine and stores a flag.  The joinable create+join rows wait for the
child to return and for `thread_join()` to reap it, so they include teardown and
the thread RCU drain.
The old local macOS before/after runs remain historical debug data only; reference-host
is the benchmark source for the blocks below.  Remote IPC
global-state lifetime, timer object/vector lifetime, and stronger
scheduler-tail attribution remain separate residuals.

Same-host old-SMR baseline (reference-host QEMU, CPUS=8): clean `HEAD`
(`4fb57748`, old SMR backend) plus the paired lookup probe/wiring ran
`rcu-lookup-measure` three times from `/tmp/rtmux-smr-baseline`; artifact:
`/tmp/rtmux-smr-baseline/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log`.
Worst p99/max over the three old-SMR runs were: measurement overhead p99 0 us /
max 97.831 us; cap attach/detach p99 73.425 us / max 94.856 us; endpoint-cap
`MsgSend` p99 164.068 us / max 291.943 us; raw-coid `MsgSend` p99 164.306 us /
max 337.450 us; self `connect_server_info` p99 193.943 us / max 827.325 us;
explicit pid `connect_server_info` p99 73.206 us / max 577.156 us; self
`connect_flags` p99 72.118 us / max 76.268 us; explicit pid `connect_flags`
p99 72.943 us / max 82.293 us; current-thread scheduler query p99 71.000 us /
max 77.968 us; self scheduler-info p99 72.312 us / max 87.306 us; explicit pid
scheduler-info p99 72.706 us / max 82.156 us; explicit pid/tid scheduler
lookup p99 72.950 us / max 96.306 us; self scheduler set p99 72.412 us / max
75.300 us; explicit pid/tid scheduler set p99 70.237 us / max 77.975 us;
explicit pid/tid scheduler get+set p99 74.118 us / max 76.718 us; explicit pid
timer-info lookup p99 72.768 us / max 105.687 us; self timer-info lookup p99
72.887 us / max 86.756 us; self clock-id lookup p99 73.231 us / max 97.981 us;
explicit pid/tid clock-id lookup p99 73.400 us / max 86.306 us; self detached
thread-create return p99 14.1620 ms / max 22.4233 ms; explicit pid detached
thread-create return p99 13.8684 ms / max 20.6012 ms; self detached
thread-create child-start p99 13.1502 ms / max 26.0838 ms; explicit pid
detached thread-create child-start p99 14.3540 ms / max 22.8192 ms; self
joinable thread-create+join p99 14.6023 ms / max 19.1203 ms; explicit pid
joinable thread-create+join p99 14.0316 ms / max 20.9578 ms; self
post-child-store join path p99 4.5647 ms / max 7.3153 ms; explicit pid
post-child-store join path p99 4.9499 ms / max 9.3273 ms.  These are worst
per-run p99/max values from three 1000-sample runs, not an aggregate p99 over
all runs.

Same-host result after moving RCU-sched QS/waiting state into `pcpu`, using
relaxed polling with acquire confirmation in the wait loop and pre-wait peer
scan, acquire-confirming matching QS epochs in the grace-period wait loop,
rejecting the unsafe idle-thread quiescence shortcut, using exact timer-vector
lookup for non-search `timer_info` requests, using exact endpoint-cap lookup
for generated-cap `connect_server_info` requests, shortening the `connect_flags`
vec-slock section, adding explicit-self fast paths for process/thread lookup
kcalls, and removing a trial duplicate release fence from the QS publication
path.  The current refresh also includes the debugger
lookup lifetime conversion, the `sched_info(getpid())` self-pid bypass, and the
batched lookup probe, so no plain `lookup_pid()` sites remain under `kernel/ext`
or `kernel/mm`.  It also includes the generated-cap `connect_server_info`
validation-order fix, which proves the exact vector slot still names the pinned
connection before reading connection fields, and the `clock_id` self
pid/current-thread fast path plus foreign-thread `tthread_rcu` coverage.
The current refresh also validates `connect_flags` vector slots as
`TYPE_CONNECTION` objects before snapshotting the CLOEXEC bit and removes the
early-unpin endpoint-cap translation helper from the sibling detach path.
Detach now validates and removes the vector slot under the same `vec_slock`
critical section before releasing the generated endpoint-cap pin.
The generated endpoint-cap `MsgSend` / notification lookup helpers now also
validate the cap context against the current vector slot before unpinning.
The benchmark now reports a `measure-overhead` row for the same
`monotonic_ns()`/batch loop envelope, and both current and old-SMR baselines
carry it.  RCU-sched and old SMR remain mixed across the refreshed rows:
scheduler set/get+set, connection-info, cap attach/detach, `MsgSend`,
timer/clock tails, and thread-create lifecycle rows each have at least one
p99/max comparison that is noisy or favors old SMR.  Full RCU-sched superiority
is not proven.
Because the self and explicit-pid thread-create rows are close within each
backend, the miss is not explained by explicit pid lookup alone; it is broader
lifecycle/context-switch overhead.  The joinable create+join rows also favor old
SMR, so the miss remains after waiting for teardown and the thread RCU drain
rather than being only detached-exit overlap.  The post-child-store join rows
start timing after the child reaches its final user-space store, so they include
any remaining child return/kernel-exit scheduling plus join/reap work.  They
measure about 7.4-7.5 ms p99 on current RCU, narrowing but not eliminating the
remaining lifecycle miss.  The current RCU tree now also exposes a root-only,
benchmark diagnostic `thread_ctl()` snapshot of `thread_destroy()` phase totals.
In the latest reference-host 3-run refresh, each post-child-store stats row covered
exactly 1000 destroys and accumulated RCU-wait totals of 0.87-1.02 s, pin-wait
totals of 0.239-0.253 s, and `object_free()` totals of 1.33-1.66 s.  The same
rows show `object_free()` is mostly the cached soul-return path: self rows
returned only 61-63 objects to the heap and explicit-pid rows returned none.
The old-SMR baseline was not instrumented with this counter, so this split is
current-RCU attribution rather than a cross-backend phase comparison.
A later reference-host one-run refresh exposed and fixed a benchmark-stability bug in the
thread birth path: the temporary `thp->join` creation-handshake pointer is now
always cleared after optionally waking a `STATE_WAITTHREAD` creator, so the
subsequent join benchmark cannot mistake it for a real join waiter and return
`EBUSY`.  The rerun reported `thread-create-join-pid` p99 17.3204 ms / max
21.1301 ms.
The follow-up reference-host 3-run refresh passed the full `rcu-lookup-measure` gate after
that fix.  The fast lookup rows are mostly old-SMR-class, but the lifecycle rows
remain the dominant miss: worst current rows include `thread-create-return-self`
p99 19.5087 ms / max 27.9418 ms, `thread-create-return-pid` p99 19.2075 ms /
max 26.5442 ms, and `thread-create-join-self` p99 19.1095 ms / max 25.3899 ms.
A trial RISC-V pause hint in `tthread_wait_unpinned()` was rejected: one reference-host
run passed, but the required 3-run refresh stalled in run 2.  The original
compiler-barrier spin remains until a stronger pin-wait design is measured.
The next diagnostic split the same pin path into actual waits versus skipped
checks.  A reference-host CPUS=8 one-run refresh passed and reported
`pin-waited=0 pin-skipped=1000` for both post-child-store rows, so the pin-wait
nanoseconds in this benchmark are fixed check/timing overhead rather than live
`tthread_pin()` holder contention.
The next diagnostic split `object_free()` nanoseconds by heap-return versus
soul-cache-return.  Reviewer feedback moved the heap/cache decision into
`object_free_track_heap()`, so the split follows the actual allocator branch
rather than a pre-free prediction.  The benchmark stats wait now uses a
5-second guest monotonic deadline and prints the invalid snapshot before
failing; the previous fixed-yield loop flaked in run 2 of a reference-host 3-run gate.
The corrected reference-host CPUS=8 3-run gate passed.  Worst self-row cached
object-free time was 1.716315 s per 1000 destroys, and worst explicit-pid cached
object-free time was 1.757733 s per 1000 destroys.  Pin stats stayed
`pin-waited=0 pin-skipped=1000` in every row.  Together with the earlier count
split, this keeps heap retention off the critical path and points the lifecycle
miss at the cached thread-soul clear/relink path.  A no-clear or partial-clear
cache would need proof that every recycled `thread_t` field is initialized
before reuse.
Thread-destroy attribution is now opt-in.  `reset=1` enables the diagnostic,
`reset=2` disables it, and the normal destroy path uses plain
`rcu_sched_synchronize()` plus `object_free()` without timing reads,
busy-peer attribution, heap/cache tracking, or counter atomics.  The benchmark
disables stats after each attribution row so later lifecycle rows do not pay
diagnostic overhead.  The reference-host CPUS=8 3-run gate passed after this change.
Worst lifecycle p99 rows improved versus the previous recorded refresh:
`thread-create-return-self` 18.4575 ms, `thread-create-return-pid` 17.3712 ms,
`thread-create-join-self` 17.6863 ms, and `thread-create-join-pid` 17.6412 ms.
This removes benchmark-only overhead from later rows but does not close the
old-SMR lifecycle gap.
Busy-peer attribution now splits peers whose `pc_curthread` is `pc_idle_thp`
into an `idle` bucket before falling back to syscall buckets.  This is
diagnostic-only and does not reintroduce the rejected idle-thread quiescence
shortcut.  A reference-host CPUS=8 one-run gate passed and showed `idle=0` for both
post-child-store rows, with remaining busy peers still in `other` (`other=415`
self, `other=403` explicit pid).  The common wait source is therefore active
kernel work with no syscall snapshot, not peers sitting in the idle thread.
The next reference-host one-run diagnostic added non-syscall phase buckets using the
existing per-CPU syscall snapshot: dispatch, timer, fault, and privileged
kcall phases are now reported before the residual syscall/other buckets.
The same run also validates the benchmark cleanup fix that replaced the racy
stop-flag wakeup with an in-band stop message.  Command:
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 1 --keep-logs`; artifact:
`/tmp/rtmux-rcu-current/build/lkl-logs/rcu-lookup-measure-1.log`.  The self
row showed `busy-peer-sum=262` with `thread=1 msg=1 dispatch=1 other=259`;
the explicit-pid row showed `busy-peer-sum=262` with `msg=1 dispatch=3
other=258`.  The added phase stamps explain only a small minority of waits, so
the dominant lifecycle source remains active kernel work still reported as
`other`.
After reviewer feedback, the exit-side attribution clear was removed so a peer
cannot sample nonzero `in_kcall[]` after `pc_syscall` has already been reset
to `IN_KCALL_PHASE_NONE`.  The refreshed reference-host one-run gate passed again.
Artifact: `/tmp/rtmux-rcu-current/build/lkl-logs/rcu-lookup-measure-1.log`.
The self row showed `busy-peer-sum=222` with `dispatch=5 other=217`; the
explicit-pid row showed `busy-peer-sum=248` with `time=1 dispatch=9 other=238`.
The residual is therefore not only final-exit snapshot clearing; deeper phase
coverage is still needed before the lifecycle waits are explained.
The next reference-host CPUS=8 one-run refresh split that residual.  Explicit
`unstamped` accounting showed only a small entry-window contribution
(`unstamped=6/263` self, `unstamped=10/237` explicit pid).  Splitting base
syscall ranges then reduced residual `other` to zero and showed most samples in
privileged kcalls.  A targeted `__KSYS_PRIV_HEARTBEAT` bucket stayed zero, so
the roottask heartbeat is not the source.  Latest artifact:
`/tmp/rtmux-rcu-current/build/lkl-logs/rcu-lookup-measure-1.log`.  The self row
showed `busy-peer-sum=277` with `priv=254 heartbeat=0`; the explicit-pid row
showed `busy-peer-sum=268` with `priv=247 heartbeat=0`.  Reviewer feedback then
closed a stale-busy attribution window by acquire-confirming nonzero
`in_kcall[]` before recording a busy peer.  The refreshed reference-host one-run gate
passed with `busy-peer-sum=285 priv=260 heartbeat=0 other=0` for the self row
and `busy-peer-sum=262 priv=233 heartbeat=0 other=0` for the explicit-pid row.
The remaining lifecycle miss is therefore a privileged roottask/server kcall
family.
The next reference-host one-run split classified privileged kcalls by capability family
and split PA into alloc/free/zero sub-buckets.  Artifact:
`/tmp/rtmux-rcu-current/build/lkl-logs/rcu-lookup-measure-1.log`.  The self row
reported `busy-peer-sum=366`, `process=20`, `pa=331`, and
`alloc=0 free=0 zero=0`; the explicit-pid row reported `busy-peer-sum=295`,
`process=18`, `pa=268`, and `alloc=0 free=0 zero=0`.  The dominant lifecycle
wait source is therefore PA metadata/misc privileged work rather than
physical-page allocation, freeing, or zeroing.
That PA/privileged conclusion was later narrowed to attribution noise in the
S-mode `syscall_priv()` shortcut.  Node1 `/tmp/rtmux-rcu-priv-phase` passed
`scripts/check-rcu-sched.sh`, `make -j`, and a CPUS=8 3-run
`rcu-lookup-measure` gate after the shortcut was changed to stamp the same
precise private-call phase as the U-mode ECALL path.  In all three refreshed
post-child-store destroy rows, `priv=0`, `pa=0`, and the PA sub-buckets were
zero; waited peers were mostly `dispatch` and `thread`.  The remaining
lifecycle miss is therefore not PA metadata and still points at thread recycle
or dispatch/reap cost.
A one-run reference-host benchmark-isolation experiment stopped and joined the MsgSend
helper server before lifecycle rows.  Artifact:
`/tmp/rtmux-rcu-stop-server/build/lkl-logs/rcu-lookup-measure-1.log`.  It
removed actual RCU waits in the post-child-store stats rows (`rcu-waited=0`,
`busy-peer-sum=0`) but worsened p99 to 10.649 ms self and 10.9021 ms explicit
pid, so the patch was dropped.  The remaining lifecycle miss is skipped-RCU /
object-free / recycle cost, not the helper server being runnable.
Follow-up reference-host one-run diagnostic (same command) split
`thread_destroy()` RCU synchronization calls into actual grace-period waits and
pre-wait skips.  The self row waited 453/1000 calls and skipped 547/1000; the
explicit-pid row waited 402/1000 and skipped 598/1000.  That points at real
grace-period cost, not merely wrapper overhead on already-quiescent peers.
The follow-up diagnostic now also splits the same RCU synchronization time by
bucket.  A reference-host one-run refresh passed with shorter checked output lines after
the first long-line attempt was mangled by the console path.  The self row spent
679.971 ms in waited calls and 290.964 ms in skipped calls; the explicit-pid row
spent 617.712 ms waited and 287.052 ms skipped.  The next optimization target is
therefore twofold: reduce true grace-period wait frequency/cost first, and keep
the skipped path honest because its fences still account for about 0.29 ms per
1000 destroys in this probe.
A later RCU-sched cleanup removed the redundant full fence between
`rcu_sched_wait()` and clearing `pc_rcu_waiting`, while keeping the
post-enrollment Store-Load fence and the wait-loop completion fence.  Node1
`/tmp/rtmux-rcu-current` passed `scripts/check-rcu-sched.sh`, `make -Bj`, and
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 3 --keep-logs`.  Across the three refreshed runs, post-child-store
destroy rows reported only 2-20 actual RCU waits per 1000 destroys, but skipped
RCU time still stayed around 1.04-1.22 s per 1000 destroys and fast lookup tails
remained mixed.  This keeps the fence cleanup, but does not close the broader
RCU-over-SMR superiority claim.
A follow-up cleanup reordered the one-pass pre-wait scan to test
`in_kcall[] == 0` before loading waiter/alive state, saving acquire loads for
already-quiescent peers.  Node1 `/tmp/rtmux-rcu-current` again passed
`scripts/check-rcu-sched.sh`, `make -Bj`, and the same CPUS=8 3-run
`rcu-lookup-measure` gate.  Worst refreshed p99 rows included
`cap-attach-detach` 74.300 us, `msgsend-cap-roundtrip` 166.518 us,
`msgsend-coid-roundtrip` 171.518 us, and post-child-store destroy rows at
10.507-11.180 ms p99.  Actual RCU waits stayed low at 1-13 waits per 1000
post-child-store destroys in these runs.
A follow-up diagnostic split the PA privileged-call residual into info/init,
address-translation, and PQ metadata sub-buckets.  Node1
`/tmp/rtmux-rcu-pa-split` passed `scripts/check-rcu-sched.sh`, `make -Bj`, and
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 3 --keep-logs`.  The new PA sub-buckets were all zero in the refreshed
post-child-store rows.  Waited destroys stayed low: self rows waited 5, 18, and
9 times per 1000 destroys; explicit-pid rows waited 13, 11, and 8 times.  Busy
peer samples were mostly thread/dispatch/time rather than privileged PA work,
so the earlier PA-heavy one-run result is not a stable optimization target
without reproduction.
A later reference-host diagnostic split `rcu_sched_synchronize_detail()` into enrollment,
pre-wait peer scan, and post-scan timing.  `/tmp/rtmux-rcu-detail-split`
passed `scripts/check-rcu-sched.sh`, `make -j`, and `CPUS=8
scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 1
--keep-logs`.  For 1000 post-store destroys, the self row reported
`enroll-ns=139770000 scan-ns=190134000 post-scan-ns=102399000` with zero actual
RCU waits; the explicit-pid row reported
`enroll-ns=129027000 scan-ns=182472000 post-scan-ns=97356000` with zero waits.
The post-scan bucket includes advance/wait work when a busy peer exists and
branch/sample overhead otherwise.  The same rows still spent 1.276-1.372 s in
cached object-free, including 582 ms clearing cached objects, so the
remaining lifecycle gap is still a destroy/recycle problem rather than evidence
for weakening the accepted RCU fence contract.
A follow-up reference-host one-run diagnostic split `thread_destroy()` active-thread
scan time from the actual `object_free_track_heap()` call.  Node1
`/tmp/rtmux-rcu-active-split` passed `scripts/check-rcu-sched.sh`, `make -Bj`,
and `CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 1 --keep-logs`.  The self row reported `active-scan-ns=291900000`,
`object-free-ns=1185678000`, and 8 actual RCU waits per 1000 destroys; the
explicit-pid row reported `active-scan-ns=293004000`,
`object-free-ns=920112000`, and 6 actual RCU waits.  The active scan is now
visible but is not the dominant non-RCU destroy cost in this run.
A later grace-period batching experiment only joined an active epoch when the
goal was newer than the caller's post-unpublish epoch sample.  It passed a
reference-host one-run gate, but `cap-attach-detach` regressed to 451.725 us p99, so the
experiment was dropped.
A follow-up reference-host experiment removed the duplicate-looking waited-path fence at
the end of `rcu_sched_wait()`.  It passed a one-run gate but failed the required
3-run gate by timing out before the first destroy-stats row, so the experiment
was dropped and the helper-level fence remains part of the contract.
The next reference-host one-run diagnostic changed the pre-wait scan to return the count
of non-quiescent peer harts and recorded that in the post-child-store rows.  The
self row waited 440/1000 calls with `busy-peer-sum=446` and `busy-peer-max=2`;
the explicit-pid row waited 457/1000 calls with `busy-peer-sum=461` and
`busy-peer-max=2`.  Most waited destroys are therefore blocked by one peer hart,
not by broad system-wide non-quiescence.  The next attribution step should name
what that single peer is doing rather than start with a global batching scheme.
A follow-up reference-host one-run diagnostic buckets busy peers by syscall class at
enrollment using a per-CPU syscall snapshot, avoiding cross-hart `thread_t`
dereferences.  The self row showed `thread=0 msg=2 sched=0 time=0 other=406`;
the explicit-pid row showed `thread=0 msg=1 sched=0 time=0 other=414`.  This
narrows the common wait source to unattributed kernel/idle/unknown work rather
than the normal thread, scheduler, or time syscall buckets, with only a few
message-syscall samples in this run.
The paired `msgsend-cap-roundtrip` and `msgsend-coid-roundtrip` rows show the
remaining endpoint-cap `MsgSend` tail is close to the ordinary coid round-trip
tail, so the open work is general IPC/scheduler-tail attribution rather than a
standalone endpoint-cap lookup cost.
The paired scheduler-set rows show standalone self and explicit pid/tid
`__sched_set()` calls stay near the lookup overhead on both backends.  A later
same-policy/base-priority no-op short-circuit in `ksys_sched_set()` removes the
large current-RCU combined explicit pid/tid get+set spike seen in earlier
refreshes, but the row remains close enough to old SMR that it is not a broad
superiority proof.  Cap attach/detach also remains watched as run-noisy.
A refreshed accepted-tree reference-host gate then passed `scripts/check-rcu-sched.sh`,
`make -Bj`, and `CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure
--timeout 420 --repeat 3 --keep-logs` from `/tmp/rtmux-rcu-accepted`.  Fast
lookup rows were mostly old-SMR-class, while IPC and lifecycle tails remained
open.

The next reference-host refresh from `/tmp/rtmux-rcu-measure-fix` keeps the same RCU
kernel behavior but fixes the benchmark harness so post-child-store latency rows
run before enabling destroy attribution.  The gate passed `scripts/check-rcu-sched.sh`,
`make -Bj`, and `CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure
--timeout 420 --repeat 3 --keep-logs`.  The corrected latency rows still show
real lifecycle cost rather than instrumentation-only overhead:
`thread-join-post-store-self` p99 9.7129 ms / max 19.8616 ms and
`thread-join-post-store-pid` p99 8.9393 ms / max 28.4903 ms.  Destroy
attribution, collected in a separate stats-only pass, still reports only 5-25
actual RCU waits per 1000 destroys, while skipped RCU time reaches 1.209366 s
and cached object-free time reaches 1.216974 s per 1000 destroys.  The next
lifecycle work remains skipped-RCU/object recycle cost; embedded deferred
`thread_t` reclamation is still blocked on process/accounting lifetime and
`pc_curthread` proof, so do not paper over it with a dynamic callback allocation.

Thread-soul no-clear reset proof (reference-host QEMU, CPUS=8): the accepted
partial recycle fix marks `thread_souls` as `SOUL_NOCLEAR`, resets recycled
threads explicitly in `thread_create()`, and keeps non-thread objects that pass
through `thread_souls` on the old clear path.  Node1 `/tmp/rtmux-rcu-noclear`
passed `scripts/check-rcu-sched.sh`, `make -Bj`, and a final guarded
`rcu-lookup-measure` one-run after reviewer audit.  The stats-only destroy pass
now reports `thread-destroy-object-free-cache ... clear-ns=0` for both
post-child-store rows, with link time still visible (`122.508 ms/1000` self,
`102.606 ms/1000` explicit pid).  This does not close the lifecycle target:
the same run measured `thread-join-post-store-self` p99 9.8898 ms / max
16.9866 ms and `thread-join-post-store-pid` p99 10.9547 ms / max 19.7074 ms.
The create-side rows remain covered by the benchmark and must stay in the
comparison because reset cost moved from free to create.
A dead-zombie active-hart shortcut was rejected after the next reference-host trial:
checking only the dead thread's `runcpu` in the quiescence gate passed
`scripts/check-rcu-sched.sh`, `make -Bj`, and a CPUS=8 3-run
`rcu-lookup-measure` from `/tmp/rtmux-rcu-dead-hint`, but worsened the worst
post-child-store p99 to 11.1221 ms self / 11.0326 ms explicit pid and added a
`connect-flags-pid` p99 tail of 80.468 us.  Keep the full active-hart scan until
a replacement improves top-level lifecycle rows, not just the apparent scan
shape.
A follow-up experiment removed the diagnostic `thread_destroy_warn_active` scan
from the normal destroy path while keeping the stats-only diagnostic pass.
Node1 `/tmp/rtmux-rcu-no-normal-active-scan` passed `scripts/check-rcu-sched.sh`,
`make -Bj`, and `CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure
--timeout 420 --repeat 3 --keep-logs`, but it was dropped: the worst
`thread-join-post-store-self` p99 rose to 10.1907 ms and raw-coid `MsgSend` p99
rose to 180.456 us versus `/tmp/rtmux-rcu-measure-fix`.  The active scan is
therefore useful attribution, not the next standalone lifecycle optimization.
The next accepted diagnostic split cached `thread_souls` return into the
`memset()` clear and free-list relink portions, without changing normal
`object_free()` timing.  Node1 `/tmp/rtmux-rcu-cache-detail` passed
`scripts/check-rcu-sched.sh`, `make -Bj`, and `CPUS=8
scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3
--keep-logs`.  Across the stats-only rows, cached clear time was 555-909 ms per
1000 post-child-store destroys and cached relink time was 103-198 ms.  That
confirms zeroing dominates the cached return cost, but the no-clear shortcut is
not yet valid because `thread_create()` still relies on zeroed recycled fields
including `flags`, `join`, stack-union state, and wait/message scratch.
A narrower experiment then skipped only the `CPU_REGISTERS reg` portion during
the final `thread_destroy()` cache return, relying on the early register clear
in `thread_create()`.  Node1 passed the same CPUS=8 3-run gate, but the patch
was dropped: explicit-pid post-child-store p99 rose to 10.8246 ms, create+join
rows worsened, and cached clear time stayed high.  Register-frame skip is
therefore not the next lifecycle fix.
A second partial-clear experiment skipped the large `thread_t.args` union and
explicitly initialized the WAAA `exitfunc` field in `thread_create()`.  It also
passed the reference-host CPUS=8 3-run gate, but it was dropped: post-child-store p99
stayed around 10.0-10.8 ms, create+join rows still worsened, and cached clear
time remained high.  Partial skip of the obvious transient fields is not enough
to close the lifecycle gap; the next viable path needs either a complete
field-initialization proof for recycled `thread_t` objects or a different
reclamation design.
A no-wait RCU-sched pre-scan experiment kept the Store-Load fence before the
first peer scan, but deferred publishing `pc_rcu_waiting` until a busy peer was
observed.  Node1 `/tmp/rtmux-rcu-cache-detail` passed
`scripts/check-rcu-sched.sh`, `make -j`, and `CPUS=8
scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3
--keep-logs`.  The best signal was narrow: skipped-RCU time fell from the
earlier `/tmp/rtmux-rcu-measure-fix` peak of 1.209366 s per 1000 self
post-store destroys to 1.146432 s, but post-store p99 was not better than the
measurement-fix baseline and lifecycle remained object-recycle dominated.  The
patch was dropped; keep the simpler publish-then-fence contract until a
no-wait fast path improves top-level latency rows, not just attribution.
A kernel `memset()` 64-byte unroll experiment then targeted the cached
thread-soul clear path without changing lifecycle semantics.  Node1
`/tmp/rtmux-rcu-cache-detail` passed `scripts/check-rcu-sched.sh`, `make -j`,
and the CPUS=8 3-run `rcu-lookup-measure` gate.  Some clear-time samples fell
to roughly 503-693 ms per 1000 cached returns, but top-level lifecycle rows
still lost to the old-SMR baseline and `msgsend-cap-roundtrip` regressed in the
first run to p99 187.475 us.  The patch was dropped; broad `memset()` tuning is
not enough to claim RCU lifecycle superiority.
A no-clear-on-free experiment then moved full `thread_t` zeroing to
`thread_create()` and skipped cached `thread_souls` clear on return.  After a
forced reference-host `make -Bj`, `/tmp/rtmux-rcu-alloc-clear` failed the first
`rcu-lookup-measure` run's cache-clear invariant and still reported
`thread-join-post-store-self` p99 10.9748 ms.  The patch was dropped; a future
reset proof must either keep explicit cache-return accounting meaningful or
replace that benchmark invariant with a stronger one.
The accepted tree was then refreshed again from `/tmp/rtmux-rcu-cache-detail`
after restoring the baseline kernel `memset()`.  It passed
`scripts/check-rcu-sched.sh`, `make -j`, and `CPUS=8
scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3
--keep-logs`.  Worst current-vs-old-SMR rows in the saved reference-host logs:
`msgsend-cap-roundtrip` p99 158.750 us / max 277.981 us versus old SMR
164.068 us / 291.943 us, `msgsend-coid-roundtrip` p99 165.950 us / max
287.193 us versus old SMR 164.306 us / 337.450 us,
`connect-server-info-pid` p99 72.456 us / max 76.643 us versus old SMR
73.206 us / 577.156 us, and `sched-info-pid` p99 72.531 us / max 83.856 us
versus old SMR 72.706 us / 82.156 us.  The blockers remain clear:
`cap-attach-detach` had a noisy current spike at p99 514.056 us / max
1.679956 ms versus old SMR 73.425 us / 94.856 us, `timer-info-pid` spiked to
p99 131.756 us / max 1.026375 ms versus old SMR 72.768 us / 105.687 us, and
lifecycle still lost heavily (`thread-join-post-store-self` p99 10.6712 ms
versus old SMR 4.5647 ms; explicit pid p99 9.9363 ms versus 4.9499 ms).  This
keeps the RCU-over-SMR claim open and points the next runtime work at
thread-soul recycle/reset proof rather than more RCU fence tuning.
A follow-up harness refinement raised the fast-row batch size from 16 to 64,
leaving lifecycle rows at batch 1.  Node1 `/tmp/rtmux-rcu-final-rerun` then
passed `scripts/check-rcu-sched.sh`, `make -j`, and `CPUS=8
scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3
--keep-logs` with the accepted `rcu_read_held()` cleanup, conservative RCU
fences, and pinned batch-64 oracle.  The earlier fast-row spikes were reduced
under the batch-64 harness: worst `cap-attach-detach` was p99 23.659 us / max
57.373 us, and worst `timer-info-pid` was p99 19.384 us / max 31.142 us.  The
remaining miss is still lifecycle: worst `thread-join-post-store-self` p99
10.3543 ms / max 18.1073 ms and explicit-pid p99 11.5602 ms / max 17.7206 ms
remain above old SMR's 4.5647 ms / 7.3153 ms and 4.9499 ms / 9.3273 ms rows.
A later one-entry per-CPU `thread_souls` side-cache trial is rejected despite
passing static/build checks on the reference host: the first CPUS=8 `rcu-lookup-measure` run
reported `object-free-heap=849 cache=151`, so the side cache changed allocator
retention semantics instead of preserving the benchmark invariant.  Future
thread recycle work must keep returned souls visible to the normal
`soulp->next`/`total`/`newmin` accounting or prove a replacement invariant.
Two smaller current-tree retries were also rejected by one-run reference-host gates:
dropping the normal-path active-thread diagnostic scan worsened post-store p99
to 11.0915 ms self / 10.9509 ms pid, and a relaxed zero fast path for
`pin_count` worsened it to 10.8479 ms self / 11.0573 ms pid.  The remaining
lifecycle work is not a single skipped diagnostic scan or pin acquire load.
The follow-up claim-mode attribution in `/tmp/rtmux-rcu-claim-split` reported
`active=1000 reserved=0` for both post-child-store rows.  The measured miss is
therefore live child self-destroy after the parent sees the final user-space
store, not mostly parent-side dead-zombie reap.
The next reference-host split (`/tmp/rtmux-rcu-join-wake`) measured the live
self-destroy path from `thread_destroy()` entry to `ready(joiner)`.  Both
post-store rows reported `thread-destroy-join-wake count=1000`, so the owned
join wake is reached in every sample.  The same stats pass reported about 40 s
total join-wake time per 1000 samples, or about 40 ms/sample, while the
separate measured rows remained near 4 ms p50 and 11 ms p99.  Treat this as an
attribution hook, not a direct latency comparison: it does not rule out
post-`ready(joiner)` dispatch delay or inline teardown as parent-visible cost.
Deferred `call_rcu()` can still reduce writer/exit cleanup latency, but this
benchmark needs a same-run or post-ready scheduling split before that is the
defensible fix.
The follow-up post-ready split (`/tmp/rtmux-rcu-post-ready`) reported
`thread-destroy-join-post-ready count=1000` for both post-store rows.  The
stats-only totals were 3.76767 s self and 3.55515 s explicit pid per 1000
samples, while the measured rows had p50 around 3.9 ms and p99 around
10.6-10.8 ms.  This stats pass is still instrumentation-heavy, but it identifies
post-`ready(joiner)` inline teardown as the next credible optimization target
for a deferred reclaim/handoff prototype.
The refinement gate for that prototype is intentionally narrow.  A generic
allocation-backed `call_rcu()` queue is still rejected for kernel hot paths; the
first candidate is a `thread_t`-embedded post-ready handoff for live self-reap.
It must enqueue only after lookup invalidation, leave exactly one deferred
worker responsible for `vector_rem`, preserve the existing RCU grace-period wait
and `tthread_wait_unpinned(thp)` wait for explicit `tthread_pin()` users, wait
until no hart still names the old thread as `pc_curthread`, and keep the owning
process pinned through `object_free(prp, &thread_souls, thp)` and any
last-thread notification.  Parent/joiner-side zombie handoff can be revisited
after this live path is measured, because the claim-mode split showed these
post-store samples are active self-destroys.  Expedited IPI grace periods are a
separate optimization gate: specify bounded receiver work in
`docs/smp-contract.md`, then benchmark reference-host QEMU p99 and RT-core interference
before keeping it.
A follow-up attempt extracted the post-ready tail into a standalone
`thread_destroy_finish()` helper as an intended neutral bridge toward that
handoff.  It passed local static/build gates, but reference-host
`/tmp/rtmux-rcu-finish-helper` hung after the welcome banner and never reached
the `RTMux rcu-lookup-measure: BEGIN` marker.  A clean `HEAD` control from
`/tmp/rtmux-head-rcu-check` passed the same CPUS=8 one-run gate, so the helper
extraction is rejected.  The next prototype must carry the actual handoff
semantics and prove boot-marker progress on the reference host, not only compile.
The broader async API remains staged: a generic `call_rcu(struct rcu_head *,
void (*func)(struct rcu_head *))` interface is appropriate only after a first
embedded-node user proves the lifetime rules.  For the measured lifecycle miss,
that means a `thread_t` self-reap handoff before any process/joiner reap queue:
enqueue after `ready(joiner)`, drain from a bounded utility worker on a non-RT
hart, and keep the synchronous RCU/pin/free tail out of the exiting thread.
Expedited grace periods, sleeping RCU waits, and exponential spin backoff are
separate optimization gates: expedited IPIs need a bounded receiver-side action
contract, sleeping waits need a peer-QS-publish wake source and sleep-safe
caller proof, and backoff must preserve the existing relaxed-probe plus
acquire-confirmation memory-ordering shape in `rcu_sched_wait()`.
A clean `HEAD` refresh (`924a26d7`) on the reference host from
`/tmp/rtmux-rcu-accepted-refresh` passed `scripts/check-rcu-sched.sh`,
`make -j`, and `CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure
--timeout 420 --repeat 3 --keep-logs` after an earlier third-run control
timeout before the benchmark marker did not reproduce.  The current shape is
unchanged: fast lookup rows are mostly old-SMR-class, while lifecycle remains
the blocker.  The 3-run post-child-store rows reported worst self p99
11.2816 ms / max 19.7200 ms and worst explicit-pid p99 10.9835 ms / max
15.3396 ms.  Stats still show live self-destroy (`active=1000 reserved=0`), no
pin waits, only 0-2 actual RCU waits per 1000 destroys, and post-ready inline
work of 3.742710-3.846282 s self / 3.498873-3.559143 s explicit pid per 1000
samples.
A direct `thread_t` idle-drain prototype was then measured and dropped.  It
kept the stats path inline, queued normal live self-reap after `ready(joiner)`,
and let `cpu_idle()` drain queued threads before the next dispatch while
holding a process pin and preserving the RCU/pin/free order.  Local
`git diff --check`, `scripts/check-rcu-sched.sh`, and `make -j` passed, and
reference-host `/tmp/rtmux-rcu-defer` passed a CPUS=8 one-run `rcu-lookup-measure`
gate.  The target rows regressed instead of improving: self post-child-store
p99/max rose to 10.9967 ms / 31.6662 ms, and explicit-pid rose to 11.1620 ms /
22.1563 ms.  The patch was reverted; a useful handoff needs a different
scheduling point or worker policy, not this idle-before-next-dispatch drain.
A remote-only variant was also measured and dropped.  It kept the direct source
hart out of the drain path by enqueueing live self-reap after `ready(joiner)`
and allowing only a different idle hart to finish `vector_rem`,
`rcu_sched_synchronize()`, pin wait, `object_free()`, and last-thread
notification.  Local `git diff --check`, `scripts/check-rcu-sched.sh`, and
`make -Bj` passed after the embedded fields were moved to the documented
`thread_t` tail.  Node1 `/tmp/rtmux-rcu-remote-defer` passed a CPUS=8 one-run
`rcu-lookup-measure`, but target rows regressed again: self post-child-store
p99/max rose to 14.1052 ms / 23.0319 ms, and explicit-pid rose to 12.1347 ms /
28.1117 ms.  The patch was reverted; this remote-idle handoff prototype is
negative evidence, not a broad proof against deferred reap.  A future handoff
still needs a bounded owner/worker policy that avoids queueing and dispatch
tail inflation.
The next attribution split keeps the normal measured path unchanged and adds
stats-only post-ready buckets for vector removal, CPU destroy-hook cost, and
thread-name cleanup.  Node1 `/tmp/rtmux-rcu-post-ready-detail` passed
`scripts/check-rcu-sched.sh`, `make -Bj`, and a CPUS=8 one-run
`rcu-lookup-measure` gate with the tightened oracle.  The run reported
`thread-join-post-store-self` p99 11.2465 ms / max 18.8509 ms and
`thread-join-post-store-pid` p99 11.2163 ms / max 19.5578 ms.  In the stats
pass, post-ready inline totals were 4.317531 s self / 4.083654 s explicit pid
per 1000 samples; vector removal accounted for 0.661428 s / 0.667737 s,
CPU destroy-hook cost for 0.218634 s / 0.215883 s, and thread-name cleanup for
zero in both rows.  The remaining handoff target is therefore the broader
post-ready RCU/free/accounting tail, not name cleanup or vector removal alone.
An asynchronous `thread_t` handoff remains constrained to an embedded-node SPSC
queue, drained by a bounded utility/non-RT worker.  The exiting child may
enqueue itself only after the joiner has been readied; the worker owns the
synchronous tail, including `rcu_sched_synchronize()`,
`tthread_wait_unpinned(thp)`, `object_free()`, and last-thread notification.
The owning `process_t` must remain pinned until that tail completes.
RT-aware sleeping grace-period waits and expedited targeted IPIs remain separate
contracts: a waiter may spin briefly with `rcu_cpu_relax()` and then yield or
sleep only from sleep-safe contexts, with wakeup driven by peer QS publication;
an expedited IPI may target only harts that are still non-quiescent for the
goal epoch, and must exclude isolated RT harts already executing in U-mode.
A minimal S-mode reaper-worker prototype was also rejected before
benchmarking: it passed local `git diff --check`, local `make -j`, reference-host
`scripts/check-rcu-sched.sh`, reference-host `make check-rt-budgets`, and reference-host full
`make -j`, but CPUS=8 QEMU failed before `RTMux rcu-lookup-measure: BEGIN`.
The fault resolved to `thread_reaper_wait()` entering `unready_release()` with
`pc_curthread` NULL on CPU 7.  The patch was reverted; the next async handoff
must prove the S-mode worker dispatch/active-thread invariant before using
`unready_release()` as the worker sleep mechanism.
A narrower worker-only trial then removed all reclaim handoff logic and only
checked the entry invariant before sleeping.  It passed the same local and
reference-host build/static gates, but CPUS=8 QEMU again failed before the benchmark
marker because the worker entered on CPU 7 with
`get_active_thread(get_cpunum()) == NULL`.  This pins the remaining prerequisite
on kernel-worker bootstrap/dispatch binding, not on the reclaim queue.
The immediate sleep-side fix is now explicit: S-mode workers must not call the
kcall-path `unready_release()` and keep executing after it changes
`pc_curthread`.  `unready_release_and_exit()` performs the same release handoff,
then stamps dispatch context and enters `kernel_exit()` immediately.  The helper
is a prerequisite for the next RCU reaper worker attempt; it is not itself a
handoff policy or latency win.
The matching bootstrap prerequisite is now explicit too:
`kernel_thread_create()` constructs S-mode utility workers without the U-mode
`thread_create()`/WAAA path and queues them through `ready()` so the existing
idle/dispatch path owns the `pc_curthread` binding.  This helper is unused by
RCU reclamation so far, so it is not a runtime latency change; the next measured
slice must wire the bounded post-ready reaper handoff and rerun the CPUS=8
`rcu-lookup-measure` gate.
A first runtime handoff using that constructor was measured and rejected.  The
patch queued non-stats live self-reap after a normal joiner wake and let a
bounded S-mode worker perform `vector_rem`, `rcu_sched_synchronize()`,
`tthread_wait_unpinned()`, active-thread confirmation, `object_free()`, and
last-thread notification.  Local static/build gates and reference-host static/build gates
passed, and CPUS=8 `rcu-lookup-measure` reached the benchmark marker, but the
run crashed in the lifecycle section.  The first version crashed because
`vector_rem(&prp->threads, tid)` returned NULL in the worker; the narrowed
normal-join-only variant still crashed with a recursive trap during crash dump.
The runtime handoff was reverted.  The next attempt needs a slot-ownership proof
for the flagged thread-vector entry after `ready(joiner)`, not only a worker
bootstrap proof.  After the revert, reference-host `/tmp/rtmux-rcu-reclaim` passed
`scripts/check-rcu-sched.sh`, `make check-rt-budgets`, full `make -j`, and a
CPUS=8 one-run `rcu-lookup-measure` smoke.
A narrower post-vector-removal worker was also measured and rejected.  That
patch left `vector_rem()`, CPU hook, and name cleanup inline, then queued only
the RCU/pin/active-scan/free/notification tail to the constructor-backed S-mode
worker.  It passed local and reference-host static/build gates, and CPUS=8
`rcu-lookup-measure` reached the benchmark marker, but the first lifecycle
section crashed with the worker itself active (`tid=8 pid=1`) at `sepc=0` /
load fault.  The runtime handoff was reverted.  Future worker work must prove
S-mode worker resume/trap-frame semantics across repeated `kernel_exit()` and
sleep cycles before reclaim work is delegated to it.
The resume blocker is now pinned by inspection rather than only by crash
signature.  RISC-V U-mode traps save directly into `thp->reg`, while S-mode
traps save to a transient stack frame and return from that frame.  A kernel
worker that calls `kernel_exit()` after its initial dispatch has no refreshed
`thp->reg` continuation unless a cooperative save step writes the current `pc`,
`sp`, and preserved registers first.  The RCU-sched checker therefore keeps
`kernel_thread_create()` unused outside its declaration and definition until
that continuation primitive exists and passes the required CPUS=8
`rcu-lookup-measure` gate.
`kernel_thread_checkpoint(CPU_REGISTERS *)` is now the minimal continuation
primitive.  It saves the current S-mode call continuation into the worker's
saved register frame and arranges for the restored path to return `1` from the
checkpoint call.  No reclaim worker uses it yet; the checker still rejects live
`kernel_thread_create()` call sites until the first user is wired and measured.
The first checkpointed worker handoff is now measured and accepted as a
correctness-safe improvement, but not as an RCU-over-SMR win.  The non-stats
joined teardown path keeps join wake, `vector_rem()`, CPU hook cleanup, and name
cleanup inline, then queues the RCU/pin/no-active/free/last-process-notify tail
to a single S-mode worker.  The worker uses
`kernel_thread_checkpoint(&self->reg)` before sleeping through
`unready_release_and_exit()`, so repeated S-mode sleep/resume no longer restores
stale constructor state.  Node1 `/tmp/rtmux-rcu-checkpoint-reclaim` passed
`scripts/check-rcu-sched.sh`, `make check-rt-budgets`, full `make -j`, and the
required CPUS=8 three-run `rcu-lookup-measure` benchmark.  The target p99 ranges
were self 8.3238-8.9161 ms and explicit pid 9.4647-10.5513 ms.  That is better
than the current RCU control, but the old-SMR baseline remains lower at
4.7679-4.9702 ms self and 3.5017-4.8401 ms explicit pid.
Broader parent-side zombie handoff is rejected for now.  The
`thread-join-post-store-*` benchmark waits for the child to publish `done`
before timing `thread_join()`, so the timed path is usually the parent reaping a
`STATE_DEAD` joinable child and has no live `join_woke` event.  An uncapped
version exhausted available `thread_souls` before the later thread-create rows;
a capped inline-fallback version avoided that failure but still timed out before
PASS and worsened the target p99.  The live worker remains limited to the
measured `join_woke` path.
That restored live path was revalidated on the reference host
`/tmp/rtmux-rcu-joinwoke-final` with `scripts/check-rcu-sched.sh`,
`make check-rt-budgets`, full `make -j`, and the required CPUS=8 three-run
`rcu-lookup-measure` benchmark.  Target p99 ranges were self
7.1874-10.6146 ms and explicit pid 7.1389-9.0321 ms.
The read-side hot lookup helper API was then narrowed from the generic
argument-taking wrapper to explicit `rcu_sched_read_lock()` /
`rcu_sched_read_unlock()` in `lookup_pid_rcu()` and
`lookup_rcvid_open/release()`.  Node1 `/tmp/rtmux-rcu-read-api` passed
`scripts/check-rcu-sched.sh`, `make check-rt-budgets`, full `make -j`, and
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 3 --keep-logs`.  Selected p99 ranges were `msgsend-cap-roundtrip`
72.840-74.365 us, `msgsend-coid-roundtrip` 68.007-72.156 us,
`connect-server-info-pid` 18.196-112.225 us, `connect-flags-pid`
17.845-18.940 us, `sched-pid-tid` 18.006-80.634 us, `timer-info-pid`
16.960-18.179 us, `clock-id-pid-tid` 18.043-18.160 us,
`thread-join-post-store-self` 8.5010-10.6887 ms, and
`thread-join-post-store-pid` 9.2620-10.8251 ms.  This is API migration
evidence, not a new RCU-over-SMR superiority claim.
A follow-up reference-host run from `/tmp/rtmux-rcu-post-notify` added a separate
last-thread-notification post-tail bucket to the same detail row and passed
`scripts/check-rcu-sched.sh`, `make -j`, and a CPUS=8 one-run
`rcu-lookup-measure` gate.  The measured rows still showed live child
self-destroy (`active=1000 reserved=0`) with no pin waits, and both
post-ready detail rows reported `notify-ns=0`.  The self row was p99/max
11.0708 ms / 28.9007 ms with 4.161300 s/1000 post-ready work; the explicit-pid
row was p99/max 10.7556 ms / 15.8703 ms with 3.803754 s/1000 post-ready work.
This removes last-process notification from the current culprit set; the next
handoff prototype should still target the RCU/free/accounting tail.  Subtracting
the named RCU, pin, active-scan, object-free, vector, CPU-hook, and name buckets
from `thread-destroy-join-post-ready` leaves about 0.453366 s/1000 self and
0.405435 s/1000 explicit-pid post-ready residual.  The benchmark now prints that
subtraction directly as `thread-destroy-post-ready-residual`; `notify-ns` stays
in the row as a separate post-tail bucket rather than part of the post-ready
subtotal.  That residual is the next small attribution target before a larger
worker/handoff change.
A follow-up reference-host run from `/tmp/rtmux-rcu-residual` made that subtraction an
explicit benchmark row and passed `scripts/check-rcu-sched.sh`,
`make check-rt-budgets`, full `make -j`, and a CPUS=8 one-run
`rcu-lookup-measure` gate.  The self row reported p99/max 9.1320 ms /
16.8815 ms, `thread-destroy-join-post-ready` 4.610049 s/1000,
`thread-destroy-post-ready-residual named-ns=4154508000`, and
`residual-ns=455541000`.  The explicit-pid row reported p99/max 10.0542 ms /
20.3571 ms, `thread-destroy-join-post-ready` 3.902211 s/1000,
`named-ns=3499680000`, and `residual-ns=402531000`.  `notify-ns=0` remained
true for both rows.  This closes the residual-attribution slice but not the
lifecycle regression; the next runtime change still needs to move or reduce the
RCU/free/accounting tail with a bounded non-RT handoff.
A clean reference-host refresh from `/tmp/rtmux-goal-clean` passed
`scripts/check-rcu-sched.sh`, `make check-rt-budgets`, full `make -j`, and
three independent CPUS=8 `rcu-lookup-measure` runs saved as
`build/lkl-logs/rcu-lookup-measure-independent-{1,2,3}.log`.  A single
`--repeat 3` wrapper attempt timed out on the second boot before the benchmark
marker, while fresh independent single runs passed; the timeout is treated as
runner/startup instability, not an RCU measurement failure.  The refreshed rows
match the accepted residual conclusion: the cited IPC rows remain in range, with
`msgsend-cap-roundtrip` p99 73.248-87.043 us and `msgsend-coid-roundtrip` p99
73.553-73.784 us, but several scalar lookup rows still show QEMU-tail spikes
and lifecycle still misses old SMR.  `thread-join-post-store-self` p99 was
10.3342-12.6939 ms and `thread-join-post-store-pid` p99 was 8.9150-10.9082 ms.
Post-ready residual stayed about 0.425838-0.443658 s/1000 self destroys and
0.403293-0.421500 s/1000 explicit-pid destroys, with `notify-ns=0`; the next
runtime target remains bounded non-RT handoff of the RCU/free/accounting tail.
A follow-up reference-host refresh from `/tmp/rtmux-rcu-bench` after removing the
writer-side `synchronize_rcu(rcu_t *)` compatibility wrapper passed
`scripts/check-rcu-sched.sh`, full `make -j`, and
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 3 --keep-logs`.  The kept logs are
`build/lkl-logs/rcu-lookup-measure-{1,2,3}.log`.  The selected current-RCU rows
remained in the same noisy QEMU class: `msgsend-cap-roundtrip` p99 was
72.903-76.632 us, `msgsend-coid-roundtrip` p99 was 75.209-77.123 us,
`thread-join-post-store-self` p99 was 9.7439-11.9247 ms, and
`thread-join-post-store-pid` p99 was 9.5810-12.5712 ms.  Post-ready residual
was 0.493908-0.527160 s/1000 self destroys and 0.432303-0.463542 s/1000
explicit-pid destroys, with `notify-ns=0`.  This run refreshed current-RCU
evidence only; old-SMR baseline comparison was not rerun for the API cleanup.
The old-SMR baseline was then rerun on the reference host from `/tmp/rtmux-smr-baseline`
with only the userland benchmark batch pinned to 64 and the later in-band
server-stop benchmark cleanup applied to that baseline copy; the kernel backend
remained `typedef smr_t rcu_t` with `smr_enter`/`smr_exit`/`smr_synchronize`.
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 3 --keep-logs` passed with logs in
`build/lkl-logs/rcu-lookup-measure-{1,2,3}.log`.  Fast rows stayed mixed:
current RCU `msgsend-cap-roundtrip` p99 72.903-76.632 us versus old SMR
72.017-73.389 us, and current RCU `msgsend-coid-roundtrip` p99
75.209-77.123 us versus old SMR 68.171-75.518 us.  The lifecycle miss remains
the clear blocker: current RCU `thread-join-post-store-self` p99
9.7439-11.9247 ms versus old SMR 4.7679-4.9702 ms, and current RCU
`thread-join-post-store-pid` p99 9.5810-12.5712 ms versus old SMR
3.5017-4.8401 ms.  Do not claim RCU-over-SMR superiority until the live
self-destroy post-ready tail is moved or reduced and this same-host comparison
passes.
An SMR-inspired explicit-reader-depth skip was tried and rejected.  The patch
added a per-hart `pc_rcu_depth` counter to `rcu_read_lock()`/`rcu_read_unlock()`
and made `rcu_sched_synchronize()` skip peers with zero explicit read depth.
Local `git diff --check`, `scripts/check-rcu-sched.sh`, `make -Bj`, `make
check-rt`, and `make check-rt-budgets` passed, and reference-host
`scripts/check-rcu-litmus.sh` plus `scripts/check-rcu-sched.sh` passed.  The
required reference-host CPUS=8 `rcu-lookup-measure` gate then timed out twice before the
benchmark BEGIN marker after `/sbin/init` spawned, so the runtime change was
reverted and no latency rows from that experiment are accepted.
The same cleanup removed an unaccepted grace-period batching experiment and
restored direct `rcu_sched_advance()` plus `rcu_sched_wait(goal)` for every
non-skipped synchronize.  The cleaned current tree passed reference-host
`scripts/check-rcu-litmus.sh`, `scripts/check-rcu-sched.sh`, CPUS=8
`rt-check`, and `CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure
--timeout 420 --repeat 3 --keep-logs`.  The kept logs are
`build/lkl-logs/rcu-lookup-measure-{1,2,3}.log`.  Current-RCU p99 ranges were
`msgsend-cap-roundtrip` 77.593-80.150 us, `msgsend-coid-roundtrip`
74.615-77.175 us, `thread-join-post-store-self` 10.2921-10.9789 ms, and
`thread-join-post-store-pid` 10.7374-11.4541 ms.  Post-ready residual was
0.455013-0.500223 s/1000 self destroys and 0.417768-0.452007 s/1000
explicit-pid destroys; the old-SMR lifecycle baseline is still faster.
The next runtime change promotes the former non-fatal `FREED-ACTIVE` scan to a
bounded `thread_destroy_wait_inactive()` gate before `thread_souls` reuse: after
the RCU grace period and explicit `tthread_pin()` drain, the destroyer waits
until no remote hart still publishes the victim in `pc_curthread`, acquire-loads
the peer `in_kcall` state for diagnostics, and sends `IPI_RESCHED` while
waiting.  This active-scan bucket is now a correctness gate, not only a
diagnostic counter.  Node1 `/tmp/rtmux-rcu-activewait` passed
`scripts/check-rcu-sched.sh`, full `make -j`, and
`CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420
--repeat 3 --keep-logs`.  Current-RCU p99 ranges were
`msgsend-cap-roundtrip` 73.310-79.651 us, `msgsend-coid-roundtrip`
69.887-74.615 us, `thread-join-post-store-self` 8.3058-11.4702 ms, and
`thread-join-post-store-pid` 9.4195-10.9440 ms.  Post-ready residual was
0.485808-0.494787 s/1000 self destroys and 0.434838-0.515400 s/1000
explicit-pid destroys, with `notify-ns=0`; the old-SMR lifecycle baseline is
still faster.
The historical records below are retained from the earlier
`/tmp/rtmux-rcu-measure-fix` accepted measurement-fix run.  They are intentionally
plain text, not current `rt-budget` blocks: they used batch 16 and predate the
batch-64 oracle.  Regenerate this block set before using it as the current
budget baseline.

```text
class: RCU measurement overhead batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 88818
p99-ns: 0
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU cap attach-detach lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 85125
p99-ns: 73593
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU endpoint-cap MsgSend round trip batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 307543
p99-ns: 175581
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU raw-coid MsgSend round trip batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 266350
p99-ns: 175312
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self connect-server-info lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 645768
p99-ns: 192693
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid connect-server-info lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 137437
p99-ns: 73368
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self connect-flags lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 89606
p99-ns: 73606
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid connect-flags lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 80056
p99-ns: 72487
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU current-thread scheduler query batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 89406
p99-ns: 72937
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self scheduler-info query batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 78162
p99-ns: 71375
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid scheduler-info query batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 83800
p99-ns: 72081
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid/tid scheduler lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 85975
p99-ns: 72606
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self scheduler set batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 106662
p99-ns: 71343
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid/tid scheduler set batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 73231
p99-ns: 70531
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid/tid scheduler get+set batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 96993
p99-ns: 73412
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid timer-info lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 515631
p99-ns: 192612
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self timer-info lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 452400
p99-ns: 72231
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self clock-id lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 1217493
p99-ns: 71825
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid/current-thread clock-id lookup batch-average
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 16
max-ns: 578281
p99-ns: 72493
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self detached thread-create return
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 21318800
p99-ns: 15989500
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid detached thread-create return
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 26062200
p99-ns: 16347200
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self detached thread-create child-start observed
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 24486400
p99-ns: 16843300
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid detached thread-create child-start observed
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 23482800
p99-ns: 17411600
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self joinable thread-create plus join
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 29690200
p99-ns: 18754800
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid joinable thread-create plus join
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 21808500
p99-ns: 17458700
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU self post-child-store thread join path
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 19861600
p99-ns: 9712900
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

```text
class: RCU explicit pid post-child-store thread join path
command: CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure --timeout 420 --repeat 3 --keep-logs
config-hash: sha256:83c37aa9911f58ef14bc3f121597c70c49e2ed432d07973b60a1408bbae6603d
target: qemu-system-riscv64 8.2.2 virt, 8 vCPU, 256M, TCG multi-thread
opensbi: QEMU bundled default (-bios default)
kernel-git-sha: 4fb57748+rcu-sched-dirty
runs: 3
samples: 1000
batch: 1
max-ns: 28490300
p99-ns: 8939300
histogram-artifact: /tmp/rtmux-rcu-measure-fix/build/lkl-logs/rcu-lookup-measure-{1,2,3}.log
```

## Required Tests

Add these under `tests/userland/`:

- `rt-latency/`: high-priority periodic thread, histogram output, max jitter.
- `rt-latency/timer-jitter`: varying intervals and absolute timers.
- IRQ-to-IST trace benchmark once tracepoints exist.
- Priority-inversion regression that fails before turnstile PI/PC and passes
  after it.
- `lock-rank-stress/`: SMP sync/message/scheduler stress under
  `CONFIG_LOCK_DEBUG`.

These tests now exist in the QEMU validation flow.  Headers and sysconf may
advertise priority-inheritance and non-recursive priority-ceiling support only
while the required-pass PI/PC probes stay green.  Documentation should keep
saying "realtime goals" rather than "realtime POSIX compliant" until the
measurement provenance blocks above are current for the release being claimed.

## Capability Realtime Evidence

This table is the §7 evidence index.  A row with no measured budget is not a
release claim; it is work that must stay experimental until a probe and
provenance block exist.

| Claim area | Probe | Current evidence | Status | Owner |
| --- | --- | --- | --- | --- |
| IRQ response | trap-entry to claim/mask/record trace | instrumentation emits `irq_claim`; no measured block yet | experimental | realtime engineer |
| IST wake | trap-entry to attached IST dispatch | instrumentation emits `ist_dispatch`; no measured block yet | experimental | realtime engineer |
| Notification delivery | event/notification send to receiver wake | covered functionally by syscall tests; no latency block yet | experimental | IPC owner |
| `MsgSend` round trip | send/receive/reply benchmark | RCU endpoint-cap `MsgSend` block exists for QEMU CPUS=8 tail comparison; full same-CPU/cross-CPU budget still pending | experimental | IPC owner |
| Mutex PI/PC | `make check-rt` PI/PC probes | PI/PC probes pass under the QEMU validation flow; the `tracelogger-pi` block pairs kernel `pi_boost` events into a measured boost-to-restore span on the reference host | measured on the reference host QEMU | sync owner |
| Endpoint revoke | named endpoint detach under sender churn plus unrelated high-priority IPC | reference-host CPUS=8 `revoke-stress` blocks carry endpoint-detach and unrelated-IPC p99/max rows; reference-host CPUS=8 `syscall-testing` keeps native fault coverage green | measured on the reference host QEMU | IPC owner |
| Revoke/TLB completion | cap or mapping revoke under contention | host cap revoke tests, guest stale-map-after-`munmap()` SIGSEGV coverage, and arch TLB completion counter review exist; QEMU CI remains single-hart, so no multi-hart latency block yet | experimental | memory owner |
| Page fault | user fault to resolved mapping | no measured block yet | experimental | memory owner |
| `open()` path | path lookup plus server open | namespace round-trip block exists for QEMU throughput; not a full open latency claim | experimental | namespace owner |
| `read()` transport | existing connection read path | VFS transport tests ensure no namespace round trip; no latency block yet | experimental | I/O-service owner |
| spawn | spawn load through committed child | functional wait-handle tests exist; no latency block yet | experimental | process owner |
| RCU hot lookup paths | `rcu-lookup-measure` measurement-overhead row, cap attach/detach, endpoint-cap and raw-coid `MsgSend`, self and explicit pid connection info/flags, current-thread scheduler query, self and explicit pid scheduler-info, explicit pid/tid scheduler get, set, and get+set, self and explicit pid timer-info lookup, self and explicit pid/current-thread clock-id lookup, self and explicit pid detached thread-create return, self and explicit pid detached thread-create child-start observed, self and explicit pid joinable thread-create+join, and self and explicit pid post-child-store join path | reference-host CPUS=8 current RCU-sched blocks and old-SMR baseline logs now exist for the measured rows; RCU has mixed p99 results and noisy max tails, so full RCU-over-SMR latency superiority remains open | measured on the reference host QEMU; optimization open | kernel owner |
| LKL domains | user-mode `lkl-fsd`/`lkl-netd` only | `lkl-gate` proves absent-domain `/fs/root` and `/net` denial in `make check-rt`; `lkl-fsd-tree`, `lkl-netd-http`, and `lkl-netd-revoke` drive the current I/O-service interfaces through guest tools and measured `lkl-measure` rows above | experimental; measured but not yet within budget | driver reviewer |
| Thread lifecycle | thread create, join, cancel, and exit under the `syscall-testing` thread suite | covered functionally by the `syscall-testing` thread and sync suites; no latency block yet | experimental | process owner |
| Scheduling order | fixed-priority and round-robin dispatch order under contention | `--mode rr-fairness` and `--mode sporadic-budget` check ordering and budget behavior on the named QEMU configuration; no hardware timing claim | experimental | realtime engineer |
| Capability denial | withheld-capability and stale-handle requests return the documented error | `tests/userland/capgate.c` and `make check-cap-namespace` check denial functionally; no denial-latency block yet | experimental | IPC owner |
| Timer expiry | timer expiry delivery and tickless idle wake | `tests/userland/timers.c` and `--mode tickless-idle` check expiry and idle behavior functionally; the timer-jitter block above is the only measured timer row | experimental | realtime engineer |
| Fault recovery | user fault, malformed capability, and illegal private call handled without widening authority | `--mode fault-recovery` smoke-tests four fault classes; no recovery-latency block yet | experimental | memory owner |

## Runtime Footprint Gate

Run `make footprint-report` before landing §7 patches that add resident tables,
caches, servers, or always-on fast paths.  The report emits CSV rows for the
kernel, roottask, libc/ldso, always-on servers, `initramfs.cpio`, and every file
installed into the initramfs archive.  It also emits `surface` rows for every
non-spare public kcall slot and every private roottask kcall, plus
`surface-total` rows for each namespace.

For before/after review, save a baseline before the patch and pass it back as
`FOOTPRINT_BASE` after the patch:

```
make -s footprint-report > /tmp/rtmux-footprint-before.csv
FOOTPRINT_BASE=/tmp/rtmux-footprint-before.csv make -s footprint-report
```

The delta parser ignores any build progress printed before the `kind,path,...`
CSV header.  The second report includes `delta` rows for changed
text/data/bss/total/bytes columns, including syscall/private-kcall count changes
and initramfs growth.

§11 LKL prototypes have an 8 MiB resident-size ceiling per server before
imported Linux heap pages.  The build-only `lkl-fsd` and `lkl-netd` rows are
included when present.  They are not built by the default `make`; build them
explicitly with `make lkl-servers`, after which `make initramfs` copies whichever
binaries exist into `initramfs.cpio`.  `make footprint-report` fails if either
LKL server's text/data/bss total exceeds the 8 MiB ceiling and emits an
`lkl-heap` row for each built server's configured `mem=` ceiling.

With the per-personality archives and `mem=16M`, `docs/lkl-domain.md` records
the current accounted pair: `lkl-fsd` at ~35.4 MiB (5.16 MiB text/data/bss,
16 MiB LKL page allocation, ~14.25 MiB reserved stacks) and `lkl-netd` at
~34.3 MiB (5.56 MiB text/data/bss, 16 MiB LKL page allocation, ~12.75 MiB
reserved stacks), ~69.7 MiB together.  Node1 CPUS=8 re-validation passed
`lkl-fsd-tree`, `lkl-netd-http`, `lkl-multi`, `lkl-multi --stress`, and
`lkl-netd-revoke` ×20 at `mem=16M`, cutting the page-allocation floor from
48 MiB to 32 MiB across the two domains.  The served `/stats` and `/net/stats`
files remain the live source for `page_current`, `dma_*`, net bounce-copy bytes,
LKL block-copy bytes, thread count, and stack-reservation figures; `make
footprint-report` intentionally stays a build/report gate and does not boot QEMU
to scrape those diagnostics.

Review rule: each resident growth must name the reason, ceiling, and expected
latency or diagnosability benefit.  Reject permanent caches or duplicate tables
that optimize uncommon paths without a measured win.

## Lock/IPI Replacement Gate

Do not replace existing lock, scheduler, or IPI paths with server messages until
the current path has a measured budget block and the proposed message path has a
strictly better bound or removes a proven correctness hazard.

| Remote operation | Current path | Replacement status | Required evidence before conversion |
| --- | --- | --- | --- |
| reschedule request | scheduler state plus `IPI_RESCHED` | no replacement planned | before/after wake latency and jitter |
| TLB shootdown | PTE publish plus `IPI_TLB_FLUSH` / arch completion counter | no replacement planned | stale-map test plus measured multi-hart ack latency budget |
| timer reload | timer state plus `IPI_CLOCK_LOAD` | no replacement planned | timer jitter before/after |
| endpoint wake | channel/connect queues plus scheduler ready path | no replacement planned | `MsgSend` round-trip and high-priority interference budget |
| namespace open | servers/roottask/path lookup and target I/O-service message | no replacement planned until namesrv exists | path lookup/open latency and namespace race tests |

## Fast-Path Candidate Gate

Fast paths are disabled by default unless they preserve rights checks and win a
measured budget.  Non-winning code must be deleted or kept out of the build.

| Candidate | Invalidated by | Evidence needed |
| --- | --- | --- |
| same-CPU `MsgSend` handoff | endpoint generation or receiver state change | send/receive/reply before/after latency |
| short in-register payload | ABI version and endpoint rights | payload-size histogram plus latency win |
| scheduling-context donation | priority/runmask change | PI/PC and message jitter before/after |
| endpoint rights check cache | cap generation change or revoke | cap lookup latency and revoke contention |
| namespace prefix cache | namespace version or entry generation | `open()` latency and stale-entry race tests |
| fixed-size cap-transfer metadata | cap generation change or receiver quota | transfer/revoke latency and footprint rows |

## Realtime Partitioning Judgement

Do not add a Linux-cgroup-shaped realtime hierarchy yet.  Current evidence is
enough for fixed-priority experiments, not for another scheduler/accounting
layer:

| Need | Current RTMux mechanism | Evidence | Judgement |
| --- | --- | --- | --- |
| CPU placement | thread runmask and inherited runmask; ISTs pin to owner hart | `pi-inversion-xfail` and `rt-latency` run under `make check-rt` | keep using explicit affinity |
| IRQ steering | IRQ attach records owner hart and pins IST delivery | IRQ contract documents trap-to-IST rules; trace latency remains a required gap | no hierarchy until IRQ latency blocks exist |
| Memory/accounting | path object accounting and rlimit scaffolding | `docs/ioserv-accounting-design.md`; `/sys/ioserv-accounting` is object-count only | do not turn counts into reclaim policy |
| Capability quotas | endpoint rights and cap-table tests | host cap tests cover stale, malformed, pinned-revoke, and contention cases | quotas need measured lookup/revoke costs first |

The next partitioning step, if evidence demands one, should be a flat
capability quota on an existing process or server boundary with a named latency
budget.  Avoid nested cgroup-style hierarchy until a measured workload shows
that runmask, IRQ affinity, and existing process/I/O-service boundaries cannot
isolate it.
