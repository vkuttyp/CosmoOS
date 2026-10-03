# Lock discipline and lockdep: testing

## What runs where

| Level | What | Command |
|---|---|---|
| Every boot with LOCKDEP=1 (debug default) | the checker runs on every acquisition through boot, all self-tests, the userland test script and the network harness; any report is a panic, so `make test` fails | `make test`, `QEMU_SMP=1 make test`, `make ARCH=aarch64 test` |
| Host (ASan/UBSan) | `test_lockdep`: the class table, edges and reachability, the decision procedure on an ABBA, a three-lock cycle and subclass nodes | `make host-test` |
| Target self-tests (debug builds) | `lockdep-order`, `lockdep-recursion`, `lockdep-irq`, `lockdep-sleep`, `lockdep-mutex`, `lockdep-contention` (the detector fires on constructed violations and stays silent on a contended wait); `vfs-concurrency` (the fixed races, on two CPUs) | `make test` |
| Release default (LOCKDEP=0) | the checker is compiled out; `might_sleep`'s panic half stays | `make BUILD=release test` |

At the original lockdep milestone, these tests passed on x86-64 (4 CPUs
and 1 CPU) and AArch64 (4 CPUs). Current October results, including UP
baseline failures, are in [the hardening report](../../audit/2026-10-03-lockdep-report.md).
The original debug boot took 16.4 s on x86-64 against 15.5 s before the
checker (about 100 000 checked acquisitions per boot), and 18.8 s against
17.8 s on AArch64.

## Host test (`tests/host/test_lockdep.c`)

| Case | What it checks |
|---|---|
| `classes` | one name is one class; the same name with the other kind is another; the table fills to `LOCKDEP_MAX_CLASSES` and then returns -1; node arithmetic at both ends |
| `edges-and-cycles` | add and duplicate edges; reachability direct and through a chain with the returned path; an unconnected node; a 21-node chain truncated to the last 8 entries; the highest node |
| `decision` | the kernel's rule replayed: an ABBA is refused, a three-lock cycle is refused through the chain, edges are added from every held lock, subclass nodes are distinct |

## Checker self-tests (`kernel/core/lockdeptest.c`)

Each arms one expectation with `lockdep_expect`, provokes the violation on
private locks, and checks that exactly one report of that kind was counted
(`lockdep_expected_hits`); the operation proceeds, so every lock is
released normally.

| Test | Steps | Proves |
|---|---|---|
| `lockdep-order` | A → B twice (the second time runs no search: the edge is known); B → A is an inversion; releasing out of order is legal; `lockdep_is_held` tracks the stack | L1, L11 |
| `lockdep-recursion` | two spinlocks from one init site nested is a recursion report; the same with `spin_lock_nested(…, 1)` is silent | L2 |
| `lockdep-irq` | direct and transitive IRQ conflicts in both observation orders, thread trylock classification, successful/failed IRQ trylock, wrong irqrestore state, and enabling with another spinlock held; self-IPI handler drives real interrupt acquisitions | L3, L14 |
| `lockdep-sleep` | `might_sleep()` under a spinlock is a report; with nothing held it is silent | L4 |
| `lockdep-mutex` | mutexes M1 → M2 with a spinlock under them is legal; M2 → M1 is an inversion on the per-thread stack; a mutex taken under a spinlock is a sleep report | L1 (mutexes), L4, L11 |
| `lockdep-contention` | CPU 1 holds L for 20 ms; this CPU spins on a plain `spin_lock(L)` with interrupts enabled while a timer callback takes M inside the wait (asserted to have fired); afterwards M → L is taken and must not be an inversion, so no phantom L → M was recorded while L was merely awaited | L11 (a waited-for lock is not held); the PR #18 review finding |
| `lockdep-bench` | warmed uncontended spin and mutex paths, nine batches of 1024 iterations, pinned thread | descriptive timing only; no performance pass threshold |
| `lockdep-first-bench` | public spin, irqsave, nested-spin and mutex acquisitions with fresh classes, then reuse; three samples per path | verifies ownership, class registration and nested search; descriptive timing only |
| `lockdep-graph-bench` | private chain/dense 16/64/256/320/1280-node graphs, allowed insertion, cycle rejection, and transitive IRQ-conflict rejection | validates each result; timing is descriptive, with no performance pass threshold |

### Spin and mutex path measurement

