# RTMux Manifesto

RTMux is a POSIX RTOS built around a modern capability-based microkernel.  It
keeps the realtime control plane native, small, measurable, and Apache-2.0,
while reusing selected Linux subsystem behavior through isolated LKL service
domains.

RTMux does not compete with Linux.  It benefits from Linux where Linux is
strongest: filesystems, networking, tooling, test oracles, and ecosystem
maturity.

This document is a strategic manifesto, not a claims ledger.  Release claims
must be backed by generated evidence:

- POSIX surface: `docs/posix-compliance.md`
- Realtime budgets: `docs/rt-budgets.md`
- LKL boundary: `docs/lkl-domain.md`
- SMP and PI contracts: `docs/smp-contract.md`, `docs/pi-pc-design.md`

## Executive Thesis

The OS design problem RTMux addresses is the conflict between deterministic
execution and ecosystem reuse.

Traditional hard-RT kernels are analyzable, but they lack the vast driver,
filesystem, network, and middleware ecosystem that Linux has accumulated.
Linux has that ecosystem, but the Linux kernel is too large, too policy-rich,
and too license-entangled to be the smallest possible hard-realtime trusted
computing base.

RTMux competes with the forced choice between Linux compatibility and a small,
analyzable RTOS control plane.  The niche is narrower and more valuable: hard
realtime, a small TCB, capability isolation, and product-friendly licensing,
while still reusing mature Linux behavior where rewriting it would be wasteful.
The HongMeng driver-reuse work frames the scale of that waste clearly:
rebuilding modern driver coverage for vehicle and consumer platforms can mean
thousands of person-years.

RTMux chooses a dual-domain design:

```
RTMux native domain
    Apache-2.0
    POSIX libc and RTOS services
    scheduler, IPC, IRQ entry, timers, capabilities
    native hard-RT applications and drivers

        capability-checked IPC
        explicit namespace mounts
        delegated fd / MMIO / IRQ / DMA handles

LKL service domain
    GPL-scoped Linux subsystem reuse
    ext4, TCP/IP, and other selected Linux behavior
    ordinary user-mode processes: lkl-fsd, lkl-netd
```

The native domain is the realtime control plane.  The LKL domain is the Linux
service plane.  They communicate by RTMux IPC, not by sharing kernel privilege.

## Product Identity

RTMux defines itself by six commitments:

1. RTMux is a POSIX RTOS.
2. RTMux is configurable from a PSE51-style minimum profile to full POSIX.1.
3. RTMux is capability-based; namespace presents resources, capabilities decide
   authority.
4. RTMux follows the modern microkernel split: kernel mechanisms, user-mode
   policy.
5. RTMux keeps the kernel and resident service set small, measured, and
   reviewable.
6. RTMux uses Linux through contained LKL services instead of competing with
   Linux.

The first commitment is a product goal and release contract.  A release earns a
POSIX claim only when the generated matrix, headers, runtime tests, and profile
checks prove it.  A claimed profile cannot contain `partial`, `stub`, or
implemented-without-test rows.  This keeps the manifesto ambitious without
turning it into an unverifiable slogan.

## POSIX First

RTMux's external programming contract is POSIX.  QNX is an architectural
inspiration, not a compatibility target.  Linux is an ecosystem source, not the
native ABI.

The full profile targets POSIX.1:

- public headers expose only implemented or deliberately fail-closed
  interfaces;
- feature macros match the configured profile;
- `sysconf`, `pathconf`, and `confstr` agree with those macros;
- each advertised interface has errno behavior and a runtime regression;
- `docs/posix-compliance.md` is generated truth.

This matters because POSIX makes RTMux usable without forcing applications into
a private vendor API.  An RTOS should be judged by portable contracts and
measured behavior, not by a collection of bespoke calls.

## Configurable From PSE51 To POSIX.1

RTMux treats POSIX as a selectable profile, not a monolithic feature blob.

| Profile | Purpose | Typical surface |
| --- | --- | --- |
| `minimal` | Small realtime image | PSE51 mandatory realtime APIs |
| `realtime` | Rich native RTOS | PSE51 plus RTMux-native filesystem/process pieces |
| `lkl` | Host LKL services | Minimal POSIX plus the libc surface required by LKL daemons |
| `full` | Complete target | Every implemented POSIX.1 option RTMux advertises |

Profiles are contracts:

- a small profile must not advertise what it does not support;
- a larger profile must prove the extra surface with tests;
- mandatory POSIX.1 rows must be classified as implemented, deferred with
  reason, unsupported with documented errno, or excluded by profile scope;
