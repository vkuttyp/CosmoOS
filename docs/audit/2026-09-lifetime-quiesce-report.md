# Prompt #3 final report: critical fix pass and kernel object lifetime & quiescence

Date: 2026-09-05. Branches: `fix/critical-pass` (Phase A, merged as PR #16,
65455d4) and `lifetime/quiesce` (Phase B, this report's PR). Source of
truth for every claim below: the code, the tests that ran, and the docs
under `docs/kernel/quiesce/`.

## 1. Critical fix summary (Phase A, PR #16, merged)

| # | Finding | Class-level fix | Regression test |
|---|---|---|---|
| 3.1 | TCP `seg_mss` took the interface registry mutex under the TCP spinlock | Path MSS decided before the lock and cached per pcb; registry lock became a spinlock; every sleeping primitive panics when entered with `preempt_count != 0` | `net-tcp-mss` |
| 3.2 | VirtIO trusted device-writable descriptor fields | Driver-private shadow chains; used ring validated and clamped; the table is write-only | host `test_virtq` with a hostile peer |
| 3.3 | No FPU/SIMD state ownership | `arch/fpu.h`: per-thread state, eager switch, XSAVE/FXSAVE, per-guest areas, XSETBV intercepted, CPUID masked | `fpu-switch`, `hv-guest-fpu`, user `--fpu-partner` |
| 3.4 | LAPIC ICR write pair interruptible | `icr_write_pair` under `arch_irq_save` | `smp-ipi-storm` |
| 3.5 | NMI/#MC without IST and with SWAPGS windows | IST slots for #DF/NMI/#MC/#DB; paranoid entry deciding SWAPGS from the GS base MSR | `trap-paranoid` |
| 3.6 | No credential model | POSIX ruid/euid/suid + groups, one privilege predicate, VFS permissions, syscalls 50–55, user exceptions kill the process | host `test_cred`, `process-fault`, `--unpriv-test` |
| 3.7 | Signing private key in git | Keys outside the tree (`COSMO_KEYDIR`), leaked key revoked in `tools/keys/REVOKED`, `check-secrets.sh` in `check-tools` and CI | build-time check |

Second targeted audit findings were fixed in 013ad09 (user-mode exceptions
no longer panic the kernel, reserved ports judged at bind time, sticky bit,
FSGSBASE, CPUID leaf 7, single-load used elements, atomic key generation,
make version check).

## 2. Lifetime architecture (Phase B)

The invariant: **an object cannot be reclaimed until every CPU that could
still hold a reference to it has passed through a provably safe quiescent
state.** Three layers deliver it.

**The mechanism** (`kernel/core/quiesce.c`, `kernel/include/kernel/quiesce_core.h`).
A read-side section is a preemption-disabled region: `quiesce_read_lock`
is `preempt_disable` plus a debug depth counter, and every spinlock and
every interrupt handler is implicitly one. A CPU is quiescent at exactly
five points: interrupt return to a context with `irq_depth == 0`,
`preempt_count == 0` and interrupts enabled in the interrupted frame;
`schedule_internal`; the idle loop before halting; `sched_start_cpu`; and
`synchronize_quiesce` itself. Grace periods are epoch-based: one global
epoch advanced by waiters, one cache-line-aligned record per CPU written
at quiescent points. `synchronize_quiesce` sleeps in half-tick steps and
kicks stragglers with a reschedule IPI; `call_quiesce` batches deferred
frees on one worker thread. The generic API contains no architecture
logic; the arch tails call one function.

**The synchronous unregisters.** `synchronize_irq` and
`interrupt_unregister_sync` (a handler is a read-side section; the
dispatch table publishes immutable `{fn, arg}` records so a handler never
runs with another registration's argument; `irq_release` and
`irq_release_msi` wait with the IRQ lock dropped and free the vector only
after). `timer_cancel_sync` (the queue names its running callback; the
caller spins on it and re-cancels, so a re-arming callback is stopped).

**The object rule.** Every kobject type has a release that frees the
memory and nothing else does; registries take their own reference and
drop it on unregister; the creator drops its own; lookups return
referenced pointers; the owner module of the release code is recorded so
`module_unload` waits for the objects (or leaves a zombie). Converted:
`struct device` (PCI, virtio-pci), `struct blkdev` (virtio-blk, with
`gone`/`submitting` so a removed device refuses bios), `struct netif`
(virtio-net and `lo`, with a six-step unregister), TCP pcbs
(`timer_cancel_sync`, accept attaches under the lock), sockets
(`kobject_tryget` across wakes). The handle table was audited and left
unchanged: it already took references under its lock and dropped them
outside it.

Full design: `docs/kernel/quiesce/design.md`; interfaces:
`docs/kernel/quiesce/api.md`; rules with their checks:
`docs/kernel/quiesce/invariants.md`.

## 3. Memory ordering

Every barrier is named in `design.md` and repeated in the code comments:

| Site | Operation | Why this strength |
|---|---|---|
| W1 waiter | `add_fetch(epoch, 1, seq_cst)` | the caller's unlink (before W1) must be ordered before the new epoch is visible to any CPU, in one total order with other waiters and with the publishers' acquire loads |
| W2 waiter | `load(cpus[c].seen_epoch, acquire)` | pairs with Q2: when it observes `>= target`, every access in that CPU's read sections before its quiescent point happens-before the waiter's free |
| Q1 publisher | `load(epoch, acquire)` | a later read section on this CPU sees every unlink that preceded the epoch advance |
| Q2 publisher | `store(seen_epoch, e, release)` | orders the CPU's earlier read-side accesses before the value the waiter acquires |
| comparison | `seen >= target` | two waiters may advance twice before one point; a single publish satisfies both |
| interrupt table | release store of the record pointer, acquire load in dispatch | the record's fields are complete before a dispatcher can load the pointer |
| `blk_unregister`/`blk_submit` | `gone` store and `submitting` RMWs, all `seq_cst` | Dekker: a submitter that did not see `gone` has raised `submitting` before the unregister reads it, or the unregister sees the increment |
| `module_owner_of` | acquire loads of `g_live[]` inside a read-side section, `acq_rel` increment | the unloader clears the slot (release), waits a grace period, then reads the count: an increment made under a section that saw the module is visible |
| `timer_cancel_sync` | spinlock acquire/release on the queue lock | `q->running` is read and written only under the lock; the callback's completion is ordered by the lock re-acquisition after it |

The host model (`tests/host/test_quiesce.c`) runs the same inline code
with four reader threads and 2000 reclaim generations under ASan and
UBSan; a wrong grace period is a use-after-free the sanitizer reports.
No TSan run and no formal litmus check were made (Apple clang has no TSan
for this target); that is listed as a gap.

## 4. Concurrency model

- Read side: `preempt_disable`/`preempt_enable`, no atomics, no shared
  writes. A read section must not block; the sleeping primitives already
  panic when entered with `preempt_count != 0`.
- Write side: unlink under the object's own lock, then a grace period
  (`synchronize_quiesce` in thread context with no spinlock held,
  asserted; `call_quiesce` from any context), then free.
- `synchronize_quiesce` cost: a few microseconds with one online CPU;
  otherwise until each other CPU's next tick return or scheduler entry:
  measured 5–6 ms with idle CPUs on QEMU, bounded by the longest read
  section otherwise (31 ms against a 30 ms section).
- Lock-order consequences: `module_unload` holds the module mutex across
  `shutdown()`, the grace period and the live-object wait (all sleep);
  `blk_unregister`, `netif_unregister`, `irq_release*` and
  `interrupt_unregister_sync` are thread-context APIs; `timer_cancel_sync`
  may run under a spinlock only if the callback never takes that lock
  (TCP's callbacks take only the network work lock).
- Preemption: `quiesce_read_unlock` is `preempt_enable`, so leaving a
  read section after waking a higher-priority thread switches at once.
  This exposed a latent ordering assumption in the network self-tests
  (the port of a just-closed connection was assumed free the instant the
  server thread finished) and is why `tcp_transfer` now waits for the
  port; it also shows that a woken thread otherwise waits for the next
  tick or the next `preempt_enable` with interrupts on (see section 8).

## 5. Tests (all ran; all passed on x86-64 with 4 and 1 CPUs and on AArch64 with 4 CPUs)

New in Phase B: `quiesce-grace`, `quiesce-call`, `irq-sync`,
`timer-cancel-sync`, `quiesce-stress`, `blk-lifetime`,
`net-netif-lifetime`, `net-accept-race`, `module-unload-busy` (kernel);
`test_quiesce` (host). The `device` test additionally checks that a
release-less registration is refused. Total: 84 kernel self-tests, 13 host
tests. What each proves is tabulated in `docs/kernel/quiesce/testing.md`.

Verification chain, run on 2026-09-05 from the final tree:

| Step | Result |
|---|---|
| `check-tools` (includes `check-secrets.sh`) | PASS |
| x86-64 debug `test` (4 CPUs) | PASS, 84 tests |
| x86-64 `QEMU_SMP=1 test` | PASS, 84 tests |
| x86-64 `BUILD=release all test` | PASS |
| x86-64 `test-crash` | PASS |
| x86-64 `MODULE_SIG_ENFORCE=0 test` | PASS |
| x86-64 `host-test` | PASS, 13 tests |
| x86-64 `analyze` | PASS |
| x86-64 `reproducible` | PASS |
| AArch64 debug `test` | first run FAIL (virtio-console file lacked the boot-complete line while the serial log had it and all 84 self-tests passed); three reruns PASS. Recorded as a flake under section 7 |
| AArch64 `BUILD=release all test` | PASS |
| AArch64 `test-crash` | PASS |
| AArch64 `host-test` | PASS |
| AArch64 `analyze` | PASS |
| AArch64 `reproducible` | PASS |

## 6. Performance

Measured from the self-test logs (QEMU TCG on an Apple Silicon host, so
absolute numbers are indicative; the ratios are the point):

| Quantity | x86-64, 4 CPUs | AArch64, 4 CPUs |
|---|---|---|
| Solo `synchronize_quiesce` (other CPUs idle) | 5.0–6.2 ms | 5.0 ms |
| Grace period against a 30 ms read section | 31–32 ms | 31 ms |
| `interrupt_unregister_sync` against a 20 ms handler | 22–23 ms | 23 ms |
| `timer_cancel_sync` against a 20 ms callback | 20 ms | 20 ms |
| Reclaim generations in 400 ms under 3 readers (sync + deferred) | 201 | 199 |
| Reads by 3 readers in that window | 4.2–5.8 M | 5.8 M |
| Read-side section cost | one `preempt_disable`/`enable` pair | same |

Steady-state cost to the rest of the kernel: one store per quiescent
point (a cache line private to the CPU), one extra branch in the
interrupt tail, one record indirection in interrupt dispatch, and a
`preempt_disable`/`enable` pair around `netif_transmit` and `netif_rx`.
Module ABI v2 adds one pointer to `struct kobject`.

## 7. Remaining risks

1. **Grace-period latency is tick-bound.** A waiter sleeps in half-tick
   steps and an idle CPU publishes only at its next interrupt return, so
   the floor is one to two ticks (4–8 ms). Callers that reclaim often
   should use `call_quiesce` (one period per batch), as `quiesce-stress`
   does.
2. **A long preempt-disabled section stalls the waiter, not the system.**
   The straggler IPI helps a halted CPU, not one spinning with preemption
   off. The populate loops the audit flagged (5.2) are the known case
   until milestone 5 adds preemption points.
3. **Not exercised by a test**: the straggler IPI (Q6), the
   `blk_submit`/`blk_unregister` window (Q11), the TCP timer-callback/free
   race (N-L3), run-time hot-unplug of virtio devices (only module unload
   drives `vpci_remove`).
4. **Ordering verified by review and sanitizers, not by a model checker.**
   No TSan, no litmus tests.
5. **Woken-thread latency in the scheduler**: a thread woken by a
   lower-priority thread runs only at the next tick, the next
   `preempt_enable` with interrupts enabled, or when the waker blocks.
   Network code, which uses `irqsave` locks throughout, had no such point
   until this pass added one at the end of `netif_transmit`; other
   subsystems still have the latency. Pre-existing, now visible.
6. **The AArch64 virtio-console flake**: once in four runs the console
   file lacked the last line while the serial log was complete. Not
   reproduced; not attributable to this change (the console path was not
   touched), but not explained either.
7. **`nd_flush` and `arp_flush` clear table entries by interface; pending
   packets parked on them are freed.** Correct, but a resolution in
   flight for a removed interface is silently dropped rather than
   reported.

## 8. Architectural debt

- The scheduler's wake path sets `need_resched` and relies on a later
  preemption point; a wake from a lower-priority thread should preempt at
  the first safe point, and `irqsave` unlocks currently skip the check
  because interrupts are off at `preempt_enable`. A deferred-preemption
  flag consumed at `arch_irq_restore` (or an explicit check after the
  restore) would close this generally (milestone 3's lock discipline work
  is the natural home).
- The `netrx` worker runs below default priority; with prompt preemption a
  busy user thread can hold packets in the receive queue for a slice.
  Per-connection locking and a priority decision belong to milestone 8.
- `synchronize_quiesce` polls; a wake-on-publish design (the last pending
  CPU wakes the waiter) would remove the half-tick floor.
- `MODULE_MAX_LIVE` is a fixed 32-slot publication array; fine today,
  a bitmap or a list with publication semantics if module counts grow.
- Zombie modules are reaped only by a later `module_unload` of the name;
  a periodic sweep or a `module_dump` hint would make leaks visible.
- `struct netif` lookups now return references, but ARP and ND entries
  still hold bare interface pointers and rely on the flushes in
  `netif_unregister`; a per-entry reference would remove that coupling.
- Lock-order documentation is by hand; lockdep (milestone 3) would check
  the "no `synchronize_quiesce` under a spinlock" and "callbacks take only
  the work lock" rules mechanically.

## 9. NEXT SUBSYSTEM

**Lock discipline and lockdep** (the audit's milestone 3), not
implemented here. Reasons, in the constitution's order:

- Correctness: this pass added rules that are enforced only by review:
  `synchronize_quiesce` never under a spinlock, `timer_cancel_sync` only
  under locks the callback never takes, `ops->transmit` never sleeping,
  module `shutdown()` never calling back into the loader. A debug-build
  lock-order checker with per-CPU held-lock stacks, a sleeping-under-
  spinlock assertion and an IRQ-safety class check turns each into a
  panic at the first violation. It would also have found the Phase A 3.1
  finding mechanically.
- Cleanliness: the documented lock orders (scheduler S2/S4, VFS V7,
  network design.md) can be checked against reality and the known
  deviations fixed in the same milestone (`vnode_lookup_cached`,
  `futex_wait` copy under the lock, `vfs_sync` and `g_mounts_lock`, the
  rename order).
- Observability: held-lock stacks in the panic report and a `lockstat`
  counter per class.
- It is a prerequisite for milestone 5's preemption points and for the
  wake-latency fix in section 8, which change where sleeping and
  spinning may happen and need the checker to prove they are safe.

Milestone 4 (verification infrastructure: fuzzers, fault injection) is
the alternative; it finds bugs of every class but does not remove the
class this pass and the next one are about.
