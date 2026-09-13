# NEXT SUBSYSTEM — a thread per vCPU, and a way to stop one

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: `vmctl --machine` runs a guest's vCPUs on one thread, a tick
each, and every artefact of that shows through the interface.** The
round-robin in `run_machine` (`userland/system/vmctl.c`, 297 of its 1379
lines) picks the next running vCPU, runs it with
`COSMO_VCPU_RUN_ONE_TICK`, and takes a turn boundary after every PSCI
call. Two corrections exist only because of that serialisation: a
`fresh[]` array recording which vCPU has never had a turn end on its own
terms, and a `SYSTEM_OFF` that is *held* for such a sibling for up to
`MACHINE_OFF_GRACE_TURNS` (64) turns. Neither is PSCI semantics -- a real
`SYSTEM_OFF` waits for nothing -- and the design document says so: they
are the correction for serialising what hardware would run in parallel.
**This unit gives each vCPU its own thread, which is what the last three
units were for**: native threads (#116) made the threads possible, and a
per-thread `errno` (#120) made it safe for those threads to call libc at
all, which `vmctl` does on every path (`printf`, `open`, `read`,
`strerror`). The fairness rule and the power-off grace then have nothing
left to correct and go.

**It is not free, and the expensive half is not the threads.** A vCPU
thread that runs untimed cannot be stopped by its siblings: the only thing
that breaks a running vCPU out of `arch_hv_vcpu_run` today is
`process_kill_pending()`, a fatal signal to the whole process
(`kernel-services/virtualization/vcpu.c:319`). So a guest CPU spinning
forever -- which `guest_offspin` is a test for -- would hang the owner
after `SYSTEM_OFF`, where the bounded hold stops it now. This unit
therefore also adds **the kick**: a way to make one vCPU leave its run,
which is the piece KVM calls `kvm_vcpu_kick` and which nothing here has.

**Eight things in the first drafts of this report were wrong, and review
found them before any of it was built** -- which is what the §68 wait is
for. The main thread could not both drain the console and join the vCPU
threads, since `join` blocks; the design said both "start a thread per
vCPU the tree promises" and "`CPU_ON` starts a thread", which would start
a secondary twice or start it before PSCI asked; and test 4 asserted
something the implementation it replaces already satisfies. Each is
answered where it appears, and the third is answered by changing what the
test measures. The fourth was a **deadlock in shutdown**: threads created
parked have to be woken *and* told to quit, because the kick only reaches
a vCPU inside the kernel's run loop and a secondary that was never
`CPU_ON`ed is parked in userland -- so `live` would never reach zero on
the most ordinary path there is, a guest that starts one secondary of four
and powers off. The park is now a three-state word, which is also what
makes the supervisor's `live == 0` mean "every thread has returned" -- and
a fifth was the mirror of it: a `CPU_ON` racing `SYSTEM_OFF` could store
`RUNNING` over `QUIT` and revive a vCPU after shutdown began, so the
states are monotonic and `CPU_ON` releases a thread with a
compare-and-swap that fails once `QUIT` is set. The sixth was that none of
that said anything about **memory ordering**, which for a lifecycle built
out of shared words is most of the correctness: there is now a table of
every word, its writer, its reader and the ordering each needs. The
seventh was that the table covered the *userland* words and stopped at the
kernel boundary, leaving out the kick's own flag -- written by one
thread's syscall and read by a different CPU's run loop, which is the one
crossing the most CPUs of all. The eighth was that the answer given for
`loaded_cpu` was **wrong and not merely unsynchronised**: a plain `int`
written by the run loop cannot be read safely by a kicker that must not
take `run_lock`, and stickiness stops a flag being lost without making a
spinning guest look at it. That is what `in_guest` and the re-check before
the VM entry are for, and they land in the arch backends -- which the
affected-files table now says.

## Problem

- **The interface carries the implementation's shape.** `fresh[]` and the
  64-turn grace are visible in behaviour: `vmctl` prints
  `guest powered off` after a delay a real machine would not take, and
  the guest's own `cpu1: up` line is the thing being waited for. A guest
  that counted its instructions against a sibling's could observe the
  difference. The design document already names this as "not PSCI
  semantics".
- **Four vCPUs share one host thread, so a guest cannot use more than one
  CPU's worth of time.** `COSMO_HV_VCPUS_MAX` is 4. A multi-core guest
  gets no parallelism at all -- the ticks interleave on one thread -- so
  every measurement of a guest's scaling measures the owner's loop.
- **A tick is host time, and the guest's progress within it is whatever
  the host scheduler allows.** That is why `cpu1: up` went missing on a
  loaded CI host in the first place, and why the fairness rule exists.
  Threads move that decision to the scheduler, which is the thing that
  knows about load.
- **The device models are already written as if they were shared, and are
  not.** `g_vio` (virtio-blk) and `g_vnet` (virtio-net) are file-scope
  globals in `vmctl.c`, touched from 26 places, all of them today reached
  from the single loop: the MMIO dispatch, the drain after every turn, and
  the tap poll. With a thread per vCPU, two guest CPUs can hit the same
  virtqueue at the same time. **This is the unit's real work**, and the
  rule has to be decided before a line of it is written -- the last time
  shared process state grew locks site by site it cost a review five
  findings.
- **The owner's own I/O becomes concurrent too.** `drain_console` runs
  after every turn from one thread; with threads, three callers can be
  inside it. It writes with `printf`, which is locked since #116, but the
  console ring it drains from is per-VM and read through one handle.

## Current implementation

**The loop** (`userland/system/vmctl.c`, `run_machine`):

```c
rc = cosmo_vcpu_run_flags(m.vcpu[cpu], &x,
                          (nr_running > 1 || m.off_pending) ? COSMO_VCPU_RUN_ONE_TICK : 0);
```

One vCPU left runs untimed -- nothing else needs the thread -- *except*
while a power-off is held, because the hold is bounded in turns and so
every turn must end. `x.kind != COSMO_VM_EXIT_PREEMPTED` clears
`fresh[cpu]`; a PSCI call sets `next = 1` so the vCPU that just brought a
sibling up cannot run on to `SYSTEM_OFF` within the same tick, which the
first boot of this mode did.

**The kernel is further along than the owner.** What is already there:

| what | where | state |
| --- | --- | --- |
| per-vCPU run serialisation | `vcpu->run_lock` (`kernel/include/kernel/hv.h`) | a mutex over "run and regs", so two *different* vCPUs already run concurrently as far as the kernel is concerned |
| per-VM state | `vm->lock` | regions, devices, `vcpus[]`, `mem_bytes` |
| the guest's console ring | `vm->console.lock`, a spinlock | commented "producer is the run loop, consumer the owner" |
| pending virtual interrupts | `vcpu->irq_lock`, a spinlock | per-vCPU |
| the guest's GICv3 distributor | `gicv3_vdist.lock`, a spinlock | "covers every field", taken by a guest's MMIO |
| a VMCS moving between host CPUs | `vmx.c:1062` | `loaded_cpu` tracked, `vmclear` then `vmptrld`, launch rather than resume |
| which host CPUs a VM has run on | `vm->ran_on`, a `cpumask_t` set atomically on **both** arches (`vmx.c:1061`, `hv_el2.c:1058`) | "CPUs that have entered this VM: where its cached translations can be" -- it exists for TLB shootdown, and it exists *because* a vCPU was always expected to move between host CPUs |

So the hypervisor was built for vCPUs that migrate between host CPUs --
`ran_on` is that assumption written down, on both architectures -- and
nothing in it assumes one thread. **What it does not have is a way to stop
one.** `vcpu_run_bounded`'s loop checks `process_kill_pending()` and
counts host-interrupt exits against `max_intr`; `v->dead` is checked once
at entry, under `run_lock`, so setting it does not disturb a vCPU already
running. There is no per-thread signal targeting either -- a native
`tgkill` is named and deferred in `docs/kernel/process/design.md` §12.

**The tests that exist** (`userland/etc/rc.test`, both required markers):

- `vmctl run --machine -c 2 guest_dtb.bin` -- the whole path through the
  real owner, gated on the guest's own lines: the `dtb:` line, the PSCI
  version, `cpu1: up ctx=1234cafe`, `cpu_on 1 -> 0`, and
  `vmctl: guest powered off`. `cpu1: up` is what the fairness rule keeps
  from going missing.
- `vmctl run --machine -c 2 guest_offspin.bin` -- a second CPU that never
  yields, proving the hold is bounded: the machine still stops and
  `HVTEST: offspin ok` is printed. Removing the bound hangs the boot for
  180 s, which is how it was proved.

Both are the artefacts of serialisation, so **both change meaning in this
unit**, and that is the part to get right rather than to delete.

## Why it matters

- **It is the consumer the thread arc was built for.** #115/#116 added
  threads and #120 made `errno` per-thread; `vmctl` is the program that
  needed both, and until it uses them the arc has no user outside its own
  tests.
- **It removes two corrections from the interface.** A guest that asks for
  `SYSTEM_OFF` should get it, not get it 64 turns later. The rule and the
  grace are honest about being corrections; deleting them is the point.
- **It is the first real multi-threaded program in the tree.** Everything
  threaded so far is `thrtest`. `vmctl` is a program someone runs, with
  device models, files and a tap -- the locking rule it needs is the one a
  second such program will copy.
- **It makes a guest's parallelism real**, which is the only way the
  hypervisor's own scaling can be measured at all.
- **It closes a named gap**: "the handle table under two threads, which
  native threads make reachable and nothing tests" (§12). Four vCPU
  threads sharing one VM handle is exactly that, on a path that matters.

## Proposed design

### A thread per vCPU, and the loop that remains

Each vCPU gets a `cosmo_thread_start` thread running its own loop: run the
vCPU untimed, handle the exit, repeat until the vCPU stops.

**Every thread is created before the guest runs, and parked.** PSCI
secondaries start powered off, so a thread per *running* vCPU would mean
`CPU_ON` creating one from inside the handler -- on the asking vCPU's
thread, mid-guest, where a failed `cosmo_thread_start` would have to
become a PSCI error code. Instead the main thread creates all of them
while it is still the only thread, each parked on its own futex word, and
`CPU_ON` writes the entry point and context and then wakes its target.
A create that fails is a startup failure, reported where startup failures
are reported, and **`CPU_ON` cannot fail for want of memory** -- which is
also what the kernel's own two-phase `process_add_thread` start does, for
the same reason.

The parked word is the thread's own, so a `CPU_ON` for a vCPU already
running is the PSCI `ALREADY_ON` it is today, decided by the owner's
`running[]` rather than by whether a thread exists.

**The park has two exits, not one**, and this is the correction that makes
shutdown terminate. The word is a state, not a flag:

| state | meaning |
| --- | --- |
| `VCPU_PARKED` (0) | created, never started; waiting to be told which |
| `VCPU_RUNNING` (1) | `CPU_ON` wrote the entry and woke it; run the guest |
| `VCPU_QUIT` (2) | leave without running: the machine is stopping |

A thread waits while the word is `PARKED`, and on waking does what the
word now says.

**The states only ever increase, and that is a rule the transitions have
to enforce rather than a description of the usual order.** `SYSTEM_OFF`
writes `QUIT` to every word; a *concurrent* `CPU_ON` -- from another vCPU
thread that has not been kicked yet, which is the normal state of affairs
during shutdown -- would otherwise store `RUNNING` over that `QUIT` and
revive the target. That thread then runs a guest nobody is waiting to
stop, `live` never reaches zero, and the owner hangs: the same failure the
three states were introduced to fix, arriving from the other direction.

So:

- **`CPU_ON` releases a thread with a compare-and-swap from `PARKED`,
  never a store.** If the word is already `QUIT` the swap fails and PSCI
  answers `DENIED` -- a guest asking for a CPU while the machine powers
  off gets a refusal, which is a coherent answer to an incoherent request.
  If it is already `RUNNING`, that is `ALREADY_ON`, as today.
- **`QUIT` is written unconditionally and is final**, because it is the
  highest state: a thread reads its word after every `cosmo_vcpu_run`
  returns and leaves on anything `>= QUIT`, so a `QUIT` that lands over
  `RUNNING` is seen at the next boundary and the kick is what makes that
  boundary arrive.
- **A machine-wide `stopping` flag is set before the first `QUIT`**, so a
  `CPU_ON` that has not yet reached its swap refuses early rather than
  swapping successfully and being torn down a microsecond later. The flag
  is the fast path; the swap is what makes the race safe whichever order
  the two land in. **Shutdown writes `QUIT` to every thread's word and wakes
it**, *and* kicks every vCPU that is actually in the guest. Both are
needed and neither is sufficient:

- The **kick** reaches a thread inside `cosmo_vcpu_run` -- it is an IPI to
  the host CPU the vCPU is loaded on, and it does nothing for a thread
  that is not running one.
- The **`QUIT` write and wake** reaches a thread still parked on its futex,
  which a secondary that was never `CPU_ON`ed will be forever. Without it
  that thread has no path out, `live` never reaches zero, and the
  supervisor waits for a shutdown that cannot complete -- which is a hang
  in the *owner*, on the ordinary path where a guest starts one secondary
  of four and powers off.
- A thread that has been released but has not yet entered its first run is
  covered by the stop flag being **sticky**: it enters, sees the flag, and
  leaves with `STOPPED` without executing a guest instruction.

`live` therefore counts **every thread created**, not every thread
running, and each decrements it exactly once on the way out by whichever
of the three paths it took. That makes the supervisor's `live == 0` mean
what it has to mean -- every thread has returned -- so the joins that
follow always reap rather than wait.

`COSMO_VCPU_RUN_ONE_TICK` stays in the ABI -- the kernel's own tests use
it and it is what `-ETIMEDOUT` is built on -- but the owner stops passing
it. `fresh[]`, `off_pending`, `off_grace`, `MACHINE_OFF_GRACE_TURNS` and
`machine_fresh_sibling` all go, along with the PSCI turn boundary.

`CPU_ON` releases a parked thread rather than marking a slot runnable.
`SYSTEM_OFF` stops the machine: the asking vCPU's thread returns, every
other thread is told to `QUIT` **and** woken, every vCPU actually in a
guest is **kicked**, and the main thread reaps them once its drain loop
sees the live count reach zero. That is the honest PSCI shape -- a real
`SYSTEM_OFF` does not wait -- and it is only implementable with the kick
below.

### The memory ordering, stated once

Every word above is shared between threads, and three of the six findings
this report collected were lifecycle races -- so the ordering is written
down here rather than left to whoever writes the line. The precedent is in
the tree: `cosmo_thread_join` read a thread's return value without an
acquire and had to be corrected in review (#116). A publication whose
ordering is implicit is a publication that is wrong on one architecture.

| word | writer | reader | ordering, and why |
| --- | --- | --- | --- |
| `entry[i]`, `ctx[i]` -- the vCPU's entry point and context | `CPU_ON`, before the swap | the released thread, after its acquire | plain writes, **published by the release swap below**. A thread that could see `RUNNING` and then a stale entry point is a guest entered at the wrong address -- on AArch64 a fault at whatever the word last held |
| `park[i]` -- `PARKED`/`RUNNING`/`QUIT` | `CPU_ON` (compare-and-swap from `PARKED`, **release**); `SYSTEM_OFF` (store `QUIT`, **release**) | the thread, in its park loop and after every `cosmo_vcpu_run` (**acquire**) | release on the write so the entry context is visible; acquire on the read so the thread that sees `RUNNING` sees that context. A *failed* swap needs no ordering -- it changes nothing |
| `stopping` -- the machine is powering off | `SYSTEM_OFF`, **before** the first `QUIT`, **release** | `CPU_ON`'s fast path, **acquire** | the flag must not become visible after the `QUIT` it precedes, or a `CPU_ON` could pass the fast path *and* find `PARKED`. It is only the fast path: the swap is what makes the race safe, so a stale read here costs a refusal the swap would have made anyway |
| `live` -- threads created and not yet returned | each thread as it leaves, `__atomic_fetch_sub` **acq_rel** | the supervisor, **acquire** | release so everything the thread did -- its last console bytes, its exit reason, test 4's timestamps -- is visible to the supervisor that sees zero; acquire so the supervisor's reads are not hoisted above it. This is the ordering the threads unit already got wrong once: the wake must be **last**, or a joiner sees a slot that is not free yet |
| a device model's state (`g_vio`, `g_vnet`) | any thread, under that model's mutex | any thread, under that model's mutex | the mutex **is** the ordering. Nothing in a device model needs an atomic of its own, which is the point of "one mutex per model" rather than "atomics where a race is noticed" |
| test 4's `[enter, exit)` timestamps | each vCPU thread, plain writes | the supervisor, after `live == 0` | published by `live`'s release, which is why the supervisor reads them **after** the count reaches zero rather than while the threads run |
| **`v->stop`** -- the kick's flag, in the kernel | `sys_vcpu_stop`, **release**, *then* the IPI | the run loop, **consumed with an `__atomic_exchange` (acq_rel)** at entry and after every host-interrupt exit | the store must be visible before the IPI that makes the target look at it, or the target exits, finds nothing and re-enters the guest -- a lost kick, which is a hang. Consumed by *exchange* rather than read-then-clear so that a stop arriving while one is being consumed is not swallowed: the exchange either returns it (this run stops) or lands after it (the next run stops) |
| **`v->in_guest`** -- one word carrying "inside a guest" and the host CPU it is on | the run loop, **release**, immediately before the VM entry; cleared **release** immediately after the exit | `sys_vcpu_stop`, **acquire** | this replaces the `loaded_cpu` read a previous draft proposed, which **was wrong** rather than merely unsynchronised: `loaded_cpu` is a plain `int` written by the run loop, `sys_vcpu_stop` cannot take `run_lock` without waiting behind the very guest it means to interrupt, and stickiness only stops the flag being *lost* -- it does nothing to make a *spinning* guest look at it. A flag that lands after the runner's last check, with the IPI sent to a CPU the vCPU has since left, leaves a guest spinning forever with its stop pending, which is the hang the kick exists to prevent |

The futex calls need no ordering on top of this: the kernel compares the
word under its own lock, so a wake between a thread's check and its wait
cannot be lost -- but the word must be written **before** the wake in
every case, which is what the release stores above give.

**The kick needs one more thing than an ordering, and it is the reason
`in_guest` exists.** Flag-then-IPI is necessary and not sufficient: the
flag can land after the runner's last look at it and before the guest is
entered, and then no IPI has anywhere correct to go. The answer is the
double-check every VMM ends up with:

1. the runner publishes `in_guest = CPU | IN_GUEST` (release) **before**
   the VM entry;
2. the runner then **re-reads the stop flag** (acquire) and abandons the
   entry if it is set;
3. the kicker stores the flag (release), then reads `in_guest` (acquire)
   and IPIs the CPU it names.

Either the flag was set before the runner's re-read -- and the entry never
happens -- or it was set after, in which case the kicker sees `IN_GUEST`
with a CPU that is still correct, because `in_guest` was published before
the entry and is not cleared until the exit. There is no third case, which
is what makes the kick a mechanism rather than a hope. `loaded_cpu` stays
what it is -- a VMCS bookkeeping field private to the run path -- and the
kick stops reading it.

### The kick

```c
#define SYS_vcpu_stop 88  /* (int vcpu) -> 0: make a running vCPU leave its run */
```

`SYS_COUNT` 88 → 89. It sets a per-vCPU `stop` flag and, if the vCPU is
loaded on a host CPU, sends that CPU an IPI (`arch_ipi_send`, which
exists). The host interrupt forces a VM exit; `vcpu_run_bounded` already
returns to the top of its loop on a host-interrupt exit, and there it
checks `stop` and returns a new exit kind:

```c
COSMO_VM_EXIT_STOPPED   /* the owner asked this vCPU to leave its run */
```

A new exit kind rather than `-EINTR`, because `-EINTR` already means a
fatal signal is pending and a caller must distinguish "the machine is
stopping" from "this process is dying". The flag is sticky until the next
`vcpu_run`, so a stop that lands between two runs is not lost -- the race
a flag-plus-IPI has if the flag is cleared by the kicker.

This is the mechanism KVM has and this tree does not, and it is the only
new kernel surface the unit needs. It also gives the *existing*
single-threaded owner something it lacked: a way out of a guest that never
yields, which today is why the last runnable vCPU may not run untimed.

### The locking rule, decided once and before the code

**One mutex per device model, taken by whoever enters it, never held
across a system call that can block on the guest.** Concretely:

- `g_vio` and `g_vnet` each gain a `cosmo_mutex_t`. Every entry point --
  the MMIO dispatch, the drain, the tap poll -- takes it. The 26 sites
  become entry points that take one lock, not 26 decisions.
- **The lock is not held across `cosmo_vcpu_run`.** Nothing else may be
  either: a thread inside its guest holds no owner lock, which is the
  invariant that keeps a guest from being able to stall the others by
  looping in MMIO.
- **The disk fd and the tap fd are each owned by their device's lock**, so
  a read and a write cannot interleave inside one request.
- **The console drain is the main thread's alone**, and the main thread is
  therefore a **supervisor, not a joiner that happens to drain.**
  `cosmo_thread_join` blocks until its thread exits, so a main thread that
  joined first could not drain at all -- and the guest's console ring is
  4 KiB that drops its *oldest* bytes when full, so a drain that stops is
  guest output silently lost, which is exactly what the harness's required
  markers are made of. The loop is therefore:

  ```
  while (live > 0) {
      drain_console(vm);
      if (tap_fd >= 0) vnet_service(&g_vnet);
      cosmo_futex_wait(&live, live_seen, DRAIN_INTERVAL_NS);   /* woken early by an exiting vCPU */
  }
  for each thread: cosmo_thread_join(...)   /* returns at once: live == 0 */
  ```

  `live` is the count of vCPU threads **created** -- parked ones included,
  for the reason the park's three states give above -- decremented with
  `__atomic_fetch_sub` and futex-woken by each thread as it leaves. The
  bounded wait is what keeps the ring drained while nothing exits; the
  wake is what stops the last drain from being a full interval late. The
  joins happen after the count reaches zero, so they are reaping, not
  waiting -- and the drain is still the main thread's alone, which is what
  keeps the output ordered.
- Counters the tests read (`served`, `draining`) become
  `__atomic_load_n`/`fetch_add`, because a count under a lock that a test
  reads from another thread is a lock the test would have to take.

The rule is stated here because the alternative -- adding a lock where a
review finds a race -- is what cost PR #42 five findings, and because the
second threaded program in this tree will copy whatever this one does.

### What the tests become

Neither machine-mode test is deleted; both are rewritten to assert the
property that replaces the artefact.

- `guest_dtb -c 2` keeps every marker, including `cpu1: up`. With threads
  the second vCPU runs concurrently rather than waiting for a turn, so the
  line appears because it is *running*, not because the power-off waited.
  The harness gate is unchanged, which is the point: the observable stays
  true for a better reason.
- `guest_offspin` becomes the **kick's** test rather than the bound's.
  `SYSTEM_OFF` arrives while the sibling spins forever; the owner kicks it
  and the supervisor reaps it. With the kick removed the boot hangs on its 180 s deadline,
  which is the same signature the bound's removal produced -- so the proof
  is the same shape as the one it replaces.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_vcpu_stop` (88), `SYS_COUNT` 88→89, `COSMO_VM_EXIT_STOPPED` |
| `kernel-services/virtualization/hvsys.c` | the handler: a handle with `VCPU_RUN`, then `vcpu_stop` |
| `kernel-services/virtualization/vcpu.c` | `stop` and `in_guest` on `struct vcpu`, the consume-by-exchange at entry and after each host-interrupt exit, the re-check before the entry, the IPI |
| `kernel/arch/x86_64/vmx.c`, `-/svm.c`, `kernel/arch/aarch64/hv_el2.c` | **the `in_guest` publication and clearing sit immediately around the VM entry, which is arch code** -- one release store before the entry and one after the exit in each backend. A previous draft's affected-files table omitted this, which is what naming the ordering without naming where it lives looks like |
| `kernel/include/kernel/hv.h` | the `stop` flag and `vcpu_stop`'s declaration |
| `libc/include/cosmo/syscall.h` | the `cosmo_vcpu_stop` stub |
| `userland/system/vmctl.c` | a thread per vCPU; the round-robin, `fresh[]` and the grace deleted; one mutex per device model |
| `tests/hv/aarch64/guest_offspin.S` | unchanged, but its meaning is now the kick |
| `kernel-services/virtualization/hvtest.c` | a kernel self-test for the kick itself, which needs no userland |
| `docs/kernel-services/virtualization/design.md`, `-/testing.md`, `-/api.md`, `-/invariants.md` | the machine-mode section rewritten; the fairness rule's removal recorded rather than erased |
| `docs/kernel/process/design.md` | §12's named consumer, satisfied |
| `README.md` | Status entry |

## New APIs

One syscall (`SYS_vcpu_stop`), one exit kind (`COSMO_VM_EXIT_STOPPED`),
one libc stub. No new device, no new ioctl. `COSMO_VCPU_RUN_ONE_TICK`
keeps its meaning and its users.

## Migration plan

1. **`SYS_vcpu_stop` and `COSMO_VM_EXIT_STOPPED` alone**, with a kernel
   self-test: a vCPU running a spin loop on one thread, stopped from
   another, must exit `STOPPED` rather than run on. Nothing depends on it
   yet, and the existing owner is untouched.
2. **The locking rule applied to `g_vio` and `g_vnet` while `vmctl` is
   still single-threaded.** Locks that nothing contends are still
   correct, and landing them first means the thread change is not also a
   locking change: if a test breaks in step 3 it is the threads.
3. **A thread per vCPU, parked at creation and released by `CPU_ON`**,
   with the round-robin, `fresh[]`, `off_pending`, `off_grace` and the
   PSCI turn boundary deleted in the same commit -- they are one mechanism
   and half of it is not a state worth shipping. The main thread becomes
   the supervisor: drain, service the tap, wait on the live count.
4. **`SYSTEM_OFF` sets `stopping`, sets every other thread to `QUIT` and
   wakes it, and kicks every vCPU in a guest**, and the supervisor reaps
   them when the count reaches zero. All of it in one step: the `QUIT`
   without the wake leaves a parked thread stuck, the wake without
   `CPU_ON`'s compare-and-swap lets a racing `CPU_ON` undo it, and the
   kick is what makes a running thread reach its next check.
5. **The tests**, then the bug-proofs.
6. **The documents**, including the design document's own account of why
   the fairness rule existed, which becomes history rather than
   disappearing.

## Tests

1. **The existing machine-mode markers, unchanged** (`guest_dtb -c 2`):
   the `dtb:` line, the PSCI version, `cpu1: up ctx=1234cafe`,
   `cpu_on 1 -> 0`, `vmctl: guest powered off`. The harness gate does not
   move; what changes is why it holds.
2. **`guest_offspin` as the kick's test**: `SYSTEM_OFF` with a sibling
   spinning forever must still stop the machine and print
   `HVTEST: offspin ok`.
3. **A kernel self-test for the kick** (`hvtest.c`), so the mechanism is
   proved where a userland program cannot be the only witness: a vCPU
   entered on a spin loop, stopped from another thread, exits `STOPPED`;
   a stop that arrives *between* runs is not lost, because the flag is
   sticky; a stop on a vCPU that is not running is not an error.
4. **Two vCPUs really run at once** -- and this is the one test in the list
   that had to be redesigned before the report was worth merging. The
   first version asserted that two guest counters both advanced within a
   wall-clock window, and borrowed `thrtest` step 3's argument that
   progress is the honest assertion and parallelism merely makes it fast.
   **That argument does not transfer**: step 3 exists to prove *progress*,
   so a weaker-but-true assertion is right there, whereas this step exists
   to prove *simultaneity* -- and two counters advancing is exactly what
   the round-robin this unit deletes already does. The test would have
   passed before the change it is meant to verify, which is the definition
   of proving nothing.

   What distinguishes the two implementations is **overlap**, so overlap is
   what the test asserts. Each vCPU thread records a monotonic timestamp
   immediately before `cosmo_vcpu_run` and immediately after it returns;
   the owner then checks that some pair of `[enter, exit)` intervals from
   *different* vCPUs intersects. Under any single-threaded loop no two
   intervals can intersect, however the ticks are interleaved -- the one
   thread is inside exactly one run at a time. Under a thread per vCPU on
   a host with two CPUs they will.

   Overlap is a *logical* property of two intervals, not a duration, so
   the assertion does not weaken under load the way a "both advanced in
   200 ms" bound would -- a loaded host makes the intervals longer, which
   makes overlap more likely rather than less.

   **It needs a host with at least two CPUs**, and says so: with
   `QEMU_SMP=1` the test prints that it is skipped and why, because a
   single-CPU host cannot produce overlap and a test that passed there
   would be asserting nothing again. The harness runs `-smp 4`.
5. **The device models under two guest CPUs**: both CPUs driving the same
   virtio-blk queue, with every request completing and the disk's contents
   correct afterwards. An unlocked queue loses or duplicates a descriptor,
   which shows as a wrong byte rather than only as a crash.
6. **The handle table from several threads** (the §12 gap): four vCPU
   threads using one VM handle, plus the main thread reading the console
   through it, with no lost or duplicated handle operation.
7. **`CPU_ON` racing `SYSTEM_OFF`**: a guest whose secondary asks for a
   third CPU in a loop while the primary powers off. The observable is
   that **the machine stops** -- with a plain store instead of the swap it
   does not, because a revived thread keeps `live` above zero. This is the
   lifecycle's own test, and like `guest_offspin` its failure is a hang
   rather than a wrong value, so the 180 s deadline is the detector.

**Bug-proofs**: **the entry's re-check of the stop flag removed** (the
window the `in_guest` double-check exists to close: a stop that lands
between the runner's last look and its VM entry is pending while the guest
spins, no IPI has anywhere correct to go, and `guest_offspin` hangs on the
180 s deadline. This is the proof that the kick is a mechanism rather than
a race, and it is also the one whose window is small enough that it needs
a spinning guest to hit at all); **the kick's flag stored after the IPI
instead of before it** (the target exits on the interrupt, finds nothing, and re-enters the
guest -- a lost kick, so `guest_offspin` hangs on the 180 s deadline,
which is the same observable as no kick at all and is why the ordering is
part of the mechanism rather than a detail of it); **the flag read and
cleared instead of exchanged** (a stop arriving inside that window is
swallowed and the vCPU runs on); **the entry context published without a
release** (the
swap made `__ATOMIC_RELAXED`: a released thread can see `RUNNING` with a
stale entry point and enter its guest at whatever the word last held,
which the owner reports as an unexpected exit -- and which a single-CPU
run cannot produce, so the proof needs `-smp 4` and is exactly the kind
that passes on the wrong machine); **`CPU_ON` releasing a thread with a
store instead of a compare-and-swap** (a `CPU_ON` racing `SYSTEM_OFF` revives a vCPU after
shutdown began, `live` never reaches zero, and the owner hangs -- the same
180 s deadline as the parked case, which is why both belong to the
lifecycle rather than to the kick); **`SYSTEM_OFF` kicking but not waking
the parked threads**
(a guest that starts one secondary of four and powers off leaves three
threads parked forever, `live` never reaches zero, and the owner hangs --
the boot's 180 s deadline again, on the most ordinary path there is, which
is why this one is first); the kick's IPI not sent (the stop flag is set but a
spinning vCPU never leaves, so `guest_offspin` hangs the boot -- the same
180 s signature the bound's removal produced); the stop flag cleared by
the kicker rather than the runner (a stop between runs is lost and the
sibling runs on); `g_vio`'s lock removed (test 5 sees a wrong byte); the
lock *held* across `cosmo_vcpu_run` (one guest looping in MMIO stalls
every other vCPU, which test 4's overlap no longer finds); **the vCPUs run
from one thread** (the round-robin restored: test 4 finds no overlap, which
is the proof that test 4 tests the thing this unit changes -- the
assertion it replaced passed both ways); and the main thread joining
before draining rather than supervising (the console ring overflows and
the guest's required markers go missing, which is the failure the drain
loop exists to prevent).

## Benchmarks

The claim is that a two-CPU guest gets more than one CPU's worth of
progress, which the round-robin cannot give at any tick length. Reported,
not gated: the boot harness runs under QEMU with `-smp 4` and its timing
is not a benchmark rig. Test 4's overlap is the *correctness* assertion;
the throughput number beside it -- both counters' totals over a fixed
window -- is the measurement, and it is printed rather than asserted
precisely because it is the load-sensitive half. The owner's own cost should fall too: a turn today ends with a
syscall return, a drain and a dispatch on every tick, and an untimed run
ends only when the guest stops.

## Risks

- **A guest can now stall the owner in a way it could not.** A vCPU thread
  runs untimed, so a guest that never exits holds that thread forever.
  That is what the kick is for, and the kick is the mitigation for the
  whole class: nothing in the owner may wait on a vCPU thread without
  being able to stop it.
- **Deleting the fairness rule changes an observable the harness gates
  on.** `cpu1: up` must still appear. With threads it should appear more
  reliably rather than less, but "should" is why test 1 keeps every marker
  and why the rule's deletion and the threads land in one commit -- so a
  bisect lands on the change that did it.
- **The locking rule is a promise about every future entry point.** A
  device model added later that forgets the lock is a race the tests may
  not reach. Stating the rule in `vmctl.c`'s own header comment, where the
  next author will read it, is the only enforcement available.
- **The VMCS migration path gets exercised far harder.** `vmclear` on a
  move is issued from the *new* CPU (`vmx.c:1066`); the same-VMCS-on-two-CPUs
  case is prevented by `run_lock`, so this is a pre-existing path meeting
  more traffic rather than a new hazard -- but it is the first thing to
  suspect if x86 guests misbehave under threads.
- **Four threads per VM against `PROCESS_MAX_THREADS` (256) is nothing**,
  but `COSMO_RLIMIT_NTHREAD` does not exist, so a process that creates
  many VMs has no limit but the thread table. Named, not fixed here.

## Alternatives considered

- **Keep one thread and shorten the tick.** Cheaper, and it makes the
  fairness rule matter *more* rather than less: a shorter tick means more
  preemptions before a first instruction. It also cannot give a guest more
  than one CPU's worth of time, which is the point.
- **A thread per vCPU without a kick, keeping `ONE_TICK`.** Each thread
  returns periodically and checks a shared stop flag, so no new syscall is
  needed. But then every thread still pays a syscall return per tick, the
  tick length is still a tuning knob in the owner, and a guest's
  `SYSTEM_OFF` still completes a tick late -- most of the artefacts
  survive. The kick is what makes the threads honest.
- **`v->dead` as the stop.** It exists, but it is read once at entry under
  `run_lock`, so it cannot disturb a running vCPU without the IPI this
  design adds anyway -- and "dead" means something else (a vCPU that has
  faulted).
- **Per-thread signals (a native `tgkill`) as the kick.** The general
  mechanism, and a larger unit: signal targeting, a per-thread pending
  set, and the delivery rules. `SYS_vcpu_stop` is the specific answer, and
  a `tgkill` later does not make it redundant -- a vCPU stop that does not
  run a signal handler is what a VMM wants.

Named and deferred: `COSMO_RLIMIT_NTHREAD`; a native `tgkill`; a vCPU's
own thread name for `/proc`; guest-visible CPU topology in the device tree
beyond the count; and the owner's device models moving out of `vmctl` into
a library once a second program needs them.