- the LKL profile must be derived from actual LKL server imports, not guessed;
- the full profile is the POSIX.1 release target.

This lets the same codebase serve tiny realtime images, richer embedded
controllers, LKL-backed gateways, and full POSIX environments.

## Modern Microkernel Split

The RTMux kernel is intentionally small.  It owns privileged mechanisms:

- CPU scheduling;
- synchronous IPC rendezvous;
- trap and interrupt entry;
- minimal virtual memory;
- capability checks;
- timer machinery;
- kernel-resident object lifetime rules.

Everything policy-rich runs outside the kernel:

- filesystems;
- network services;
- path management;
- process loading policy;
- hardware service policy;
- recovery and supervision;
- POSIX and Linux compatibility policy;
- Linux subsystem reuse through LKL.

This mechanism/policy split is the reason RTMux can pursue formal reasoning,
fault containment, and licensing containment.  A filesystem bug should be a
server bug.  A network stack crash should be a service restart.  A bad path
string should not become authority.

The split is also an efficiency rule.  A new kernel mechanism, resident server,
or cross-domain data path must name its TCB delta, fast-path cost, authority
source, and rollback criterion before it lands.  Speculative framework work does
not belong in the kernel.

## Capability-Based Authority

RTMux rejects ambient authority.

A path names a resource.  A capability authorizes use of it.

The capability model gives RTMux these properties:

- handles carry explicit rights;
- rights can be attenuated before delegation;
- delegation is visible;
- revoke is part of the design;
- stale handles fail deterministically;
- sender identity comes from kernel IPC metadata, not payload fields;
- MMIO, IRQ, DMA, fd, and namespace binds are explicit grants.

The core authority model is no longer aspirational: the global channel
namespace, roottask identity shortcuts, kernel-private IRQ/trace/IPC/clock gates,
per-connection rights, and typed process/thread-capability milestones are
capability-derived.  Future capability consumers must bring a concrete need and
fresh revoke-latency evidence.

POSIX credentials remain POSIX credentials.  Capability work does not replace
uid/euid permission checks such as `cred_may_access`; it removes non-POSIX
ambient authority from kernel-private operations.

This model is what makes LKL safe enough to be useful.  `lkl-fsd` and `lkl-netd`
are not privileged Linux kernels.  They are user-mode services that receive
specific delegated resources.

## Formal Verification Suitability

RTMux is not formally verified today.  It is designed to make formal
verification tractable.

The key enabling choices are:

- small privileged state space;
- explicit kernel objects;
- capability table semantics;
- synchronous IPC state transitions;
- bounded priority inheritance rules;
- generated POSIX/profile matrices;
- measurable realtime budgets;
- user-mode policy separated from kernel invariants.

The verification roadmap is incremental:

- static lock-rank and acquisition-order checks;
- capability delegation/revocation algebra models;
- RV64 litmus tests for IPC, RCU-sched, and shared-ring memory ordering;
- CBMC-style harnesses for resource lifetime and message queue code;
- trace-derived validation against realtime budgets.

The goal is not to prove every line of every user-mode server.  The goal is to
prove the kernel mechanisms that make user-mode servers containable.

## Dual-Domain Licensing Model

RTMux's main tree is Apache-2.0.  That matters for embedded and industrial
systems because native control logic, board support, and product-specific
drivers often need proprietary distribution.

LKL/Linux-derived code is GPL-scoped.  RTMux does not hide that.  It confines
that code to ordinary service processes.

The engineering boundary is:

- native RTMux kernel, libc, roottask, native services, native drivers, and
  native RT apps remain in the Apache-2.0/proprietary-friendly domain;
- LKL-derived service processes are the GPL-scoped Linux reuse domain;
- communication crosses process boundaries through RTMux IPC and documented
  service protocols;
- modified LKL/Linux-derived code should be handled with GPL-compliant source
  distribution when shipped;
- external GPL test suites remain out-of-tree.

This is a technical architecture, not legal advice.  It is still a core reason
RTMux exists: realtime control code should not have to become Linux kernel code
just to read ext4 or speak TCP/IP.

## LKL Service Domains

LKL turns Linux into a library-style architecture port with a host-operations
boundary.  RTMux uses that boundary to run selected Linux subsystems in
contained processes:

