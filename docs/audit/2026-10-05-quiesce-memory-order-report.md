# Quiescence memory ordering: models, negative controls, and an onlining fix

Date: 2026-10-05. Branch `quiesce-memory-order` from `main` at `28f1323b`.
Scope: `docs/plan.md` §3, "Validation — memory ordering": TSan host models
and memory-order litmus tests for the lifetime protocol itself, with
targeted negative controls.

## Summary

The epoch protocol's orderings were documented and reviewed, and the
threaded host model ran only under ASan/UBSan. Three checks now cover them,
each with negative controls that must fail:

| Check | What it proves | Command |
|---|---|---|
| Six litmus tests, herd7 7.58, RC11 model | every execution the C11 model permits, for each pattern: grace period race-free, unlink visible after W1, the release sequence through two waiters, onlining, the grace-period wake | `make litmus` |
| Epoch-core host model under TSan | every reader access happens-before its free, via the protocol's own atomics, on real threads (five tests, three threaded, two of those new) | `make host-test-quiesce-tsan` |
| Hardware store-buffering run (Apple M1) | the onlining reordering occurs on shipping hardware | `tests/litmus/hw/online_sb.c` (manual) |

**The litmus model found one real ordering hole.** A grace period read the
online CPUs *before* advancing the epoch, and a CPU coming online
published *before* marking itself online with a release store. Under RC11
a waiter can miss the new CPU while that CPU's first read section misses
the unlink. The section can then hold the object past its free
(`online-old.litmus`: allowed). Two `seq_cst` fences fix it (W1b in the
waiter, Q0 in `sched_start_cpu`), and removing either one allows the
outcome again. The window was AP bring-up (`quiesce_init` runs before
`smp_init`), so it was latent. CPU hotplug, an open plan item, would have
made it routine.

Everything else held: no other order is weaker than its claim, and the
grace-period wake, which looks like store-buffering, is protected by the
waitqueue lock.

## Baseline

- `main` at `28f1323b`. The orders were `quiesce_core.h` W1
  `add_fetch(seq_cst)`, W2 `load(acquire)`, Q1 `load(acquire)`, Q2
  `exchange(acq_rel)` (the documents still said "release store");
  `sync_quiesce_counting` read `cpu_online_mask()` before W1;
  `sched_start_cpu` published, then stored `online` with release.
- `docs/kernel/quiesce/invariants.md` Q3 and the
  [lifetime report](2026-09-lifetime-quiesce-report.md) §3 recorded the gap:
  "no TSan run (Apple clang lacks it for this target); no litmus-level
  model check". TSan does run on this host (clang 21, arm64). The tree
  already had TSan targets for lockdep and interrupts, but none for this
  model.
- Tools: herdtools7 7.58 (opam, OCaml 5.2.1) locally; Debian testing/sid
  package 7.58-2 in CI. herdtools7 is not in Debian trixie, the main CI
  job's container.

## Litmus tests

`tests/litmus/quiesce/`, checked by `tests/litmus/run_litmus.py` against
`rc11.cat` (Lahav et al., "Repairing Sequential Consistency in C/C++11",
PLDI 2017). The model was calibrated on textbook store-buffering first:
release/acquire allowed, `seq_cst` fences on both sides forbidden, a fence
on one side only allowed.

| File | Pattern (kernel orders) | Verdict |
|---|---|---|
| `gp.litmus` | section's plain read; Q1, Q2 ∥ W1, W2, then the free as a plain write if the target was seen | **race-free**; the free is reachable |
| `unlink.litmus` | unlink (relaxed); W1 ∥ Q1, Q2, then a lookup | the lookup after publishing the new epoch cannot find the object: **forbidden** |
| `two-waiters.litmus` | P0 unlinks and advances 0→1; P1 advances 1→2; P2 publishes having read 2 | P2 cannot miss P0's unlink: **forbidden**. The release sequence through P1's RMW is what makes `>=` sound |
| `online-old.litmus` | the old onlining order | waiter skips the CPU while its reader finds the object: **allowed** |
| `online.litmus` | W1, W1b, then the snapshot ∥ online store, Q0, then the reader | **forbidden** |
| `wake.litmus` | Q2, then the waitqueue checked under its lock ∥ enqueue under the lock, then the CPUs re-read | lost wake: **forbidden** |

**Witnesses.** Each conjunct of a forbidden outcome is checked reachable
on its own (eight checks), so no "forbidden" verdict comes from a
condition no execution could approach.

**Negative controls.** Each weakens one order or removes one step, and
each must flip the verdict:

| Control | Result |
|---|---|
| `gp`: Q2 relaxed | data race |
| `gp`: W2 relaxed | data race |
| `unlink`: Q1 relaxed | allowed |
| `unlink`: W1 relaxed | allowed |
| `two-waiters`: the second waiter loads and stores instead of an RMW | allowed |
| `online`: no W1b | allowed |
| `online`: no Q0 | allowed |
| `wake`: `waitqueue_empty` without the lock | allowed |

Writing the controls corrected the models three times, recorded because
each was a vacuous result that read as a pass:

1. The first `gp` modelled the free as a store that the reader's earlier
   load would have to read. That is a load-buffering cycle, which RC11
   forbids for *every* order, so Q2 and W2 relaxed stayed "Never". The
   hazard is a data race between the section's access and the free, so
   `gp` now uses non-atomic accesses and RC11's race check, and its
   controls produce the race.
2. The first fix added a second publish after the AP marks itself online.
   A control that removed it stayed "Never": the fences alone forbid the
   outcome. The second publish was dropped from the fix.
