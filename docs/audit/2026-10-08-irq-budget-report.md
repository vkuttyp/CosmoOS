# Bounded completion handling in interrupt context

`docs/testing/flakes.md`, "`virtio-remove-inflight` held for 184 s, CPU 0
in virtio-blk's completion loop" (PR #330's x86-64 CI, run 37698731544):
one CPU took no tick for 22 s while `vblk_done` ran, and the held pass
accepted 1.2 million requests where it normally sees about a hundred. The
entry named the next steps: count what one call of the handler pops,
build a run in which the submitter outpaces the handler, then bound the
loop. This unit does the three, finds the same shape in three other
drivers, and bounds them all with one mechanism.

## 1. The mechanism, and how the handler is its own submitter

`vblk_done` pops completions until `virtq_pop` finds the used ring empty,
in interrupt context. Each completion goes to `bio_complete`, which ends
with `drain_pending`: the block layer's queue of bios the driver refused
for want of a slot is resubmitted from there, so the handler hands the
device its next request itself. Against a device that completes at once,
a non-empty pending queue keeps the ring from ever being seen empty. In
CI the queue was kept full by `virtio-remove-inflight`'s submitter on
another CPU; the handler needs no other CPU to keep going, only a device
that has the next completion ready when it asks.

## 2. The instrument and the reproduction

- **Per-call count** (`virtio-blk: count completions popped per vblk_done
  call`): the most popped in one call, the calls that popped more than the
  ring holds, and a debug warning on each such call. Reached by the
  self-tests through the block layer's driver hooks (module ABI 7 -> 8).
- **`blk-irq-budget`** (`kernel/device/devtest.c`): 64 reads on vda, one
  per slot. Each bio's `done` calls `blk_test_resubmit_from_done`, which
  puts it back on the pending queue (what `driver_submit` does on a
  refusal), so the `bio_complete` running it resubmits it. `done` then
  waits, to 1 ms, until the device has finished another request
  (`unconsumed`), so the handler's next pop always finds one. One second
  of that, then: the storm ran (at least eight rounds), the adversary held
  (the next completion ready 90 % of the time), no call popped more than
  the ring, and no CPU went more than 250 ms without a tick. The tick now
  keeps a per-CPU maximum gap in self-test builds
  (`timer_test_tick_gap_max_ns`), the interval the debug "no tick for"
  detector judges.

