# Kernel object lifetime and quiescence: testing

## What runs where

| Level | What | Command |
|---|---|---|
| Host (ASan/UBSan, real threads) | `test_quiesce`: the epoch arithmetic, a negative model, and four reader threads against an updater that frees after each grace period (2000 generations) | `make host-test` |
| Target self-tests (debug builds) | `quiesce-grace`, `quiesce-call`, `irq-sync`, `timer-cancel-sync`, `quiesce-stress`, `blk-lifetime`, `net-netif-lifetime`, `net-accept-race`, `module-unload-busy` | `make test` |
| Single CPU | the same tests take their one-CPU branches (the calling CPU is quiescent by construction; self-IPI for `irq-sync`) | `QEMU_SMP=1 make test` |
| AArch64 | everything above; `irq-sync` uses an SGI through `arch_ipi_send` | `make ARCH=aarch64 test` |

Every test above ran and passed on x86-64 (4 CPUs and 1 CPU) and AArch64
(4 CPUs) when this pass was verified; the timings below are from those runs.

## Host test (`tests/host/test_quiesce.c`)

| Case | What it checks |
|---|---|
| `epoch-math` | record size and alignment (64 bytes); nobody pending after everyone published; offline CPUs never counted; two waiters advancing twice are both satisfied by one later publish (`>=`); the highest CPU slot (63) |
| `negative-model` | with one CPU unpublished the algorithm refuses to declare the grace period over; it does once that CPU publishes |
| `threads` | four reader "CPUs" alternate 64-read sections with a quiescent point; the updater swaps the pointer, waits, poisons and frees 2000 objects. Any reader past its section would read freed memory: ASan fails the run. Zero bad magics observed |

## Target self-tests (`kernel/core/quiescetest.c`)

| Test | Steps | Proves |
|---|---|---|
| `quiesce-grace` | a solo `synchronize_quiesce` (epoch +1, counted); a reader pinned to CPU 1 enters a nested `quiesce_read_lock`, dereferences the object for 30 ms with preemption disabled; the main thread unlinks and waits; on return the reader's `done` flag (set inside the section) is 1, the object is poisoned and freed, the wait took ≥ 20 ms | Q1, Q2 (ticks inside the section do not count), nesting. Measured: reader held 30 ms, grace period 31–32 ms; solo grace period 5–6 ms with 4 CPUs (the others halted: one tick) |
| `quiesce-call` | eight `call_quiesce` heads, half submitted with preemption and interrupts off; all run once, in submission order, with `irq_depth == 0 && preempt_count == 0`, after one grace period | Q4, Q5 |
| `irq-sync` | a vector from `arch_vector_alloc`, a handler that spins 20 ms; raised on CPU 1 with `arch_ipi_send`; once entered, `interrupt_unregister_sync` on CPU 0 returns only after the handler set `done` (≥ 10 ms); `irq_syncs` +1; a second unregister is `-ENOENT`; the argument is poisoned and freed. One CPU: self-IPI, then the sync form is immediate | Q7. Measured: unregister_sync took 22–23 ms against a 20 ms handler |
| `timer-cancel-sync` | pending timer: `cancel_sync` true then false; a callback spinning 20 ms armed on CPU 1 by a pinned thread: `cancel_sync` returns after `done`, ≥ 10 ms, `timer_sync_waits` +1; a callback that re-arms every 1 ms: after `cancel_sync` the state is IDLE and the fire count is unchanged 30 ms later | Q8. Measured: cancel_sync took 20 ms; the re-arming timer stopped after 2 fires |
| `quiesce-stress` | readers on every other CPU spin in `quiesce_read_lock` sections checking the current object's magic (yielding every 1024 reads); the updater replaces the object for 400 ms alternating `synchronize_quiesce` + free and `call_quiesce`; all deferred frees drain; zero bad reads; every CPU's read depth is 0 afterwards | Q1 under load. Measured: 3 readers, 4.2–5.8 M reads, 101 synchronous + 100 deferred generations in 400 ms (≈ 4 ms per grace period) |