```
native RT app / RTMux libc
        |
        | open/read/write/msg_send via RTMux IPC
        v
roottask namespace + capability routing
        |
        | delegated endpoint
        v
lkl-fsd or lkl-netd process
        |
        | LKL syscall + host_ops
        v
Linux ext4 or TCP/IP code inside user space
```

The LKL services are implementation details behind RTMux names and
capabilities.  A native program should open a path such as `/fs/root/...`
through RTMux libc and roottask namespace routing.  Roottask resolves that
mount point to a delegated channel for `lkl-fsd`; clients do not gain ambient
Linux authority and do not talk to a privileged Linux kernel.

RTMux gains:

- Linux ext4 behavior;
- Linux TCP/IP behavior;
- a stable syscall/host-ops boundary;
- a path to additional Linux subsystem reuse;
- Linux test oracles and ecosystem knowledge.

LKL gains:

- process isolation;
- capability-scoped authority;
- explicit MMIO/IRQ/DMA/fd delegation;
- native IRQ ownership;
- revoke and crash containment;
- scheduling and latency budgets around the Linux domain.

LKL's cost is honest too.  A whole subsystem runs in one address space, so
isolation is per-domain, not per-driver: a bug in ext4 cannot reach the native
kernel, but it can corrupt the rest of `lkl-fsd`.  LKL's internal threads are
opaque to the native scheduler, which is exactly why the LKL domain needs an
external priority ceiling and CPU budget instead of in-stack RT tuning.  And
today's boundary is synchronous IPC, so bulk data still copies unless a measured
shared-ring data plane justifies its extra authority surface.  These are the
tradeoffs RTMux accepts for a stabler upstream boundary than DDE-style per-driver
imports; they are not solved by wishing them away.

The LKL domain is useful precisely because it is not the kernel.

## Comparison With Existing Approaches

| Approach | Strength | Cost RTMux avoids | RTMux tradeoff |
| --- | --- | --- | --- |
| PREEMPT_RT Linux | Full Linux ecosystem in one kernel | Linux remains the realtime and licensing path | RTMux gives up broad Linux ABI compatibility in the native path |
| Xenomai 4 / EVL | Strong dual-kernel realtime model through Dovetail | Linux remains privileged and coupled to the RT core | RTMux must prove its own RT mechanisms |
| DDE-style Linux driver reuse | Fine-grained Linux driver import | High maintenance against changing Linux internal APIs | RTMux imports larger LKL domains but uses a stabler boundary |
| HongMeng-style Linux Driver Containers | Broad Linux driver reuse with control/data split | Complex co-management of Linux objects and native isolation | RTMux starts narrower with LKL services and capabilities |
| seL4 sDDF | Isolated user-level drivers with shared-memory transport | Requires rigorous per-device component design | RTMux borrows the discipline only where a measured hot path needs it |
| Rump kernels | Smaller, cleaner NetBSD subsystem reuse | Smaller Linux behavior and hardware ecosystem | RTMux accepts LKL footprint for Linux behavior |
| SGX-LKL / Gramine | Linux-like application containment | Compatibility/security goals, not RT budgets | RTMux narrows LKL to services behind RTMux IPC |
| RTMux + LKL | Native RTOS plus isolated Linux subsystem reuse | LKL footprint and latency must be controlled | Unique fit for embedded RT systems needing selected Linux behavior |

## Control Plane And Data Plane

RTMux should keep synchronous IPC for control:

- mount;
- open;
- ioctl/devctl;
- route setup;
- capability delegation;
- revoke;
- configuration and teardown.

This is also where older V-Bus-style designs stop being enough.  A shared pipe
plus a doorbell works for control and low-rate notifications, but it becomes
the bottleneck if every NVMe block or network packet crosses as a synchronous
message payload.

Bulk data should not be forced through synchronous IPC forever, but a separate
data plane is earned, not assumed.  High-volume storage and networking may need:

- shared memory explicitly delegated by capability;
- virtio-style descriptor rings;
- bounded queue depth;
- generation checks for revoke;
- RV64 acquire/release ordering proven by litmus tests;
- IOMMU/IOPMP/PMP where hardware exposes it, or a documented trusted
  bounce-buffer fallback;
- synchronous IPC used only to set up and tear down the ring.

This is the lesson RTMux should borrow from HongMeng-style control/data-plane
driver reuse and seL4 sDDF-style shared-memory transports.  The native kernel
keeps authority.  The data path avoids unnecessary copies and context switches
only after baseline IPC cost, revoke behavior, and DMA containment are measured.

## Realtime Discipline

Hard-RT is not raw throughput.  Hard-RT is bounded latency.

