# CosmoOS 2.0 roadmap (proposed)

Written 2026-10-29 at v1.0.14, on main at `f073ad71`; the MS track
(messaging at scale) added the same day on `4b1c55ec`. **Proposed, not
adopted.**

Its goal: make CosmoOS competitive with Linux and FreeBSD for the
workloads it chooses, with its kernel written in Nova so that it is
memory safe.

**Written without today's restrictions.** At the owner's direction, this
roadmap is not bound by the rules CosmoOS works under now: the scope
rules in [`AGENTS.md`](../AGENTS.md#scope-rules) (one milestone at a
time, at most five PRs each, 1.2's direction A first), the kernel rules
for Nova in [build/nova.md](build/nova.md#rules-for-nova-in-the-kernel)
(locks, allocation and object lifetimes in C; Nova only at leaf
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
| Memory safety | 982 lines of Nova in four ports (`gpt`, `cputime`, `nice`, `fair`) of about 227,000 kernel-side lines: under 0.5% | All ring-0 code in Nova except a named assembly and C floor (below: today's assembly is about 1% of ring-0 lines, the floor a few percent); every parser of untrusted input in Nova by the end of phase 1 | `scripts/nova-share.py` (SA1), in CI |
| Hardware | QEMU `q35`/`virt` and Parallels; no physical machine | Two physical servers per architecture in hardware-in-the-loop CI; common server NICs and storage | `hil` CI job (HW3) |
| Performance | Not measured against Linux or FreeBSD | Within 15% of the better of Linux and FreeBSD on the benchmark set, same VM and hardware; no worse than 1.5x on any one | `make bench-compare` (PF1), dated reports |
| Scale | 64 CPUs, no NUMA, no huge pages, no swap | 256 CPUs, two NUMA nodes, huge pages, swap | PF3's tests on QEMU and HW3's machines |
| Linux programs | musl static/PIE/dynamic; glibc untested; about 20 common calls missing | glibc programs from a mainstream distribution's packages run; the Linux Test Project's chosen subset passes | LX1's test lists |
| Connections | 64 handles per process (`HANDLE_TABLE_SIZE`); TCP connections in 256 buckets keyed by local port; per-CPU timer queues with a sorted-list insert | 2,000,000 persistent TCP connections in one guest, held by an Erlang messaging server | MS6's load test, in `make bench-compare` |
| Server software | BusyBox, dropbear, tcc; A4-A6 (curl, OpenSSH, git, Python) in progress | Also nginx, PostgreSQL, Redis, a Go and a Rust toolchain, an OCI container runtime | Each port's own test suite |
| Security | Verified boot, signed modules, user ASLR, privileges | Also KASLR, Secure Boot, measured boot, verified root, capability-based sandboxing, syscall filtering, multiple users | SE1-SE2's acceptance tests |
| Observability | lockdep, self-tests, serial logs | Tracing with programmable probes, performance counters, a kernel debugger workflow | OB1-OB2 |

## The Nova rule

The target is a kernel written in Nova, not a C kernel with Nova parts.
The rule from adoption on:

1. **All new kernel code is written in Nova, and no new C is written.**
   New subsystems, drivers, parsers, fixes that rewrite a function: all
   Nova. There is no exemption process for new C.
2. **All existing C is converted.** Every C file in ring 0 is rewritten in
   Nova, by the SA track below, whether or not a feature needs it. The
   order is by risk, not by size: first what parses input an attacker or a
   damaged disk controls (the code behind the 15 fuzz targets: ELF, module
   ELF, cosmofs metadata, network frames and TCP segments, DHCP and DNS,
   Linux syscall arguments, USB descriptors, virtqueues, `pkg` archives,
   LZ4, framebuffer descriptions); then the code with the most defects in
   its history (the backlog's `correctness` lines by subsystem); then
   whole subsystems, the network stack, cosmofs, VFS, the Linux
   personality, memory and the scheduler.
3. **The floor that stays assembly and C is named and small.** Nova can
   express raw pointers, MMIO and C function pointers, but not
   instructions it has no syntax for. What stays outside Nova: boot entry
   and the loader's hand-off, trap, interrupt and system-call entry and
   exit stubs, context switch, the user-copy routines with their fault
   fixups (`uaccess.S`), the atomic and barrier primitives and the
   lowest-level spin lock, TLB and cache maintenance, the hypervisor's
   world switch (`vmx_run.S`, the SVM and EL2 equivalents), and Nova's
   runtime shim (`nova_rt_core.c`, `nova_panic.c`). Each is a function a
   Nova caller reaches through `extern "C"`, with its contract written as a
   `/// Safety:` comment on the Nova declaration. Everything else, locks
   built on the primitive (mutexes, rwlocks, wait queues), allocation,
   kobjects, quiescence, per-CPU data, the scheduler, is Nova. The floor
   also keeps the one-instruction C wrappers Nova has no syntax for (MSR,
   port I/O and system-register access, `cpuid`, `rdtsc`, `wfi`). Today's
   assembly is 1,886 of 195,417 ring-0 lines (about 1%); with those
   wrappers and the shim the floor is a few percent.
4. **The kernel rules for Nova are replaced.** "Locks stay in C",
   "allocates nothing" and "no current port takes a lock" go. Nova code
   takes locks through Nova guards (`SpinLock<T>`, `Mutex<T>`: the data is
   reachable only through the guard, and holding a spin lock is an atomic
   context the compiler checks), allocates through `KernelHeap` and
   `ObjectCache<T>` with failure as a `Result`, and holds kernel objects as
   `KRef<T>`, whose drop is the release. Nova's repository built most of
   these as demonstrations (its C5: `KernelHeap`; C7: `KRef<T>`,
   `SpinLock<T>`, `IrqSpinLock<T>`, `Mutex<T>`, `Quiesce`), and a Nova
   module loaded and unloaded 1,000 times against CosmoOS (C8);
   `ObjectCache<T>` is still to write, and none of them is yet a library
   the kernel depends on ([Nova prerequisites](#nova-prerequisites), N3). The rules that stay: `--no-float`; `unsafe` only
   at the floor's boundary, each with its `/// Safety:` comment; context
   effects (`@context(irq)`, `@context(atomic)`) on every function that
   runs there; no failing check reachable in interrupt context.
5. **A port proves itself, then the C is deleted.** Each conversion has a
   host test, a fuzz target where it takes input, the in-kernel tests, and
   a differential test against the C it replaces, run until the C is
   deleted in the same PR or the next. Its benchmark is within 5% of the C
   on both architectures with clang (Nova's C6 measured 0.98-1.06x for
   `fbvalid`), or the owner accepts the slowdown with the number recorded.
   The C and the Nova versions never both ship.
6. **No weakened Nova checks to save time.** A check that costs too much is
   removed by a proof the compiler accepts (Nova's `optimisations.md`), not
   by `unsafe` or a wrapping operation that hides a real overflow.
7. **Generated C is a build artefact.** Today the generated C is committed
   beside each `.nova` file so that building needs no Nova compiler. With
   the whole kernel in Nova that doubles the tree for no reader. From SA1
   on, the build runs the pinned novac and the generated C is not
   committed; CI's "Nova sources" check becomes a reproducibility check of
   the build (`make reproducible`). novac does not run on CosmoOS, so a
   self-hosted build (SW0) needs the generated C from somewhere: source
   releases ship it ([Nova prerequisites](#nova-prerequisites), N4).

Userland follows the same rule where it is CosmoOS's own code: `libc/`,
`userland/` (the native tools, `svc`, `syslogd`, `cosmo-*`) and `pkg/`
are converted in SA12. Third-party ports (BusyBox, musl, dropbear, the
servers) stay as their upstreams write them.

## Nova prerequisites

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
  on the Nova code, so nothing is written twice.
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
nova:        N4 decided, K2a ─► SA1;  N1, N3 ─► SA2, SA3;  N5 ─► phase 2  (Nova prerequisites)
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
| 1 | 2.0-alpha | The Nova foundation | SA1-SA5, SW0, PF1, HW1 | about 13x |
| 2 | 2.0 | The I/O half in Nova, real machines | SA6-SA9, HW2, HW3, SE1, NT1 | about 19x |
| 3 | 2.1 | The core in Nova, server parity, connections at scale | SA10, SA11, NT2, LX1, LX2, PF2, PF3, SW1, MS1-MS4 | about 24x |
| 4 | 2.2 | No C left; containers, observability, storage; the messaging server | SA12, SA13, SE2, CT1, CT2, OB1, OB2, ST1, ST2, SW2, MS5, MS6 | about 22x |
| 5 | 2.3 | Virtualization, and the hybrid kernel's other half | VM1, VM2, UK1 | about 6x |

About 83.5x of work in all. Run serially at 1.x's pace (1.0x is about ten
days) that is about two and a quarter years; in three streams, about ten
to thirteen months.

---

## SA. Safety: the kernel in Nova

### SA1 Measure, gate, and build from Nova

`scripts/nova-share.py` counts ring-0 lines per tree and per subsystem:
Nova, the floor (assembly and the named C wrappers), and C still to
convert. CI's `nova-share` job prints the table on every PR and fails one
that adds a line of C outside the floor. The build runs the pinned novac
itself and the generated C leaves the tree (the Nova rule, point 7);
`make reproducible` checks that two builds agree. A panic in Nova code
names the Nova file and line in the crash report on both architectures.

- [ ] **Acceptance:** CI's `nova-share` job runs on every PR and on main
  and fails a test PR that adds a C function under `kernel-services/`; a
  clean checkout builds with only the pinned novac and the cross
  toolchain, and `make reproducible` passes; `crash-nova` (a self-test
  behind `CONFIG_SELFTEST`) fails a Nova bounds check and the harness
  finds the Nova file:line in the serial log on both architectures.

Size: about 0.7x.

### SA2 Allocation and object lifetimes in Nova

Nova's C5 `KernelHeap`, a new `ObjectCache<T>` over `kmem_cache`, and C7's
`KRef<T>` (a counted kobject reference whose drop is the release) come into
the tree as the kernel library (N3), behind
the fault injection the C allocators have (`CONFIG_FAULTINJECT`).
Allocation failure is a `Result` a caller handles or returns, never a
panic. The proving conversion has real allocation and lifetimes: the
kernel object and handle layer (`kernel/object/object.c`, `handle.c`;
495 lines, used at 159 `HANDLE_RIGHT_*` sites), with its census tests.

- [x] **Acceptance:** the object and handle layer in Nova passes its
  self-tests and a fault-injection sweep (every allocation failed in turn)
  with no leak in the object census; the differential host test against
  the C agrees on a recorded trace of 10,000 operations before the C is
  deleted; the matrix on both architectures.

Progress: the object census, `kobject_discard` and the recorded trace
(#491); the object layer in Nova (`kernel/object/object.nova`, over
`object_floor.c`), which gives the recorded digest (#493, 2026-10-30);
the handle table in Nova (`kernel/object/handle.nova`, a Nova-owned type C
embeds by value, with novac 0.5.0's ADR 0029), which gives it too
(2026-10-30). The hand-written C of both is gone. The sweep,
`object-fault-sweep` (`kernel/core/faulttest.c`), fails every allocation
of seven object-creating paths in turn (pipe, eventfd, epoll with a
registration, TCP, unix pair, file, process: 52 attempts on x86-64, 51 on
AArch64) and finds the
census unchanged after each (2026-10-30). With the matrix green on both
architectures, SA2's acceptance is met.

Size: about 1.0x.

### SA3 Locks, waits and quiescence in Nova

Mutexes, read-write locks, wait queues, completions and quiescence
(`kernel/core/`'s synchronisation above the spin-lock primitive) are
rewritten in Nova, with Nova's C7 guards as their interface: the data a
lock protects is reachable only through its guard, and the compiler
refuses a sleep while a spin lock's guard is live. lockdep sees every
acquisition with today's classes, callback classes and completion classes
(L21). Pipes and FIFOs (`kernel/ipc/pipe.c`, `fifo.c`) convert with them,
as the first users.

- [ ] **Acceptance:** `test-smp2`, `test-chaos` and every lock, wait,
  quiescence, pipe and FIFO self-test pass on both architectures with the
  Nova versions and the C deleted; lockdep's graph self-tests pass; a
  negative compile test refuses a sleep under a spin lock's guard.

Size: about 1.5x.

### SA4 Driver modules in Nova

Nova's C8 loaded and unloaded a Nova `.ko` 1,000 times against CosmoOS on
both architectures. SA4 makes that the way drivers are written: the
module ABI and the device API as Nova interfaces, a template under
`drivers/nova-template/`, signing through `modsign.py` as today. The
proving driver is virtio-rng (`drivers/virtio/virtio_rng.c`), small and
present on every CI boot.

- [ ] **Acceptance:** `virtio_rng.ko` from Nova passes `test-modcycle`
  (1,000 cycles, both architectures) and the randomness self-tests, with
  the C driver deleted.

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

- [ ] **Acceptance:** every fuzz target in `tests/fuzz/` drives Nova code
  and none drives C; each family's benchmark within the rule's 5%.

Size: about 3.0x.

### SA6 The network stack

`kernel-services/network/` (23,773 lines, 11,841 of them self-tests):
mbufs, interfaces, ARP and neighbour discovery, IPv4 and IPv6, ICMP, UDP,
TCP, the firewall and NAT, sockets, AF_UNIX, the per-CPU receive workers.
The self-tests convert after the code they test, so the C tests check the
Nova first.

- [ ] **Acceptance:** the network harness, `test-net-login`, `nettest`'s
  self-tests and the net fuzz targets pass on both architectures with no C
  left in `kernel-services/network/`; PF1's network numbers within 5% of
  the C build's.

Size: about 3.0x.

### SA7 Storage: cosmofs, VFS, block and pools

`kernel-services/filesystem/` (26,934 lines), `kernel-services/vfs/`
(9,436), `kernel/block/` (5,162) and `kernel-services/storage/` (837):
cosmofs with its snapshots, mirrors, encryption, scrub and resilver; the
VFS, page cache and mount namespaces; the block layer. Converted in
dependency order (block, pool, cosmofs, VFS), each step on the existing
suites.

- [ ] **Acceptance:** `test-install`, `test-upgrade`, `test-snapshot`,
  `test-fsck`, `test-mirror`, `test-encrypt` and `test-large` pass on both
  architectures with no C left in those trees; an image written by the C
  build mounts and checks clean under the Nova build, and the reverse
  (until 2.0's format break, if SA7 takes it).

Size: about 4.5x.

### SA8 The Linux personality and the system-call layer

`compat/` (7,426 lines) and `kernel/syscall/` (3,413): both system-call
tables, argument decoding (SA5 did the parsers), the Linux signal frames
and the personality's state.

- [ ] **Acceptance:** `test-busybox`, the dynamic and PIE tests, and the
  Linux self-tests pass on both architectures with no C left in
  `compat/` or `kernel/syscall/`.

Size: about 1.5x.

### SA9 Every driver

`drivers/` (15,328 lines): PCI, ACPI, the IOMMUs (VT-d, SMMUv3), NVMe,
AHCI, e1000e, USB and the virtio drivers, each a Nova module (SA4). HW2's
new drivers are written in Nova from the start, in the same stream.

- [ ] **Acceptance:** every driver module is built from Nova and passes
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
  architectures; `nova-share.py` reports only the floor in
  `kernel/arch/` and `kernel-services/virtualization/`.

Size: about 2.5x.

### SA12 CosmoOS's own userland

`libc/` (5,949 lines), `userland/` (26,611: the native tools, `init`,
`svc`, `syslogd`, `netcfg`, `cosmo-upgrade`, `cosmo-snap`,
`fsck.cosmofs`, `cosmo-pool`, `cosmo-key`) and `pkg/` (3,505), on Nova's
hosted profile. libc keeps a C ABI for the ports that link it; its
implementation is Nova, and it gains the `math.h` it lacks (in Nova).

- [ ] **Acceptance:** `test-upgrade`, `test-install`, the `pkg` tests and
  the native-tool tests pass on both architectures with no C left in
  `libc/`, `userland/` or `pkg/` (the ports excepted).

Size: about 3.0x.

### SA13 The last C

Headers become Nova interfaces, with C headers kept only for the floor
and for libc's public ABI; the floor is audited function by function (each
one's `/// Safety:` contract against its callers); `nova-share.py`'s
number for ring 0 is the floor and nothing else.

- [ ] **Acceptance:** `nova-share.py` on main reports zero lines of
  convertible C in ring 0 and in CosmoOS's own userland; a dated report
  lists the floor, its size, and each function's contract.

Size: about 1.0x.

---

## SW0 Self-hosting (1.2's direction A, folded in)

1.2's A2-A6 as they are written in [roadmap-1.2.md](roadmap-1.2.md#a-self-hosting-software-chosen-2026-10-27-after-b):
`make` and the build tools, a ports tree that builds on CosmoOS, nano and
curl, OpenSSH and git, Python. They run in the features stream from the
start of phase 1, not before it, and new CosmoOS-side code they need is
in Nova.

- [ ] **Acceptance:** 1.2's A2-A6 acceptance tests, each green on main.

Size: about 4.0x.

---

## HW. Real machines

Roadmap 1.2 deferred these as C2, C4, C5 and C6; they move here.

### HW1 KASLR and Secure Boot

KASLR (1.2's C2, BOOT-1): a relocatable kernel, the slide chosen by the
loader from `EFI_RNG_PROTOCOL` or the CPU, symbolisation adjusted. The
loader signed for UEFI Secure Boot with a documented key enrolment
(BOOT-2). The relocation processing in the loader is written in Nova (an
ELF relocation parser is untrusted input to the loader).

- [ ] **Acceptance:** `test-security` boots three times and requires three
  different kernel bases, with crash symbolisation still correct; a QEMU
  boot with OVMF's Secure Boot enrolled with the project key boots the
  release image, and refuses a loader with one byte changed.

Size: about 1.0x.

### HW2 Server drivers, in Nova

Drivers for the hardware HW3's machines have, written as Nova modules
(SA4): one Intel and one other common server NIC (for example `igc` or
`ixgbe` class, and a Broadcom or Mellanox ConnectX class, chosen with the
machines), AHCI and NVMe on real controllers (on SA9's Nova drivers),
xHCI for a USB console and install media, and an AML interpreter for ACPI
power-off and the DSDT-described devices (DRV-4; a parser of firmware
input, fuzzed). On AArch64, device-tree discovery (DRV-5) if the chosen
machine needs it. All in Nova, in the conversion stream beside SA9.

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
Nova; a hot path's Nova port held to the rule's 5%.

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
the swap slot allocator are new code, in Nova.

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
The option parsers are written in Nova as part of SA5's network family.

- [ ] **Acceptance:** a harness against a Linux peer checks each TCP option
  on the wire (window scale, SACK blocks after injected loss, timestamps);
  a guest reaches an IPv6 address beyond its link through a QEMU router;
  nginx (SW1) sets its sockets' options without a refusal.

Size: about 2.0x.

### NT2 Netlink and operator tools

`AF_NETLINK` route sockets (LX-7) so Linux tools (`ip` from iproute2,
BusyBox `ip`) work; a DHCPv6 client and a caching resolver (NET-18, part).
The netlink message parser is in Nova.

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
written in Nova (they decode user memory: SA5's second family).

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
on), and Nova itself: `novac` built and run on CosmoOS, so that Nova code
can be developed on the system it runs on.

- [ ] **Acceptance:** Go's `go test std` subset and Rust's `cargo test` on
  a chosen crate set pass on the target; `novac` built on CosmoOS
  regenerates every `.nova` port in the tree byte for byte
  (`make nova-check` on the target).

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

**Built here, in Nova; not imported.** FreeBSD's `netinet` is not taken
into the tree. It depends on FreeBSD's mbufs, UMA allocator, epoch
reclamation, callouts, taskqueues, `ifnet` and VNET, so porting it means
porting a large slice of FreeBSD's kernel as C, against the Nova rule.
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
Nova (SA2's allocation and object lifetimes; the socket-buffer part on
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
(`somaxconn`). In Nova, on SA6's converted stack.

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
epoll. In Nova.

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
per-CPU cache. In Nova. This takes the multiqueue virtio-net part of
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
is in Nova.

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
and the TPM event log parsers are in Nova.

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
evaluator are in Nova.

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
limits (SEC-10). New code in Nova.

- [ ] **Acceptance:** a process in new PID and network namespaces sees PID
  1 and only its own interfaces; a memory limit stops a process at its
  limit without affecting the host; a CPU weight shares a CPU as set.

Size: about 2.0x.

### CT2 An OCI runtime

An OCI runtime (crun, or a native one in Nova) over CT1, SE2's filter and
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

### OB2 Programmable probes, in Nova

Linux's eBPF and FreeBSD's DTrace both run user-supplied code in the
kernel behind a verifier or an interpreter. CosmoOS can do this
differently: a probe is a small Nova program, checked by novac's borrow
checker and context effects (no sleep in an atomic probe), compiled and
signed as a module, and attached to OB1's tracepoints with a bounded run
time (OBS-5). What eBPF's verifier proves at load time, Nova's checker
proves at compile time; the signature replaces load-time trust.

- [ ] **Acceptance:** a probe counting system calls by process, written in
  Nova, built on the target with `novac` (SW2), attaches, reports and
  detaches 1,000 times without a leak; a probe that sleeps in an atomic
  tracepoint is refused at compile time.

Size: about 1.5x.

---

## ST. Storage

### ST1 Send and receive

Incremental snapshot send and receive (CFS-12): a stream format, ancestry
checks, recovery from an interrupted receive. The stream parser is in
Nova.

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
`svc`. With drivers in Nova, the case for moving them to user space is
weaker than it is for C, since memory safety removes a large share of
what isolation protects against. UK1 measures that trade instead of
assuming it: one driver (a NIC) runs as a user-space server over the
IOMMU, beside its in-kernel Nova version, and PF1 compares them.

- [ ] **Acceptance:** a report with the two versions' PF1 numbers, the
  failure-isolation test (the user-space driver killed and restarted under
  load, the system staying up), and a recommendation for the owner.

Size: about 1.5x.

---

## Risks

- **Nova's maturity.** Nova's README says "early development, not
  production-ready". Every milestone that depends on a Nova feature must
  find any compiler defect, not hide it: the differential test against C
  and novac's own reference interpreter (Nova M7) are the guard. A defect
  is fixed in Nova and the pin moved, never worked around in generated C.
- **Converting working code.** About 190,000 lines that pass today's
  tests will be rewritten. Every conversion can bring back a defect the C
  had fixed. The guards: the differential test against the C until it is
  deleted, the self-tests converted after the code they check, the
  probes' `--old` history (each fixed defect has a test that failed
  before its fix; those tests must pass against the Nova), and the
  validation matrix per PR.
- **Two projects in step.** The kernel in Nova makes Nova's compiler part
  of CosmoOS's trusted base and its schedule part of CosmoOS's. A CosmoOS
  milestone that waits for a Nova feature says so in its first PR, and the
  Nova side is its own change in Nova's repository. A miscompile is now a
  kernel defect: Nova's random-program fuzzer and reference interpreter
  (Nova M7) run on every pin bump.
- **Performance of checks.** Bounds and overflow checks stay on in every
  build, in every hot path, once the scheduler, memory and network are
  Nova. PF2's targets may need the compiler to remove more of them by
  proof; if a hot path cannot meet 5%, the owner decides, with the number.
- **Build time.** Building from Nova (the rule's point 7) adds novac to
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

Adopt it, rewrite `AGENTS.md`'s scope rules and `build/nova.md`'s kernel
rules to match in the first PR, and start three streams at once: SA1
(the gate and the Nova build), PF1 (the benchmark set every later
performance claim needs) and SW0 (self-hosting, already under way). Then
SA2-SA4, which make allocation, locks and drivers possible in Nova:
without them the conversion can reach only parsers and pure functions,
as it does today. SA2-SA4 wait for Nova's N1 and N3 (Nova 0.2.0), so the
conversion stream's first work is SA5, the parsers, which needs nothing
new from Nova.