**The reproduction, before the fix** (`tools/irq-budget-probe.py --tree
before`, the test's own commit; four CPUs unless stated):

| run | completions in the storm | most popped in one call | longest tick gap | verdict |
|---|---|---|---|---|
| x86-64 | 15,790 | 15,754 | 1,002 ms (CPU 0) | FAIL |
| x86-64, one CPU | 18,631 | 15,202 | 1,002 ms | FAIL |
| AArch64 | 16,980 | 16,936 | 1,002 ms (CPU 0) | FAIL |
| x86-64, 12 s storm | 189,882 | 189,841 | 12,000 ms | FAIL, `hard lockup: cpu 0 no tick for 10000 ms ... (seen from cpu 3)`, watchdog `no progress for 12001 ms` |
| AArch64, 12 s storm | 245,124 | 245,082 | 12,003 ms | FAIL, the same two lines |

The device had the next completion ready on every resubmission but one,
in every run. The 12 s runs give the CI sighting's own signature. With
the probe's kernel ELF, the stalled CPU's NMI sample symbolises: module
text, called from `drain_pending` → `to_driver`, inside `isr_common`,
which had interrupted the test thread's own `driver_submit`. The handler
is resubmitting from the pending queue, which is §1's mechanism.


## 3. The audit

Every interrupt-context completion or receive loop the task named, and
what bounds it on `main` (8fd216d5).

| Loop | Context | Bounded on main? By what | Can a device or a submitter keep it running? | Change |
|---|---|---|---|---|
| `vblk_done` (virtio_blk.c) | IRQ | No: until the used ring is empty | Yes: `bio_complete` resubmits from the pending queue (any submitter, or the handler itself); reproduced | Budget + worker |
| `virtq_pop`'s skip loop (virtqueue.c:218) | IRQ (inside every callback) | No: skips bad entries until `last_used == used->idx` | A device: one that keeps advancing `used->idx` with bad ids | At most a ring of skips per call |
| `vnet_rx_done` (virtio_net.c) | IRQ | Yes: by `VNET_RX_BUFS` (32) posted buffers, reposted only after the loop | No | Budget (uniform), same bound |
| `vnet_tx_done` (virtio_net.c) | IRQ | No | Yes: transmits on other CPUs refill the ring while it drains | Budget + worker |
| `vrng_done` (virtio_rng.c) | IRQ | Yes: resubmits from its completion, but only until `VRNG_BUDGET` bytes are collected | No (total bounded) | Budget (uniform) |
| `queue_process` (nvme.c:248) | IRQ, or the polled admin thread | No: until the phase bit flips | Yes: `bio_complete` resubmits, as in virtio-blk | Budget + worker (the admin queue's polled path steps aside in the poll function) |
| `port_complete` (ahci.c, from `ahci_irq`) | IRQ | Yes: one snapshot of PxCI, at most 32 slots per interrupt | No: a slot resubmitted meanwhile is not in the snapshot | None |
| `slots_fail` (ahci.c:279) | Thread (`port_restart`, `disk_detach`, `ahci_debug_presence`) | Yes: `recovering` refuses submissions, so at most 32 | No | None (not interrupt context) |
| `ahci_worker` (ahci.c:941) | Thread | Waits on its waitqueue | n/a | None (not interrupt context) |
| `rx_process` (e1000e.c:105) | IRQ | Yes: the ring (`E1000E_RING`); the tail is written after the loop, so the device cannot reuse a descriptor during it | No | None |
| `tx_reclaim` (e1000e.c) | IRQ | Yes: `tx_used`, under the lock transmit also takes | No | None |
| `ring_flush` (xhci.c:615) | Thread, endpoint stopped | Yes: the ring's requests | No | None (not interrupt context) |
| event loop (xhci.c:997) | IRQ | No: until the cycle bit says caught up | Yes: a completion callback submits the next transfer and the model posts its event while the handler runs (USB storage's chained exchange) | Budget + worker; EHB left set past the budget |
| `xhci_worker` (xhci.c:1182) | Thread | Waits on its waitqueue | n/a | None (not interrupt context) |

## 4. The design: one mechanism, `irq_poll`

`kernel/core/irqpoll.c`, `kernel/include/kernel/irqpoll.h`; the design
section is `docs/kernel/interrupt/design.md`, "Bounded completion
handling".

- The handler calls `irq_poll_sched(ip)`, which runs the driver's
  `poll(ip, budget)` there, with interrupts off. `poll` consumes at most
  `IRQ_POLL_BUDGET` (32) completions and returns how many.
- If it used the whole budget, the irq_poll is queued on this CPU's
  `irqpoll/N` worker (pinned, default priority). The worker calls `poll`
  a budget at a time and yields between batches, re-queueing itself
  while `poll` keeps using its budget.
- One consumer at a time. State is under the irq_poll's own lock:
  `running`, `scheduled`, `queued`, `again`, `disabled`. A handler that
  finds `poll` running or queued sets `again` and returns. Whoever is
  consuming hands to the worker instead of going idle while `again` is
  set.
- `irq_poll_disable` unqueues a deferral, waits for a running `poll` to
  return, and blocks new ones. `irq_poll_synchronize` waits only for a
  `poll` running now. Lock order: the irq_poll's lock, then a worker's
  list lock, never the reverse. Neither is held across `poll`.

The option of disabling device callbacks, draining, and re-enabling
with a recheck (virtio's `VIRTQ_AVAIL_F_NO_INTERRUPT`) was not taken.
It applies to virtio alone, not to NVMe or xHCI, so it could not be one
design. The `again` flag already makes an interrupt during a deferral
harmless. xHCI gets the suppression for free: past the budget the
handler leaves EHB set, so the controller raises nothing more until the
worker catches up.

**Where it is used.**
- **virtio:** the virtqueue core owns one irq_poll per queue. Callbacks
  become `unsigned (*)(vq, budget)`, `virtq_interrupt` schedules, and
  `virtq_free` disables after the transport's teardown. Converted:
  `vblk_done`, `vnet_rx_done`, `vnet_tx_done`, `vrng_done`.
- **NVMe:** one irq_poll per queue. `nvme_poll` holds the admin queue's
  step-aside for the polled path, so the worker steps aside as well.
  `queue_free` disables it.
- **xHCI:** the event ring, with `irq_poll_disable` in remove and in
  probe's failure path, and `irq_poll_synchronize` in the cancel path.
- `virtq_pop` skips at most a ring's worth of bad device entries per
  call.

## 5. What the design guarantees, and how

**The deferred remainder runs.**
- A deferral puts the irq_poll on a started worker's list, under that
  list's lock, and wakes the worker.
- The worker is a pinned kernel thread at the default priority, runnable
  until its list is empty, and the scheduler's time slicing runs it.
- It cannot be lost: a handler that defers sets `scheduled` under the
  irq_poll's lock. Only two things clear it: the worker, just before
  calling `poll`, and `irq_poll_disable`, which is the teardown asking
  for no more.
- Before the workers start (boot, before any driver module loads), a
  handler that uses its budget polls on, as before. Deferral is
  impossible there, and no driver is loaded yet.

**Completion order.** Only one `poll` runs at a time, so completions are
consumed in the order the device posted them, as before.

**Ownership.**
- Unchanged inside each driver. `vblk_done` still decides by pointer,
  under `vb->lock`, whether a popped bio is still in the slot table, and
  only then completes it.
- `vblk_timeout` and the removal walk clear the same table under the
  same lock. Whichever clears a slot owns the bio, wherever `vblk_done`
  runs.
- NVMe's command-id slots and xHCI's ring requests are unchanged in the
  same way.

**Quiesce and removal.**
- `virtq_free` used to end with the transport's teardown: mask the
  vector, unregister it, `synchronize_irq` (a handler is a quiesce
  read-side section).
- It now also calls `irq_poll_disable` before freeing the ring. Every
  virtio remove and reset path freed the queue *before* walking its slot
  table, so the walk is again the only consumer. `virtio-remove-inflight`
  keeps its meaning:
  - its hold is checked before every pop, in the handler and in the
    worker alike;
  - a held `poll` returns having consumed nothing, so the irq_poll goes
    idle and the remove finds the parked completions;
  - its Q11b stamps still bracket the queue teardown, which now ends
    with the worker stopped.
- NVMe's remove disables after `synchronize_irq` (`queue_free`), as
  does xHCI's.
- xHCI's cancel used `synchronize_irq` to mean "every callback of a
  request retired a moment ago has returned". It now also waits for the
  worker's pass (`irq_poll_synchronize`).

**Lockdep** stays clean. On both architectures, a debug boot with
LOCKDEP=1 prints only the self-tests' expected reports. The irq_poll
lock is an IRQ-safe class, taken with irqsave from the handler and the
worker.

## 6. Tests and the probe

- `blk-irq-budget` passes on the fixed tree with one call popping at
  most 32, in every boot of the validation matrix. The longest tick gap
  during the storm was 4–15 ms, against a 4 ms tick period and the
  250 ms bound. One call's 32 completions take well under one tick. The bound is listed in `docs/testing/flakes.md`,
  "The list", as load-sensitive. The deterministic claim is asserted
  beside it: no call pops more than the ring holds.
- `tools/irq-budget-probe.py`:
  - `--tree before` checks out the test's own commit, without the fix;
  - `--old` sets `IRQ_POLL_BUDGET` to `UINT32_MAX` on this tree, which
    restores the drain-until-empty loop;
  - `--storm-ms` lengthens the storm;
  - the expected verdict is the exit status.
  - `--tree before` and `--old` fail the test on both architectures
    (§2's table, and `--old`: 15,645 popped on x86-64, 16,267 on
    AArch64, 1,002 ms each).
- The existing device, removal, timeout and benchmark tests all pass in
  the validation matrix (§8): `virtio-remove-inflight` with its held and
  held-inside passes, the virtio-blk reset and timeout tests, NVMe,
  AHCI, USB storage and HID, `blk-bench`, `net-bench`, `net-nicbench`.

## 7. Throughput, before and after

`tools/bench-ab.py` built `main` and this branch in clones and booted
them alternately, after, before, four boots each, at default priority,
one boot at a time. Every number is the median of four. The spread
between boots of the same side is typically ±10–15 %, and both the host
and QEMU have per-boot modes (`docs/testing/flakes.md`): eth1's ARP
rate, for one, sits near 2,800 or near 8,000 by boot, on either side.

| metric | x86-64, LOCKDEP=1 | AArch64, LOCKDEP=1 | x86-64, LOCKDEP=0 |
|---|---|---|---|
| vda read 64 KiB, MiB/s | 334 → 314 | 313 → 385 | 590 → 548 |
| vda 4 threads × 4 KiB, req/s | 13,328 → 12,599 | 11,509 → 13,050 | 26,639 → 28,343 |
| nvme0n1 read 64 KiB, MiB/s | 311 → 291 | 267 → 312 | 473 → 460 |
| nvme0n1 4 threads × 4 KiB, req/s | 17,004 → 16,459 | 13,606 → 14,475 | 26,689 → 24,909 |
| ahci0p1 read 64 KiB, MiB/s | 338 → 336 | 278 → 286 | 542 → 543 |
| sda (USB, xHCI) read 64 KiB, MiB/s | 243 → 232 | 201 → 161 | 419 → 415 |
| net-bench tcp, 1 flow, MiB/s (steer 0) | 27 → 26 | 27.5 → 27.5 | 63 → 67.5 |
| net-bench udp, sends/s (steer 0) | 18,308 → 17,147 | 17,484 → 20,900 | 77,702 → 84,718 |
| nicbench eth0 udp, sends/s | 7,014 → 6,742 | 1,671 → 4,423 | 36,951 → 40,757 |

The LOCKDEP=1 runs compare 8fd216d5 with this branch before its rebase
(3ac2f6d0); the LOCKDEP=0 run compares d9889d5e with 6ebeac6b. Neither
rebase touched these paths.

**What it shows.**
- **x86-64 with lockdep on: a consistent 3–6 % loss** on the
  one-request-at-a-time block benchmarks and on network UDP. The
  budget never binds there: one request in flight means one completion
  per interrupt. What each interrupt does pay is the irq_poll's lock,
  taken twice. Under LOCKDEP=1 a spin lock acquisition costs about
  2.9 µs on x86-64 debug (`lockdep-bench`, path=spin), and a 4 KiB
  request takes about 150 µs, so two more acquisitions predict about
  4 %.
- **x86-64 with lockdep off: no consistent cost.** The same metrics move
  both ways inside the boot spread (vda 64 KiB −7 %, vda 4-thread +6 %,
  UDP +9 %). The loss above is lockdep's price for one more lock class
  on the interrupt path, not the design's.
- **AArch64 with lockdep on: no cost visible.** Two of the four after
  boots drew the host's fast mode (UDP about 24,000/s against about
  17,500/s, vda about 400 MiB/s) and none of the before boots did. The
  after medians are therefore higher, which is not to the change's
  credit. Within the slow mode, after boots 0 and 2 read vda at 376 and
  394 MiB/s against before's 308–324, so no loss there either.
- **The storm is where the budget costs throughput, by design.** In
  `blk-irq-budget`'s 1 s storm (LOCKDEP=1), the unbounded handler
  completed 15,688–15,790 requests on x86-64 (four CPUs; 18,631 on one)
  and 16,312–16,980 on AArch64. With the budget the validation boots
  completed 9,486–15,387 on x86-64 and 11,466–16,717 on AArch64,
  depending on CPU count and the chaos migrator. That is up to about a
  third fewer.
- The unbounded loop had its CPU to itself, with interrupts off, for as
  long as the refilling lasted. The worker hands the CPU back every 32
  completions, takes two locks per batch, and yields to whatever else
  is runnable, which during the boot is the rest of the self-test
  machinery. Trading a storm's peak rate for a CPU that keeps its tick
  is the change's purpose. A device that never runs dry now costs its
  CPU a share of its time rather than all of it.

A lock-free fast path (an atomic state word claimed by compare-and-swap)
would remove the lockdep cost on debug boots. It was not done: a
release kernel does not pay it, and the lock makes the hand-off's state
machine easy to check.

## 8. Validation

Local, both architectures, at default QEMU priority, one boot at a time,
on the final tree (rebased on main dbcbbb2b, with #332 and #333):
host-test, `make fuzz` (50,000 mutations per target), analyze, debug
boots at 1, 2 and 4 CPUs, `test-smp2`, `test-chaos`,
`test-harness-retry`, and release build and boot. Every one passed. The
network harness passed in every debug boot. No lockdep report beyond
the self-tests' expected ones; the class-table peak was 408 of 512.

The first pass, before the rebase, found three things. None was the
budget's doing, and each is recorded:
- **host-test and `make fuzz` did not link.** `test_virtq` and
  `fuzz_virtq` build `virtqueue.c` alone and lacked `irq_poll`; both now
  stub it, since their queues are polled.
- **`lockdep-graph-bench` went over its budget** under the chaos
  migrator, as it had on main the same day (#333 gave it 20 s).
- **The AArch64 harness-retry boot failed `net-tcpverdict`.** That is a
  race between the test's rule and the delayed-ACK timer, live since
  #322. It is shown with an injected adversary and fixed by waiting for
  the ACK (`docs/testing/flakes.md`, "`net-tcpverdict`: the delayed ACK
  met the rule first").

A stale local build directory also failed one x86-64 boot by rejecting
two modules for the old module ABI. Its dependency files named relative
targets, so the ABI bump never rebuilt them. CI builds from scratch and
cannot meet this.

## 9. Symbolisation

CI uploaded the kernel ELF only on success, so run 37698731544's PCs
could not be read. A new step, on failure, uploads every debug build's
`kernel.elf` and `modules/*.ko` (plain, chaos and harness-break builds).

## 10. What is not done

- Module text is not symbolised by the harness. The kernel frames of the
  12 s storm read; the module frames need each module's load base,
  which the boot log does not print. Printing it at load would let the
  harness's symboliser cover modules too.
- `IRQ_POLL_BUDGET` is one constant for every device. Linux tunes its
  NAPI weight per driver. Nothing here needed a second value.
- The workers are created once, at boot, for the CPUs online then. CPU
  hotplug does not exist in this kernel.