RTMux's hard-RT path is native:

- IRQ first-stage handling;
- timer expiry;
- scheduler dispatch;
- synchronous IPC;
- priority inheritance;
- native drivers and ISTs;
- capability lookup and revoke.

LKL is outside the hard-RT path unless a measured item admits it.  Linux
filesystem and TCP/IP code are valuable, but they are not assumed to be
deterministic.

The roadmap is concrete:

- bound non-preemptible kernel windows;
- keep tickless and timer behavior covered by regression traces;
- keep hot process, thread, channel, and connection lookups under RCU-sched
  lifetime rules;
- keep IRQ/timer notifications allocation-free after attach;
- isolate LKL workers and ISTs by priority and runmask;
- add zero-copy rings only after IPC baselines and DMA-containment evidence exist;
- record physical-board latency blocks before claiming hard-RT compliance.

## Priority Inheritance Across Domains

QNX demonstrates why message-driven priority inheritance matters: a server that
receives a request from a high-priority client must not run as a low-priority
background thread.  QNX also documents the harder case: server boost when all
server threads are busy and a high-priority client becomes SEND-blocked.

RTMux should keep that lesson, but not blindly apply it to LKL.

Native RTMux IPC can support bounded priority donation and server-chain
verification.  LKL is different.  LKL contains Linux code whose internal locks,
allocators, and code paths are not hard-RT bounded.  Letting a hard-RT client
boost an entire LKL network stack to top realtime priority would be unsafe.

The correct mixed-criticality rule is asymmetric:

- native servers may receive normal IPC priority donation;
- LKL service threads must have a priority ceiling;
- LKL domains need temporal budgets and backpressure;
- LKL IRQ/IST affinity should stay on LKL/background harts unless measured
  otherwise;
- native hard-RT control loops must remain schedulable under LKL load.

In scheduler terms, LKL should be treated as a budgeted service plane:

- inherited priority is capped by an LKL priority ceiling;
- CPU time is bounded by a sporadic-server style budget;
- exhausted LKL budget applies backpressure instead of stealing hard-RT time;
- native ISTs can wake LKL work, but the work still runs under the LKL budget.

This preserves the useful part of QNX-style message-driven priority inheritance
without letting non-deterministic Linux internals become top-priority RT code.

## Hard-RT Mechanisms

RTMux should strengthen hard-RT claims through a few boring kernel properties:

- non-preemptible windows are measured and bounded;
- one-shot/tickless timer behavior is regression-tested;
- hot process, thread, channel, and connection lookups use RCU-sched only within
  checked non-blocking read sections;
- IRQ and timer notifications allocate nothing after attach;
- shared rings use acquire/release ordering and litmus tests;
- first-stage IRQ handlers acknowledge and defer work to ISTs.

These are small mechanisms, not a second policy framework.  They keep the
native path analyzable while LKL remains a restartable, lower-criticality
service domain.

## Strategic Roadmap

The manifesto implies these engineering priorities.

### 1. Make Evidence Generated

Turn realtime and POSIX claims into generated artifacts:

- POSIX matrix generated from ledger rows, implementation, and tests;
- profile checks for `minimal`, `realtime`, `lkl`, and `full` that reject
  claimed `partial`, `stub`, or implemented-without-test rows;
- mandatory POSIX.1 row classification;
- trace-derived `docs/rt-budgets.md` blocks;
- CI gates for stale handles, revoke, owner death, and service crash;
- hardware latency runs before hard-RT claims.

### 2. Prove The Native Kernel Mechanisms

Focus verification on the small kernel:

- capability algebra;
- IPC queue and reply ownership;
- priority inheritance and ceiling behavior;
- RCU-sched lifetime;
- lock rank;
- RV64 memory ordering;
- timer and interrupt critical windows.

Every microkernel architecture change should also carry a footprint report:
kernel text/data/bss, syscall/kcall surface, resident servers, initramfs size,
and any positive privileged or resident growth tied to a budget row or rollback
note.

### 3. Contain LKL Hardware Authority

LKL domains must never receive ambient hardware control:

- MMIO/IRQ/DMA must be delegated explicitly;
- DMA containment starts with a platform inventory: QEMU virt and FU740 must say
  whether usable IOMMU/IOPMP/PMP enforcement exists;
- bounce buffers are fallback for trusted devices, not malicious-device
  isolation;
- revoke must mask IRQ, stop DMA, reset device state, and invalidate stale caps.

