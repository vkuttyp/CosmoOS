# NEXT SUBSYSTEM — the NVMe admin path leaves before complete() has let go

> Constitution §68 report. Takes up a panic on `main` itself (run
> 36082559265, 2026-09-25, the merge of #242, a report that changed no
> code): an aarch64 debug boot died 10 s in, while probing NVMe, with
> `spin_unlock` asserting that a lock it was releasing was not held. The
> cause is one driver skipping a rule the completion primitive documents,
> in a loop three other drivers write correctly by hand. This report
> proposes the fix, a primitive that makes the rule impossible to skip,
> and a test that checks the rule rather than waiting for the race.

## Problem

```
KERNEL PANIC: assertion failed: __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0
              at kernel/core/spinlock.c:117 (spin_unlock)
CPU: 1  context: boot (no threads yet)
  spin_is_held  kernel/core/spinlock.c:152
  ??            (the nvme module's text)
  gicv2_eoi     kernel/arch/aarch64/gic.c:294
  handle_irq    kernel/arch/aarch64/trap.c:115
  idle_main     kernel/scheduler/sched.c:85
  held by this CPU (1):
    [0] spin 'nvme-admin'#0 [irq]
```

The line before it in the log is `irq: MSI vector 1081 on CPU 1
(nvme-ioq)`: the controller's probe was issuing admin commands, now
answered by interrupt. CPU 1, in the NVMe interrupt handler, held the spin
lock named `nvme-admin`, and releasing it found the lock word already
zero. That name belongs to one object: the `struct completion` that
`admin_cmd` keeps on its stack (`drivers/nvme/nvme.c:314`).

### The rule, and the one place that skips it

`complete()` sets `done` and wakes the waiters under one hold of the
completion's own lock (`kernel/scheduler/completion.c`), and
`wait_for_completion` takes that lock once after it has seen `done`. That
handshake is what lets a waiter free a completion that lives on its
stack. `kernel/include/kernel/completion.h` states the rule:
`completion_done()` alone gives no such guarantee, and a caller that polls
it must call `wait_for_completion` before the memory goes.

`admin_cmd` polls:

```c
for (unsigned waited = 0; waited < NVME_ADMIN_TIMEOUT_MS && !completion_done(&w.done); waited++) {
    thread_sleep_ms(1);
    if (c->admin.vector < 0)
        queue_process(q);          /* polled before the vector exists */
}
...
if (!completion_done(&w.done)) {
    ...                            /* the timeout path -- which does call wait_for_completion (:352) */
}
... return rc;                     /* done was seen: returns with no handshake */
```

When the poll sees `done`, `admin_cmd` returns without the handshake. The
interrupt handler may still be inside `complete()`, between setting `done`
and releasing the lock, or waking a queue that lives in that frame. The
next admin command's `completion_init` reuses the same stack address and
writes a zero into the lock word the handler is about to release. That is
the assertion above.

**The other three drivers that poll a completion do the handshake**, each
by hand:
- `drivers/usb/xhci.c:282` (the command ring);
- `drivers/storage/ahci.c:450` (a synchronous command);
- `drivers/usb/usb.c:181` (a synchronous transfer).

NVMe's loop is the fourth copy of the pattern and the one that got it
wrong. The completion unit that introduced the handshake fixed the same
bug class inside `complete()` itself, after AHCI hit it (selftest
`completion-race`). The callers were left to remember it.

### Measured

**CI.** I scanned every failed run's log in the recent history, 50 on
`main` and 100 on branches, for the assertion. It appears in exactly one:
run 36082559265, the plain aarch64 debug boot. The race needs the
interrupt to land on another CPU and the waiter's 1 ms poll to fall inside
the few instructions between `done` and the unlock, so it is rare. But it
is on the boot path of every machine with an NVMe disk.

**Deterministic, from the mechanism.** `tools/nvme-admin-probe.py` widens
exactly that window: a 3 ms spin inside `complete()`, after `done` is set
and before the wake and the unlock. It applies only to completions whose
lock is named `nvme-admin`, and only when called from interrupt context.
Its `--fixed` mode also adds the handshake to `admin_cmd`, the one line
this report proposes. One debug boot per architecture per mode, each
confirmed to have booted:

| mode | aarch64 | x86-64 |
| --- | --- | --- |
| window widened | **panic at 8.6 s**: page fault writing `0xffff800000000081`, in the nvme module's probe under `module_load_boot` | **hang**: nothing after `irq: MSI vector 67 on CPU 1 (nvme-admin)`, the harness's 180 s timeout, and the lockup detector reported nothing |
| window widened, handshake added | PASS (125.9 s) | PASS (117.0 s) |

The symptoms differ from CI's, as a use-after-return's do: which write
lands in the reused frame first decides whether it is the lock word (CI's
assertion), a wait-queue pointer (the aarch64 fault), or something that
stops the boot silently (x86-64). The window is the same one, and the
handshake closes it on both architectures.

### Why it matters

- **It is the boot path.** NVMe's probe runs at module load on every boot
  of the test machines, and the panic stops the boot before init.
- **It is on `main`.** It landed with no code change in the commit that
  failed, so a re-run would have hidden it as a flake.
- **The rule has now been broken twice, once in the primitive and once in
  a caller.** A rule each caller must remember, written out four times by
  hand, will be broken again by the fifth caller.

## Current implementation

- **`struct completion`** (`kernel/include/kernel/completion.h`): a spin
  lock, `done`, and a wait queue. Its API is `completion_init`, `complete`
  (interrupt-safe), `wait_for_completion` (sleeps, then does the
  handshake), and `completion_done` (a query, with no handshake).
- **`wait_event_timeout(wq, cond, ns)`** (`kernel/include/kernel/wait.h`)
  exists, is tested on its own, and returns whether `cond` held.
- **The four polling callers.** Each sleeps in a loop of fixed steps
  (1 ms for NVMe, 100 µs for xHCI and AHCI, 250 µs for USB) until
  `completion_done` or a deadline. Three then call `wait_for_completion`
  when `done` was seen; NVMe does not. NVMe's loop also has a second job:
  before its admin vector exists, it runs `queue_process` itself, so the
  completion is signalled by the waiting thread.

## Design

### 1. The fix

`admin_cmd` does the handshake when its poll has seen `done`, the line
`--fixed` applies. The timeout path already does it (`:352`).

### 2. A primitive that does the handshake itself

```c
/* Wait until `c` is completed or `timeout_ns` passes. True: completed,
 * and complete() has finished with `c` -- the caller may free it. False:
 * timed out; `c` may still be completed later, and the caller must not
 * free it until it has either stopped whatever will complete it or waited
 * with wait_for_completion. */
bool wait_for_completion_timeout(struct completion *c, uint64_t timeout_ns);
```

It is built on `wait_event_timeout`. When the event has happened, it takes
the completion's lock once before returning, the same handshake
`wait_for_completion` does. A caller can no longer see `done` without the
handshake coming with it.

**All four interrupt-driven polling loops use it**: NVMe's admin wait
once its vector exists, xHCI's command wait, AHCI's synchronous command,
and USB's synchronous transfer. The hand-written `wait_for_completion`
after the loop goes, and each timeout path keeps its own logic (orphaning
the slot, restarting the port, cancelling the transfer, then waiting).
NVMe keeps its own loop only for the polled phase, where the waiting
thread completes the command itself and no other CPU is inside
`complete()`.

A side effect: the four waits wake on the completion instead of at the next
poll step, up to 1 ms sooner for NVMe. None of the four is a hot path;
they are probe-time and error-path commands.

`completion_done` stays: the self-tests read it on objects whose lifetime
is not the stack (a process's `exited`). The header's rule is reworded to
point polling callers at the new primitive.

### 3. A test that checks the rule, not the race

`complete()` becomes `complete_linger(c, 0)`. `complete_linger(c,
linger_ns)` is the same body with a per-call spin between setting `done`
and the wake, and it is the test's handle on the window: a per-call
argument on the real path, not state the production path reads.

A new self-test, `completion-timeout`, needs two CPUs (it skips on one,
saying so). A completer thread on CPU 1 calls `complete_linger(&c, 2 ms)`,
and the test thread, waiting in `wait_for_completion_timeout(&c, 1 s)`,
must see:
- **true**; and
- **the completion's lock free the moment it returns** (not held by the
  lingering completer), which is the handshake stated directly.

That runs for 200 rounds, the completion re-initialised each round as the
next caller's frame would be. A timed-out wait on a completion nobody
completes returns **false**, decided by the return value, not a duration.