`lockdep-bench` runs with either `LOCKDEP=0` or `LOCKDEP=1` when self-tests
are enabled. Compare debug builds in separate output trees, changing only
the lockdep setting; release builds change more than the checker and are
not a matched control. For example, with the same architecture and QEMU
settings, run `make BUILD=debug LOCKDEP=1 test` and
`make BUILD=debug LOCKDEP=0 OUT=out/bench-off test`.

The test warms each path 64 times, then reports min/median/max guest-clock
nanoseconds per iteration across nine batches of 1024 iterations. `spin`
and `irqsave` each contain one acquire/release pair; `nested` contains an
outer irqsave pair and an inner plain pair with a previously recorded edge.
`mutex` is a blocking acquire/release pair on a private mutex; `mutex-try`
is a successful trylock/release pair on the same object. Initialization
and first-use class/edge registration happen before timed samples. The
trylock path asserts success so a refused acquisition cannot look faster.
`empty` is a loop/dispatch/clock control, reported separately without
subtracting it. Pinning keeps clock readings CPU-local. Interrupts and
scheduling remain enabled, other CPUs continue running, and results include
their interference. Printing and affinity changes are outside the samples.
These measurements cover warmed uncontended object locks, not cold graph
searches, lock contention, priority-inheritance waits, or a hardware-independent overhead
ratio. QEMU/TCG results describe that emulator and host workload only.

### First-acquisition measurement

`lockdep-first-bench` runs the same public acquisition calls in debug
`LOCKDEP=0/1` builds. Three samples each use new names for plain spin,
irqsave, nested spin (outer irqsave plus inner plain), and mutex locks.
The live graph is preserved. With lockdep enabled, these samples consume
18 permanent classes: three each for spin and irqsave, six for nested
pairs, and six for mutexes and their internal spinlocks. Names have static
storage; no class pointer refers to expired scratch. Repetition is limited
to keep headroom in the 320-class pool, which is not enlarged for timing.

Each sample initializes private objects outside timing, verifies empty
class caches, times the acquisition through ownership, then releases and
checks the result outside timing. A second acquisition of those same
objects measures reuse without reinitialization. Class caches must remain
stable. Enabled builds require class-count growth and a new-edge search
for nested pairs; the global deltas use lower bounds because other CPUs
can add unrelated classes or dependencies. No expected-report suppression
is armed. A temporary negative control gave every sample the same class
names: the second spin sample failed the fresh-class check as intended.

The interval includes clock reads, path selection, primitive acquisition,
class lookup/registration, applicable usage/edge checks, and held-stack
publication. Unlock, validation, statistics snapshots, printing and
affinity changes are outside it. The nested value covers **two** public
acquisitions, without dividing by two. A separate empty-path control is
reported, never subtracted. Three values produce min/median/max, not a
statistical confidence claim. Pinning keeps clock reads on one CPU;
interrupts and scheduling follow the ordinary primitive's rules, and
other CPUs continue running. New classes do not imply cold CPU caches.

Results describe first use against the live boot graph at the reported
class range, not a graph-size sweep or a worst-case acquisition. Guest
clock granularity can produce zero for short intervals, especially with
lockdep disabled; those values cannot support overhead ratios. QEMU/TCG
timings include host scheduling and translation effects. Contended locks,
priority inheritance and native hardware remain separate measurements.

### New-edge core measurement

`lockdep-graph-bench` runs only with lockdep enabled. It allocates a private
graph and search scratch, then measures the real reachability, IRQ-edge,
and insertion helpers in the same order as acquisition checking. Each
graph has 16, 64, 256, or 320 classes with one active node per class (subclass
zero). An additional 1,280-node case populates all four subclasses of
every class; the core's fixed bitmap capacity is unchanged.

The `insert` and `irq-bridge` cases start with two disjoint chains and
propose their missing middle edge. In `irq-bridge`, only the first class
is IRQ-used and the last is IRQ-enabled, so the bridge must be refused.
The `cycle` case starts with one chain and proposes last-to-first, which
must be refused with the expected truncated cycle path.

Each case also runs on dense DAGs containing every forward edge within
its component(s). At 320 active nodes the cycle graph has 51,040 edges;
the two-component insertion/IRQ graphs have 25,440. Their absent bridge
requires an unsuccessful reverse-reachability search over the entire
second component. Dense cycle rejection finds the direct first-to-last
edge and returns a two-node path, so density does not imply worst-case
latency. Setup checks the expected edge count. Every operation checks its
result, edge count, and proposed-edge presence; returned cycle paths check
their expected endpoints/length and each consecutive edge, and rejected
IRQ edges check both reported endpoints.

