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
vCPU untimed, handle the exit, repeat until the vCPU stops. The main
thread creates the VM, loads the image, builds the device tree, starts one
thread per vCPU the tree promises, and then joins them.

`COSMO_VCPU_RUN_ONE_TICK` stays in the ABI -- the kernel's own tests use
it and it is what `-ETIMEDOUT` is built on -- but the owner stops passing
it. `fresh[]`, `off_pending`, `off_grace`, `MACHINE_OFF_GRACE_TURNS` and
`machine_fresh_sibling` all go, along with the PSCI turn boundary.

`CPU_ON` starts a thread rather than marking a slot runnable. `SYSTEM_OFF`
stops the machine: the asking vCPU's thread returns, and the others are
**kicked** and then joined. That is the honest PSCI shape -- a real
`SYSTEM_OFF` does not wait -- and it is only implementable with the kick
below.

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
- **The console drain is the main thread's alone.** Rather than three
  threads racing to drain one ring, the main thread drains it while the
  vCPU threads run, which is also what makes the drain's own ordering
  observable.
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
  and joins. With the kick removed the boot hangs on its 180 s deadline,
  which is the same signature the bound's removal produced -- so the proof
  is the same shape as the one it replaces.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_vcpu_stop` (88), `SYS_COUNT` 88→89, `COSMO_VM_EXIT_STOPPED` |
| `kernel-services/virtualization/hvsys.c` | the handler: a handle with `VCPU_RUN`, then `vcpu_stop` |
| `kernel-services/virtualization/vcpu.c` | `stop` on `struct vcpu`, the check in `vcpu_run_bounded`, the IPI |
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
3. **A thread per vCPU**, with the round-robin, `fresh[]`, `off_pending`,
   `off_grace` and the PSCI turn boundary deleted in the same commit --
   they are one mechanism and half of it is not a state worth shipping.
4. **`SYSTEM_OFF` kicks and joins.**
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
4. **Two vCPUs really run at once**, which the round-robin could not do:
   a guest whose two CPUs each increment their own counter in memory, with
   the owner asserting both advanced within one wall-clock window. This is
   the assertion the unit exists for, and it needs two CPUs to pass --
   a single-CPU host makes it pass by preemption, which is the same
   weaker-but-true shape `thrtest` step 3 uses deliberately.
5. **The device models under two guest CPUs**: both CPUs driving the same
   virtio-blk queue, with every request completing and the disk's contents
   correct afterwards. An unlocked queue loses or duplicates a descriptor,
   which shows as a wrong byte rather than only as a crash.
6. **The handle table from several threads** (the §12 gap): four vCPU
   threads using one VM handle, plus the main thread reading the console
   through it, with no lost or duplicated handle operation.

**Bug-proofs**: the kick's IPI not sent (the stop flag is set but a
spinning vCPU never leaves, so `guest_offspin` hangs the boot -- the same
180 s signature the bound's removal produced); the stop flag cleared by
the kicker rather than the runner (a stop between runs is lost and the
sibling runs on); `g_vio`'s lock removed (test 5 sees a wrong byte); the
lock *held* across `cosmo_vcpu_run` (one guest looping in MMIO stalls
every other vCPU, which test 4's window catches); and the console drain
moved back into the vCPU threads (interleaved output, which the marker
lines themselves detect).

## Benchmarks

The claim is that a two-CPU guest gets more than one CPU's worth of
progress, which the round-robin cannot give at any tick length. Measured
as test 4 measures it -- both counters' advance in one wall-clock window,
on a host with at least two CPUs -- and reported, not gated: the boot
harness runs under QEMU with `-smp 4` and its timing is not a benchmark
rig. The owner's own cost should fall too: a turn today ends with a
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
