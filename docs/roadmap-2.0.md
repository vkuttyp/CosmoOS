# CosmoOS 2.0 roadmap (proposed)

Written 2026-10-29 at v1.0.14, on main at `f073ad71`; the MS track
(messaging at scale) added the same day on `4b1c55ec`. **Proposed, not
adopted.**

Its goal: make CosmoOS competitive with Linux and FreeBSD for the
workloads it chooses, with its kernel written in CeeSafe so that it is
memory safe.

**Written without today's restrictions.** At the owner's direction, this
roadmap is not bound by the rules CosmoOS works under now: the scope
rules in [`AGENTS.md`](../AGENTS.md#scope-rules) (one milestone at a
time, at most five PRs each, 1.2's direction A first), the kernel rules
for CeeSafe in [build/ceesafe.md](build/ceesafe.md#rules-for-ceesafe-in-the-kernel)
(locks, allocation and object lifetimes in C; CeeSafe only at leaf
functions), and 1.x's promise of stable formats and ABIs. Where it
replaces one of them it says what replaces it. If it is adopted, those
documents are rewritten to match in its first PR; until then they stand
for the work in progress. Each milestone is still done when its
acceptance test is green on main, its box ticked in the same PR.

Backlog references (`NET-8`, `SEC-10`, ...) are lines of
[`backlog.md`](backlog.md); a milestone that takes a line strikes it.
Sizes are relative to 1.1 (1.0x = about ten days of the same kind of
work). Facts about today's tree come from the
[2026-10-28 audit](audit/2026-10-28-state-of-the-project.md) unless stated.

## What "competitive" means here

CosmoOS cannot match Linux's hardware breadth or FreeBSD's thirty years of
tuning. It can be the better choice for a defined job, measured against
both in the same machine. This roadmap picks one:

> **A memory-safe server operating system** for virtual machines and a
> small set of real x86-64 and AArch64 servers: it runs the server
> software people deploy, at a performance close to Linux and FreeBSD on
> the same machine, with a security model at least as strong, and with
> its kernel written in a memory-safe language, which neither of them
> has.

Out of scope for 2.0, as for 1.x: desktops, laptops, Wi-Fi, Bluetooth,
sound and 3D graphics (DRV-16), and broad hardware support.

### The bars

Each bar is a number a CI job or a dated report can produce. "Today" is
the 2026-10-28 tree.

| Area | Today | Bar at the end of this roadmap | Measured by |
|---|---|---|---|
| Memory safety | 982 lines of CeeSafe in four ports (`gpt`, `cputime`, `nice`, `fair`) of about 227,000 kernel-side lines: under 0.5% | All ring-0 code in CeeSafe except a named assembly and C floor (below: today's assembly is about 1% of ring-0 lines, the floor a few percent); every parser of untrusted input in CeeSafe by the end of phase 1 | `scripts/cee-share.py` (SA1), in CI |
| Hardware | QEMU `q35`/`virt` and Parallels; no physical machine | Two physical servers per architecture in hardware-in-the-loop CI; common server NICs and storage | `hil` CI job (HW3) |
| Performance | Not measured against Linux or FreeBSD | Within 15% of the better of Linux and FreeBSD on the benchmark set, same VM and hardware; no worse than 1.5x on any one | `make bench-compare` (PF1), dated reports |
| Scale | 64 CPUs, no NUMA, no huge pages, no swap | 256 CPUs, two NUMA nodes, huge pages, swap | PF3's tests on QEMU and HW3's machines |
| Linux programs | musl static/PIE/dynamic; glibc untested; about 20 common calls missing | glibc programs from a mainstream distribution's packages run; the Linux Test Project's chosen subset passes | LX1's test lists |
| Connections | 64 handles per process (`HANDLE_TABLE_SIZE`); TCP connections in 256 buckets keyed by local port; per-CPU timer queues with a sorted-list insert | 2,000,000 persistent TCP connections in one guest, held by an Erlang messaging server | MS6's load test, in `make bench-compare` |
| Server software | BusyBox, dropbear, tcc; A4-A6 (curl, OpenSSH, git, Python) in progress | Also nginx, PostgreSQL, Redis, a Go and a Rust toolchain, an OCI container runtime | Each port's own test suite |
| Security | Verified boot, signed modules, user ASLR, privileges | Also KASLR, Secure Boot, measured boot, verified root, capability-based sandboxing, syscall filtering, multiple users | SE1-SE2's acceptance tests |
| Observability | lockdep, self-tests, serial logs | Tracing with programmable probes, performance counters, a kernel debugger workflow | OB1-OB2 |

## The CeeSafe rule

The target is a kernel written in CeeSafe, not a C kernel with CeeSafe parts.
The rule from adoption on:

1. **All new kernel code is written in CeeSafe, and no new C is written.**
   New subsystems, drivers, parsers, fixes that rewrite a function: all
   CeeSafe. There is no exemption process for new C.
2. **All existing C is converted.** Every C file in ring 0 is rewritten in
   CeeSafe, by the SA track below, whether or not a feature needs it. The
   order is by risk, not by size: first what parses input an attacker or a
   damaged disk controls (the code behind the 15 fuzz targets: ELF, module
   ELF, cosmofs metadata, network frames and TCP segments, DHCP and DNS,
   Linux syscall arguments, USB descriptors, virtqueues, `pkg` archives,
   LZ4, framebuffer descriptions); then the code with the most defects in
   its history (the backlog's `correctness` lines by subsystem); then
   whole subsystems, the network stack, cosmofs, VFS, the Linux
   personality, memory and the scheduler.
3. **The floor that stays assembly and C is named and small.** CeeSafe can
   express raw pointers, MMIO and C function pointers, but not
   instructions it has no syntax for. What stays outside CeeSafe: boot entry
   and the loader's hand-off, trap, interrupt and system-call entry and
   exit stubs, context switch, the user-copy routines with their fault
   fixups (`uaccess.S`), the atomic and barrier primitives and the
   lowest-level spin lock, TLB and cache maintenance, the hypervisor's
   world switch (`vmx_run.S`, the SVM and EL2 equivalents), and CeeSafe's
   runtime shim (`ceesafe_rt_core.c`, `ceesafe_panic.c`). Each is a function a
   CeeSafe caller reaches through `extern "C"`, with its contract written as a
   `/// Safety:` comment on the CeeSafe declaration. Everything else, locks
   built on the primitive (mutexes, rwlocks, wait queues), allocation,
   kobjects, quiescence, per-CPU data, the scheduler, is CeeSafe. The floor
   also keeps the one-instruction C wrappers CeeSafe has no syntax for (MSR,
   port I/O and system-register access, `cpuid`, `rdtsc`, `wfi`). Today's
   assembly is 1,886 of 195,417 ring-0 lines (about 1%); with those
   wrappers and the shim the floor is a few percent.
4. **The kernel rules for CeeSafe are replaced.** "Locks stay in C",
   "allocates nothing" and "no current port takes a lock" go. CeeSafe code
   takes locks through CeeSafe guards (`SpinLock<T>`, `Mutex<T>`: the data is
   reachable only through the guard, and holding a spin lock is an atomic
   context the compiler checks), allocates through `KernelHeap` and
   `ObjectCache<T>` with failure as a `Result`, and holds kernel objects as
   `KRef<T>`, whose drop is the release. Nova's repository built most of
   these as demonstrations (its C5: `KernelHeap`; C7: `KRef<T>`,
   `SpinLock<T>`, `IrqSpinLock<T>`, `Mutex<T>`, `Quiesce`), and a CeeSafe
   module loaded and unloaded 1,000 times against CosmoOS (C8);
   `ObjectCache<T>` is still to write, and none of them is yet a library
   the kernel depends on ([CeeSafe prerequisites](#ceesafe-prerequisites), N3). The rules that stay: `--no-float`; `unsafe` only
   at the floor's boundary, each with its `/// Safety:` comment; context
   effects (`@context(irq)`, `@context(atomic)`) on every function that
   runs there; no failing check reachable in interrupt context.
5. **A port proves itself, then the C is deleted.** Each conversion has a
   host test, a fuzz target where it takes input, the in-kernel tests, and
   a differential test against the C it replaces, run until the C is
   deleted in the same PR or the next. Its benchmark is within 5% of the C
   on both architectures with clang (Nova's C6 measured 0.98-1.06x for
   `fbvalid`), or the owner accepts the slowdown with the number recorded.
   The C and the CeeSafe versions never both ship.
6. **No weakened CeeSafe checks to save time.** A check that costs too much is
   removed by a proof the compiler accepts (CeeSafe's `optimisations.md`), not
   by `unsafe` or a wrapping operation that hides a real overflow.
7. **Generated C is a build artefact.** Today the generated C is committed
   beside each `.cee` file so that building needs no CeeSafe compiler. With
   the whole kernel in CeeSafe that doubles the tree for no reader. From SA1
   on, the build runs the pinned ceesafec and the generated C is not
   committed; CI's "CeeSafe sources" check becomes a reproducibility check of
   the build (`make reproducible`). ceesafec does not run on CosmoOS, so a
   self-hosted build (SW0) needs the generated C from somewhere: source
   releases ship it ([CeeSafe prerequisites](#ceesafe-prerequisites), N4).

Userland follows the same rule where it is CosmoOS's own code: `libc/`,
`userland/` (the native tools, `svc`, `syslogd`, `cosmo-*`) and `pkg/`
are converted in SA12. Third-party ports (BusyBox, musl, dropbear, the
servers) stay as their upstreams write them.

## Nova prerequisites

> **Since 2026-10-31 the language is CeeSafe**, the fork of Nova 0.6.0
> (github.com/CeeSafe/CeeSafe): Nova is no longer developed, and the tree
> moved to the pinned ceesafec in one change (`docs/build/ceesafe.md`). This
> section is the record of the 2026-10-29 review of Nova's compiler; N1-N3
> were met in Nova 0.2.0-0.6.0, and the open items (N4-N6) are CeeSafe's.

Added 2026-10-29 from a review of this roadmap against Nova's compiler
(Nova's [milestone 14](https://github.com/vkuttyp/Nova/blob/main/docs/milestone-14.md),
proposed; its open decisions OD-28 to OD-31). The Nova rule assumes Nova
can write whole subsystems. Today it writes leaf functions whose state
lives in C, which is all the four ports needed. These are what it lacks,
and the CosmoOS milestone each one gates:

| | Nova lacks | Gates | Nova's plan |
|---|---|---|---|
| N1 | **Global and per-CPU state.** Nova has only compile-time `const`; `static var count: u64 = 0;` is a syntax error. A kernel's tables, singletons and per-CPU data have nowhere to live in Nova. | SA2, SA3 | M14 K1: `static` items whose mutable state is reached only through `Share` types (lock guards, atomics), statically initialised C state (`SPINLOCK_INIT`), `@percpu` statics accessed where the CPU cannot change (OD-28). **In novac 0.2.0** (Nova ADR 0024): `static` and `static mut`, built at compile time; per-CPU data is a library type (`PerCpu<T>`, N3) |
| N2 | **Nova-typed calls between files.** Each `.nova` file compiles alone and they meet only through `@export` C functions, so references, regions, effects and `Send`/`Share` stop at every file boundary. Building the kernel as one whole program instead was measured on a synthetic 202,000-line package: 13.1 s in novac and 33.8 s in clang for one 1,076,000-line C file, on every change, on one core. | SA1 (the build), SA6 onward | M14 K2: first C in several translation units (K2a), then separate compilation per package with interface files (OD-29) |
| N3 | **A kernel library.** `KernelHeap`, `KRef<T>`, the lock guards and `Quiesce` exist as Nova's C5 and C7 demonstrations with their own C glue; `ObjectCache<T>` is not built. | SA2, SA3 | M14 K3: the owner chose Nova's repository instead (Nova ADR 0025): `lib/kernel`, in novac 0.2.0, selected with `novac --kernel cosmoos`. Its boot self-test `nova-kcore` runs here (`kernel/nova/kcore_test.nova`, 2026-10-29). `ObjectCache<T>` and a static `Mutex` are still open |
| N4 | **novac on a self-hosted CosmoOS.** novac is a .NET NativeAOT binary for macOS, Linux and Windows; it does not run on CosmoOS, and the Nova rule's point 7 makes every kernel build run it. | SA1, SW0 | M14 K4: source releases carry the generated C, so a self-hosted build needs no novac; changing a `.nova` file needs a host with novac (OD-31) |
| N5 | **An outside audit.** Once whole subsystems are Nova, the compiler is part of the kernel's trusted base. Nova's 2026-10-29 review found two soundness holes, one of them in a release (fixed in novac 0.1.2), and that review was by the same model that wrote the compiler. | phase 2 (SA6 onward) | M14 K6: the owner engages a reviewer; Nova keeps its `safety-audit.md` current |
| N6 | **Hot-path speed.** Nova's measured gaps to C are 0.98-1.06x (`fbvalid`), 1.13-1.20x (sort) and 1.26-1.40x (string building); its atomics are calls into the runtime, about 2x slower than inline C on a two-thread ring (Nova's ADR 0021). | PF2, SA6, SA10 | M14 K5: inline atomics (Nova's OD-27) first, then the check elimination PF1's numbers call for. **Inline atomics are on Nova's main** (ADR 0026; the ring within 1.24-1.34x of inline C), for the next novac |

So the order gains these edges: SA1 after N4's decision and K2a; SA2 and
SA3 after N1 and N3; phase 2 after N5. SA5 (parsers) and PF1 need
nothing new and can start at once. N1 and K2a change Nova's language and
output, so they come in **Nova 0.2.0**, and this tree moves its pin to it
in one PR, as every pin bump does (`docs/build/nova.md`, "Upgrading the
compiler").

## How the work runs

This replaces `AGENTS.md`'s one-milestone-at-a-time rule for 2.0's work.

- **Streams in parallel.** Up to three streams run at once, each its own
  branch and PR series: the conversion stream (the SA track), a platform
  stream (HW, PF, VM) and a features stream (NT, LX, SE, CT, OB, ST, SW).
  Two streams never change the same subsystem in the same week: a
  subsystem's conversion (SA6-SA11) goes first, and its features follow
  on the CeeSafe code, so nothing is written twice.
- **No PR cap.** A milestone takes the PRs it needs; its report says how
  many and why.
- **Convert, then extend.** Where a feature milestone and a conversion
  touch the same subsystem, the conversion lands first: NT1 after SA6,
  ST1 and ST2 after SA7, LX1 after SA8, HW2's new drivers with SA9, PF3
  after SA10.
- **One format and ABI break, at 2.0.** 1.x kept every released on-disk
  format and the native ABI compatible. 2.0 may change them once, where a
  conversion or a feature is simpler for it, with `cosmo-upgrade`
  migrating a 1.x system or, where it cannot, a documented reinstall. The
  Linux ABI does not break: it is Linux's.
- **The validation matrix stays.** It is what lets a whole-subsystem
  conversion be trusted. What changes is cost: a conversion PR runs the
  matrix once, and the streams share the KVM host PF1 adds so that the
  matrix's boots stop being the bottleneck.

## Order

```text
ceesafe:        N4 decided, K2a ─► SA1;  N1, N3 ─► SA2, SA3;  N5 ─► phase 2  (CeeSafe prerequisites)
conversion:  SA1 ─► SA2 ─► SA3 ─► SA4 ─► SA6 (network) ─► SA7 (storage, VFS) ─► SA8 (Linux) ─► SA9 (drivers)
                     └─► SA5 (parsers, first)            ─► SA10 (core) ─► SA11 (arch, hypervisor) ─► SA12 (userland) ─► SA13 (the last C)
platform:    PF1 ─► HW1 ─► HW2 (with SA9) ─► HW3 ─► PF2 ─► PF3 (after SA10) ─► VM1 ─► VM2 ─► UK1
features:    SW0 (self-hosting) ─► SE1 ─► NT1 (after SA6) ─► LX1 (after SA8) ─► LX2 ─► NT2 ─► SW1
             ─► SE2 ─► CT1 ─► CT2 ─► ST1, ST2 (after SA7) ─► OB1 ─► OB2 ─► SW2
messaging:   MS1 (after SA2, SA6) ─► MS2 ─► MS3 ─► MS4 ─► MS5 (after PF3) ─► MS6
```

The MS track runs in the features stream's slot when that stream is
between subsystems, or as a fourth stream if the owner adds one; it
changes the network, the handle table and the timers, so it follows the
same convert-then-extend rule.

| Phase | Version | Theme | Milestones | Size |
|---|---|---|---|---|
| 1 | 2.0-alpha | The CeeSafe foundation | SA1-SA5, SW0, PF1, HW1 | about 13x |
| 2 | 2.0 | The I/O half in CeeSafe, real machines | SA6-SA9, HW2, HW3, SE1, NT1 | about 19x |
| 3 | 2.1 | The core in CeeSafe, server parity, connections at scale | SA10, SA11, NT2, LX1, LX2, PF2, PF3, SW1, MS1-MS4 | about 24x |
| 4 | 2.2 | No C left; containers, observability, storage; the messaging server | SA12, SA13, SE2, CT1, CT2, OB1, OB2, ST1, ST2, SW2, MS5, MS6 | about 22x |
| 5 | 2.3 | Virtualization, and the hybrid kernel's other half | VM1, VM2, UK1 | about 6x |

About 83.5x of work in all. Run serially at 1.x's pace (1.0x is about ten
days) that is about two and a quarter years; in three streams, about ten
to thirteen months.

---

## SA. Safety: the kernel in CeeSafe

### SA1 Measure, gate, and build from CeeSafe

`scripts/cee-share.py` counts ring-0 lines per tree and per subsystem:
CeeSafe, the floor (assembly and the named C wrappers), and C still to
convert. CI's `cee-share` job prints the table on every PR and fails one
that adds a line of C outside the floor. The build runs the pinned ceesafec
itself and the generated C leaves the tree (the CeeSafe rule, point 7);
`make reproducible` checks that two builds agree. A panic in CeeSafe code
names the CeeSafe file and line in the crash report on both architectures.

- [x] **Acceptance:** CI's `cee-share` job runs on every PR and on main
  and fails a test PR that adds a C function under `kernel-services/`; a
  clean checkout builds with only the pinned ceesafec and the cross
  toolchain, and `make reproducible` passes; `crash-cee` (a self-test
  behind `CONFIG_SELFTEST`) fails a CeeSafe bounds check and the harness
  finds the CeeSafe file:line in the serial log on both architectures.

Progress: `scripts/cee-share.py` and CI's `cee-share` job, which runs on
every pull request and push and fails one whose tree has more C to
convert than the commit it started from; a test commit adding a C
function under `kernel-services/` failed it (+5 lines), one removing a
line passed (2026-10-31). On main that day ring 0 was 3,946 lines of CeeSafe,
a floor of 3,494 (assembly and CeeSafe's runtime shim) and 190,560 lines of
C to convert (631 of them the ports' `*_floor.c`): 2.0% CeeSafe. Then
`crash-cee`: a `CRASH_TEST=5` kernel calls `kernel/ceesafe/crash_test.cee`
with an index past its table before the self-tests run, and `make
test-crash-cee` (CI's stress part) requires `KERNEL PANIC: ceesafe panic:
index out of bounds at kernel/ceesafe/crash_test.cee:10:18`, the line the
source marks; it passes on x86_64 and aarch64 (2026-10-31). Its hook in
`kernel_main` is C (self-tests and `CRASH_TEST` are C's), so the same
change ports the framebuffer validator (`kernel/core/fbvalid.cee`, CeeSafe's
C6 port, held there to the C's host test, fuzz target and a 20,000-case
differential test) and `kernel/` has less C to convert, not more. Then the build runs the pinned ceesafec and the generated C leaves the
tree (`build/cee.mk`, into `out/cee`; the CI image carries ceesafec), and
`make dist` writes a source tarball with the generated C beside each
source, which releases publish and which builds with no novac (Nova's ADR
0023). `make reproducible` builds its second tree from such a tarball,
with a `ceesafec` that fails if run, and found every output the same
(2026-10-31). Met 2026-10-31: the `cee-share` job runs on every PR and
push; a clean checkout builds with the pinned ceesafec and the cross
toolchain, and `make reproducible` passes in CI on both architectures;
`crash-cee` names its CeeSafe file:line on both.

Size: about 0.7x.

### SA2 Allocation and object lifetimes in CeeSafe

Nova's C5 `KernelHeap`, a new `ObjectCache<T>` over `kmem_cache`, and C7's
`KRef<T>` (a counted kobject reference whose drop is the release) come into
the tree as the kernel library (N3), behind
the fault injection the C allocators have (`CONFIG_FAULTINJECT`).
Allocation failure is a `Result` a caller handles or returns, never a
panic. The proving conversion has real allocation and lifetimes: the
kernel object and handle layer (`kernel/object/object.c`, `handle.c`;
495 lines, used at 159 `HANDLE_RIGHT_*` sites), with its census tests.

- [x] **Acceptance:** the object and handle layer in CeeSafe passes its
  self-tests and a fault-injection sweep (every allocation failed in turn)
  with no leak in the object census; the differential host test against
  the C agrees on a recorded trace of 10,000 operations before the C is
  deleted; the matrix on both architectures.

Progress: the object census, `kobject_discard` and the recorded trace
(#491); the object layer in CeeSafe (`kernel/object/object.cee`, over
`object_floor.c`), which gives the recorded digest (#493, 2026-10-30);
the handle table in CeeSafe (`kernel/object/handle.cee`, a CeeSafe-owned type C
embeds by value, with novac 0.5.0's ADR 0029), which gives it too
(2026-10-30). The hand-written C of both is gone. The sweep,
`object-fault-sweep` (`kernel/core/faulttest.c`), fails every allocation
of seven object-creating paths in turn (pipe, eventfd, epoll with a
registration, TCP, unix pair, file, process: 52 attempts on x86-64, 51 on
AArch64) and finds the
census unchanged after each (2026-10-30). With the matrix green on both
architectures, SA2's acceptance is met.

Size: about 1.0x.

### SA3 Locks, waits and quiescence in CeeSafe

Mutexes, read-write locks, wait queues, completions and quiescence
(`kernel/core/`'s synchronisation above the spin-lock primitive) are
rewritten in Nova, with Nova's C7 guards as their interface: the data a
lock protects is reachable only through its guard, and the compiler
refuses a sleep while a spin lock's guard is live. lockdep sees every
acquisition with today's classes, callback classes and completion classes
(L21). Pipes and FIFOs (`kernel/ipc/pipe.c`, `fifo.c`) convert with them,
as the first users.

- [x] **Acceptance:** `test-smp2`, `test-chaos` and every lock, wait,
  quiescence, pipe and FIFO self-test pass on both architectures with the
  CeeSafe versions and the C deleted; lockdep's graph self-tests pass; a
  negative compile test refuses a sleep under a spin lock's guard.
  Met 2026-10-31: with every SA3 port in (wait queues, completions,
  semaphores, the mutex, quiescence, pipes and FIFOs: their C is now
  generated from CeeSafe), `test-smp2` and `test-chaos` pass on x86_64 and
  aarch64 (debug), as do their lock, wait, quiescence, pipe, FIFO and
  lockdep self-tests; `tests/ceesafe/negative/sleep-under-spin.cee` is the
  negative test. Two `test-chaos` boots first failed on time bounds
  (`process-user`, `irqpoll-boost`) at a host load average near 30, and
  passed when rerun on a quieter host.

Scope (2026-10-30): CosmoOS has no read-write lock, so none is converted
(none is added either). The types become Nova's, as SA2's did (Nova ADR
0029), with their member names kept, so the static initialisers and the
callers stay. Progress: a host test first, `tests/host/test_sync.c`:
wait queues, completions, semaphores and the mutex on a deterministic
one-CPU scheduler, 300 seeds per workload, with a recorded trace digest
the CeeSafe versions must reproduce. It found a spurious priority-donation
cycle warning after a mutex handoff, fixed in `mutex.c`. Then the wait
queues in CeeSafe (`kernel/scheduler/wait.cee`, over `wait_floor.c`; ceesafec
0.6.0's ADR 0030), which give the recorded digest (2026-10-30); and the
completions and semaphores in CeeSafe (`completion.cee`, `semaphore.cee`),
which give it too (2026-10-30); and the mutex with its priority
inheritance (`mutex.cee`), which gives it as well (2026-10-30). Then
quiescence (`kernel/core/quiesce.cee`, over `quiesce_floor.c`), the
first port drafted by Nova's `c2nova port` and finished by hand; its
self-tests pass on both architectures (2026-10-30). Then the pipe's ring
and anonymous pipes (`kernel/ipc/pipe.cee`, over `pipe_floor.c` and a
killable wait in `wait_floor.c`; 2026-10-31), and named pipes
(`kernel/ipc/fifo.cee`, over `fifo_floor.c`; 2026-10-31). The negative
compile test is `tests/ceesafe/negative/sleep-under-spin.cee`, which `make
cee-check` (CI's "CeeSafe sources" job) holds to exactly its seven refusals:
a mutex, a grace period, a completion, a semaphore and a sleep under spin
and interrupt-saving spin guards, interrupts off and preemption off, with
the same calls accepted where sleeping is allowed (2026-10-31).

Size: about 1.5x.

### SA4 Driver modules in CeeSafe

Nova's C8 loaded and unloaded a Nova `.ko` 1,000 times against CosmoOS on
both architectures. SA4 makes that the way drivers are written: the
module ABI and the device API as CeeSafe interfaces, a template under
`drivers/ceesafe-template/`, signing through `modsign.py` as today. The
proving driver is virtio-rng (`drivers/virtio/virtio_rng.c`), small and
present on every CI boot.

- [x] **Acceptance:** `virtio_rng.ko` from CeeSafe passes `test-modcycle`
  (1,000 cycles, both architectures) and the randomness self-tests, with
  the C driver deleted.
  Met 2026-10-31, locally, with ceesafec 0.2.0: `drivers/virtio/virtio_rng.cee`
  replaces the C, over the device API and floor in `drivers/ceesafe/`
  (`README.md` there), with the template in `drivers/ceesafe-template/`;
  `scripts/cee-gen.py`'s `MODULES` generates the module and its
  `COSMO_MODULE` entry. `module.cycle=` now takes over a module the boot
  loaded, so `make test-modcycle MODCYCLE_ENTRY=modules/virtio_rng.ko`
  passed on x86-64 (1,000 cycles in 30.4 s) and AArch64 (32.5 s), every
  count back where it started; `test-entropy` passed on both, its
  devices-only boot seeded by the CeeSafe driver alone (`devices 256`);
  `VRNG-RESET-REPOST` passes in every self-test boot, and
  `tools/virtio-rng-repost-probe.py` passes both ways on x86-64 (removing
  the stopping check makes the self-test fail). That devices-only boot
  caught the one porting bug: a null virtqueue cookie, which `virtq_add`
  refuses, so nothing was posted while every other source seeded the
  pool.

Size: about 0.7x.

### SA5 Every parser of untrusted input

Before whole subsystems, the parts an attacker reaches first: one parser
family per PR, each with its fuzz target kept and its differential test
against the C. In order of exposure: network frames, TCP segments and
options, DHCP and DNS (`fuzz_net_frame`, `fuzz_tcp_segments`,
`fuzz_dhcp_dns`, `fuzz_net_config`); Linux syscall argument decoding
(`fuzz_linux`); ELF and module ELF (`fuzz_elf`, `fuzz_modelf`); cosmofs
metadata (`fuzz_cosmofs`); virtqueues, USB descriptors and ACPI tables
(`fuzz_virtq`, `fuzz_usb_desc`); `pkg` archives and the INDEX
(`fuzz_pkg`); LZ4 and framebuffer descriptions (`fuzz_lz4`,
`fuzz_fbvalid`; Nova's C6 already wrote both). GPT is done.

- [x] **Acceptance:** every fuzz target in `tests/fuzz/` drives CeeSafe code
  and none drives C; each family's benchmark within the rule's 5%.
  Met 2026-11-01 with LZ4, the last family, as the scope below defines it:
  every decoder the fifteen fuzz targets reach is CeeSafe. Every benchmark
  is within 5% but three the owner accepted with their numbers recorded:
  the Linux decoders (1.05x native x86-64), the ACPI MADT decoder (1.13x)
  and pkg's `version_cmp` (1.25x).

Scope (the owner, 2026-10-31): SA5 converts the **decoders**, every check
and field read on input bytes, not the subsystems around them. A fuzz
target over the network stack, cosmofs or the Linux layer still drives
those subsystems' C until SA6-SA8 convert them; what SA5 requires is that
every parser it reaches is CeeSafe. Each family: the C decoders moved
unchanged behind a header, a host test with a digest recorded from that
C over seeded inputs, the CeeSafe port giving the same digest, the
benchmark, then the C deleted.

Progress: network frames (2026-10-31): `kernel-services/network/netparse.cee`
(C interface `kernel/net/netparse_types.h`, ceesafec's) -- Ethernet, ARP, the IPv4 header and its
checksum and fragment check, IPv4 headers quoted in ICMP errors, IPv6,
neighbour solicitations and advertisements, the UDP header, and the
transport identifiers NAT and the firewall key on. `tests/host/test_netparse.c`'s
digest over 200,000 inputs is the C's; the benchmark is 0.98x the C on
arm64 and on x86-64 (Rosetta), after bulk copies and 48-bit MAC compares
took the first port's 1.07-1.09x back. All four network fuzz targets
run it. TCP segments and options (2026-10-31): tcp_input's header check
and fields and the options walk (`netparse_tcp`, `netparse_tcp_mss`),
with a digest of their own over 200,000 headers and option lists
(`0x35875a1cf041750b`, the C's); 0.97x the C on arm64 and 0.95x on
x86-64 (Rosetta). tcp_input keeps the decoded fields, not the raw
header, so the reset it builds reads them too. DHCP and DNS
(2026-10-31): the options walk both ends share (dhcp.h's `dhcp_opt` is a
wrapper over it), the tap server's frame and message checks, the
client's message and lease with the host name a server offers, the DNS
proxy's id and its resolver address; a digest of their own over 100,000
inputs (`0x27c8c7e8ec420cdd`, the C's); 1.01x the C on arm64 and 1.03x
on x86-64 (Rosetta). The DNS proxy reads only the id: the queries and
answers it relays are not parsed here. Linux syscall arguments
(2026-10-31): `compat/linux/lxconv.cee` -- open flags, PROT bits,
socket addresses both ways, the guest's termios, wait statuses, dirent
types, getdents64 records -- so `fuzz_linux` drives no C; `stat`,
`statx` and termios going out stay in `convert.c` (Linux's `struct
stat` differs between the architectures, and they encode the kernel's
data rather than decode a guest's). Digest `0x3a72da1935783ee6` over
100,000 inputs, the C's. Benchmark: 1.04x the C on arm64 (39.1 ns
against 37.5); 1.07x on x86-64 under Rosetta (50.4 against 47.1),
where identical machine code (`lx_wait_status`, the same instructions
in both builds) measured 4 ns apart, so Rosetta's figure is not a
native one. Native x86-64 (2026-11-01; Debian 12, clang 14 at -O2, the
C from 03380f69's parent and the port's C from ceesafec 0.3.2, both the
recorded digest, pinned to one core): 1.05x (46.0 ns against 43.9, the
median of nine runs, 1.04-1.06), accepted by the owner (2026-11-01),
as the rule allows.
No new C (the CeeSafe rule's point 1, cee-share's gate): the decoders'
header is ceesafec's, with results as bools and named facts rather than
codes C would need a hand header for, and the `module.cycle=` parsers
SA4's take-over needed are CeeSafe (`kernel/module/modspec.cee`), so
`kernel/` has 24 lines less C to convert than before SA4.
ELF and module ELF (2026-10-31): `kernel/process/elfparse.cee` (the user
executable validator; `elf.c` keeps the loader) and
`kernel/module/modelf.cee` (the module image validator, the metadata
check and the loader's accessors; `modelf.c` is gone), so `fuzz_elf` and
`fuzz_modelf` drive no C. Digests `0x9a2e2083c6cf3e58` (100,000 images)
and `0xeaa7bf3ed05aa6ff` (50,000 images and records), the C's.
Benchmarks: module ELF 1.02x the C on arm64 (1.10x on x86-64 under
Rosetta); the executable validator 1.09x on arm64 and 1.12x under
Rosetta, from 2.5x before each header was copied out once and decoded
from an array of known length, the rest code-generation quality:
accepted by the owner at 1.09x (2026-11-01), as the rule allows. The UEFI loader's ELF reader (`boot/uefi/elf.c`) is
not in this family: no fuzz target drives it, and the loader is not
built with ceesafec.
cosmofs metadata, batch 1 (2026-11-01):
`kernel-services/filesystem/cosmofs/cfsparse.cee` -- metadata headers and
their CRC32C, directory entries, block addresses and extents, the checks
on the paths a crafted image reaches first (every metadata read, every
lookup and readdir, every data map). Digest `0x548e0e13fea79b80` over
20,000 rounds, the C's; 1.03x the C on arm64, 1.02x on x86-64 (Rosetta),
after names were copied and compared a segment at a time.
cosmofs metadata, batch 2 (2026-11-01): the records a mount reads first,
added to `cfsparse.cee` -- the superblock's identity, shape, CRC and
peek check, a member label's CRC and identity, the member table's count
and each member, the free and orphan records' counts, an inode slot's
identity (the C step was `cfsmount.c`, now gone). Digest
`0x5d91f9b738036dfe` over 50,000 rounds, the C's. `CFSPARSE_BENCH=mount`:
the field checks 0.88x the C on arm64 and 0.85x on x86-64 (Rosetta),
read as imported struct fields; with the block CRCs, the same C crc32c
in both, 1.00x. The copy-state check stays C: it compares a record with
the live device's region size.
cosmofs metadata, batch 3 (2026-11-01): the records snapshots, rollback
and encryption keep, added to `cfsparse.cee` -- a snapshot list's entry
under either layout (`cfs_snaplist_get`), the rollback record and its
CRC32C (`cfs_rollback_parse`, `cfs_rollback_crc`) and a key slot's shape
(`cfs_keyslot_valid`; the C step was `cfsrecs.c`, now gone). Digest
`0xb62644e6cd01c30c` over 50,000 rounds, the C's. Found on the way and
fixed in the C first: a key slot's `8 * lanes` wrapped in 32 bits, so
0x20000000 lanes passed with any memory cost; lanes are now held to
Argon2id's 2^24 - 1, as user space's are. `CFSPARSE_BENCH=records`, best
of five alternated runs: the list entry 0.88x the C on arm64 and 1.00x
on x86-64 (Rosetta), the rollback record (mostly its CRC) 1.00x and
1.02x, the key slot 0.57x and 1.00x. The cosmofs family's decoders are
CeeSafe; the copy-state check stays C (it reads the live device's region
size).
virtq/USB/ACPI, part 1, the virtqueue (2026-11-01):
`drivers/virtio/virtq_used.cee` -- what the driver decides about a
used-ring element the device returned (`virtq_used_check`: an id outside
the table or not in flight is skipped, a length beyond the chain's
writable bytes clamped, a legacy device's on a read-only chain taken as
nothing written) and the chain's reclaim by the driver's records alone
(`virtq_reclaim`); the C step was `virtq_used.c`. The ring's single
read, its barriers, the lock and the log stay in `virtqueue.c`. The
virtio module links CeeSafe's runtime and the driver floor, as the
CeeSafe driver modules do. Digest `0xb37c1a028a201a5a` over 20,000
queues, the C's. `VIRTQ_BENCH=1`: the CeeSafe 3.2 ns an element against
the C's 6.1 on arm64, 4.6 against 9.8 on x86-64 (Rosetta), best of five
noisy runs.
virtq/USB/ACPI, part 2, USB descriptors (2026-11-01):
`drivers/usb/usb_desc.cee` -- the configuration descriptor walk
(`usb_parse_config`, U5), whole; the C step made it return what it
dropped past the limits for `usb.c` to log. Each descriptor is copied
out once and recorded field by field. The xhci module links CeeSafe's
runtime and the driver floor. Digest `0x7758cfbcf4bd5d6d` over 20,000
configurations, the C's. `USBDESC_BENCH=1`, best of the runs: 1.04x the
C on arm64 (26.5 ns against 25.5), 0.94x on x86-64 (Rosetta); recording
from the bytes without the copy was 0.86x on arm64 but 1.08x under
Rosetta.
virtq/USB/ACPI, part 3, ACPI tables (2026-11-01):
`drivers/acpi/acpi_parse.cee` -- the checksum, the RSDP's signature and
checksums, the root table's entries and the MADT walk, into a state
`acpi.c` keeps and logs from (the C step, `acpi_parse.c`, made the walk
report what it used to log). The generated C finds `acpi_internal.h`
through `kernel.mk`. Digest `0x117ff5b066c8c9ab` over 20,000 rounds,
the C's. `ACPI_BENCH=1`, the MADT decoder: 1.13x the C on arm64 (102 ns
a table against 90) and 1.10x on x86-64 (Rosetta), after each entry's
fields were read from a slice of the length its type needs; 1.62x
under Rosetta before, and a copy into a fixed buffer was slower still.
The MADT is decoded once a boot; the 1.13x was accepted by the owner
(2026-11-01), as the rule allows.
`pkg` archives and the INDEX (2026-11-01): `pkg/pkgparse.cee` -- the
ustar header decoder, versions and their comparison, dependency
constraints, names, sonames, path confinement, hex, times, and the
manifest and INDEX stanzas (the C step, `pkgparse.c`, moved them out of
`manifest.c`, `version.c` and `tar.c` behind `pkgparse.h`, which reports
why it refuses for `manifest.c` and `tar.c` to word). `pkg.elf` is the
first user program to link CeeSafe code: the generated C, CeeSafe's
runtime and a panic hook that aborts (`pkg/ceesafe_panic.c`). Digest
`0xced1250ae40a6b1c` over 20,000 rounds, the C's. `PKG_BENCH=1`, best
of 15: a manifest of 200 files 0.85x the C on arm64 and 0.59x on x86-64
(Rosetta), an INDEX of 300 entries 0.77x and 0.55x, a tar of 40 members
0.27x and 0.54x (the header checksum vectorises), `version_cmp` 1.25x
and 1.12x. A 256-entry table for hex digits, and wrapping arithmetic
where an earlier check bounds the value, took the first port's 1.18x
(manifest, arm64) and 1.30x (tar, Rosetta) back; four other shapes of
the version loop were no faster. `version_cmp` runs once a dependency
check; the 1.25x was accepted by the owner (2026-11-01), as the rule allows.
LZ4 (2026-11-01): `kernel/core/lz4_decode.cee` -- the block decoder that
reads cosmofs's compressed blocks off the disk, whole; the compressor,
which reads only the filesystem's own data, stays C in `lz4.c` (the C
step moved the decoder into `lz4_decode.c` unchanged). Digest
`0xcd72ee02e42e9ec3` over 20,000 rounds of valid, cut, padded and
damaged streams, built streams and noise, of the result and the whole
output buffer: the C's, with clang and gcc. `LZ4_BENCH=1`, the median
of nine runs: 1.045x the C on arm64 (3,146 ns a 4 KiB block against
3,000) and 1.039x on x86-64 (Rosetta). The framebuffer validator was
already CeeSafe (`kernel/core/fbvalid.cee`, SA1), so `fuzz_lz4` and
`fuzz_fbvalid` drive no C decoder.

Size: about 3.0x.

### SA6 The network stack

`kernel-services/network/` (23,773 lines, 11,841 of them self-tests):
mbufs, interfaces, ARP and neighbour discovery, IPv4 and IPv6, ICMP, UDP,
TCP, the firewall and NAT, sockets, AF_UNIX, the per-CPU receive workers.
The self-tests convert after the code they test, so the C tests check the
CeeSafe first.

- [ ] **Acceptance:** the network harness, `test-net-login`, `nettest`'s
  self-tests and the net fuzz targets pass on both architectures with no C
  left in `kernel-services/network/`; PF1's network numbers within 5% of
  the C build's.

Order (2026-11-01): bottom-up, so each module's callers are still the C
its tests already exercise -- the checksum, Ethernet, loopback and the
address helpers; mbufs, interfaces, ARP and neighbour discovery, IPv4
and IPv6 with ICMP; UDP, raw sockets, the firewall and NAT, sockets,
TCP; DHCP, the tap services, `procnet`; then `nettest`. Each port has a
host test with a digest recorded from its C, unless one exists, and a
benchmark of its hot paths. PF1 does not exist yet: its network numbers
are owed before this phase's acceptance, against the C build of a commit
before the first port.

Progress: the checksum (2026-11-01): `kernel-services/network/cksum.cee`
-- buffer and chain sums, the pseudo-headers, `m_csum_complete`, with
`cksum_floor.c` keeping its export to modules; `struct mbuf`'s packet
header is named (`struct mbuf_pkt`) so CeeSafe can import it.
`tests/host/test_cksum.c`'s digest `0xee64aae0b44b750f` over 20,000
rounds, the C's (lines and branches 100%). `CKSUM_BENCH=1`, median of
nine: 1.066x the C on arm64 for 1,500 bytes, 1.091x for 64 KiB, 1.061x
for a 64 KiB chain; 0.85-0.88x on x86-64 (Rosetta). On arm64 clang
vectorises the C's pair loop with `ld2`, which no loop CeeSafe emits
today reaches (eight forms were measured; the fastest weighs each byte by
its place); accepted by the owner (2026-11-01), as the rule allows.
The address helpers (2026-11-01, with ceesafec 0.4.1, whose ADR 0041 adds
imported unions, atomic fields and the log line SA6 needs):
`kernel-services/network/inet.cee` -- netaddr equality, the unspecified
test, the conversions from and to the user's sockaddr shape, and
`netaddr_str` with lib/core's `TextWriter` in place of ksnprintf.
`struct netaddr`'s union is imported as it is. `tests/host/test_inet.c`'s
digest `0x5bff0d7e00d6e5df` over 20,000 rounds, the C's (clang on macOS,
gcc on Linux). `INET_BENCH=1`, median of nine at -O2: `netaddr_equal`
0.97x the C on arm64 and x86-64 (Rosetta); the user-shape round trip
0.92x and 0.58x, after the first port's 1.06-1.16x (byte stores through
pointers that may alias, and a local array's stack-protector check).
Ethernet (2026-11-01): `kernel-services/network/ether.cee` --
`ether_input`, `ether_output` and `eth_broadcast`, over the C mbufs and
protocols; the interface's receive-error counter is an atomic field
shared with C. netparse gains `netparse_ether_word`, its Ethernet verdict
as one word, since an imported struct cannot be a CeeSafe local, and
`ether_output`'s `dst` is declared a pointer in `ether.h` (gcc's
-Warray-parameter refuses an array parameter beside the generated
pointer). `tests/host/test_ether.c` runs `mbuf.c` on the host over a
malloc-backed cache with allocation failures, stubs for the protocol
inputs and `netif_transmit`, and checks every mbuf freed: digest
`0xd21f822ad04f292a` over 20,000 rounds, the C's (macOS clang, Linux
gcc). `ETHER_BENCH=1`, median of nine at -O2 with each mbuf's allocation:
1.00x the C both ways on arm64, 1.01x and 1.00x on x86-64 (Rosetta).
Loopback (2026-11-02, with ceesafec 0.5.0, whose ADR 0042 lets CeeSafe
define the C objects a kernel defines): `kernel-services/network/loopback.cee`
-- `lo` and its ops table are `static mut` C objects with C's designated
initialisers (the transmit a `@callback`, the release C's
`netif_release_static`), the test filter a nullable C function pointer, and
`loopback_init` sets the addresses and registers it. `netif.h`'s anonymous
address struct is named (`struct netif_ip4`) so CeeSafe can import it.
`tests/host/test_loopback.c` checks every field C reads of the registered
interface and the transmit over 20,000 packets with and without a filter:
digest `0x92930b5a63e79528`, the C's (macOS clang, Linux gcc). No
benchmark: the transmit is a flag change and a call, with no loop.
Packet buffers (2026-11-02): `kernel-services/network/mbuf.cee` -- mbufs,
clusters, chains (prepend, pullup, adjust, copy out, append, copy), shared
references and queues, with `mbuf_floor.c` keeping the module exports. The
reference counts are atomic fields shared with C; the statistics an
`IrqSpinLock` (one lock per operation where the C took two or three, so a
snapshot is still one instant); a packet header is copied by C's own size
of it (`layoutOf`), so a field added in C cannot be missed. The double-free
check stays debug-only through `COSMO_CONFIG_DEBUG()`, a C constant macro
`compiler.h` gains for CeeSafe. The double-enqueue warning no longer names
the queue (lib/kernel imports `spinlock_t` opaque); the refusal and the
count, which `net-mbufq-double` tests, are unchanged.
`tests/host/test_mbuf.c`: 4,000 histories of 24 steps over every
operation, the queues, and allocation failures (52,656 allocations, 1,699
failed), every buffer's length, flags, room and bytes after each step, and
every mbuf and cluster freed: digest `0xfd419bfa31e68b04`, the C's (macOS
clang, Linux gcc). `MBUF_BENCH=1`, median of nine at -O2: receive 0.93x
the C, send 0.93x, copy out 1.00x, queue 0.92x on arm64; 0.95x, 0.96x,
1.01x, 0.93x on x86-64 (Rosetta).
The interface layer (2026-11-02, with ceesafec 0.6.0, whose ADR 0043
emits an export only in the builds whose C had it):
`kernel-services/network/netif.cee` -- the registry, the per-CPU receive
queues and their workers, the transmit path, work items and the barrier,
with `netif_floor.c` keeping the module exports. The C state is CeeSafe's
own `static mut` C objects: the registry's list and spin lock, the per-CPU
array (`struct net_cpu`, moved to `kernel/net/netif_cpu.h` so both sides see
one layout), the barrier items. What the data paths read without a lock is
atomic: an interface's flag word, IPv4 address and counters, a work item's
claim (`net_work.queued` is now a `uint32_t`), a worker's readiness, and
the hooks, kept as addresses in atomics and called back as C function
pointers. `netif_tx_probe_*` exist only `#if CONFIG_SELFTEST` and
`netif_test_worker_hooks_set` only `#if CONFIG_DEBUG` (`@cfg`); the
self-test timing and the debug hooks fold away through
`COSMO_CONFIG_SELFTEST()` and `COSMO_CONFIG_DEBUG()`. `wait_event` is
written out (prepare, test, block, finish). `netif_owns_ipv4/6` take no
reference, as in the C.
`tests/host/test_netif.c`: 6,000 steps over up to eight interfaces --
registration (valid and refused), fw_cfg and DHCP configuration, lookups,
default and connected routes, addresses, up and down, forwarding, owned
addresses, listings, transmits (checksum finished or not, up, down, gone),
receives queued and purged, unregistering, work items -- with every kernel
service it calls a stub folded into the digest: `0x83fa866653f4b673`, the
C's (macOS clang, Linux gcc). `NETIF_BENCH=1`, median of nine at -O2:
receive 0.96x the C, transmit 0.99x, find 1.02x, flow hash 0.79x, owns
1.00x, connected 1.00x on arm64; 0.97x, 1.00x, 1.05x, 0.70x, 0.98x, 1.03x on
x86-64 (Rosetta). The flow hash reads a frame's first segment in place where
the C copied it.
Address resolution (2026-11-03): `kernel-services/network/arp.cee` -- the
neighbour table keyed by (interface, address), resolutions that park a
packet while a request is out, what requests and replies may teach it
(RFC 826, N25), and the ageing pass with its retries, timeouts and
expiry, which runs the other protocols' ageing too. The table and the
counters its lock covered are an `IrqSpinLock`'s data, interfaces and
parked packets held by address; the four counters the C added outside
the lock are atomics. The debug build's two park hooks are `@cfg`
exports, as in the C. netparse gains `netparse_arp_bytes`, the ARP
decoder's fields as 16 bytes, since an imported `struct netparse_arp`
cannot be a CeeSafe local; `ether.h` declares `arp_resolve`'s and
`arp_lookup`'s MAC as a pointer, as `ether_output`'s.
`tests/host/test_arp.c`: 20,000 rounds over three interfaces and 96
neighbours, more than the table holds -- resolutions, bursts that evict,
lookups, frames in of every kind, flushes, deletions, listings, ageing
and the work item, the hooks, allocation failures -- every mbuf freed and
every interface reference given back: digest `0xc5ce56f4e28bdf48`, the
C's (macOS clang, Linux gcc). `ARP_BENCH=1`, the median of nine at -O2: a
reachable resolution 1.00x the C, a request for us answered 1.00-1.03x on
arm64; 1.01-1.03x and 0.98-1.02x on x86-64 (Rosetta).
Neighbour discovery (2026-11-03): the C step moved it out of `ipv6.c`
into `nd.c` with its own output (the direct-to-MAC half of
`icmpv6_send`, which only ND used) and its own counter
(`ip_stats.nd_pending_dropped`, read through `nd_pending_count`), and
fixed a leak its host test found: a solicitation that taught the table
an asker whose entry was incomplete left the packet parked on the
now-reachable entry, never sent, and lost when the entry expired; it is
sent now, as ARP's is. Then `kernel-services/network/nd.cee`: the table
of 32 link-local neighbours keyed by (interface, address), resolutions,
solicitations and advertisements in, ageing, and the debug build's park
hooks, as the ARP port (an `IrqSpinLock` over the table, interfaces and
packets by address). netparse gains `netparse_nd_bytes`, as
`netparse_arp_bytes`. `tests/host/test_nd.c`: 20,000 rounds over three
interfaces and 48 neighbours -- resolutions, bursts that evict,
lookups, solicitations and advertisements of every kind, flushes,
ageing, the hooks, allocation failures -- every mbuf freed and every
reference given back: digest `0xc4e59aaf81eb00c3`, the fixed C's (macOS
clang, Linux gcc). `ND_BENCH=1`, the median of nine at -O2: a reachable
resolution 0.70x the C on arm64 and 0.75x on x86-64 (Rosetta), where an
address compare that folds every byte's difference lets clang compare
words, as the C's `in6_equal` did (an early-exit byte loop was 1.22x
under Rosetta); a solicitation for us answered 0.98-1.00x and
0.97-1.01x.
No new C, repaid and owed (2026-11-03). The push of 2026-11-03 (the
installed-system fixes: init as pid 1, the self-tests' skips, the AHCI
rescan, test disks by serial; and SA6's netif headers) added 309 lines
of C to convert (`cee-share --gate cd5e99db`: kernel/ +175, drivers/
+90, userland/ +44), against this section's rule. Repaid: `netif_cpu.h`
moved beside `netif.cee`, its only user (kernel/ -66); the test-disk
lookup and the serial decoding moved into `kernel/block/blkserial.cee`
(kernel/ -24, drivers/ -12); and `drivers/storage/diskid.cee`, the ATA
IDENTIFY, NVMe Identify and SCSI INQUIRY and READ CAPACITY decoders three
drivers had (drivers/ -63; its host test's
digest `0x4949c54aa80780f2`, the C's, macOS clang and Linux gcc; NVMe and
SCSI at or under the C's cost, ATA IDENTIFY 2.0x, 25 ns against 13 on
arm64, once a disk attach: accepted by the owner, 2026-11-03, as the rule
allows). Owed: drivers/ +15 (virtio-blk's GET_ID
request and USB storage's string-descriptor request) until SA9 converts
those drivers; kernel/ +85 (`devtest.c`'s and
`iommutest.c`'s skips and rescan, init as pid 1 in `process.c`, the
`COSMO_CONFIG_*()` macros CeeSafe needs) until SA10 converts those
files, and userland/ +44 (init's test-disk check) until SA12; a CeeSafe
check in init now would need its own object tree and panic hook and
repay less than half.
IPv6 (2026-11-03): `kernel-services/network/ipv6.cee` -- routing (the
loopback for our addresses, the default interface for link-local and
multicast ones), the source choice, output through ND or the loopback,
input with its checks and its dispatch to ICMPv6, UDP and TCP, and
ICMPv6's echo and the dispatch of ND's messages. A received header is
copied to a 40-byte local and handed on by address (`struct ipv6_hdr`
is packed); addresses are read in place through slices. The counters
are atomics; `ipv6_get_stats` zeroes the record and fills IPv6's fields.
`tests/host/test_ipv6.c`: 20,000 rounds over a loopback and an Ethernet
interface -- datagrams in of every shape and destination, ICMPv6 echoes
and ND messages, datagrams out past the MTU and past 65,535 bytes,
through ND's three answers, with and without headroom, the routing and
source choices -- every mbuf freed and every reference given back:
digest `0x58f5b59e3be23bcc`, the C's (macOS clang, Linux gcc), 100% of
the C's lines and branches. `IPV6_BENCH=1`, the median of five runs'
medians: an echo answered 1.04x the C and a 512-byte datagram sent
1.02x on arm64; 1.03x and 1.02x on x86-64 (Rosetta), after reading the
addresses in place took the first port's 1.05-1.13x back.
IPv4 and ICMP (2026-11-03): `kernel-services/network/ipv4.cee` -- the
ICMP rate limit and the path MTU cache (each an `IrqSpinLock`'s data),
routing and the route list, the source choice, output through the
firewall's OUTPUT verdict and host flow, ARP or the loopback, ICMP
errors, echo and fragmentation-needed with its quoted-flow
confirmation, forwarding with anti-spoof, the FORWARD verdict and NAT,
input with DHCP, NAT's return path and the INPUT and host chains, and
the debug build's forwarding hook. Headers are read and written as
bytes; a received one is copied to a local array and handed on by
address (`struct ipv4_hdr` is packed), as are the `struct netaddr` and
host flow it builds (their C layouts as words). The firewall's verdicts,
C enums with no negative values, are declared `u32`, an `unsigned int`
for clang and gcc alike. netparse gains `netparse_l4_flow`, the ports
and sequence as one word. `tests/host/test_ipv4.c`: 20,000 rounds over a
loopback, an uplink, a masquerading tap and a forwarding link, every
hook a seeded stub -- every mbuf and allocation freed and every
reference given back: digest `0x003b7ba636ab8d7a`, the C's (macOS clang,
Linux gcc); 98.6% of the C's lines and 96.1% of its branches, the rest
unreachable from the host or a second thread's. `IPV4_BENCH=1`, the
median of five runs' medians: an echo answered 1.01x the C and a
512-byte datagram sent 1.02x on arm64; 1.01x and 1.00x on x86-64
(Rosetta).
UDP (2026-11-03): `kernel-services/network/udp.cee` -- binding with
ephemeral ports, connecting, sending over IPv4 and IPv6, the receive
path (exact address over wildcard, connected sockets only from their
peer, quiet datagrams only for a connected socket), port-unreachables,
ICMP errors for a connected flow, NAT's port question and the
/proc/net listing. The pcbs stay C objects each socket embeds, on this
file's list under its spin lock as before; a `struct netaddr` is read
and written by its fields, one built here a local of five words. netparse
gains `netparse_udp_flow`, its UDP fields as one word. The C step fixed
a bug: a port-unreachable quoted the UDP header from the bytes in front
of the data, which are stale heap when `m_prepend` has to allocate; the
header is now kept and written into the quote. `tests/host/test_udp.c`:
20,000 rounds over eight sockets of both families -- binds, connects,
sends, datagrams in of every length, checksum and destination, queues
overflowing, sockets being released -- every mbuf freed and every wake
reference given back: digest `0x653e074bd035e785`, the C's (macOS clang,
Linux clang and gcc); 98.2% of the C's lines and 95.7% of its branches,
all but the ephemeral range exhausted (16,383 bound sockets).
`UDP_BENCH=1`, the median of five interleaved runs: a 512-byte datagram
received and read 1.01x the C and one sent 1.01x on arm64; 0.99x and
1.00x on x86-64 (Rosetta).
Raw ICMP sockets (2026-11-03): `kernel-services/network/raw.cee` -- the
pcbs on their id-ordered list, sends as built, and delivery of a copy
of each ICMP message to every raw ICMP socket, sixteen at a time: wake
references under the lock, copies after it, the next batch after the
last id taken. The pcb is allocated with `kzalloc(layoutOf<raw_pcb>())`,
the C's `sizeof`. `tests/host/test_raw.c`: 20,000 rounds over up to
forty sockets -- pcbs made and freed with messages queued, sends refused
every way, messages delivered with copies and prepends failing, sockets
being released and queues full, the message handed in unchanged (N31)
-- every mbuf and pcb freed and every wake reference given back: digest
`0x51770db96e3ebf5e`, the C's (macOS clang, Linux clang and gcc); 100%
of the C's lines and branches. `RAW_BENCH=1`, the median of five
interleaved runs: a 64-byte echo reply delivered to four sockets 1.03x
the C and a message sent 0.99x on arm64; 0.97x and 1.00x on x86-64
(Rosetta).
The firewall (2026-11-03): `kernel-services/network/fw.cee` -- the
guests' and the host's rule lists and default policies, attachment and
purge, and the FORWARD, INPUT, host and OUTPUT chains over a bounded
flow table shared by the guests' and the host's quotas, with the
OUTPUT fast path an atomic read without the lock. Everything else is one
`IrqSpinLock`'s data, the host object a ninth guest slot no datagram
can name. netparse gains `netparse_l4_view`. The configuration fuzz
target's doors are the debug builds' (as ipv4's test exports are), and
its rule hook a setter (`fw_test_set_rule_matched`); the port calls it
just after the lock is released, since a C function pointer carries no
effect list and may not be called under a spin lock (CeeSafe's
type-system.md §32). `tests/host/test_fw.c`: 20,000 rounds over ten
guest addresses competing for eight slots, their taps, an uplink and
the world -- rules by every owner, every chain, replies to recorded
flows, floods past both shares, ageing and flushes -- every mbuf freed:
digest `0xc99643fb9523e2f5`, the C's (macOS clang, Linux clang and gcc);
99.7% of the C's lines and 96.8% of its branches, all but a refusal for
a full table with the initiator's share unspent, which the shares
(summing to the table) leave unreachable. `FW_BENCH=1`, the median of
five interleaved runs: a datagram on a recorded flow 0.99x the C and one
judged by a full rule list 0.97x on arm64; 1.00x and 0.98x on x86-64
(Rosetta).
NAT (2026-11-03): `kernel-services/network/nat.cee` -- masquerade of
guest UDP, TCP and ICMP echo out the uplink and the replies and ICMP
errors back, inbound port forwarding with its handshake-gated hold, the
port-forward rules and their configuration strings, and the guests'
shares of the conntrack table; the table and the rules are two
`IrqSpinLock`s' data, taken rules first as before. Checksums are patched
incrementally (RFC 1624) on big-endian words. The C step fixed a bug: an
ICMP error quoting a masqueraded flow had its checksum recomputed over
the whole message from the first mbuf though only the quote was pulled
up, reading past it on a chained packet (AddressSanitizer); it is summed
over the chain. `nat.c`'s static assertion that lent ports stay below
the host's ephemeral range moved to `tests/host/test_nat.c` (CeeSafe has
none, and a header would be new C). `tests/host/test_nat.c`: 20,000
rounds over ten guests, an uplink and the world -- masquerades and
refusals, replies and errors split across mbufs, forwarded connections
through a full handshake, floods past a share and the table, every port
lent, rules added, deleted, cleared and parsed -- every mbuf freed and
every reference given back: digest `0xea7a64d4cbe6636a`, the C's (macOS
clang, Linux clang and gcc), and the parser's 412 warnings the C's word
for word; 99.8% of the C's lines and 95.9% of its branches, all but a
guard the next port never trips. `NAT_BENCH=1`, the median of five
interleaved runs: a datagram masqueraded 0.95x the C and its reply
1.03x on arm64; 0.97x and 1.00x on x86-64 (Rosetta).
Sockets (2026-11-03): `kernel-services/network/socket.cee` -- the
blocking calls over the TCP, UDP, raw and unix transports, the
asynchronous error word delivered once (N21) with its peek-and-commit
pair, the wake references and the debug build's held wake. The C step
moved the seven waits onto `wait_floor.c`'s killable wait over
condition functions, as pipe.c's were. What CeeSafe 0.6.0 cannot build
stays in `socket_floor.c`: the socket's kobject type (a constant C
object of the port's exported operations) and the type test against
it, as `pipe_floor.c` keeps the pipe's. `socket.h`'s `nonblock` became
a word (CeeSafe's atomic bool is one), the struct's `type` field is
declared with `@cname`, and the held wake now waits on the armed
socket's own queue, so it allocates nothing. `tests/host/test_socket.c`:
20,000 rounds over up to eight sockets of every family and type, over
stub transports whose world moves between a wait's asks -- connections
made and refused, data and datagrams arriving, errors and shutdowns
landing, waiters killed -- every socket, pcb and mbuf freed and no mutex
held: digest `0xc27cb34a9bd69c1d`, the C's (macOS clang, Linux clang
and gcc); 99.7% of the C's lines and 95% of its branches, all but the
error word's two compare-exchange retries, which need a second thread.
The whole history runs in the C's time (41 ms on arm64; 55 against 57
on x86-64, Rosetta); the transports carry a socket call's cost.
TCP (2026-11-03): `kernel-services/network/tcp.cee` -- the RFC 793
state machine, the RFC 6298 retransmission timer, slow start,
congestion avoidance and fast retransmit, delayed ACKs, TIME_WAIT,
RFC 5961 challenge ACKs, the SYN cache and its cookies, keepalive, the
orphaned FIN_WAIT_2 timeout, out-of-order reassembly and path-MTU
notifications, over pcbs that stay C objects (struct tcp_pcb) reached
by their fields, each under its own spin lock, with the table's lock
inside it. What CeeSafe cannot hold stays in `tcp_floor.c`, called
through functions: the pcb fields only some builds have (the debug
builds' liveness mark, the self-test builds' arming time), the host
targets' state-change hook (TCP_HOST_TEST), and the debug builds'
self-test seams (a held timer callback, a held ACK), as tcp.c had them.
netparse gains `netparse_tcp_words`. A batch's packet is held as an
address, so the empty batch every call starts from is a static: building
it field by field cost the port 10% against the C's in-place zeroing.
The C step's host test runs TCP over the TCP fuzz target's host
(`tests/fuzz/shim_net.c`) with the rest of the stack as the kernel's:
four histories of a listener and up to six connections to two peers --
opens of every kind (SYN floods past the cache, cookies, simultaneous
SYNs, resets, silence), the peer's segments of every flag, offset,
window, option and length, in order and out, corrupted and malformed,
the host's calls, duplicate ACKs, simultaneous close, TIME_WAIT, ICMP,
the firewall's OUTPUT chain refusing the host's segments -- every frame
transmitted with correct checksums and nothing left allocated: digest
`0x62a476ae9c5d6fbd`, the C's (macOS clang, Linux clang and gcc);
89.9% of the C's lines, the rest IPv6 (the shim's link has none), a pcb
ended between lookup and lock, pullup failures and the debug builds'
self-test seams. `TCP_BENCH=1`, the median of five interleaved runs on
one connection through the whole host stack: a 1,400-byte segment
received and read 1.03x the C and one sent and acknowledged 1.05x on
arm64; 1.02x and 1.00x on x86-64 (Rosetta).

DHCP (2026-11-03): `kernel-services/network/dhcp.cee` -- the client, one
per autoconfigured interface: DISCOVER, OFFER, REQUEST and ACK, the
retransmission backoff, the give-up in REQUESTING, renewal unicast to
the server (through the lease's router when the server is off the
link), rebinding, the lease's end, NAKs, the boot wait. The four slots
are a CeeSafe table holding the C's timer, work item and lock by value,
each slot's lock set up once; a timer only queues the work, and every
message is built after the lock is dropped, as the C did. netparse gains
`netparse_dhcp_bytes` and `netparse_dhcp_lease_bytes` (a header's C type
cannot be a CeeSafe local), checked against the structs in
`test_netparse.c`. The C step fixed a log line: an ACK naming no DNS
server logged "0 DNS server(s)0.0.0.0". Its host test runs the client
over the TCP target's host with the stack as the kernel's: four
interfaces with two servers each, a router and a server beyond it,
replies mostly right and otherwise wrong in every field, length and
checksum, ARP answered or not, full rings, time in small and long steps,
kicks, stops and restarts, the fifth client refused, the boot wait; every
frame, logged line, return, lease list and address in the digest,
`0x10330ab4022bbc46`, the C's (macOS clang, Linux clang and gcc); 98.7% of
the C's lines, the rest two cross-CPU windows, `m_getcl` failing and an
unreachable default. `DHCP_BENCH=1`, the median of five interleaved runs
of an exchange (NAK, OFFER, ACK) through the host stack: 1.04x the C on
arm64, 1.03x on x86-64 (Rosetta).

The tap services (2026-11-03): `kernel-services/network/tapsvc.cee` --
the DHCP server a tap's input filter runs for its one guest, and the DNS
proxy's two threads (a query upstream under an id lent at random, an
answer back only from the configured upstream, SERVFAIL without one).
An instance is a CeeSafe object behind `tapsvc.h`'s opaque struct (its
generated `tapsvc_types.h` gives C its layout), allocated zeroed, under
the C's three locks; each side's datagram buffer is the instance's, off
its thread's stack. The C step fixed two things: the proxy read
datagrams into 512 bytes, which `ksock_recvfrom` cuts to silently, so an
EDNS answer reached the guest cut short and malformed (the buffer is
now 2048: IPv4 drops fragments, so nothing longer than a link's MTU
arrives); and the counters were copied as a plain struct while added to
atomically. The `TAPSVC_HOST_TEST` doors are gone from `tapsvc.h`:
`fuzz_dhcp_dns` now drives the service through `tapsvc_start`, as the
host test does, with the tap, sockets, threads and firmware
configuration its own and the proxy's loops run in place. netparse's
`netparse_dhcp_bytes` grows to the fields the server reads. The host
test: nine taps (the ninth refused), sockets and threads failing at
every step of the start, resolver strings of every shape, every DHCP
message from the guest holding the lease or a second, queries and
answers of every size from the upstream, a stranger or the wrong port,
the pending table filled and expiring; every frame, datagram, logged
line and return in the digest, `0x2d2700174801ec6b`, the C's (macOS
clang, Linux clang and gcc); 98.6% of the C's lines, the rest allocation
failures and the 4,096 tries for a free id. `TAPSVC_BENCH=1`, the median
of interleaved runs of a round (DISCOVER, REQUEST, a query and its
answer): 1.01x the C on arm64, 1.04x on x86-64 (Rosetta) and 1.05x on
native x86-64 (Linux gcc).

The tap (2026-11-04): `kernel-services/network/tap.cee` -- the tap
interface (a netif whose far end is userland), `/dev/net/tap` (a tap per
open, its subnet, forwarding, masquerade, services and firewall
attachment, all of it gone at the last close) and `/dev/net/tapctl` (a
command applied whole or refused; the snapshot of the stack's tables in
the netctl.h records). A tap is a CeeSafe object behind `tap.h`'s
opaque struct, its netif first; each netctl record is built in a zeroed
scratch through its C type's fields and copied out at its own size, so
no padding of the stack reaches userland. The snapshot's sections share
one record scratch and one table scratch in turn: the first build gave
each its own and the kernel's 2 KiB frame limit refused `ctlRead` (8 KiB);
it is now about 1.5 KiB in one frame, against the C's 1.9 KiB over two. `tap_floor.c` kept the debug
builds' release seam (two exported variables) until nettest's last
batch, which moved it into `tap.cee`. The C step published the
input filter with release stores and acquire loads (function last,
cleared first) and stated the caller's contract, moved the reader's wait
to `wait_floor_until_killable`, and moved the UAPI assertions into the
host test, which now also asserts every value `tap.cee` writes as its
own constant against the headers -- the first build of the port failed
one (`NETIF_NODEFAULT` is bit 4). `tap.h` declares the MAC as a pointer
(gcc's -Warray-parameter). The host test runs `tap.c` alone over real
mbufs, the stack around it seeded stubs: opens to the ninth, failing
registrations, services and allocations, writes of every length and
privilege, reads blocking, killed and non-blocking, the queue past 64,
every control command right and wrong, the snapshot into buffers of
every size; every call with its arguments, frame and returned byte in
the digest, `0xedefcf1c515836e7`, the C's (macOS clang, Linux clang and
gcc); 99.1% of the C's lines, the rest two unreachable branches.
`TAP_BENCH=1` (a frame in, a frame out, a snapshot every 64 rounds):
1.01x the C on arm64, 0.98x on x86-64 (Rosetta), 1.00x on native x86-64
(Linux gcc).

`/proc/net` (2026-11-04): `kernel-services/network/procnet.cee` -- the
eight files BusyBox's ifconfig, route, netstat and arp parse, in Linux
6.x's layout, from the stack's listings. Its fields are written by small
formatters of its own (padded decimal, upper-case hexadecimal, padded
strings, Linux's line padding) rather than a format string;
`procnet_floor.c` keeps `procnet_names`, data procfs reads. The C step's
host test renders through the kernel's own `kvsnprintf` (linked in;
the harness's `ksnprintf` is weak for it), and the digest is the same
through the host's: listings of every size and value, a file that does
not exist, the allocator failing at any allocation, the text's form
checked line by line; digest `0x4b8286238c3a895b`, the C's (macOS clang,
Linux clang and gcc); 98.6% of the C's lines, the rest a line too long
for any format here. `PROCNET_BENCH=1`: /proc/net/tcp with 400 sockets
0.91x the C on arm64 and 0.81x on x86-64 (Rosetta), /proc/net/dev with
32 interfaces 0.92x and 0.86x -- no format string to parse.
`test-net-login` reads it through BusyBox.

The self-tests (`nettest`, begun 2026-11-04), in batches by area, each
leaving the rest C: a batch's evidence is the debug boot on both
architectures (525/525) with each of its tests' log lines what the C's
were, timings aside, its frames under the kernel's 2 KiB, and the values
it writes as its own constants asserted in `nettest.c` against the
headers. The C's shared release machinery (the `nt_*` wrappers and the
census) stays C until the last batch. Batch 1, `nettest_base.cee`:
net-mbuf, net-mbufq-double, net-cksum and net-arp; a C struct no
CeeSafe local holds (a queue, the counters) is built in an aligned,
zeroed scratch, and a failed check names itself in words rather than a
line number. Batch 2, `nettest_lo.cee`: net-lo-udp, net-lo-tcp,
net-tcp-delack, net-tcp-nagle-peer, net-tcp-mss and net-lo-tcp-loss,
with the bulk transfer they share with net-rx-dup and net-tcp-reorder
(`nettest_tcp_transfer`, exported back to the C). The test servers'
state (`struct tcp_server`) moved to a private `nettest.h` with the
framework wrappers the batch calls; a server lives in a static room of
the batch's, since its thread can outlive the test, and an address in
`NettestAddr`, `struct netaddr`'s layout asserted in `nettest.c`.
tcp-pcb-timer-free and net-rx-dup, which need the debug and
fault-injection builds' hooks, wait for a batch of their own. Batch 3,
`nettest_sockerr.cee`: net-sockerr-udp, net-sockerr-spoof,
net-sockerr-accept, net-sockerr-locking and net-rxhook-grace, with the
hand-built ICMP unreachable the first two inject and their own
`wait_until` for a socket's error. A socket the C put on every path is
held in a slot of the test's and put after the test's body, however
the body ended. Batch 4, `nettest_tcp.cee` (milestone 8's hardening):
net-tcp-syncache, net-tcp-rfc5961, net-tcp-reorder, net-tcp-keepalive,
net-icmp-limit, net-raw-icmp and net-nonblock, with the injected
segment, the loopback filters and the holding server. `wait_until`
is a `Pred` enum matched in one `holds`, a case per predicate the C
had, under the C's contract (one field, or a monotone sum of monotonic
ones); a counter struct is read into a room and copied out into the
batch's own snapshot, so no frame holds two. The pipe ends' read and
write, function-pointer fields CeeSafe cannot call, go through two
wrappers in `nettest.c`. Batch 5, `nettest_nif.cee` (unit 11):
net-steer and net-csum-offload. Each registers a fake interface, a C
object the batch defines as `lo` is (`static mut` with designated
initialisers, its ops table CeeSafe callbacks), zeroed and filled in
at the test's start as the C's memset did; registration, the
creator's reference and the kept packet go through the release
framework. Batch 6, `nettest_tap.cee`: the frame builders every tap
test uses (`nettest_wrap`, `nettest_mk_udp`, `nettest_mk_tcp`,
`nettest_mk_icmp`, `nettest_l4cksum`, `nettest_l4_ok`,
`nettest_seed_arp`, `nettest_build_udp`, `nettest_recv_ip`,
`nettest_mk_dns`), exported under the C's names so the tap tests still
in `nettest.c` build their frames in CeeSafe, and selftest_tap,
net-route, net-forward, tap-filter, net-dhcp and net-dns. Batch 7,
`nettest_fwd.cee`: net-pf-clear, net-dnat, net-multiguest and
net-firewall, with the firewall tests' guest helpers exported as
`nettest_fwt_send`, `nettest_fwt_recv` and `nettest_fwt_mk_icmp` for
the chain tests still in C. A firewall rule or control command the
kernel takes is built in a zeroed room, as the C's designated
initialisers zeroed what they did not name; the NAT-state release and
the control snapshot's length stay C (tap-release-order and tapctl
use them). Batch 8, `nettest_ctl.cee`: net-tapctl, and the walk of a
control snapshot's sections, exported as `nettest_netctl_flows_off`
and `nettest_netctl_snapshot_len` for the flow-listing tests still in
C. A section header may lie at any offset its predecessors leave (the
flow section's carries 64-bit counters), so each is copied into an
aligned room before it is read, as the C memcpy'd it. Batch 9,
`nettest_input.cee`: net-input (the INPUT chain), and the checksummed
ICMP messages the host chains test with, exported as
`nettest_fwt_ones_sum`, `nettest_fwt_mk_icmp_ck`,
`nettest_fwt_mk_needfrag` and `nettest_fwt_recv_echo_reply`. The C's
`FWT_RISES` counter wait is one function over a `Counter` enum (it
stays a macro in `nettest.c` for the tests there). Batch 10,
`nettest_hostin.cee`: net-hostinput (the host chain), and the world side
of the uplink tap the host-state tests drive a connection with, exported
as `nettest_hin_mk_tcp`, `nettest_hin_send`, `nettest_hin_recv`,
`nettest_hin_drain`, `nettest_hin_recv_stream`, `nettest_hin_accept`,
`nettest_hin_recv_sock` and the two listeners; the datagram read back,
`struct hin_seg`, moves to `nettest.h`. The host's outbound connect, whose
thread may outlive the test, stays in `nettest.c` behind
`nettest_hin_connect`, `_done` and `_join`. Batch 11,
`nettest_hoststate.cee`: net-zero-window-probe, net-tcp-sws,
net-fin-acks-last-data and net-hoststate (the host's own state), with the
echo-reply hook (a callback writing atomics), the Need-Fragmentation and
port-unreachable quoting a host segment, and the wait for the next data
segment; a pcb's retransmit count is read atomically through
`nettest_tcp_retransmits`, as the C read it. Batch 12,
`nettest_flows.cee`: net-flows-nat and net-flows-fw (the operator's flow
listing), with the read of the whole `/dev/net/tapctl` snapshot they
share; the snapshot's sections are walked by `nettest_ctl.cee`'s exports.
Batch 13, two files: `nettest_output.cee`, net-output and net-tcpverdict
(the OUTPUT chain, and its verdict carried back into the connection),
with the OUTPUT rule they build; and `nettest_tapdev.cee`, tap-ready (the
tap's file waited on, a killed reader) and net-dhcp-client (the kernel's
client against a fixture interface, whose transmit is a callback), the
latter exported only under `CONFIG_SELFTEST` (`@cfg`), where `nettest.c`
keeps the C's skip otherwise. Batch 14, `nettest_hooks.cee`:
tcp-pcb-timer-free and net-rx-dup, the two that need the debug and
fault-injection builds' hooks, each body exported only where its hooks
exist (`@cfg("CONFIG_DEBUG")`, `@cfg("CONFIG_FAULTINJECT")`) and the C's
skip kept in `nettest.c` otherwise, with the timer test's pinning
wrapper; its armer and releaser threads read static state, not the
test's frame. Batch 15, `nettest_lifetime.cee`: an interface's lifetime
against the neighbour tables -- net-arp-retry-unregister and
net-nd-retry-unregister (debug-only, the C's skips kept),
net-arp-flush-counts, net-nd-flush-counts and net-netif-lifetime -- with
fake interfaces of their own; the parked retry's deadline and done flag
are static, and the refused duplicate's never-initialised object is
checked byte for byte, the kernel library's `kobject` being opaque. Batch 16,
`nettest_neigh.cee`: a neighbour entry is (interface, address) --
net-arp-per-interface, net-nd-per-interface and net-neigh-down-race
(debug-only, the C's skip kept) -- the ARP and ND messages built in
CeeSafe and handed straight to the input functions; the racing caller's
state is static and the park hooks are chosen by an enum of the two tables. Batch 17,
`nettest_worker.cee`: netif-remove-worker and netif-rxq-count, debug-only
(the C's silent passes kept), which park CPU 0's receive worker through the
worker hooks; their completions are static C objects, and `nettest.c`'s own
fake interface, with no user left, is gone. Batch 18, `nettest_accept.cee`:
net-accept-race, net-census-wake-ref and net-accept-order (the latter two
debug-only, the C's skips kept), with the listen-backlog step net-lo-tcp
shares and the wait they used. The step takes the debug build's ACK hold
as function parameters, so its all-builds export names no debug hook; a
one-line `nettest_census_take` lets CeeSafe import the census beside its
struct, which selftest.h gives the same name. Batch 19, `nettest_seam.cee`:
net-netif-flags, net-tap-release-order (debug-only) and
net-syncookie-seeded (CONFIG_SELFTEST-only), the C's skips kept; tap.h's
after-purge seam is two C variables CeeSafe cannot assign, so `nettest.c`
gains `nettest_tap_set_after_purge`, and the toggler's and the seam's state
are static. Batch 20, `nettest_nat.cee`: net-nat (UDP,
TCP and ICMP masqueraded and restored, an ICMP error translated back,
anti-spoof, the guest's share, expiry) and net-second-nic, the frames and
read-back headers in rooms; `find_other_interface` stays in `nettest.c`
for net-nicbench. Batch 21, `nettest_nicbench.cee`:
net-nicbench, the ARP phase's hook state static; the chaos build's
reported-not-asserted reply count is the C's `#if CONFIG_SCHED_CHAOS`,
now nettest.h's `NETTEST_CONFIG_SCHED_CHAOS()` read into a static, which
the C compiler folds (in nettest.h, not compiler.h, so no tree's C
grows). `find_other_interface` is gone from `nettest.c`. Batch 22,
`nettest_bench.cee`: net-bench, the loopback benchmark (TCP with one and
two flows, UDP, steering off and on); the flows' and the receiver's state
static, the per-round RTO-backoff reset behind `COSMO_CONFIG_SELFTEST()`. Batch 23,
`nettest_harness.cee`: net-harness, the host harness's guest side -- the
echo services on ports 7 and 8 and the back-connection's retried
exchange, its `NETTEST:` lines printed through kprintf byte for byte as
the C printed them; the injected break of the first attempts
(`make test-harness-retry`) is nettest.h's `NETTEST_HARNESS_BREAK()`. The
last batch, `nettest_frame.cee`: the release framework -- the census
the runner takes around each test, the acquisitions that register their
release with it in the same call, the hooks and settings restored however
a test returns, the servers' threads, the host's outbound connect and
its reap list. `nettest.c` is `nettest_floor.c`, which keeps the C
CeeSafe cannot hold: the skips of tests whose seams a build lacks (an
export under `#if !CONFIG_X`), the tap seam's setter, and every port's
constants asserted against the headers, as `fifo_floor.c` keeps
`fifo.cee`'s. Then the skips and the seam left the C: each test whose
seams a build lacks skips in CeeSafe on a static from the build's constant
macro (`COSMO_CONFIG_DEBUG()`, nettest.h's `NETTEST_CONFIG_FAULTINJECT()`),
which the C compiler folds, so no other build's code names a hook; the
tap's after-purge seam is `tap.cee`'s, an atomic hook and argument set
through `tap_test_set_after_purge`, and `tap_floor.c` is gone.
`nettest_floor.c` keeps only the asserts.

Size: about 3.0x.

### SA7 Storage: cosmofs, VFS, block and pools

`kernel-services/filesystem/` (26,934 lines), `kernel-services/vfs/`
(9,436), `kernel/block/` (5,162) and `kernel-services/storage/` (837):
cosmofs with its snapshots, mirrors, encryption, scrub and resilver; the
VFS, page cache and mount namespaces; the block layer. Converted in
dependency order (block, pool, cosmofs, VFS), each step on the existing
suites.

- [x] **Acceptance:** `test-install`, `test-upgrade`, `test-snapshot`,
  `test-fsck`, `test-mirror`, `test-encrypt` and `test-large` pass on both
  architectures with no C left in those trees but what only C can write
  (`blk_floor.c`'s module exports: EXPORT_SYMBOL's section is C's); an image
  written by the C build mounts and checks clean under the CeeSafe build,
  and the reverse
  (until 2.0's format break, if SA7 takes it).

Progress (the block layer first, each step on a host test holding the
port to the C's digest, as SA6's were). `part.cee`: partitions as block
devices -- the scan, the forwarding driver, removal waiting out the
forwarded bios, the lookups -- each partition a C object the file
defines, its blkdev first. `tests/host/test_part.c` holds it to the
C's digest (0x003796d5fcf97df9, clang and gcc); the GPT type GUIDs are
copied from `gpt.cee`, a C array not being importable by the pinned
ceesafec, and `gpt.h` and `part.h` take their GUIDs as pointers, as
the generated declarations do (gcc's -Warray-parameter). `ramblk.cee`:
the debug builds' RAM disk and write recorder, held to the C's digest by
`tests/host/test_ramblk.c` (0x3e265dd7cda910d8); the C step fixed two
bugs the test found -- a NULL `priv` released through when a creation
failed part way, and deferred bios never completed after a stalled
worker was stopped. `blk.c`, the C step: `tests/host/test_blk.c` drives
the registry, the checks, the pending queue, the timeout scan, resets,
unregistration, the flush sequences and the synchronous helpers against
drivers the test owns (0xf136565ff61bcea3, clang and gcc; every line but
the parked submitter's spin, which needs a second CPU). It found a NULL
dereference: a BIO_PREFLUSH write whose pre-flush failed completed the
caller's bio with its in-flight link never initialised (a zeroed bio from
`blk_write_flags` crashed); the sequence now initialises it. `gone` and
`recovering` became `uint32_t`, so the port reaches them as atomics.
`blk.cee` gives that digest on both compilers (ceesafec 0.18.0, pinned
for it). `struct blkdev` and `struct bio` stay C, every driver filling
them; the synchronous helpers keep their bio and completion in a zeroed
stack slot of words, checked against their C size, since a CeeSafe local
cannot hold C state; `blk_floor.c` keeps the module exports.
`blktest.cee` and `parttest.cee`: the block layer's five self-tests
(blk-queue, blk-segments, blk-timeout, blk-timeout-skew, blk-gpt), each
exported in every build and passing at once outside the debug build, as
SA6's skips do; their lines in the boot log are the C's (timings aside).
Their bios and tables live in kzalloc'd memory where the C kept them on
the stack, and `struct blk_part_info`, named like the function that fills
it, is read as bytes at offsets `blk_floor.c` asserts. blk-queue's count
is read before the RAM disk goes; the C read it after `ramblk_destroy`.
`kernel/block/` has no C left but `blk_floor.c`.

`pool.cee`, the pool next: `tests/host/test_pool.c` drives members,
copies and their states, the missed-region bitmaps, resilvers, spares and
the I/O against devices of every geometry that fail some of the time,
with allocations failing on request (0xb210836b3a1c3655, clang and gcc;
every line but the clamp `region_set` notes cannot happen and the restore
after an `attach` that `fits` already passed). The C step fixed a leak it
found: `pool_set_copies` growing a member failed part way and kept the
bitmaps of the slots it gave back, which nothing past `ncopies` frees.
`struct spool` stays C, cosmofs reading and filling it; a copy is swapped
through a stack slot of words, checked against its C size, and the
detach's line names the device before the pool's reference on it goes
(the C read the name after). `pool_floor.c` keeps the sysctl's
`g_pool_fault_errors` and the spare slots, an array of pointers the
pinned ceesafec does not import. `kernel-services/storage/` has no C left
but `pool_floor.c`.

cosmofs next, its files one at a time, leaves first, each on a host test
where it can be held alone. `cosmofs_reach.cee`, the tree walk clones,
rollbacks and the snapshot accounting use: `tests/host/test_cfsreach.c`
builds trees in a block store it owns -- inode maps, slots in place and
out of it, direct, compressed and chained extents, chains that loop,
checksum trees of one level and two, blocks unreadable, of the wrong kind
or outside the pool -- and fakes the buffers and the address map
(0x18b576f00e985902, clang and gcc; every line and branch). The port reads
each block as a slice of CFS_BLOCK bytes at offsets the test asserts, so
no read can leave it; its reasons for a bad block are an enum.
`cosmofs_clone.cee`, the writable clones: `tests/host/test_cfsclone.c`
fakes the snapshot list, the trees (what a root reaches follows from the
root, and some cannot be walked) and the transaction, and drives
creation, loading, entering and leaving with roots that move, frees
inside and outside a clone, publishing, readiness and deletion
(0xf455c79b8ec77804, clang and gcc; every line). A snapshot entry the C
kept on the stack is a zeroed slot of words checked against its C size;
the walks' marks and the list's write walk are `@callback` functions.
`cosmofs_rollback.cee`, the rollback: `tests/host/test_cfsrollback.c`
keeps the record's own checks (cfsparse's) and fakes the rest of cosmofs
-- requests under every state of the record, cancels back to a hold,
the checker's drop, BOOT_OK, the protection a record gives, and the
switch at mount with every refusal and failure it has
(0x06458013d8846511, clang and gcc; every line). Its record, entries and
inodes are stack slots of words checked against their C sizes;
`cosmofs_rollback_floor.c` makes the one call whose prototype CeeSafe
cannot write (`cfs_snapshot_new_uuid`'s pointer to arrays), and
`cfs_snapshot_lookup_uuid` takes its uuid as a pointer, as the generated
declaration does. `tools/b1-rollback-probe.py` patches the port.
`cosmofs_scrub.cee`, the scrub: `tests/host/test_cfsscrub.c` builds the
filesystems it walks -- copies that do and do not verify, superblock
slots, one or two members' allocation maps, snapshots sharing the live
tree's blocks, deadlists, extent chains, checksum trees of one and two
levels, compressed extents, blocks the open transaction holds -- and
runs it paced and unpaced, repairing and not, while a writer commits in
every pause (0x036f8de81cde37d2, clang and gcc; every line, region and
branch). The pacing state, the set of blocks seen, the entries, inodes
and extents are stack slots of words checked against their C sizes;
`cosmofs_scrub_floor.c` keeps `g_cosmofs_scrub_rate`, the sysctl's
variable, which CeeSafe cannot export.
`cosmofs_crypt.cee`, the keys: `tests/host/test_cfscrypt.c` runs the
real ChaCha20-Poly1305, SHA-512 and BLAKE2b under a pool, buffers, a
transaction and a random source it owns -- file keys and nonces, the
seeding wait, version-1 key blocks written, unwrapped, loaded and
rotated, the MAC key, version-2 key blocks, the key table to full, and
every /dev/fsctl key operation on a device and a mount
(0x98e1087b49e6224c, clang and gcc; every line, every branch but one
that cannot be taken). The key table is static arrays under its
spinlock; the hash states, the superblock and the MAC key are stack
slots of words checked against their C sizes; `cosmofs_crypt_floor.c`
makes the one call CeeSafe cannot declare (`cfs_mac_ok`'s `char`
domain). The cipher, hash and key prototypes take pointers, as the
generated declarations do.
`cosmofs_resilver.cee`, the resilver, its thread and the pool commands:
`tests/host/test_cfsresilver.c` builds `struct spool` itself and models
the pool's operations, the labels, the holds and the devices -- copies
in every state rebuilt whole and by region, an ONLINE copy's holes,
sources that disagree, spares taking over or refusing, copies failing
while the lock is let go, both finishes, replaces that detach, and every
pool command with malformed names and every refusal; the thread runs a
few windows at a time (0x2524b2fde370ae3b, clang and gcc; all but the
branches that cannot be reached). `rs_stop` is a word, as `AtomicU32`
is; `cosmofs_resilver_floor.c` keeps `g_cosmofs_resilver_rate` and makes
the `cfs_pace` call (its name is the pacing state's too); the debug
seams are `@cfg("CONFIG_DEBUG")` exports.
`cosmofs_member.cee`, the members: `tests/host/test_cfsmember.c` runs
the real label and member-table checks (cfsparse, CRC32C) over devices,
superblocks, a pool, buffers and an allocator it owns -- labels written,
cleared and read back sealed by CRC or MAC, member tables loaded valid
and every way malformed, copies set up from their records, lagging,
tagged and newer devices, spares, the copy-state records prepared and
filled, tables stored, labels stamped, holds past the limit, the linear
addresses, read repair and verify-all (0xf295f4d39f4bf4fd, clang and
gcc; every line and branch that can be reached). The labels, superblock
slots and table entries the C kept on the stack are slots of words
checked against their C sizes; the buffer cache's list is walked with
`List`; `cosmofs_member_floor.c` makes the two calls with a `char` domain
(`cfs_mac_seal`, `cfs_mac_ok`) and keeps the record's asserts.
`cfs_newer_member0` takes its uuid as a pointer.
`tools/cfs31-probe.py` and `tools/b3-copystate-probe.py` patch the port;
their `--old` texts compile.
`cosmofs_check.cee`, the structural check: `tests/host/test_cfscheck.c`
runs it with the real entry decoders (cfsparse) over filesystems it
builds and owns -- inode maps of one and two levels, runs in order, out
of order, overlapping and compressed, extent chains, checksum trees of
one and two levels, directories written with the format's encoder
(long names, repeats, colliding hashes, malformed and dangling entries,
two parents, too deep), snapshots with member tables and deadlists,
rollback records and holds, the free and orphan records, the pending
blocks and a bitmap that does and does not agree -- on a mount (plain,
repairing, offline, at boot) and on an unmounted device, the page
allocator and kmalloc failing at every point (0xe84d1de85355a449, clang
and gcc; every line and branch that can be reached). The bit maps, the
inodes, snapshot entries and rollback records the C kept on the stack
are slots of words checked against their C sizes; the link counts are a
CeeSafe pair of page pointers; `cosmofs_check_floor.c` takes a page from
the page allocator and gives it back (`kernel/page.h`'s conversions are
inline). `tools/b3-mirror-probe.py` patches the port, and
`tools/cosmofs-extent-fail-probe.py` takes the check's two lines out of
it by their text; both `--old` texts compile.
`cosmofs_snap.cee`, the snapshots: `tests/host/test_cfssnap.c` runs them
with the real list decoders (cfsparse) over filesystems it builds and
owns -- lists in both layouts, one block or several, in a loop, full or
unreadable; entries with and without identity, clones among them;
member tables and indexes whose bitmaps say what each snapshot holds;
deadlists empty, short, one short of full, full, in a loop or outside
the pool -- running every exported operation in random sequences while
commits, copies, allocations, reads and kmalloc fail, then directed
rounds that make the fill, a deletion and the identity repair fail at
each step (0xcdbb968635a6b0f6, clang and gcc; every line and branch that
can be reached). Entries the C kept on the stack are slots of words
checked against the C size; the list walk's callbacks are `@callback`
functions; `cosmofs_snap_floor.c` reads `test_no_raise`, a debug build's
field. `cfs_snapshot_new_uuid` and `cfs_snapshot_find_uuid` take plain
pointers (gcc's -Warray-parameter). Found on the way: the C passed
`memcpy` a NULL destination for an empty deadlist block (fixed in the C
step), and `deadlist_settle` returns -ENOMEM for one, since `kmalloc(0)`
is NULL (kept; only a damaged pool has such a block).
`tools/release-describe-probe.py` puts the message's old words back in
the port by their text.
`cosmofs_core.c` is taken apart a section at a time, each moved into a
file of its own unchanged, then held to a digest and ported.
`cosmofs_buf.cee`, the first, the header and superblock seals and the
buffer cache: `tests/host/test_cfsbuf.c` runs them with the real
BLAKE2b, CRC32C and decoders over blocks sealed by CRC and by MAC,
damaged in each field a header has, read through a repair with a good
copy or none; superblocks of every version with and without their key;
and a cache filled past its size, with buffers held, dirtied, cleaned,
copied on write and set aside (0x42be94733b86cd16, clang and gcc; every
line and branch that can be reached). `cosmofs_buf_floor.c` keeps the
metadata MAC, whose domain is a C `char`; the set-aside's array of
buffer pointers is reached by its offset, which an imported struct's
field cannot be.
`cosmofs_alloc.cee`, the second, allocation and the inode map:
`tests/host/test_cfsalloc.c` runs it with the real address and slot
checks over pools of one to three members, some spanning several bitmap
chunks, partly allocated, with a reserve and now and then a free count
the bitmap does not bear out; runs hinted and not, metadata and data,
until full; blocks freed deferred and exempt, inside a clone and not,
while krealloc fails; orphans; and an inode map with holes, read, written
tagged and untagged, prepared, allocated and discarded
(0x583d1da8b67f7b98, clang and gcc; every line and branch).
`cfs_note_dirty` stayed in `cosmofs_core.c`: it starts the writeback
thread.
`cosmofs_commit.cee`, the third, the commit and its records:
`tests/host/test_cfscommit.c` runs it with the real buffer cache,
allocator and decoders over a disk it keeps -- formats 8 to 15, a
mount's replay of the free and orphan records (sound, damaged, in a loop,
too long), transactions that allocate, free, write inodes and orphan
hundreds of them, on pools nearly full, with snapshots holding blocks,
copies changing state at every flush and the hooks that stop a commit
(0xddff362db23559a0, clang and gcc; every line and branch that can be
reached -- the orphan replay's "already gone" inode cannot, since
`cfs_inode_read_raw` refuses an emptied slot first).
`cosmofs_commit_floor.c` keeps the two test seams a build configuration
decides: the debug build's stop-at-this-commit and `COSMOFS_STOP`'s
crash point. `cfs_sync_vnodes` stayed in `cosmofs_core.c` with the VFS
side of the mount.
`cosmofs_format.cee`, the fourth, making a pool and its test hooks:
`tests/host/test_cfsformat.c` runs it with the real key-slot check and
CRC32C -- one device or many, mirrored or not, every format version,
labels, pools in the clear, wrapped-key and with sealed slots, the hooks
that write an older format or poison a free record, while opens, writes,
flushes, kmalloc and the key and label writers fail
(0x4d8818e293f81719, clang and gcc; every line and branch that can be
reached). The user key the C passed through two statics is a parameter;
the struct cfs it kept on the stack is a slot of words, checked against
its C size. `cosmofs.h`'s two array parameters became pointers.
`cosmofs_corrupt.cee`, the fifth, the structural check's corruption
hook: `tests/host/test_cfscorrupt.c` runs it with the real DVA mapping
and entry and snapshot-list decoders over every kind, on filesystems of
every version with holes, long names, noise and snapshot lists that end,
loop or run into noise, while the inode, allocator, directory, buffer
and kmalloc calls fail (0x8e1237dd995b9aa1, clang and gcc; every line
and branch that can be reached). `cfs_dirent_free_run` and
`cfs_dirent_put`, inline in `cosmofs_format.h`, are written out in it.
`cosmofs_hooks.cee`, the sixth, a mount's figures, the key operations on
a mounted pool and the test hooks, moved out of `cosmofs_core.c`:
`tests/host/test_cfshooks.c` runs every hook on mounts with and without
a filesystem, of every version, mirrored or not, encrypted or not
(0x677b20b102df3691, clang and gcc; every line and branch).
`cosmofs_hooks_floor.c` keeps the three hooks whose fields only a debug
or self-test build's struct cfs has.
`cosmofs_core.cee`, the seventh, mount and unmount, the writeback thread
and the first-change mark -- what was left of `cosmofs_core.c`:
`tests/host/test_cfscore.c` mounts pools whose superblock slots take
every shape, behind a newer member 0 or not, in the clear and encrypted
at versions 1 and 14, over several members, with rollback switches that
fail and roots that fall back or are not directories, then commits, runs
the writeback thread, writes pages back and unmounts, while every stage
and kmalloc fail (0x25fe84ab2d7cdbbf, clang and gcc; every line and
branch that can be reached -- not a committer before the mount is live,
which is the invariant, nor a bitmap chunk past its member's span).
`cosmofs_core_floor.c` keeps the fs_type table and `cosmofs_init`, the
walk of the vnode hash under its spinlock, and the lock assertion only a
lockdep build makes. `tools/p3-residue-probe.py`'s freecount defect now
moves the comparison in the `.cee`; `tools/analyzer-fix-probe.py`'s old
mode for its cosmofs cases fails to compile its baseline against today's
headers, as it did on `origin/main` before this (to be mended with the
extent-fail probe once `cosmofs.c` is ported).
`cosmofs_snapctl.cee`, the eighth and the first of `cosmofs.c`'s
sections, snapshots through /dev/fsctl: `tests/host/test_cfssnapctl.c`
runs every operation with good and bad names, listings of none to
hundreds of snapshots, paged, sized or not, with a rollback record
pending, held or absent (0x82439c542373f871, clang and gcc; every line
and branch that can be reached). The two helpers it shares with the
.snapshots directory's rmdir are `cfs_snap_by_name` and
`cfs_snap_in_use`; the once-per-root mark says which count it feeds,
since a field holding a function cannot be called.
`cosmofs_csum.cee`, the ninth, a file's checksums: the one- and
two-level index, CRC and authenticated entries, verified reads and their
repair, compressed records, the scrub's block check and the raises to 14
and 15. `tests/host/test_cfscsum.c` runs it with the real CRC32C over
files of every checksum kind on pools of versions 9 to 15, entries near
the start and past each index, good, rotten and unreadable copies, tags
that open and do not (0x807c60c6070274a7, clang and gcc; every line and
branch that can be reached). `cosmofs_csum_floor.c` keeps the fault
seam, an inline function where fault injection is off.
`cosmofs_data.cee`, the tenth, a file's blocks: its runs and chain,
lookups, runs and records set over what was there, truncation, and the
copy-on-write writes of blocks and records, moved out of `cosmofs.c`
with them. `tests/host/test_cfsdata.c` runs it with the real run checks
and CRC32C over maps the test builds -- chains of this generation or an
older one, compressed records, thousands of runs up to and past the
bound, cycles, wild pointers, runs out of order -- with blocks written
in the clear, CRC'd and sealed, compressed or not (0xd968a547580d35fd,
clang and gcc; every line and branch that can be reached).
`cosmofs_data_floor.c` keeps the fault seams, the debug build's note,
the run bound a self-test lowers, and `cfs_set_extent`, whose run comes
by value.
`cosmofs_file.cee`, the eleventh, a regular file's vnode operations:
pages read and written over holes, blocks and records, record-sized
write-backs, the run bound and the pool's space charged at the write,
truncation, setattr, sync and eviction. `tests/host/test_cfsfile.c` runs
them on files of the live tree, a snapshot's and a clone's, mounted or
not, failed or not (0xde01af8468dcf4cc, clang and gcc; every line and
branch that can be reached). `cosmofs_file_floor.c` keeps the debug
build's stuck-page check, the run bound a self-test lowers and the
self-test hook that counts runs.
`cosmofs_vnode.cee`, the twelfth, vnodes made for the live tree, a
clone's or a snapshot's and their inodes written through:
`tests/host/test_cfsvnode.c` (0x9c4898b008ad2569, clang and gcc; every
line and branch that can be reached). The vnode operation tables are
named in `cosmofs_internal.h` now; `cosmofs_vnode_floor.c` hands out
their addresses and keeps the inode's fault seam.
`cosmofs.cee`, the thirteenth, what the directory operations share: the
directory block helpers, the .snapshots directory, snapshots by name and
the tree locks, after the operations themselves moved to
`cosmofs_dir.c`. `tests/host/test_cfsdirblk.c` runs them with the real
entry decoders (0x79a7a841980074bb, clang and gcc; every line and branch
that can be reached). `cosmofs_floor.c` keeps the vnode operation
tables.
`cosmofs_dir.cee`, the fourteenth and the last of `cosmofs.c`, a
directory's operations: lookups through `.snapshots`, clones and
snapshots, creation given back halfway, symlinks, hard links, unlink,
rmdir (a snapshot's too), rename and its rollback, readdir.
`tests/host/test_cfsdir.c` runs them with the real directory helpers and
entry decoders (0x00fc036656d8ef7b, clang and gcc; every line but a
link's rollback failing twice over). `cosmofs_dir_floor.c` kept
`cfs_readdir`'s entry point and the call through its callback, whose
type takes an enum, until ceesafec 0.23.0 imported the enum (below); it
is gone. No C of cosmofs's VFS side is left but the floors.
`cosmofs_keytest.cee`, the first of the self-tests, cosmofs-encrypted-root:
a self-test has no host digest, so its evidence is the boot test, which
passes with every line the C printed (x86_64 debug, 2026-11-07).
`cosmofs_keytest_floor.c` keeps its fault-injection and debug-only parts
and asserts the layout the CeeSafe half writes as constants. A check's
reason keeps the C's form, "check failed: COND at line N", N its line in
the .cee.
`cosmofscrash.cee`, the crash-consistency harness cosmofs-replay: in the
boot test it replays 410 prefix images of 227 writes over 9 sync points,
none stranding a block or putting one on two deadlists (x86_64 debug,
2026-11-07). `cosmofscrash_floor.c` lists a directory through its
vnode's readdir, a function an operations table names, and is the
self-test in a build without CONFIG_DEBUG.
`cosmofstest_lib.cee`, the first of `cosmofstest.c`'s helpers, the ones
its test groups share: files written and read back, the mount at
/mnt/eng, incompressible pages, rotted and flipped blocks, the
structural check's fixture and its failure message, a snapshot request.
The tests still in C reach them through `cosmofstest.h`;
`cosmofstest_lib_floor.c` keeps the engine mount's fault seams and
their self-test, cosmofs-fixture-cleanup, which only a build with
CONFIG_SELFTEST and CONFIG_FAULTINJECT has.
Then the rest of `cosmofstest.c`, by group, after which it is gone:
`cosmofstest_basic.cee` (the scratch disk's pool, format, ops and crash,
and the transaction engine's holes to metadata-csum-id),
`cosmofstest_media.cee` (badmap, snapshot, mirror, mirror-stale,
compress), `cosmofstest_extent.cee` (extent-bound, -rebuild, -raw,
-fail), `cosmofstest_check.cee` (crypt, badmembers, the structural
check's manufactured faults), `cosmofstest_orphan.cee` (the orphan
record's replays, check-partial, fsctl-check), `cosmofstest_freelog.cee`
(symlinks, the free record, snapshot deadlists), `cosmofstest_names.cee`
(old formats, two members, snapshot identity, long names, hard links),
`cosmofstest_rollback.cee` (rollbacks and clones), `cosmofstest_b2.cee`
(roadmap 1.2 B2's defects, cosmofs-nomem) and `cosmofstest_copies.cee`
(copy states, resilvers, spares, attach). A C test that jumped to its
cleanup is a body that returns at its first failed check and a caller
that cleans up after it. Their floors assert the layout each writes as
constants, call `cosmofs_stats` (an extern block cannot name a function
and a struct alike), hold the variables and fault seams a build
configuration decides, and are the debug-only tests in a release build.
The boot test passes every one (x86_64 debug, 2026-11-07); the
analysis baselines lose the two `cosmofstest.c` entries whose functions
moved.

`procfs.cee`, /proc, the last of `kernel-services/filesystem/`'s C:
`tests/host/test_procfs.c` walks the mount over random process tables
-- processes visible and not, running, stopped and exited, with
threads, terminals, limits, privileges, command lines, environments,
roots, namespaces and handles of every kind -- every directory listed
and resumed, every name looked up, every file read and every link read
into buffers of every size, while allocations and the vnode cache fail
(0x750297368390729d, clang and gcc; every line). The C step fixed what
it found: `parse_pid`'s "not a pid", -1 in an unsigned `pid_t`, was
looked up as pid 4294967295; and `render_loadavg` called two counters
as arguments of one call, in an order the compiler chose. The port
writes each text as `ksnprintf` did, a piece at a time from where the
last ended, cut at the buffer's end and counted whole, so every text
and length is the C's. `procfs_floor.c` keeps the operation tables and
the file system type, the compiler's name
for /proc/version and the debug build's count of live texts, the
threads' `container_of`, `sched_loadavg`'s unsigned longs, and
`procnet_names`, a C array. `kernel-services/filesystem/` has no C left
but floors.

`memdev.cee`, `/dev/null` and `/dev/zero`, the first of
`kernel-services/vfs/`: `tests/host/test_memdev.c` runs `memdev_init`
while `ramfs_mkchr` fails now and then, and reads and writes both
devices at random, every byte of the buffer and its guards in the
digest (0xb166a187062d2cf0, clang and gcc). `memdev_floor.c` keeps the
two operation tables and the warning's format.

`mountns.cee`, mount namespaces: `tests/host/test_mountns.c` runs
random histories -- mounts seen by some namespaces, namespaces copied
from one another while the allocator fails part-way through the copy,
references dropped until a namespace dies and takes the mounts only it
could see with it, newest first -- with every lock, detach, orphaning
and list in the digest, the lock order checked and every reference
freed (0x20d2c61ff98c5c64, clang and gcc; every line).
`mountns_floor.c` keeps the initial namespace, a static whose list head
points into itself, and hands over vfs.c's mount table, its lock and
the root mount, C globals CeeSafe cannot name.

`blkctl.cee`, `/dev/blkctl`: `tests/host/test_blkctl.c` runs random
sessions -- opens with and without the privilege, commands of every op
well formed and not, devices mounted, partitioned, read-only or gone,
their operations failing now and then -- with every result read back
and the keyed command's copy checked wiped as it is freed
(0xa8eae3202115e31f, clang and gcc; every line). The C step fixed what
it found: a command refused before it reached a device (no privilege,
too short, wrong version, data or a label refused) left the previous
command's result to be read as its own, though `blkctl.h` says a failed
command leaves none. `blkctl_floor.c` keeps a file's state, a struct
round a `struct mutex` whose size the build's lock debugging decides,
and the operation table. The generated C redeclares
`bootinfo_boot_partuuid` and `vfs_format_keyed`, whose headers write
array parameters, with pointers: clang accepts that, and gcc's
`-Warray-parameter` warns.

`pagecache.cee`, the page cache: `tests/host/test_pagecache.c` runs
random sessions on six files of three mounts, one whose cache is its
store under a page budget -- reads, writes, syncs whose writepage and
writepages fail or report nonsense, dirty checks that refuse,
truncations, drops, faults through mappings that execute and hold their
frames, reclaim under a changing limit with busy locks and references
that cannot be had, and the dirty bound's throttle with a write-back
thread running while the writer sleeps -- over a frame arena smaller
than the files (0x1c24149eb69a9ee7, clang and gcc; every line, the
branches left being other threads' races). The statistics are an
`IrqSpinLock` and the limits and counts atomics, in CeeSafe;
`pagecache_floor.c` keeps the LRU, a static whose list head points into
itself, and its lock, the dirty bound's waitqueue and lockdep class,
whose calls name their caller's return address, the injected readpage
failure and the lockdep assertion, which some builds make inline, and a
frame's direct-map address. The hash buckets, an array of pointers no
imported field may be, are reached at the cache's start, where the
floor asserts they are.

`ramfs.cee`, the in-memory filesystem: `tests/host/test_ramfs.c` runs
random histories of mounts -- every kind of node made, looked up,
linked, unlinked, renamed over one another and across directories and
listed a few entries at a time; devices made by path and by directory
with tables of every shape and opened with and without the privilege;
anonymous files; the boot archive copied in; every tree released by
unmount -- while the vnode and heap allocators fail now and then
(0x34bf9feeddcfa426, clang and gcc; every line but a link with no
target, which no symlink makes). A directory entry and a node's private
state are CeeSafe structs, laid out by C's rules: the digest records
every allocation's size, and the port's are the C's. `ramfs_floor.c`
keeps the six operation tables and `ramfs_fs_type`, C data naming the
operations, and `bootarchive_entry`, a function CeeSafe cannot name
beside its struct.

`fsctl.cee`, `/dev/fsctl` and /proc/mounts: `tests/host/test_fsctl.c`
runs random sessions -- opens with and without the privilege, commands
of every op well formed and not, against filesystems with every pass
and with none, mounts that cannot be acquired, devices mounted or not,
passes that fail or overrun their room -- with every result read back,
the key command's copy checked wiped, and /proc/mounts rendered into
buffers of every size (0x9be41a29127374e0, clang and gcc; every line).
The pass reports and requests the C kept on its stack stay there, in
slots of u64s whose sizes the floor asserts; the op tables become
subtractions whose order the floor asserts. `fsctl_floor.c` keeps a
file's state round a `struct mutex`, the operation table and the
spinlock /proc/mounts is rendered under; the mount table's lock and the
root mount come from `mountns_floor.c`.

`vfstest_*.cee`, the VFS's 35 self-tests, moved group by group out of
`vfstest.c` (now gone) as the cosmofs ones were, each proven by both
boot tests rather than a digest: `vfstest_base.cee` (crc32c, the page
cache, symlinks, ramfs, fsync, namespaces, concurrency),
`vfstest_mount.cee` (budgets, limits, the last put, mount ids and
pins), `vfstest_dev.cee` (/dev/fsctl, character devices, the bounce),
`vfstest_wb.cee` (write-back errors, the dirty bound, the benches,
pinned frames), `vfstest_walk.cee` (renames, named lookups, unmounts
from a start) and `vfstest_fill.cee` (filled reads, dirty-bounded
writers). Every check fails with its condition and its line in the
`.cee`. `vfs-put-race` now stops its racers before it returns on a
failed check, where the C left them spinning on its frame.
`vfstest_floor.c` keeps the test devices' tables and completions, the
fault-injection seams and the release builds' stand-ins for the tests
that need injection or the AIO test body, and asserts the constants and
stack-slot sizes.

`vfs_*.cee`, the VFS core, ported out of `vfs.c` (now gone) in five
parts against one host test: `tests/host/test_vfs.c` runs twelve epochs
of random histories on a filesystem of the test's own whose every
operation fails now and then, with the real mount namespaces, plus
scripted cases for what the histories reach too rarely
(0x4170262f9ea8d3cf, clang and gcc; every line but five places no
single thread reaches). `vfs_vnode.cee` (vnodes, the held-walk seam),
`vfs_mount.cee` (registry, mounts, the root switch, sync, device claims,
init, formats, passes), `vfs_walk.cee` (permissions, the walk, lookups,
an open's walk), `vfs_file.cee` (open files) and `vfs_dir.cee` (the
namespace operations). `vfs_floor.c` keeps the mount table and its
locks, the registry's and claims' lists, the kobject types (the file's
io type among them), the seam's lock, queues and record, the waits the
C wrote as macros, a debug build's poisoning of a freed vnode, the release builds'
stand-ins for the seam, and the asserted constants. The test found two
kernel bugs, fixed after the port (the digest moved once, to
0x45371c2423e2e476): `vfs_umount_at` locked the mountpoint of an old
root that has none (now -EINVAL), and the last holder of a mount
namespace unmounted a mount only it saw with files open on it or
mounts nested on it (now the mount waits as an orphan, reaped when its
last user goes).

The C enum: `vfs_dirent_cb` and `vnode_ops.mknod` take an `enum
vnode_type`, which a CeeSafe function type could not spell, so every
readdir and mknod went through a C trampoline -- CI's UBSan
(`-fsanitize=function`) refuses a call through a pointer whose type
names the enum to a function whose type names `uint32_t`. ceesafec
0.23.0 imports C enums (its ADR 0079: `enum vnode_type: u32;`), so the
readdirs of ramfs, procfs and cosmofs are their tables' entries and call
the reader's callback directly, `ramfs_mknod` is its table's entry, and
`vfs_mknod` and `file_readdir` call through the operation table; the
trampolines in `ramfs_floor.c`, `procfs_floor.c` and `vfs_floor.c`, and
`cosmofs_dir_floor.c` whole, are gone (8bbd5bd2).

The floors (2026-11-09): ceesafec 0.24.0's ADR 0080 gives CeeSafe what
they held, and every SA7 floor is gone but `blk_floor.c`, which keeps only
the module exports (EXPORT_SYMBOL's section is C's). A floor's asserts
are its `.cee`'s extern constants, which the C compiler checks, or
`@cvalue` ones for a macro the `.cee` names differently and for a size;
its operation tables, list heads, locks and wait queues are `.cee`
statics, exported where a C header names them (`g_mounts`,
`vfs_orphans`); its `#if CONFIG_*` code and the release builds' stand-ins
are `if (cfg("X"))` blocks, which are C's `#if`. Waits on a condition go
through `wait_floor_until*` (`kernel/scheduler/wait_floor.c`, which
gained the killable timed one). Every digest is unchanged, and the debug
and release kernels, the fuzz build, the host tests, the analysis and both
boots pass at each step. The port found four ceesafec bugs, fixed in
0.24.2 (an inverted `cfg` test, a pointer constant's cast, a `cfg` arm moved
out of its `#if`, a callback only a `cfg` block names), which CI pins; its
formatter is the one the sources are formatted with.

Cross-image (2026-11-09): published releases' cosmofs is C (v1.0.13 has no
`.cee` in it). `test-large` checks both directions. Its write stage runs
this build's `fsctl check` on the root v1.0.13 installed and upgraded, and
the check finds it clean. Its old stage boots v1.0.13, which mounts the two
pools this build wrote, reads their files back by SHA-256 and checks both
clean. `test-upgrade` and `test-be` boot this build on roots v1.0.3 and
v1.0.7 wrote. Local run, x86_64 release with ceesafec 0.24.2: `test-large`
PASS in 3732 s (f418495d).

Size: about 4.5x.

### SA8 The Linux personality and the system-call layer

`compat/` (7,426 lines) and `kernel/syscall/` (3,413): both system-call
tables, argument decoding (SA5 did the parsers), the Linux signal frames
and the personality's state.

- [ ] **Acceptance:** `test-busybox`, the dynamic and PIE tests, and the
  Linux self-tests pass on both architectures with no C left in
  `compat/` or `kernel/syscall/`.

Progress (2026-11-10, the smallest files first). `kernel/syscall/`:
`personality.cee`, `uaccess.cee` (`might_sleep` with the caller's
address through `callerAddress()`), `uaccesstest.cee` (which guard is
absent from `user.h`'s `ARCH_USER_GUARD_IS_SMAP()`: `cfg` tests a
macro's value, and `ARCH_*` are only defined or not) and `syscall.cee`
(the call's `struct syscall_args` a CeeSafe struct of the same layout,
checked by the C compiler, so the dispatch allocates nothing).
`compat/linux/`: `convert.c` joined `lxconv.cee` -- `struct stat`'s two
fields whose widths differ between the architectures are written in
`cfg` blocks on `linux_abi.h`'s `LX_ABI_IS_X86_64`, and
`tests/host/test_linux.c` gained the encoders' digest from the C on
both layouts (`0x7159817551c8dcff` AArch64, `0x15ff4a3eb99bf16f`
x86-64) -- and `netioctl.cee` (`struct lx_ifreq` as its 40 bytes at
checked offsets). Each step passed the builds, the analysis and both
boot tests; the dispatcher and the encoders `test-busybox` too, the
ioctls `test-net-login`, the self-tests `test-guard`. Left: `signal.c`,
then `native.c` and `syscalls.c` in sections.

Size: about 1.5x.

### SA9 Every driver

`drivers/` (15,328 lines): PCI, ACPI, the IOMMUs (VT-d, SMMUv3), NVMe,
AHCI, e1000e, USB and the virtio drivers, each a CeeSafe module (SA4). HW2's
new drivers are written in CeeSafe from the start, in the same stream.

- [ ] **Acceptance:** every driver module is built from CeeSafe and passes
  `test-modcycle`, its device tests and the platform boots (`q35`, `virt`,
  Parallels); no C left in `drivers/`.

Size: about 2.0x.

### SA10 The core

The rest of `kernel/` above the floor (about 60,000 lines): memory
(`pmm`, buddy, slab, `kmalloc`, `vmm`), the scheduler, processes,
threads and signals, timers, interrupts, I/O and `aio`, the tty and PTYs,
the device model, the module loader, security (`cred`, random), the log
and diagnostics, lockdep itself. Order: leaves first (timer, tty, log),
then memory, then the scheduler and processes, lockdep last.

- [ ] **Acceptance:** the whole validation matrix on both architectures
  with no C left in `kernel/` outside `kernel/arch/` and the floor;
  PF1's syscall and context-switch numbers within 5% of the C build's.

Size: about 6.0x.

### SA11 Architecture code and the hypervisor

`kernel/arch/` (17,637 lines of C) down to the floor: page tables, APIC
and GIC, timers, CPU bring-up, the FPU state, the SMMU and VT-d glue;
and `kernel-services/virtualization/` (5,231): SVM, VMX, AArch64 EL2, the
device backends' kernel side. The world switches stay assembly.

- [ ] **Acceptance:** the matrix and the hypervisor's guest tests on both
  architectures; `cee-share.py` reports only the floor in
  `kernel/arch/` and `kernel-services/virtualization/`.

Size: about 2.5x.

### SA12 CosmoOS's own userland

`libc/` (5,949 lines), `userland/` (26,611: the native tools, `init`,
`svc`, `syslogd`, `netcfg`, `cosmo-upgrade`, `cosmo-snap`,
`fsck.cosmofs`, `cosmo-pool`, `cosmo-key`) and `pkg/` (3,505), on CeeSafe's
hosted profile. libc keeps a C ABI for the ports that link it; its
implementation is CeeSafe, and it gains the `math.h` it lacks (in CeeSafe).

- [ ] **Acceptance:** `test-upgrade`, `test-install`, the `pkg` tests and
  the native-tool tests pass on both architectures with no C left in
  `libc/`, `userland/` or `pkg/` (the ports excepted).

Size: about 3.0x.

### SA13 The last C

Headers become CeeSafe interfaces, with C headers kept only for the floor
and for libc's public ABI; the floor is audited function by function (each
one's `/// Safety:` contract against its callers); `cee-share.py`'s
number for ring 0 is the floor and nothing else.

- [ ] **Acceptance:** `cee-share.py` on main reports zero lines of
  convertible C in ring 0 and in CosmoOS's own userland; a dated report
  lists the floor, its size, and each function's contract.

Size: about 1.0x.

---

## SW0 Self-hosting (1.2's direction A, folded in)

1.2's A2-A6 as they are written in [roadmap-1.2.md](roadmap-1.2.md#a-self-hosting-software-chosen-2026-10-27-after-b):
`make` and the build tools, a ports tree that builds on CosmoOS, nano and
curl, OpenSSH and git, Python. They run in the features stream from the
start of phase 1, not before it, and new CosmoOS-side code they need is
in CeeSafe.

- [ ] **Acceptance:** 1.2's A2-A6 acceptance tests, each green on main.

Size: about 4.0x.

---

## HW. Real machines

Roadmap 1.2 deferred these as C2, C4, C5 and C6; they move here.

### HW1 KASLR and Secure Boot

KASLR (1.2's C2, BOOT-1): a relocatable kernel, the slide chosen by the
loader from `EFI_RNG_PROTOCOL` or the CPU, symbolisation adjusted. The
loader signed for UEFI Secure Boot with a documented key enrolment
(BOOT-2). The relocation processing in the loader is written in CeeSafe (an
ELF relocation parser is untrusted input to the loader).

- [ ] **Acceptance:** `test-security` boots three times and requires three
  different kernel bases, with crash symbolisation still correct; a QEMU
  boot with OVMF's Secure Boot enrolled with the project key boots the
  release image, and refuses a loader with one byte changed.

Size: about 1.0x.

### HW2 Server drivers, in CeeSafe

Drivers for the hardware HW3's machines have, written as CeeSafe modules
(SA4): one Intel and one other common server NIC (for example `igc` or
`ixgbe` class, and a Broadcom or Mellanox ConnectX class, chosen with the
machines), AHCI and NVMe on real controllers (on SA9's CeeSafe drivers),
xHCI for a USB console and install media, and an AML interpreter for ACPI
power-off and the DSDT-described devices (DRV-4; a parser of firmware
input, fuzzed). On AArch64, device-tree discovery (DRV-5) if the chosen
machine needs it. All in CeeSafe, in the conversion stream beside SA9.

- [ ] **Acceptance:** each driver has a QEMU device test where QEMU emulates
  it (e1000e, NVMe, AHCI, xHCI) and runs on HW3's machines; the AML
  interpreter powers off QEMU's `q35` through `_S5` and has a fuzz target.

Size: about 2.5x.

### HW3 Hardware in the loop

1.2's C4, C5 and C6: one physical x86-64 server and one AArch64 server
(for example an Ampere Altra class machine), each booting the release
image from USB, installing to its disk, with a network; then a CI job
that runs on them.

- [ ] **Acceptance:** CI's `hil` job passes on main for both machines:
  install, boot, `test-net-login`'s stages, and PF1's benchmarks recorded.

Size: about 1.5x, plus the machines.

---

## PF. Performance and scale

### PF1 A benchmark set against Linux and FreeBSD

Nothing in the tree measures CosmoOS against the systems it competes
with. PF1 adds `make bench-compare`: the same VM (and later HW3's
machines) booting CosmoOS, a Linux distribution's kernel and FreeBSD, with
the same CPU count, memory and virtio devices, running:

- syscall and context-switch latency (an lmbench-style set: `getpid`,
  `read` of `/dev/zero`, pipe ping-pong, fork+exec+wait);
- network: TCP and UDP throughput and request rate between two guests on
  one host bridge (iperf3, and a request/response test);
- storage: `fio` random and sequential read/write on a virtio and an NVMe
  disk, through the file system and raw;
- a CPU-bound build: BusyBox built by the on-target compiler (A1);
- later, as the ports land: nginx static files, PostgreSQL `pgbench`,
  Redis `redis-benchmark`.

Methodology follows [Benchmark runs](development.md#benchmark-runs):
alternated boots, matching debug configurations, KVM and HVF where the
host has them (TCG numbers are not comparable across systems).

- [ ] **Acceptance:** `make bench-compare` runs on a KVM host and writes a
  dated report with each benchmark's numbers for all three systems and
  their spread over five alternated runs; CI runs a short version weekly.

Size: about 1.0x (iperf3 and fio are ports).

### PF2 Closing the measured gaps

Work chosen from PF1's numbers, not before them (NET-19's own rule: only
with measured benefit). The likely candidates, in the order PF1 is likely
to show them: zero-copy I/O (`sendfile`, `splice`, user-mapped buffers;
NET-19), multiqueue NVMe with interrupt moderation (multiqueue
virtio-net is MS4's), TSO and LRO, page-cache read-ahead, and the scheduler's wake-up path. New code in
CeeSafe; a hot path's CeeSafe port held to the rule's 5%.

- [ ] **Acceptance:** PF1's report shows CosmoOS within 15% of the better
  of Linux and FreeBSD on each benchmark, and no worse than 1.5x on any,
  on a KVM host.

Size: about 2.5x, depending on what PF1 shows.

### PF3 Scale: CPUs, NUMA, huge pages, swap

More than 64 CPUs (SMP-2: a multi-word `cpumask_t`, x2APIC, parallel AP
bring-up, IRQ spreading for every multiqueue device); NUMA topology and
node-aware allocation and placement (MEM-5); user huge pages, asked for
by the program (MEM-4; MS5 adds transparent promotion);
swap to a file or partition (MEM-6, without compression or deduplication
at first). The NUMA policy, the huge-page allocator's accounting and
the swap slot allocator are new code, in CeeSafe.

- [ ] **Acceptance:** QEMU boots with 256 vCPUs and two NUMA nodes and
  passes `test-smp2`-class tests at that size; a self-test shows memory
  allocated on the requesting CPU's node; a program's huge-page mapping is
  backed by 2 MiB pages (both architectures); a guest with 256 MiB runs a
  workload needing 512 MiB through swap without the OOM killer.

Size: about 2.5x.

---

## NT. Networking parity

### NT1 TCP and IPv6 complete

TCP window scaling, SACK, timestamps, ECN, fast recovery and sender Nagle
(NET-8); IPv6 beyond the link: routing, router discovery and neighbour
discovery against a real peer (NET-10); `setsockopt` and `getsockopt` for
the options server software sets (LX-8: `SO_REUSEADDR`, `SO_REUSEPORT`,
`SO_RCVBUF`, `SO_SNDBUF`, `SO_KEEPALIVE`, `TCP_NODELAY`, `IPV6_V6ONLY`).
The option parsers are written in CeeSafe as part of SA5's network family.

- [ ] **Acceptance:** a harness against a Linux peer checks each TCP option
  on the wire (window scale, SACK blocks after injected loss, timestamps);
  a guest reaches an IPv6 address beyond its link through a QEMU router;
  nginx (SW1) sets its sockets' options without a refusal.

Size: about 2.0x.

### NT2 Netlink and operator tools

`AF_NETLINK` route sockets (LX-7) so Linux tools (`ip` from iproute2,
BusyBox `ip`) work; a DHCPv6 client and a caching resolver (NET-18, part).
The netlink message parser is in CeeSafe.

- [ ] **Acceptance:** iproute2's `ip addr`, `ip route` and `ip link`
  report and change the guest's configuration; a guest gets an address
  by DHCPv6 from the harness.

Size: about 1.0x.

---

## LX. Linux compatibility

### LX1 glibc and the missing calls

The audit lists the common calls that reach `lx_unknown` today: `waitid`,
`sendfile`, `splice`, `copy_file_range`, `fallocate`, `truncate`,
`flock`, `preadv`/`pwritev`, `inotify_*`, POSIX timers,
`rt_sigtimedwait`, `close_range`, `faccessat2`, `openat2`, `pidfd_open`,
the `*xattr` family, `mlock`, `capget`/`capset`, `unshare`; and `rseq`
and `clone3` return `-ENOSYS`. LX1 implements them and runs glibc-linked
programs taken unchanged from a mainstream distribution's packages (for
example Debian's coreutils, findutils and grep). The new handlers are
written in CeeSafe (they decode user memory: SA5's second family).

- [ ] **Acceptance:** a chosen subset of the Linux Test Project's syscall
  tests (the list checked in, each exclusion named with its reason) passes
  on both architectures; Debian's `coreutils` binaries pass their own
  test suite's chosen subset.

Size: about 2.0x.

### LX2 `/sys`, `/proc` and devices as Linux programs see them

A Linux-compatible `/sys` for what programs read (CPUs, block devices,
network interfaces, `class/`, `devices/system/cpu`), the `/proc` files
server software reads (`/proc/self/*` complete, `/proc/sys/net`,
`/proc/pressure`), and a `udev`-compatible device event source (LX-11).

- [ ] **Acceptance:** `lscpu`, `lsblk` (util-linux) and `ss` report the
  machine correctly; PostgreSQL's and nginx's startup reads succeed.

Size: about 1.5x.

---

## SW. Server software

### SW1 Web and database servers

Ports, built on the target (A3's ports tree), each passing its own test
suite or its documented subset: nginx, PostgreSQL, Redis, and sqlite.
Their benchmarks join PF1's set.

- [ ] **Acceptance:** each port's suite (PostgreSQL's `make check`,
  Redis's `runtest` subset, nginx's test suite, sqlite's `testfixture`
  subset) passes on both architectures in CI.

Size: about 1.5x.

### SW2 Language toolchains

Go and Rust toolchains on the target (both need the Linux calls LX1 adds,
threads and `futex` at scale, and `mmap` behaviour the runtimes depend
on), and CeeSafe itself: `ceesafec` built and run on CosmoOS, so that CeeSafe code
can be developed on the system it runs on.

- [ ] **Acceptance:** Go's `go test std` subset and Rust's `cargo test` on
  a chosen crate set pass on the target; `ceesafec` built on CosmoOS
  regenerates every `.cee` port in the tree byte for byte
  (`make cee-check` on the target).

Size: about 2.0x.

---

## MS. Messaging at scale

The workload this track is measured by: a chat server in the style
WhatsApp ran on FreeBSD, an Erlang/BEAM program holding about two million
persistent TCP connections per machine. Each connection is a phone that is
mostly idle, waiting for a message. What it needs from the kernel is
cheap idle connections, lookups and timers that cost the same at a
million connections as at a thousand, event notification that scales with
the active connections rather than the registered ones, the network path
spread across CPUs, and large pages under a large heap.

**Built here, in CeeSafe; not imported.** FreeBSD's `netinet` is not taken
into the tree. It depends on FreeBSD's mbufs, UMA allocator, epoch
reclamation, callouts, taskqueues, `ifnet` and VNET, so porting it means
porting a large slice of FreeBSD's kernel as C, against the CeeSafe rule.
What made FreeBSD fit that workload is a short list of mechanisms (kqueue,
a 4-tuple connection table, per-CPU timer wheels, receive-side scaling,
superpages, large descriptor limits), and each is a milestone below. The
designs are BSD-licensed and documented; MS2-MS5's design notes name the
FreeBSD (and Linux) mechanism each follows and where it differs.

Today (measured on main at `4b1c55ec`):

- a process has at most 64 handles: `HANDLE_TABLE_SIZE` in
  `kernel/include/kernel/handle.h` sizes a fixed array;
- TCP connections sit in `TCP_HASH_SIZE` (256) buckets keyed by the local
  port alone (`kernel/include/kernel/net/tcp.h`), so every connection to a
  server's one port is in one bucket;
- the listen backlog is at most 16 (`TCP_MAX_BACKLOG`) and the window at
  most 65,535 bytes (no window scaling, which NT1 adds);
- each CPU's timer queue is a list kept sorted on insert
  (`kernel/timer/timer.c`), so arming a timer walks the CPU's other
  timers; every connection arms several (retransmission, delayed ACK,
  keepalive);
- virtio-net uses one queue pair; epoll exists, with callback wait
  entries and a ready list ([I/O design](kernel/io/design.md)); kqueue
  does not;
- user memory is mapped in 4 KiB pages only.

### MS1 Millions of handles

A handle table that grows: a two-level table (or radix tree) in place of
the fixed array, its memory charged to the process, with `RLIMIT_NOFILE`
settable up to 16 million and the Linux personality's limit following it.
Socket buffers are allocated when data arrives, not when the socket is
made, and returned when they drain, so an idle connection costs its pcb,
its socket and its handle. The table, its growth and its lookup are in
CeeSafe (SA2's allocation and object lifetimes; the socket-buffer part on
SA6's converted stack).

- [ ] **Acceptance:** one process holds 1,000,000 established TCP
  connections over loopback (clients bound to 16 local addresses); a
  self-test reports the kernel memory per idle connection, and the bar is
  under 4 KiB; closing them returns the memory to within 1% of the
  starting count.

Size: about 1.0x.

### MS2 A connection table that scales

Established connections in a hash table keyed by the 4-tuple, resized as
it fills, with lookups that take no lock (readers under SA3's
quiescence, as FreeBSD's `in_pcblookup` reads under its network epoch);
listeners in their own table, with `SO_REUSEPORT` groups so that each CPU
can accept on its own listener (NT1 parses the option; MS2 distributes
by it); the backlog and the SYN cache sized by the administrator
(`somaxconn`). In CeeSafe, on SA6's converted stack.

- [ ] **Acceptance:** a self-test measures the cost of a lookup with
  1,000 and with 1,000,000 connections in the table, and the second is
  within twice the first; on a KVM host, the accept rate of a
  `SO_REUSEPORT` server with 8 listeners on 8 CPUs is at least 6 times
  that of one listener.

Size: about 1.0x.

### MS3 kqueue

`kqueue` and `kevent` as native calls, with FreeBSD's semantics:
changes and events in one call, the `EVFILT_READ`, `EVFILT_WRITE`,
`EVFILT_TIMER`, `EVFILT_SIGNAL`, `EVFILT_PROC` and `EVFILT_USER`
filters, and `EV_CLEAR`, `EV_ONESHOT` and `EV_DISPATCH`. They are built on
the callback wait entries and ready lists epoll already uses, so that
their cost follows the active events, not the registered ones; CosmoOS's
libc exposes them with FreeBSD's `<sys/event.h>`. Linux programs keep
epoll. In CeeSafe.

- [ ] **Acceptance:** libkqueue's test suite, built against the native
  calls, passes on both architectures; a self-test registers 1,000,000
  sockets, makes 1,000 ready, and the cost of one `kevent` collecting them
  is within twice the cost with 1,000 registered.

Size: about 1.0x.

### MS4 The network path across CPUs

Multiqueue virtio-net (`VIRTIO_NET_F_MQ`) with receive-side scaling
(`VIRTIO_NET_F_RSS` where the device offers it, a Toeplitz hash in the
driver where it does not), one queue pair and one interrupt per CPU; a
connection's processing and its timers kept on the CPU its packets
arrive at; and the per-CPU timer queues replaced by hierarchical timer
wheels, as FreeBSD's callout wheel is per CPU, so that arming,
re-arming and cancelling a timer is constant time. pcbs come from a
per-CPU cache. In CeeSafe. This takes the multiqueue virtio-net part of
NET-19 from PF2.

- [ ] **Acceptance:** on a KVM host with 8 vCPUs and 8 queue pairs, PF1's
  TCP request/response rate is at least 6 times the 1-vCPU rate; lockdep's
  contention counts show no lock shared between CPUs on the per-packet
  receive path; a self-test arms 1,000,000 timers and the cost per arm is
  within twice the cost at 1,000.

Size: about 1.5x.

### MS5 Transparent superpages

Anonymous user memory promoted to 2 MiB pages without the program asking,
by reservation as FreeBSD does it: a fault in an aligned 2 MiB range of a
large mapping reserves the physical range, the range fills with 4 KiB
pages, and when it is full the mapping is promoted to one 2 MiB entry;
demotion on a partial `munmap`, `mprotect` or memory pressure. Both
architectures. Builds on PF3's huge-page allocator; the reservation logic
is in CeeSafe.

- [ ] **Acceptance:** a self-test touches a 1 GiB anonymous mapping and
  finds it mapped by 2 MiB entries on both architectures, then splits one
  by `mprotect` and checks the contents; BEAM's heap under MS6's load
  shows (in `/proc/self/smaps`' `AnonHugePages`) most of its size in
  2 MiB pages.

Size: about 1.0x.

### MS6 Erlang/OTP and the messaging server

Erlang/OTP ported (`ports/otp`, built on the target, SW0's toolchain)
with its kernel-poll backend on epoll under the Linux personality, its
own test suite's chosen subset passing; and a messaging server: ejabberd
(XMPP, Erlang), or, if ejabberd's dependencies cost more than they prove,
a small checked-in Erlang server with the same shape (log in, hold the
connection, deliver messages to online users, store them for offline
users on cosmofs with batched `fsync` through the native asynchronous I/O
path). A load generator runs on other guests with many source addresses.

- [ ] **Acceptance:** on a KVM host, one CosmoOS guest with 16 vCPUs and
  32 GiB holds 2,000,000 connections, each sent a message every 60 s
  (about 33,000 a second), for one hour, with no connection dropped and
  the 99th-percentile delivery latency under 50 ms; the same test runs in
  `make bench-compare` against Linux and FreeBSD in the same guest, and
  CosmoOS is within PF2's bar (15% of the better of the two) on
  connections held, memory per connection and latency.

Size: about 2.0x.

Not in this track: the "C10M" figure (ten million connections) is a
cluster's number, not a machine's; the track's bar is per machine, and
spreading users over machines is the application's job.

---

## SE. Security

### SE1 Measured boot and a verified root

Measured boot into the TPM (BOOT-3) and the encrypted root's key sealed to
the measurements (BOOT-9); the root file system verified as a tree, with
cosmofs's checksummed metadata rooted in a signed hash (BOOT-4); signature
versions against rollback (BOOT-5). The manifest, the tree-root signature
and the TPM event log parsers are in CeeSafe.

- [ ] **Acceptance:** with swtpm under QEMU, an unchanged system unlocks
  its root unattended and a changed kernel does not; changing one block of
  `/sbin` on the disk makes the boot refuse the root; an older signed
  manifest is refused.

Size: about 2.0x.

### SE2 Capabilities, sandboxing and users

Make handle rights the security model rather than one model beside
ambient privileges and uids: a capability mode in the style of FreeBSD's
Capsicum (a process that enters it reaches only what its handles allow),
a syscall filter in the style of seccomp for Linux programs, multiple
users with login, and audit logging of privileged operations (SEC-2).
Fix the `/etc` escalation (SEC-1). The filter's rule language and its
evaluator are in CeeSafe.

- [ ] **Acceptance:** a process in capability mode cannot open a path it
  holds no directory handle for, on both personalities; OpenSSH's
  privilege-separated child (A5) runs in it; a seccomp-filtered Linux
  program is killed on a denied call; a non-root user cannot write `/etc`.

Size: about 2.5x.

---

## CT. Containers

### CT1 Namespaces and resource control

PID, network and IPC namespaces beside the mount and UTS namespaces that
exist, and a cgroups-v2-compatible hierarchy for CPU, memory and I/O
limits (SEC-10). New code in CeeSafe.

- [ ] **Acceptance:** a process in new PID and network namespaces sees PID
  1 and only its own interfaces; a memory limit stops a process at its
  limit without affecting the host; a CPU weight shares a CPU as set.

Size: about 2.0x.

### CT2 An OCI runtime

An OCI runtime (crun, or a native one in CeeSafe) over CT1, SE2's filter and
cosmofs clones for image layers; then a container image pulled and run
(SEC-11).

- [ ] **Acceptance:** the OCI runtime's own test suite's chosen subset
  passes; an `alpine` image from a registry runs `apk add` and a shell.

Size: about 1.5x.

---

## OB. Observability

### OB1 Tracing and counters

Per-CPU structured tracing with static tracepoints across the scheduler,
memory, block, network and system calls; hardware performance counters;
`ktrace`, `kstat` and `cosmo-prof` over them (OBS-4); kernel-side
symbolisation and crash-state preservation (OBS-3); a documented GDB
remote workflow per architecture (OBS-2).

- [ ] **Acceptance:** `cosmo-prof` samples a running BusyBox build and
  names its hottest kernel functions; `ktrace` records a system call trace
  of a process; a crash's state survives a warm reboot and is read back.

Size: about 2.0x.

### OB2 Programmable probes, in CeeSafe

Linux's eBPF and FreeBSD's DTrace both run user-supplied code in the
kernel behind a verifier or an interpreter. CosmoOS can do this
differently: a probe is a small CeeSafe program, checked by ceesafec's borrow
checker and context effects (no sleep in an atomic probe), compiled and
signed as a module, and attached to OB1's tracepoints with a bounded run
time (OBS-5). What eBPF's verifier proves at load time, CeeSafe's checker
proves at compile time; the signature replaces load-time trust.

- [ ] **Acceptance:** a probe counting system calls by process, written in
  CeeSafe, built on the target with `ceesafec` (SW2), attaches, reports and
  detaches 1,000 times without a leak; a probe that sleeps in an atomic
  tracepoint is refused at compile time.

Size: about 1.5x.

---

## ST. Storage

### ST1 Send and receive

Incremental snapshot send and receive (CFS-12): a stream format, ancestry
checks, recovery from an interrupted receive. The stream parser is in
CeeSafe.

- [ ] **Acceptance:** a snapshot stream sent from one guest is received by
  another and the trees compare equal; a truncated or corrupted stream is
  refused without changing the target; a fuzz target drives the parser.

Size: about 1.5x.

### ST2 Parity layouts and multiple pools

Striping and RAID-Z-like parity (CFS-13) and more than one pool, with the
failure, replay and repair coverage the mirrors have (B2, B3).

- [ ] **Acceptance:** a single-parity pool of four disks survives the loss
  of any one, resilvers onto a replacement, and passes `fsck` and scrub;
  `test-mirror`-class tests for the new layout on both architectures.

Size: about 2.0x.

---

## VM. Virtualization

### VM1 The x86 hypervisor on Intel, and SMP guests

1.2's C1: the VMX backend executed (VIRT-1, VIRT-2), then more than one
vCPU per guest, on SVM, VMX and AArch64 EL2.

- [ ] **Acceptance:** a CI job on a VMX host runs the hypervisor's guest
  tests; a Linux guest with four vCPUs boots and runs a parallel build on
  all three backends.

Size: about 2.0x.

### VM2 Guest state, migration, passthrough

Full vCPU and device state in the UAPI, snapshots and live migration
(VIRT-5); dirty tracking and ballooning (VIRT-4); device passthrough with
IOMMU isolation (VIRT-6).

- [ ] **Acceptance:** a running guest migrates between two CosmoOS hosts
  with its TCP connections kept; an NVMe controller is passed through to a
  guest with its DMA confined.

Size: about 2.5x.

---

## UK. The hybrid kernel's other half

### UK1 A driver in user space (evaluated)

The master prompt asks for a hybrid kernel, not a monolithic one; today
nothing runs outside ring 0 except the VM device backends, `syslogd` and
`svc`. With drivers in CeeSafe, the case for moving them to user space is
weaker than it is for C, since memory safety removes a large share of
what isolation protects against. UK1 measures that trade instead of
assuming it: one driver (a NIC) runs as a user-space server over the
IOMMU, beside its in-kernel CeeSafe version, and PF1 compares them.

- [ ] **Acceptance:** a report with the two versions' PF1 numbers, the
  failure-isolation test (the user-space driver killed and restarted under
  load, the system staying up), and a recommendation for the owner.

Size: about 1.5x.

---

## Risks

- **CeeSafe's maturity.** CeeSafe's README says "early development, not
  production-ready". Every milestone that depends on a CeeSafe feature must
  find any compiler defect, not hide it: the differential test against C
  and ceesafec's own reference interpreter (CeeSafe M7) are the guard. A defect
  is fixed in CeeSafe and the pin moved, never worked around in generated C.
- **Converting working code.** About 190,000 lines that pass today's
  tests will be rewritten. Every conversion can bring back a defect the C
  had fixed. The guards: the differential test against the C until it is
  deleted, the self-tests converted after the code they check, the
  probes' `--old` history (each fixed defect has a test that failed
  before its fix; those tests must pass against the CeeSafe), and the
  validation matrix per PR.
- **Two projects in step.** The kernel in CeeSafe makes CeeSafe's compiler part
  of CosmoOS's trusted base and its schedule part of CosmoOS's. A CosmoOS
  milestone that waits for a CeeSafe feature says so in its first PR, and the
  Nova side is its own change in Nova's repository. A miscompile is now a
  kernel defect: CeeSafe's random-program fuzzer and reference interpreter
  (CeeSafe M7) run on every pin bump.
- **Performance of checks.** Bounds and overflow checks stay on in every
  build, in every hot path, once the scheduler, memory and network are
  CeeSafe. PF2's targets may need the compiler to remove more of them by
  proof; if a hot path cannot meet 5%, the owner decides, with the number.
- **Build time.** Building from CeeSafe (the rule's point 7) adds ceesafec to
  every build and every CI job; SA1 measures it.
- **Testing under TCG.** Most CI runs under TCG, where timing differs from
  hardware and PF1's numbers are meaningless. PF1 needs a KVM host; HW3
  needs real machines; both cost money and maintenance.
- **CI time.** `test-large` already sets main's CI time at 32-80 minutes
  (TEST-23). Phases 2 and 3 add long suites (PostgreSQL, LTP, Go). Each
  milestone adding a suite to CI says which jobs a PR runs
  ([which jobs a pull request runs](build/testing.md#which-jobs-a-pull-request-runs)).
- **Scale of the work.** About 83.5x in all: about two and a quarter
  years serially at 1.x's pace, ten to thirteen months in three streams. The phases are ordered so
  that each leaves a system better at its job than before if work stops
  there, and so that the conversion reaches what attackers touch first
  (parsers in phase 1, the I/O subsystems in phase 2).

## What this roadmap does not do

- Chase Linux's driver breadth, desktops or laptops.
- Convert third-party ports (BusyBox, musl, dropbear, the servers): they
  stay as their upstreams write them.
- Break the Linux ABI, or break the native ABI and on-disk formats more
  than once (at 2.0, with a migration or a documented reinstall).

## Recommendation

Adopt it, rewrite `AGENTS.md`'s scope rules and `build/ceesafe.md`'s kernel
rules to match in the first PR, and start three streams at once: SA1
(the gate and the CeeSafe build), PF1 (the benchmark set every later
performance claim needs) and SW0 (self-hosting, already under way). Then
SA2-SA4, which make allocation, locks and drivers possible in CeeSafe:
without them the conversion can reach only parsers and pure functions,
as it does today. SA2-SA4 wait for Nova's N1 and N3 (Nova 0.2.0), so the
conversion stream's first work is SA5, the parsers, which needs nothing
new from CeeSafe.