Each of nine samples times one operation after two warmup samples. Graph
setup, allocation, validation and logging are outside the interval. Setup
rebuilds the graph before every sample, so no sample takes the live
validator's known-edge shortcut. “New edge” does not mean a cold CPU cache:
setup and warmup can populate caches. The pinned thread leaves IRQs and
scheduling enabled. Results report min/median/max guest nanoseconds.
These are core algorithm costs, excluding raw-lock contention, class
registration, held-stack scans, statistics, and reports. They do not
measure end-to-end acquisition latency or establish a worst-case bound.
The full-capacity dense cycle graph has 818,560 edges; its two-component
cases have 408,960. IRQ labels apply to all subclasses: the full chain's
safe endpoint is subclass 3 of the first class, while the dense case uses
subclass 0; both reach subclass 0 of the last IRQ-enabled class. The tests
check these endpoints explicitly. Output identifies topology and the
pre-operation edge count for each of the 30 cases.

The host `dense-capacity` sanitizer test fills the same 1,280-node dense
DAG, checks a two-entry path buffer, forces a full traversal with an absent
usage label, and initializes every node as a BFS source. These exercise
queue capacity and duplicate suppression under ASan/UBSan.
The benchmark never registers its classes or edges in the live graph.

### Deterministic search work

Host `search-work` enables `LOCKDEP_CORE_TEST_WORK` only in
`tests/host/test_lockdep.c`. Counters in that translation unit's scratch
count search starts, visited clears, seed candidates, queue operations,
adjacency-word reads and parent steps in the actual shared helpers.
Normal kernel and threaded-host builds have no counter fields or updates.
Tests reset counters between operations; composite IRQ helpers accumulate
all searches rather than silently replacing the count with the last one.

The test covers all 1280 nodes: an absent usage label on a dense DAG
attains 25,600 adjacency-word reads; all nodes as initial sources test
duplicate suppression; an isolated target forces an exhaustive failed
reachability search; a full chain attains 2,559 parent steps even with no
output path; and a cyclic graph still terminates within the bound. It also
checks empty-source and self-reachability shortcuts, four-search fresh
usage validation, zero-search warmed usage, usage-conflict rejection and
both one- and two-search IRQ-edge decisions. General per-search bounds are
checked alongside exact counts for these chosen topologies.

A temporary negative control repeats every usage-search adjacency-word
scan. Its graph answers still pass the existing tests and closure oracle,
but `search-work` fails the read bound. ASan/UBSan cover the instrumented
helpers; the existing threaded TSan test covers the uninstrumented ones.
An optimized freestanding wrapper comparison for x86-64 and AArch64 found
identical before/after assembly for the search helpers and scratch-size
function with counters disabled. These checks establish work counts, not
a wall-clock maximum, complete acquisition cost, or NMI safety.

## VFS concurrency (`kernel-services/vfs/vfstest.c`, `vfs-concurrency`)

Two threads pinned to CPU 0 and CPU 1 (or both on CPU 0 with one CPU):

1. For 200 ms one thread renames `/tmp/vc/a/x` into `/tmp/vc/a/b/y` and
   back while the other removes and recreates `/tmp/vc/a/b`. Every result
   is one of the legitimate outcomes (`-ENOENT` when `b` is gone or `y` has
   moved, `-ENOTEMPTY` when `y` is inside `b`, `-EEXIST`); afterwards `x` is
   in exactly one of its two places. Under the old address-order rename
   this was the audit's ABBA against rmdir; under the checker any inversion
   would also have panicked.
2. For 200 ms both threads open and close `/tmp/vc/shared`; afterwards the
   vnode count equals the created files exactly (ramfs pins its vnodes, so
   a duplicate vnode for one inode would show as one extra). Measured:
   1273 rename rounds against 2579 rmdir/mkdir rounds and 14 665 open/close
   calls on x86-64; 494 / 1327 / 18 487 on AArch64.

The test leaves the namespace as it found it (vnode count back to the
start).

## The recorded lock order