## Object lifetime self-tests

| Test | File | Steps | Proves |
|---|---|---|---|
| `device` (extended) | `kernel/device/devtest.c` | `device_register` without a release → `-EINVAL`; refcounts unchanged otherwise (init + bus + find = 3) | Q9 |
| `blk-lifetime` | `kernel/device/devtest.c` | the 27th device of a prefix is `-ENOSPC` with no kobject and no owner count; register refused without release; register → refcount 2; `blk_find` → 3; a bio completes; `blk_unregister` → not findable, refcount 2, `gone`, `blk_submit` `-ENODEV`; creator's put leaves the finder's; the finder's put runs the release once | Q9, Q10, Q11 |
| `net-netif-lifetime` | `kernel-services/network/nettest.c` | a duplicate name is `-EEXIST` with no kobject and no owner count; register refused without release; register → 2; find → 3; a transmit and a receive succeed; `netif_unregister` → not findable, `GONE` and not `UP`, refcount 2; transmit `-ENODEV`; `netif_rx` dropped (queue length unchanged, `rx_packets` still 1); release runs once after both puts | Q9, Q10, Q12 |
| `net-accept-race` | `kernel-services/network/nettest.c` | a listener on loopback; a client thread connects 64 times and drops each connection at once (half after a `shutdown`); every accept returns a child whose pcb names the new socket; no client failure | Q13 (invariant) plus a stress of the former race window |
| `module-unload-busy` | `kernel/module/modtest.c` | (plus: a zombie `cosmotest_dep` keeps `cosmotest` pinned until reaped, and its release calls into the dependency) `module_owner_of` of a kernel address is NULL and of the fixture's export is the module; the fixture hands out a kobject whose release is in module text (`live_objects` 1); `module_unload` with a 50 ms timeout returns `-EBUSY` after waiting, the module is not live and its symbols are gone; `kobject_put` runs the release from the zombie's text (`cosmotest_released == 1`); a second unload frees the zombie (0), a third is `-ENOENT`; the name loads and unloads again | Q15, Q16 |

`tcp_transfer` (the helper behind `net-lo-tcp` and `net-lo-tcp-loss`) now
waits, bounded, until the port it used can be bound again: the server's
child leaves `LAST_ACK` when the `netrx` worker processes the client's
final ACK, and since `quiesce_read_unlock` is a prompt preemption point
the higher-priority test thread can run ahead of the worker for a few
milliseconds. The old ordering passed by relying on the worker not being
preempted until the next tick.

## Benchmarks (from the self-test logs)

| Quantity | x86-64, 4 CPUs (QEMU TCG) | AArch64, 4 CPUs | Notes |
|---|---|---|---|
| Solo `synchronize_quiesce`, other CPUs idle | 5.0–6.2 ms | 5.0 ms | one tick period (4 ms) plus the `TICK_NS/2` poll |
| Grace period against a 30 ms read section | 31–32 ms | 31 ms | bounded by the reader, as required |
| `interrupt_unregister_sync` against a 20 ms handler | 22–23 ms | 23 ms | |
| `timer_cancel_sync` against a 20 ms callback | 20 ms | 20 ms | spin, no tick granularity |
| Reclaim generations under 3 readers, 400 ms | 201 | 199 | ≈ 4 ms per grace period; `call_quiesce` batches pay one period for the batch |
| Read-side section cost | one `preempt_disable`/`enable` pair | | no atomics, no shared writes |

## Running

```sh
make host-test                      # test_quiesce among the host tests
make test                           # every self-test, 4 CPUs
QEMU_SMP=1 make test                # one CPU
make ARCH=aarch64 test
```

## Gaps

- No test drives the straggler IPI (Q6) or the `blk_submit`/`blk_unregister`
  window (Q11); both are reviewed, not exercised.
- No thread-sanitizer run of the host model.
- `synchronize_quiesce` latency is tick-bound (one to two ticks with idle
  CPUs); a wake-on-publish design would shorten it and is listed under
  architectural debt in the final report.