### 4. Split LKL Control And Data Paths

Keep IPC for control.  Use shared rings for bulk data only after:

- the synchronous IPC fast path has a recorded baseline;
- the buffer authority model is explicit;
- generation/revoke is designed;
- memory ordering is tested;
- fallback copy paths remain available;
- before/after latency and CPU cost justify the extra surface.

### 5. Shape LKL As A Restartable Service Plane

LKL should behave like a service, not a second kernel:

- crash detected by supervision;
- delegated caps revoked;
- stale client handles fail deterministically;
- new service generation published;
- restart latency recorded;
- native RT work continues during failure and recovery.

### 6. Target Mixed-Criticality Embedded Systems

RTMux is especially relevant where a product needs both:

- deterministic native control loops;
- rich Linux ecosystem behavior.

Examples include:

- robotics and autonomous systems using ROS/Autoware-class middleware;
- industrial gateways that need reliable networking/storage plus native control;
- medical and laboratory devices with hard control loops and modern connectivity;
- edge systems that need POSIX portability but cannot put every component in the
  Linux kernel timing/licensing path.

RTMux should target these niches, not general-purpose Linux replacement.

## Application Domains

RTMux's best market is mixed-criticality embedded software, where one product
needs Linux ecosystem behavior and a hard-RT control plane on the same silicon.

Automotive and robotics are the clearest fit.  Autoware/ROS-class middleware
needs POSIX, networking, storage, and Linux familiarity.  Drive-by-wire,
braking, steering, motor control, and safety monitors need bounded native
latency.  RTMux should let those live in separate authority and timing domains
instead of forcing a two-ECU split or placing everything under Linux timing.

Industrial gateways are another fit.  LKL can provide ext4 and mature TCP/IP
behavior, while native RTMux code keeps PLC-like control loops and fieldbus
timing outside the Linux failure domain.  If `lkl-netd` crashes or is attacked,
roottask can revoke its caps, reset the device path, and restart service
without stopping native control.  This is also the shape that security
frameworks such as IEC 62443-4-2 reward: the network-facing attack surface sits
in a revocable service domain, not in the control-loop TCB.

Medical and laboratory devices fit the same pattern.  Certification effort
under standards like IEC 62304 can focus on the small native kernel mechanisms
and product control logic, while complex Linux-derived storage or connectivity
runs as contained user-mode services with explicit capabilities and measurable
interference budgets.  The value is scope reduction: you certify the small
native TCB and the application, not a monolithic Linux kernel.

## What RTMux Must Prove

The manifesto becomes real only if these claims are backed by tests and
measurements:

- POSIX profile checks prove each advertised profile.
- `docs/posix-compliance.md` remains generated truth.
- `docs/rt-budgets.md` contains hardware latency blocks before hard-RT claims.
- Capability delegation and revoke pass stale-handle, crash, owner-death, and
  timeout tests.
- LKL crash/restart reclaims delegated resources.
- DMA containment is enforced or its trusted fallback is documented.
- LKL latency and native non-interference stay within budget.
- GPL-scoped code remains in its service domain and out-of-tree test suites
  remain isolated.

RTMux should be ambitious in design and conservative in claims.

## Source Notes

External references used for positioning:

- QNX priority inheritance and server boost:
  https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/ipc_Priority_inheritance_messages.html
- Xenomai 4/EVL core overview: https://v4.xenomai.org/core/index.html
- TU Dresden DDEKit: https://os.inf.tu-dresden.de/ddekit/
- HongMeng kernel OSDI 2024 paper:
  https://www.usenix.org/system/files/osdi24-chen-haibo.pdf
- seL4 Device Driver Framework:
  https://trustworthy.systems/projects/drivers/ and
  https://github.com/sel4-cap/sDDF
- NetBSD rump kernel manual/tutorial:
  https://man.netbsd.org/rumpkernel.7 and
  https://www.netbsd.org/docs/rump/sptut.html
- LKL project material: https://lkl.github.io/ and
  https://github.com/lsds/lkl
- SGX-LKL: https://lsds.doc.ic.ac.uk/projects/sgx-lkl and
  https://github.com/lsds/sgx-lkl
- Gramine documentation: https://gramine.readthedocs.io/ and
  https://gramineproject.io/
- Linux kernel license rules:
  https://docs.kernel.org/process/license-rules.html
- PREEMPT_RT documentation:
  https://www.kernel.org/doc/html/next/core-api/real-time/index.html
- Autoware overview: https://autoware.org/autoware-overview/