`selftest_run_all` ends with `lockdep_dump_graph()`, so a debug boot with
`LOCKDEP=1` contains the edges the run recorded (`lockdep: edge …`, `a -> b` meaning b
was taken while a was held; the original milestone recorded 133 classes
and 415 edges on x86-64 with 4 CPUs). The VFS and network excerpts below
are from that original run; the scheduler section uses the October graph.

**Scheduler (S2, S4, S24; current October graph).** The per-CPU classes
`runqueue0` through `runqueue3` have edges to higher-numbered runqueues on
both architectures. AArch64 also records each runqueue → `asid` from
address-space switching; this path also applies to x86-64 when address-space
tags are available. `lockdep-rq-order`'s expected diagnostic while holding
`runqueue1` records edges to `klog-ring`, `console`, `virtio-console`,
`virtio-pci` and `virtq` on both architectures. ASID rollover can also log
under its lock. These diagnostic paths are real observed dependencies,
not evidence that arbitrary subsystem locks may nest under a runqueue.
No GIC edge is expected from IPI sending. Check new outgoing edges against
these call paths and the increasing-CPU pair rule, rather than requiring
zero successors. The complete current graph is
[`2026-10-03-lock-order.tsv`](../../audit/2026-10-03-lock-order.tsv).

**VFS (V7).**

```
mutex 'mounts'      -> mutex 'vnode'#0, mutex 'cosmofs', spin 'mount-hash', spin 'mounts'
mutex 'rename'      -> mutex 'vnode'#0, mutex 'vnode'#1, mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'vnode'#0     -> mutex 'vnode'#1, mutex 'vnode'#2, mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'vnode'#1     -> mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'vnode'#2     -> mutex 'cosmofs'
mutex 'file'        -> mutex 'vnode'#0, mutex 'pagecache', mutex 'cosmofs'
mutex 'pagecache'   -> mutex 'cosmofs', spin 'pagecache', spin 'pagecache-stats'
mutex 'mount-sync'  -> mutex 'vnode'#0, mutex 'pagecache', mutex 'cosmofs', spin 'mount-hash'
mutex 'cosmofs'     -> spin 'blk-sync', spin 'virtio-blk', spin 'virtq', spin 'virtio-pci', spin 'mount-hash'
```

`spin 'mount-hash'` has no successor except the allocator locks: a leaf.
Nothing takes a vnode lock under `mount-hash`, `pagecache` or `cosmofs`.

**Network.**

```
mutex 'socket'      -> spin 'tcp', spin 'udp', spin 'arp', spin 'netifs', spin 'net-rxq', spin 'socket',
                       spin 'g_worker_wq', spin 'timer_queue', spin 'random', spin 'virtq', spin 'virtio-pci'
spin 'tcp'          -> spin 'timer_queue', spin 'random'
spin 'udp'          -> spin 'udp-rxq'
mutex 'devices', mutex 'modules' -> spin 'netifs', spin 'netif'
```

The documented order (`sock->lock` → protocol spinlock → `arp`/`nd` →
`netif->lock` → driver → mbuf) is consistent with this: the protocol
spinlocks never nest `arp`, `nd` or `netif` (ARP resolution and
transmission run after the protocol lock is dropped), and the driver locks
appear only under the socket mutex through `transmit`.

To regenerate the lists: `grep 'lockdep: edge' out/x86_64-debug/boot-test.log | sort -u`.

## Running

```sh
make test                          # debug default: checker live
QEMU_SMP=1 make test
make ARCH=aarch64 test
make host-test                     # test_lockdep among the host tests
make BUILD=release all test        # LOCKDEP=0 by default
make BUILD=release LOCKDEP=1 OUT=out/release-lockdep kernel # checker enabled
```

## Gaps

- The graph covers what the boot runs exercise; a lock order only a
  production workload takes is recorded only when that workload runs on a
  build with `LOCKDEP=1`.
- No kernel-level futex race test; the lost-wake argument for `wake_seq` is
  by construction and exercised by the musl tests.
- The `vfs-concurrency` stress found no fault before the fixes were applied
  under the checker's own boots because the checker already refuses the old
  rename order; the pre-fix ABBA was reproduced only by review.
- Trylocks add no blocking edge. Successful IRQ trylocks add no IRQ-use
  label, while successful thread trylocks with IRQs enabled are included in
  IRQ-safety validation.

## October hardening regression coverage

### Concurrent graph model

