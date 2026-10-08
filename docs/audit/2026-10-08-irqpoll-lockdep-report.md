# irq_poll under lockdep, module symbols, and the rule-removal rule (2026-10-08)

A follow-up to PR #334 ([irq-budget report](2026-10-08-irq-budget-report.md)).
It covers four items: irq_poll's teardown waits made visible to lockdep;
those waits made real sleeps; module text symbolised by the boot harness;
and one rule for traffic in flight when a firewall or NAT rule is removed.

## 1. irq_poll's waits are lockdep callback waits

**The hazard.** `irq_poll_disable` and `irq_poll_synchronize` waited for a
running `poll` in a `sched_yield` loop that lockdep could not see. That is
the shape `timer_cancel_sync` had before callback classes
([callback-classes report](2026-10-05-lockdep-callback-classes-report.md)):
a teardown holding a lock that the driver's poll takes would deadlock
silently.

**What changed** (`kernel/core/irqpoll.c`; lockdep design.md, "Callback
classes", irq_poll):

- Every call of `poll` runs inside a callback class named by its function
  (`run_poll`: `lockdep_callback_enter`/`exit`), in the handler and in the
  `irqpoll/N` worker. The class is cached in `struct irq_poll`'s
  `lockdep_class`.
- `irq_poll_set_class` lets a shared poll name the class by the function
  that really runs. Every virtqueue's poll is the core's `virtq_poll`, which
  calls the driver's callback, so `virtq_alloc_on` names the class by the
  callback: `vblk_done`, `vnet_rx_done`, `vnet_tx_done`, `vrng_done`. A
  single `virtq_poll` class would have checked one driver's teardown
  against every other driver's locks.
- Both waits call `lockdep_callback_wait` on every call, then
  `might_sleep`. A held lock that a poll takes is reported as
  `LOCKDEP_R_CALLBACK`, and the wait is skipped (a disable still disables
  and unqueues). Any other spinlock is `might_sleep`'s report. The report
  name is now "a callback wait (timer_cancel_sync, irq_poll) holds a lock
  the callback takes".
- The worker runs `poll` with preemption off. The class sits on the CPU's
  held stack (L11), which belongs to this thread only while no other thread
  runs on the CPU. `poll` never sleeps, since it also runs in interrupt
  context.

**What it adds over `might_sleep`, honestly.** Because the waits sleep, a
waiter can hold no spinlock (release builds panic in `might_sleep`), and
a poll can take no mutex. So a mutex-holding waiter cannot close a cycle
through a poll's class. The class adds three things:
- it names the deadlock as one, where `might_sleep` only says "sleep in
  atomic context";
- it records the wait-first edge before `might_sleep` reports;
- it covers a poll that waits for its own irq_poll, and any non-spinlock
  class a future poll reaches.

The probe below shows this directly: with the annotation removed, the same
case is still caught, but only as a sleep report.

**No report on the current tree.** Debug boots at one, two and four CPUs
on both architectures with LOCKDEP=1 (§7) print only the self-tests'
expected reports.

**IRQ usage.** A poll now runs in two contexts: interrupt context in the
handler, and a thread with interrupts on in the worker. A read of every lock
reachable from the six polls found every one taken with
`spin_lock_irqsave`:

- **virtio:** `vq->lock`, the transport's `v->lock`, `vb->lock`;
- **block layer:** `bd->qlock`;
- **completions:** `c->lock`;
- **DMA, IOMMU, kmalloc and slab** locks;
- **network:** the mbuf queues;
- **random:** `g_lock`;
- **NVMe:** `q->lock`;
- **xHCI and USB:** `x->lock`, HID, tty, signal and the process table.

The only plain `spin_lock`s are in irqpoll.c itself, and they nest inside an
irqsave. No poll path sleeps or takes a mutex. A plain lock in a worker pass
would be held with interrupts on, and `check_usage` would report it the
first time the worker ran that poll. The worker ran `vblk_done` in every
`blk-irq-budget` storm of this unit.

**Self-test `lockdep-irqpoll`** (testing.md). A poll run as a handler
runs it takes A. Then, holding A:
- `irq_poll_synchronize` on a second irq_poll of the same function reports
  once;
- `irq_poll_disable` reports once, and the irq_poll is still disabled
  afterwards.

With A released, both waits are silent and real. A poll that defers and
takes W only in the worker makes a wait holding W report. An unrelated
spinlock across the wait gives only the sleep report. Waiting first (a
sleep report, with the edge recorded) and then running the poll that takes
C gives the inversion.

**Probe** `tools/lockdep-irqpoll-probe.py`. Each mode removes one part of
the annotation in a throwaway worktree, and the test then dies on an
unexpected sleep report at the wait. The `ok` line never prints, and for
`no-worker-class` the handler case's two expected reports come first.

| mode | x86-64 | AArch64 |
|---|---|---|
| `no-wait` (the waits do not acquire the class) | PASS | PASS |
| `no-class` (no poll runs inside it) | PASS | PASS |
| `no-worker-class` (the worker's call outside it) | PASS | PASS |

**A lockdep defect the probe surfaced.** The first AArch64 `no-wait` run
printed the sleep report interleaved with two bogus ones from CPU 0's idle
thread:

- "irqsave acquisition is missing from the per-CPU held stack";
- "release of a lock that is not held".

The first report sets `g_off`. A spin_lock on another CPU then skipped its
push, and `lockdep_irqsave_acquired` (which never checked `g_off`) reported
the push missing. Now `lockdep_irqsave_acquired` and
`lockdep_irqrestore_check` return while the checker is off, and `report()`
drops any report that finds it already off (`__atomic_exchange_n`). The
rerun printed only the real report. This has been latent since the raw
interrupt-pairing unit: any fatal report on a busy SMP boot could garble
the console this way.

**Class table.** The peak is 440 of 512 (85%) on the x86-64 four-CPU debug
boot. The completion-waits tidy-up recorded 408 on 2026-10-07, before #330 to #334 and this unit. That is still below the 90% warning, but
the margin is about 20 classes. The figure for each validation boot is in §7.

## 2. The waits sleep

`irq_poll_disable` and `irq_poll_synchronize` used to yield in a loop.
They now sleep on the irq_poll's `idle_wq`:

- The waiter raises `waiters` under the irq_poll's lock before it first
  reads its condition.
- Every finish (handler or worker), and the worker dropping a disabled
  deferral, reads `waiters` under the same lock and wakes the queue after
  releasing the lock. So a finish after the raise wakes the waiter, and a
  finish before it is seen by the condition.
- The hot path pays one load of `waiters` under a lock it already holds.

The semantics are unchanged: disable unqueues a deferral and waits for
`running` and `scheduled` to clear; synchronize waits only for the run
current at the call (`runs` changes or `running` clears). The design doc
gave no reason to keep the busy-wait.

## 2a. A livelock the validation found: the worker's priority, bounded

The first validation matrix failed two x86-64 boots in the same place:
`virtio-remove-inflight` took 36 s at `QEMU_SMP=2`, and under the chaos
migrator its boot timed out at 240 s, with the test 211 s in. The
watchdog's dump named it. `irqpoll/0`, at the highest priority, had run
8 s with 3 switches. `kmain`, the test thread, was "READY on cpu 0 for
8129 ms behind 'irqpoll/0'". The test's submitter (`vrm`, CPU 1) kept the
queue refilled, so the worker's poll always used its whole budget, and its
`sched_yield` gave way to nothing. The thread that would have stopped the
submitter could not run. (The worker's frames in that dump were the first
module frames the new symboliser resolved in anger: `vblk_done
[virtio_blk]`, `virtq_poll [virtio]`, `run_poll`.)

This is #334's stated cost ("a device that never runs dry keeps that
CPU's threads waiting as long as it lasts"), and here it is a livelock.
The CI run of #334 did not hit it: `virtio-remove-inflight` was not among
any boot's slowest tests. Whether this unit's per-batch cost (the lockdep
class and the preemption bracket) made it likelier was not measured. The
mechanism needs neither.

**The bound.** A backlog keeps its worker at the highest priority for
`IRQ_POLL_HOLD_NS` (10 ms, one slice), measured from the wake that began
it. Past that, the worker drops to `SCHED_PRIO_DEFAULT` with
`sched_reprioritize` and time-slices with the CPU's other threads until
its list is empty. It then returns to the highest priority before it
sleeps. The worker holds no mutex, so priority inheritance never touches
it. The remainder still always runs, because the worker never leaves its
run queue. This is the ksoftirqd shape: prompt first, then fair.

**The test.** `blk-irq-budget` now runs a default-priority bystander pinned
to every CPU, spinning, each recording its longest wait to run while the
storm is on. The bound is 250 ms, the same load-sensitive class as the
tick gap.

| `QEMU_SMP=2`, x86-64 | before the bound | after |
|---|---|---|
| bystander's longest wait (CPU 0) | 998 ms (the whole storm): FAIL | 14 ms |
| longest tick gap | 5 ms | 5 ms |
| `virtio-remove-inflight` | 36,322 ms (matrix) | 152 ms |
| completions in the 1 s storm | 11,841 | 2,720 |

The storm's completions fall because the worker now shares its CPU with
the bystander. That is the point of the bound, and the test's own
non-vacuity checks (eight rounds, next completion ready 90% of the time)
still pass.

## 3. Module symbolisation (irq-budget report §10)

**The loader** prints one stable line per module after `module: loaded`:

    [ INFO] module: base virtio_blk text 0xffffffff88014000 size 0x2000 rodata 0xffffffff88018000 data 0xffffffff8801b000

**The boot harness** (`symbolize` in `tests/boot/run_boot_test.py`):

- It reads those lines, and resolves an address in `[text, text + size)`
  against `out/<arch>-<build>/modules/<name>.ko.unsigned`. The default
  modules directory is next to `--kernel`; `--modules` overrides it.
- It lays out the text group as `modelf_parse` does (each allocatable
  executable section in section order, at its alignment). The function
  name comes from the object's symbol table. The line comes from
  `llvm-symbolizer` over the object.
- A module with several text sections gets a temporary copy whose sections
  are placed at their group offsets (`llvm-objcopy
  --change-section-address`). `virtio.ko` has `.text` and
  `.text.unlikely.`.
- The latest load covering an address wins.
- Host test: `tests/boot/test_module_symbols.py` (9 checks, in
  `host-test`).

**Shown on a probe-forced stall in a module.**

*x86-64*, `tools/irq-budget-probe.py --old --storm-ms 12000` (the budget
removed). The hard-lockup sample of CPU 0, as the harness printed it:

    hard lockup: cpu 0 no tick for 10000 ms
      0xffffffff8800b7d2  vpci_notify [virtio]      drivers/virtio/virtio_pci.c:227
      0xffffffff88014d36  vblk_submit [virtio_blk]  drivers/virtio/virtio_blk.c:0
      0xffffffff8007f081  to_driver                 kernel/block/blk.c:583
      0xffffffff8007e913  drain_pending             kernel/block/blk.c:534
      0xffffffff8801470b  vblk_done [virtio_blk]    drivers/virtio/virtio_blk.c:0
      0xffffffff8800a7eb  virtq_poll [virtio]       drivers/virtio/virtqueue.c:58
      0xffffffff80006264  run_poll                  kernel/core/irqpoll.c:70
      0xffffffff80100272  x86_trap_dispatch         kernel/arch/x86_64/trap.c:87

Before the multi-section placement, the `virtio` frames carried names but
`??` lines.

*AArch64* has no NMI, so a CPU with interrupts masked cannot be sampled
(`lockup-sample-irqoff` records this). The `--old` 12 s storm also split
into 7.5 s and 4.5 s tick gaps, below the 10 s hard-lockup line. The stall
shown there is instead the budgeted tree's worker under a 14 s storm
(`--storm-ms 14000`). `irqpoll/0` passes the soft-lockup line, which the
design accepts ("the soft-lockup detector reports it past 10 s"). Its
sample, from the harness's table:

    soft lockup: cpu 0 running 'irqpoll/0' for 10000 ms
      #1 0xffffffff8040b658  vpci_notify [virtio]      drivers/virtio/virtio_pci.c:227
      #2 0xffffffff80414bd8  vblk_submit [virtio_blk]  drivers/virtio/virtio_blk.c:0
      #3 0xffffffff80081aa4  to_driver                 kernel/block/blk.c:583
      #4 0xffffffff80081368  drain_pending             kernel/block/blk.c:534
      #5 0xffffffff80414664  vblk_done [virtio_blk]    drivers/virtio/virtio_blk.c:287
      #6 0xffffffff8040a7a8  virtq_poll [virtio]       drivers/virtio/virtqueue.c:58
      #7 0xffffffff800068ac  run_poll                  kernel/core/irqpoll.c:70

The same 14 s storm on x86-64 gives the same frames (`irqpoll/0` past
10 s). A return address on a call's last instruction maps to line 0 here
(`virtio_blk.c:0`), as it does for kernel frames. The harness does not
subtract one from return addresses, for either.

## 4. When a rule is removed while traffic it allowed is in flight

**Before this unit:**

- A firewall flow outlived the rule that accepted it, as Linux conntrack
  does.
- `nat_pf_del` reaped its forward's DNAT translations. Since #331,
  `nat_pf_clear` reaps all of them. Linux keeps those entries until they
  expire or are flushed.

**The rule chosen, network invariant N28.** Removing a rule ends the state
that copies the rule, and only that.

- A DNAT translation holds a copy of its forward (the guest address and
  port it rewrites to), so it ends with the forward.
- A firewall flow holds only the endpoints of traffic a rule admitted. It
  names no rule, so it outlives rule deletes, a new DROP rule and policy
  changes.

The reason: a kept copy goes on acting as the rule. A forward removed and
re-added for the same host port with another target would steer a live
client to the old guest, and while no forward is listed, the closed port
would still reach a guest. A kept flow acts as nothing the operator
configured.

Firewall and NAT already behaved this way, so this is a statement and an
oracle, not a behaviour change.

**Oracles** (`tests/fuzz/fuzz_net_config.c`):

- The existing check already holds: every DNAT entry matches a forward in
  the model by protocol, host port, guest address and guest port.
- New: a rule add or delete, or a policy change, leaves the firewall's
  flows exactly as they were (same list, same order, read at one clock),
  and a forward delete or clear also leaves every masquerade entry as it
  was.
- Control: a mutant `nat_pf_del` that reaps masquerade entries too fails
  with "a forward delete changed the masquerade entries (1 -> 0): N28".

## 5. Tests and tools added

- `lockdep-irqpoll` (self-test), `tools/lockdep-irqpoll-probe.py`.
- `tests/boot/test_module_symbols.py` (host-test).
- `fuzz_net_config`: the N28 kept-state oracle.

## 6. Found, not fixed here

The IRQ-usage audit read every teardown path and turned up two items for
the inventory (§1.3):

- **virtio-net's remove drains before the poll stops.** `vnet_remove`
  resets the device and pops both queues before `virtq_free` releases the
  vector and disables the irq_poll. A handler or worker pass in that window
  still runs `vnet_rx_done`:
  - it decrements `rx_posted` without a lock;
  - it hands a frame to the unregistered interface;
  - it re-posts buffers to the reset device, which the free never returns.

  The drain also frees without `dma_unmap`. This predates #334, since the
  handler was live in the same window. It was found by reading; it is not
  yet reproduced.
- **Fault-injection points skip interrupt context.** `FI_BLK_COMPLETE`,
  `FI_NET_RX_DUP` and `FI_KMALLOC` can therefore fire in the worker's half
  of a poll and never in the handler's.

## 7. Validation

Filled in from the run of HEAD; QEMU at nice 0 throughout.

(see the PR description for the final table)