The completion is static in the test, so a missing handshake is a failed
check with a name, not the memory corruption the probe produces.

### 4. The invariant

The scheduler invariants gain: **a completion's waiter never returns with
the completion's lock held by `complete()`**. Every path that returns
"done" has done the handshake. Checked by `completion-timeout`, and for
the driver by `tools/nvme-admin-probe.py`.

### 5. The §70 gate

**Correctness.** One missing handshake added. The poll-then-handshake
pattern replaced by a primitive in which the handshake cannot be skipped.

**Concurrency.** The handshake is the existing one: one acquire of the
completion's lock after `done` is seen. No new lock and no new order.
`complete_linger`'s spin runs with the lock held and interrupts masked,
and only a test ever passes a non-zero value.

**Ownership and lifetime.** This is the point: a stack completion's
lifetime ends only after its completer has let go.

**Security.** None.

**Failure.** Unchanged timeouts, and each driver's timeout path is
unchanged.

**Performance.** The waits wake on the event instead of the next poll
step. That cost nothing measurable, and it is on probe and error paths.

## Affected files

| file | change |
| --- | --- |
| kernel/include/kernel/completion.h, kernel/scheduler/completion.c | `wait_for_completion_timeout`, `complete_linger`; the rule's wording |
| drivers/nvme/nvme.c | the handshake: `wait_for_completion_timeout` once the vector exists; the polled phase keeps its loop |
| drivers/usb/xhci.c, drivers/storage/ahci.c, drivers/usb/usb.c | the hand-written loop plus handshake becomes `wait_for_completion_timeout` |
| kernel/scheduler/schedtest.c, kernel/core/selftest.c | `completion-timeout` |
| tools/nvme-admin-probe.py | follows `complete()`'s new shape |
| docs | the scheduler's completion API and invariants; the NVMe, USB and AHCI driver docs where they describe their waits; `docs/testing/flakes.md` (the sighting); README Status |

## APIs

- **New in the kernel**: `wait_for_completion_timeout(c, ns)` and
  `complete_linger(c, ns)`. Both go into module ABI v1 beside the other
  completion functions, since NVMe, xHCI, AHCI and USB are modules.
- **Unchanged**: `complete`, `wait_for_completion`, `completion_done`.

## Migration plan

One PR: the primitive and its test, the four drivers, the probe, the
documents.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `completion-timeout` (new) | 200 lingered completions from another CPU: each wait returns true with the lock already free; a wait nobody completes returns false | `wait_for_completion_timeout` without its handshake: the lock is still held on return |
| `nvme`, `ahci-identify`, `ahci-io`, `usb-enum`, `usb-storage` (existing) | the drivers' commands, through the new wait (xHCI's command ring is driven by `usb-enum`) | a wait that returns false on success: probe fails |
| `ahci-timeout`, `usb-storage-timeout` (existing, fault injection) | a command that never completes | the build establishes which of the four waits each reaches, and says so; a timeout path it reaches is proved by it |
| `tools/nvme-admin-probe.py` (not a boot test) | the widened window against the built driver | NVMe's handshake removed: the panic or hang in the table above |

## Benchmarks

None: probe-time and error paths.

## Risks

- **A driver's timeout logic depends on the poll.** Each of the four is
  read in the build and its timeout path kept as it is; only the wait for
  the event changes. The existing driver tests cover the success paths.
  `ahci-timeout` and `usb-storage-timeout` inject a command that never
  completes, but through the block path, and whether that path reaches the
  synchronous waits converted here is not yet established. The build
  traces it, and any timeout path no test reaches is named as such.
- **`complete_linger` in a production binary**: one comparison with zero
  on `complete()`'s path, kept because a test hook compiled only into
  test builds is a path the release build never runs.

## Alternatives considered

- **Only the one-line fix in NVMe.** It closes today's bug. But the
  pattern stays written out four times, three right and one wrong, and
  the fifth caller will copy one of them.
- **Make `complete()` never touch the completion after setting `done`**
  (wake first, then set `done`). A waiter can then sleep past a wake that
  has already happened, unless `done` is checked under the lock, which is
  the handshake again.
- **A debug assertion when a stack completion goes out of scope.** Each
  caller would have to remember to assert, which is the same memory the
  primitive removes.