`tests/host/test_lockdep_threads.c` runs four writers and two diagnostic
readers against the real graph helpers. An acquire/release atomic word
models the kernel raw writer lock. Writers register classes, validate IRQ
usage, and check and insert edges under that lock; readers capture the
published class range under the lock, then inspect immutable names/kinds,
atomic usage flags, and atomic edge words without it. Opposing edge
attempts and changed usage labels exercise rejected cycles and IRQ paths.
After joining, an independent transitive-closure oracle checks acyclicity,
edge counts, and the absence of IRQ-used to IRQ-enabled paths.

`make host-test` includes this model under ASan/UBSan. Run
`make host-test-lockdep-tsan` separately for ThreadSanitizer (the runtime
cannot be combined with ASan); it builds a distinct host binary and fails
on a race report. This requires a host compiler and runtime supporting
TSan. The model checks graph publication and access discipline, not kernel
interrupt entry, CPU migration, held stacks, timer-profile lifetime, or
NMI reentrancy. Readers additionally capture private graph snapshots under
the raw lock and verify their edge counts and metadata ranges outside it
while writers continue. The single-threaded `snapshot` case mutates and
frees its source graph before verifying that the captured graph is intact.

The same sanitizer binary also tests remote held-stack snapshots: a
stopped mid-update writer is refused immediately, an oversized count is
rejected, and readers racing a writer must see one complete generation
across every entry and field. The kernel `lockdep-order` regression checks
snapshot publication from real acquisitions, irqsave metadata, a release
out of order, and the final empty stack.

The same host generation test also runs with the eight-entry mutex capacity,
allocating exactly that many source/output entries. An oversized count must
be refused before any array access. `lockdep-mutex` checks actual mutex and
trylock metadata, removal out of order, an unfinished writer, and the final
empty stack. It also reads a referenced writer thread's initial held pair,
samples during 1024 acquire/release rounds, and retains an extra reference
across join to verify the exited stack is empty. The accepted concurrent
sample count is logged; busy reads are allowed and must return count zero.
If the worker does not acknowledge completion within two seconds, the test
panics while retaining its thread reference and stack-owned probe; it must
neither block in join nor return while the worker can still access the probe.
Only an acknowledged worker is joined for the exited-stack check.
This extends single-target snapshot coverage, not simultaneous global state.

On x86-64, `trap-paranoid` adds real local-APIC NMI delivery while the
validator graph raw lock is held. The handler takes only the bounded
held-stack snapshot: a stable stack must show the held irqsave lock,
and an interrupted update with an odd sequence must return unavailable.
The test requires actual handler completion in each case. Its raw-lock
probe stops after a 100 ms delivery timeout; after releasing test locks,
the caller panics with an explicit diagnostic while retaining the handler
and its live stack argument. A pending NMI cannot be safely cancelled,
so the failure path must neither return nor send another probe.
The raw-lock
probe hook exists only with `CONFIG_LOCKDEP && CONFIG_SELFTEST`; it is not
a production callback API. This validates the NMI-safe snapshot reader,
not tracked lock acquisitions or graph mutations from NMI/#MC handlers.

### Failure detection and boundary cases

The valid IRQ-restore control runs without arming a report expectation.
Only deliberately invalid operations arm one: a zero-report control must
not leave an expectation that could suppress a later real violation.

Host `irq-dependencies` covers a maximum-node path and usage-last conflicts; `irq-oracle` compares every pair against an independent transitive-closure implementation over 32 generated DAGs. Host `metadata-lifetime` frees/reuses a name buffer and checks content
identity, kind separation, maximum length and overlong rejection.
`path-bounds` builds a 1280-node chain, iterates the returned bounded path
under ASan/UBSan, checks a zero-capacity request and self reachability.
`lockdep-order` additionally observes nine edges independently, detects
the closing ten-lock cycle, and checks an unheld release without performing
an invalid primitive unlock. `module-load` now uses a fixture lock with a
name in unloadable module rodata, checks stable class count on reload, and
the final graph dump accesses that copied name after unmapping the module.

`timer-cancel-sync` holds a lock while a real timer callback blocks on it
and attempts synchronous cancellation. The expected callback-lock report
returns the test from the wait; after releasing the lock, the callback
completes and a subsequent cancellation confirms it is idle.

`LOCKDEP=0 OUT=...` tests debug without instrumentation; `BUILD=release
LOCKDEP=1 OUT=...` enables the checker in release. Use separate output
trees for flag overrides; make does not track changes to command lines.