3. The first `wake` control removed the publisher's lock operations but
   kept the non-overlap condition that refers to them, which made that
   condition unsatisfiable. The control now rewrites the condition too.

## TSan host model

`tests/host/test_quiesce.c`, built a second time with `-fsanitize=thread`
(`make host-test-quiesce-tsan`). The orders are named macros in
`quiesce_core.h` (`QUIESCE_MO_Q1`, `_Q2`, `_W1`, `_W2`, `_FENCE`), which
only the negative builds override, so the model runs the kernel's code.

- `threads` (existing), plus two new tests:
  - `two-waiters`: two updaters reclaiming concurrently. Each publishes
    its own CPU while waiting, and the epoch must end at exactly 2000.
  - `online-late`: four CPUs come online mid-run in the kernel's order,
    and the waiter reads the mask after W1/W1b.
- Negative builds Q2-relaxed and W2-relaxed: each must report
  `WARNING: ThreadSanitizer: data race` (write in the reclaim against a
  reader's earlier read). Both do.

**A TSan blind spot found on the way.** The negative builds first ran
clean, even with *every* order relaxed. Bisecting showed that TSan's
`memset` interceptor on this platform does not report a race against
earlier reads, and `free` reports only later accesses. The model poisoned
with `memset` then `free`, so the conflicting write was invisible to TSan.
The poison now starts with a scalar store to `magic`, the field every
reader checks. The minimal reproduction: a relaxed handover followed by
`obj[0] = …` is reported; the same handover followed by `memset(obj, …)`
is not.

## Hardware

`tests/litmus/hw/online_sb.c` on the M1 (2,000,000 iterations per run):

| Order | Forbidden outcome |
|---|---|
| old (release store, acquire load, no fences) | 5, then 0 |
| fenced (W1b/Q0 equivalent) | 0, 0 |

This shows the reordering happens on shipping hardware at a rate no boot
test could rely on. It is why the verdicts come from the model and not
from a stress run.

## The fix

| Site | Change |
|---|---|
| `kernel/core/quiesce.c`, `sync_quiesce_counting` | `cpu_online_mask()` moved after `quiesce_core_begin` and `quiesce_core_after_begin()` (W1b) |
| `kernel/scheduler/sched.c`, `sched_start_cpu` | `quiesce_core_after_online()` (Q0) right after the release store of `pc->online` |
| `kernel/include/kernel/quiesce_core.h` | the two fence helpers; the orders as named macros |

Generated code, checked by disassembly:
- **x86-64:** W1b and Q0 are each `lock orl $0x0,(%rsp)`. W1 is
  `lock xadd`, already a full barrier on x86.
- **AArch64:** one `dmb ish` at each site.

A grace period costs milliseconds, so the cost is not measurable.

## Validation

On `a34c3edc` (the fix and both models; the docs commit follows it).
Every row ran, and each verdict is the tool's own line (`out/qval/*.log`).

| Step | Result |
|---|---|
| `make litmus` (herd7 7.58, RC11) | PASS: 6 verdicts, 8 witnesses, 8 negative controls |
| `make host-test-quiesce-tsan` | PASS; Q2-relaxed and W2-relaxed builds each report a data race |
| `make host-test` (ASan/UBSan, all host suites) | PASS |
| x86-64 debug, 4 CPUs | PASS 125.2 s |
| x86-64 debug, 2 CPUs (`test-smp2`) | PASS 123.6 s |
| x86-64 `test-chaos` | PASS 129.2 s |
| x86-64 release build + boot | PASS 17.0 s |
| AArch64 debug, 4 CPUs | PASS 131.8 s |
| AArch64 debug, 2 CPUs | PASS 137.3 s |
| AArch64 `test-chaos` | PASS 140.0 s |
| AArch64 release build + boot | PASS 20.2 s |
| `make analyze`, both architectures | "static analysis: clean"; this incremental run printed 3 warnings, all in code this branch does not touch (`sched.c:667` -- the earlier line 662, moved by the Q0 insertion -- `smptest.c:1763`, `devtest.c:481`) |

All 11 quiescence-family self-tests (`quiesce-*`, `irq-sync`,
`timer-cancel-sync`) passed in each of the four debug boots. A one-CPU
boot was not run: neither fence is on a path that runs without a second
CPU, other than W1b in a grace period, which every boot above takes.

## Remaining

- The litmus files are hand transcriptions of the code's orders. A change
  in `quiesce_core.h`, `quiesce.c` or `sched_start_cpu` must be mirrored in
  them. Nothing generates them from the source.
- The models cover the epoch core, onlining and the wake. Other lifetime
  users (`call_quiesce`'s list, `module_owner_of`, `blk_unregister`'s
  Dekker pair, `timer_cancel_sync`) keep their reviewed arguments
  (lifetime report §3). The `blk` pair is `seq_cst` throughout, and the
  others ride on locks.
- The lockdep and interrupt TSan targets are still not in CI. This
  increment added only the quiescence one.

## Docs changed

- `docs/kernel/quiesce/design.md`: the ordering pseudocode (snapshot after
  W1b, Q2 as an `acq_rel` exchange), a "CPUs coming online" section, and
  the wake argument.
- `docs/kernel/quiesce/testing.md`: a "Memory ordering" section, the new
  tests and commands. The "no TSan run" gap is removed.
- `docs/kernel/quiesce/invariants.md` Q3: the full set of orders and its
  checks.
- `docs/plan.md` §3 item checked; next-increments item 2 struck through.
- `docs/audit/2026-09-deferred-work-inventory.md`: §4's ordering bullet and
  the TSan-models row.
- `docs/history/subsystem-units.md`: one entry.
