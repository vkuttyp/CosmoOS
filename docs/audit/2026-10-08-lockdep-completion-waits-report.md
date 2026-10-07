# Lockdep completion waits: completion classes, and what the suite never drives

Date: 2026-10-08. Branch `lockdep-completion-waits` from `main` at
`e3d9e6fe`. Scope: the second half of `docs/plan.md` §1 "Implementation —
callback-wait dependencies". Timer callback waits were done on 2026-10-05
([callback-classes report](2026-10-05-lockdep-callback-classes-report.md));
two gaps kept the item open: a wait on a completion that a callback or
another thread signals was not modelled, and a callback path that never
executed contributed no edge. Part A closes the first; Part B makes the
second a named list. A housekeeping commit precedes both.

## Summary

- **Completion waits are in the lock graph.** Each completion name is a
  lockdep class (`LOCKDEP_KIND_COMPLETION`). A wait records the mutexes
  held across it (`mutex → completion`); a `complete()` in thread context
  records the mutexes held at the call (`completion → mutex`), kept
  pending in the thread until its next completion event, mutex release or
  exit commits them. A cycle is `LOCKDEP_R_COMPLETION`, from whichever side
  closes it. Only mutexes can be on such a cycle in this kernel: the waiter
  holds no spinlock (`might_sleep`), and no spinlock node reaches a mutex
  node, so an interrupt or a timer callback that signals records nothing
  and is no gap. No history is kept, which is what crossrelease and DEPT
  kept and paid for in false positives.
- **The tree reported nothing.** Full debug suites at one, two and four
  CPUs on both architectures, the chaos boots and the network harness
  produced no `LOCKDEP_R_COMPLETION` report beyond the four the new test
  expects; the graph's edges into and out of completion classes are listed
  below and every one is a mutex the survey predicted.
- **A shape the first design would have misreported**, found in the survey
  before the first boot: the RAM block device completes a bio synchronously
  inside `submit`, so `sync_io` under a VFS mutex completes its own
  `blk-sync` holding that mutex and then waits. The signal side is
  therefore deferred, and discarded when the thread's next completion event
  is its own wait for the same object; the same rule covers a cancel after
  a timed-out wait (USB, AHCI). The control that exercises it panics the
  boot when the rule is removed.
- **Coverage listing.** After every debug suite the runner prints the timer
  callback functions set up and never run, and the completion classes
  never signalled or never waited for. On this tree: one callback function
  of twenty (`delack_timer`, TCP's delayed acknowledgement, set up 242
  times and never fired) and four completion classes: one production class that is polled rather
  than waited for (`xhci-first-scan`), and three test classes (one
  completed by nobody by design, two whose test joins the threads instead).
- **The class table went from 384 to 512.** The 38 completion classes took
  a full x86-64 debug boot to 382 of 384.

## Housekeeping (the first commit)

- `docs/plan.md` §7 gained an item for the netif flag reads: the writers
  publish with release stores, but `fw.c`, `ipv4.c` and `nat.c` read
  `NETIF_FORWARD`, `NETIF_MASQUERADE` and `NETIF_LOOPBACK` with plain
  loads, several per packet, so a runtime toggle can be seen differently by
  two checks of one packet; read the word once per packet with a relaxed
  atomic load. The `netif_set_up` comment and N25 no longer say that every
  data-path reader acquires.
- `tools/neigh-down-race-probe.py` takes one race at a time
  (`--old arp-input|arp-resolve|nd-input|nd-resolve`) and requires the
  failure at that race's own check, by line number in the patched test
  source. The advertisement path has no mode: `nd_input_na` never
  allocates, so a flush leaves it nothing to complete under either order.
  Four CPUs, about 90 s to the verdict each:

  | `--old` | x86-64 | AArch64 |
  |---|---|---|
  | `arp-input` | FAIL at `!arp_lookup(&d.nif, r.ip4, mac)` | the same |
  | `arp-resolve` | FAIL at `r.rc == -ENETUNREACH` (the ARP resolve case) | the same |
  | `nd-input` | FAIL at `nd_resolve(&d.nif, &peer6, mac, NULL) == -EINPROGRESS` | the same |
  | `nd-resolve` | FAIL at `r.rc == -ENETUNREACH` (the ND resolve case) | the same |

- `arp_test_park_before_lock` and `nd_test_park_before_lock` read the
  arming word with a relaxed load before the CAS, so the unarmed hook costs
  the input path no atomic read-modify-write.

## Part A: completion waits

### 1. Survey: every non-test completion

The waiter column names the locks typically held across the wait, the
signaller column the context that calls `complete()` and what it holds at
the call. "Self" means the signaller is the waiter's own thread, inside
the waiter's call chain.

| Completion (name) | Initialised by | Waited by (holding) | Signalled by (context; mutex held at the call) | Signaller kinds |
|---|---|---|---|---|
| `thread-exit` | `thread_create` (the creator) | `thread_join`: tests, process teardown, `vfs_sync` under `mount-sync` (the graph shows `mount-sync → thread-exit`) | `thread_exit`, the exiting thread; nothing (a mutex held at exit is `LOCKDEP_R_EXIT_HELD`) | one |
| `process-exit` | process creation (the parent) | `process_wait_exit`: the self-test runner, tests | `process_exit`, the exiting process's thread, after the table lock is dropped; nothing | one |
| `reap-hold` (debug seam) | `reap_hold_arm` (the test thread) | the reaping or exiting path parked at the seam; nothing | the test thread (`process_test_release_reap`); nothing | one |
| `vm-file-hold`, `vm-anon-hold` (debug seams) | the arming test thread | the faulting thread, with the space lock dropped first | another faulting thread of the same space (`file_hold_release`, `anon_hold_release`), after installing the page; nothing | one |
| `blk-sync` | `sync_io`, per call | `blk_read`/`blk_write`/`blk_flush` callers: cosmofs and the VFS (`vnode`, `pagecache`, `cosmofs`, `mounts`, `rename`, `mount-sync`, `file` in the graph), tests | `sync_done` from `bio_complete`: the virtio-blk, NVMe and AHCI interrupt handlers; **the submitter itself** when the RAM block device completes in `submit` (self; the submitter's locks); the RAM block device's worker in its deferred mode; the block timeout thread through a driver's `timeout` (under `blkdevs`, the block registry mutex); `blk_unregister` failing in-flight bios (under `blkdevs`); USB storage's `usbs_finish` from a transfer callback (interrupt), or from a cancel or removal (thread, under `usb-hcd`) | many |
| `netif-barrier` | `netif_unregister` step 4 | `netif_unregister` under `netif-unregister` | the network worker running `barrier_fn`; nothing. Every item ahead of it in the worker's queue (`input_one` and the receive path, `age_work`, `pcb_work`) takes spinlocks only: the only `mutex_lock` calls in `kernel-services/network` are `netif_unregister`'s own and the `ksock_*` system-call paths | one |
| `usb-sync` | `usb_sync_msg`, per call | `usb_control_msg`/bulk callers: enumeration under `usb-hcd`, the hub and HID drivers, USB storage's probe under `blkdevs`, module init under `devices`/`modules` | the xHCI event handler (interrupt); the waiter itself through `usb_cancel` after its timed wait gave up (self); `ring_flush` from `usb_device_left` on removal (thread, under `usb-hcd`) | three |
| `nvme-admin` | `admin_cmd`, per call | `admin_cmd` under `nvme-admin` (the admin mutex), at bring-up under `devices`/`modules` | `queue_process` from the interrupt handler; `controller_die` from the timeout path (thread, under `blkdevs`); the waiter itself through `queue_process` in the no-vector fallback, which **polls** `completion_done` instead of waiting (self; `nvme-admin` held) | three |
| `ahci-sync` | `cmd_sync`, per call | `cmd_sync`: IDENTIFY at probe under `ahci-hotplug`, tests under `devices`/`modules` | `port_complete` (interrupt); `slots_fail` from `port_restart`: the waiter itself after its timed wait gave up (self), the block timeout thread for a bio victim (under `blkdevs`), a detach from `ahci_debug_presence` (under `ahci-hotplug`); the task-file error path (interrupt) | several |
| `xhci-cmd` | `xhci_cmd`, per command, under `xhci-cmd` (the command mutex) | `xhci_cmd` under `xhci-cmd`, at probe under `usb-hcd`, `blkdevs`, `devices` | the event handler (interrupt); nothing in thread context | one |
| `xhci-first-scan` | probe | **polled** (`completion_done`) by module init under `g_controllers_lock`; never waited for | the xHCI port worker (thread); nothing | one |

Test completions (`smptest.c`, `schedtest.c`, `vfstest.c`, `lockuptest.c`,
`lockdeptest.c`) are not in the table; they are in the coverage listing
below where it matters.

What the survey settles:

- **No signaller holds a mutex at `complete()` that any waiter of its
  class holds across the wait.** The cross-thread signallers that do hold
  one (`blk_unregister` and the timeout thread under `blkdevs`, USB removal
  under `usb-hcd`, an AHCI detach under `ahci-hotplug`) hold locks no
  `blk-sync`, `usb-sync` or `ahci-sync` waiter holds. The AHCI probe does
  wait under `ahci-hotplug`, and a detach completes slots under
  `ahci-hotplug`, but a detach and a probe of one port exclude each other
  on that mutex: the waiter's own wait is what keeps the detach out, which
  is the dependency the graph records (`ahci-hotplug → ahci-sync`), and the
  closing edge (`ahci-sync → ahci-hotplug`) would need a detach to complete
  a sync slot while a probe holds the mutex, which the mutex forbids. If a
  boot ever drives it, it is a report to read, not to dismiss.
- **Three self-signalling sites**, two after a timed-out wait (USB, AHCI)
  and one synchronous in `submit` (the RAM block device), are why the
  signal side is deferred (design.md, "Self-signalling").
- **One site the model does not see**: the NVMe no-vector fallback polls
  rather than waits, so its self-signal under `nvme-admin` is committed at
  the next mutex release as if another thread had made it. The path runs
  only without an admin vector, which bring-up always has under QEMU; a
  report there would be this class conflation, and the fix is a handshake
  wait in the fallback (which the frame-lifetime argument would welcome).
- **The "lock, unlock, complete" shape** -- a signaller that takes and
  releases a mutex on its way to `complete()`, which a waiter holding that
  mutex would block -- has no production instance: the thread signallers
  above hold their mutex *at* the call or take none.

### 2. The design, in brief

`docs/kernel/lockdep/design.md`, "Completion waits", has the argument;
`invariants.md` L21 the rule. The points a reader of the code needs:

- `completion_init` classifies the completion's own spinlock at once (as
  its first acquisition would) and keys the completion class through it in
  a 512-entry table, so `struct completion` gains no field (module ABI v4
  unchanged, `LOCKDEP=0` layout identical) and every hook is one load and
  one table read after the first use of a name.
- The wait side is `lockdep_callback_wait`'s primitive with its own report
  kind: acquired, never held, on every call, done or not.
- The signal side records nothing in interrupt context or with no mutex
  held (the common case costs one load and one table read). With mutexes
  held it stashes `{object, node, held mutex nodes}` in
  `thread.completion_pending` (40 bytes, every build). `lockdep_release` of
  a mutex, `lockdep_thread_exit`, and the next completion hook commit it
  with the cycle check; the thread's own wait for the same object discards
  it.
- `completion_done()` records nothing: a query, not a wait.

### 3. Against crossrelease and DEPT

Crossrelease (Linux 4.14, reverted in 4.15) added, at a completion's
release, a dependency from the completion to every lock the releasing
context had acquired since the wait began, out of a per-task lock history;
DEPT (RFCs 2022–2024, unmerged) generalises that to every wait/event pair.
The history is where their false positives came from: unrelated locks a
kworker took for an earlier item, locks taken and dropped for other reasons
on the way to the release. Both also recorded only while a wait was in
flight, so a signal before its wait left nothing.

This design keeps no history. The signal side records what is **held at
the call**, which is a prerequisite by construction; the wait side what is
held at the wait, as every lockdep edge is; both persistently, so both
orders are caught. What it gives up is the "lock, unlock, complete" shape,
bounded by the survey above. The one subtlety the no-history rule had to
add is the deferred commit for self-signals, which crossrelease's window
got for free (a lock held before the wait began was outside its history).

### 4. The graph on this tree

A full x86-64 debug boot (four CPUs) records these edges touching the
production completion classes (test classes omitted), read from the
runner's graph dump:

| From | To | Why |
|---|---|---|
| `vnode`#0/#1/#2, `pagecache`, `cosmofs`, `mounts`, `rename`, `mount-sync`, `file` | `blk-sync` | cosmofs and the VFS do synchronous block I/O under their mutexes |
| `blk-sync` | `blkdevs` | a `blk-sync` was completed by a thread holding the block registry mutex: `blk_unregister` failing in-flight bios, or the timeout thread through a driver's `timeout`. A sync I/O made under `blkdevs` could never be rescued by removal; no caller makes one |
| `mount-sync` | `thread-exit` | `vfs_sync` joins a thread under its mutex |
| `netif-unregister` | `netif-barrier` | the unregister waits for the worker barriers under its mutex |
| `nvme-admin` (mutex), `devices`, `modules` | `nvme-admin` | admin commands under the admin mutex, at bring-up under the device and module registries |
| `xhci-cmd` (mutex), `usb-hcd`, `blkdevs`, `devices` | `xhci-cmd` | xHCI commands under the command mutex, at enumeration under the host-controller mutex, from USB storage's probe under the block registry |
| `usb-hcd`, `blkdevs`, `devices`, `modules` | `usb-sync` | USB transfers from enumeration, storage probe and module init |
| `ahci-hotplug`, `devices`, `modules` | `ahci-sync` | IDENTIFY at probe under the port's hotplug mutex and the registries |

Every edge into a completion class comes from a mutex (fact 1); the one
edge out of a completion class goes to a mutex (fact 3); no cycle closes.
The boot's 430 self-tests, the user-mode suite and the network harness
produced no `LOCKDEP_R_COMPLETION` report other than the four the test
expects. The RAM block device's synchronous completions under the VFS
mutexes, which the undeferred design would have turned into `blk-sync →
vnode` and a report at the next sync read under `vnode`, left no edge.

### 5. Tests and controls

`lockdep-completion` (`kernel/core/lockdeptest.c`, pinned, every case
through the real APIs on private mutexes and completions named per case):

1. a signaller holding L1 completes, then a wait holding L1: reported at
   the wait, the completion already done;
2. a wait holding L2, signalled by a thread holding nothing (no report),
   then a complete() of the same class holding L2: reported on the
   signaller's CPU when its unlock of L2 commits the complete();
3. `M3 → L3` recorded elsewhere, the signaller holding M3, a wait holding
   L3: reported at the wait (`L3 → C → M3 → L3`); the signaller never took
   L3;
4. a timed wait against a signaller holding L4: reported;
5. silent: a timed wait holding L5 that gives up, then this thread
   completing its own object holding L5 and waiting for the handshake
   (the USB and AHCI shape);
6. silent: a timer callback completing under a spinlock while the waiter
   holds L6;
7. silent: a self-IPI handler completing under a spinlock while the waiter
   holds L7;
8. silent: a signaller holding L8 against a waiter that released L8 before
   the wait;
9. silent: this thread completing its own object under L9 before waiting
   (the RAM block device shape), twice.

A silent case that reports panics the boot, so the controls are checked by
every debug boot. The first boot of the branch failed case 3 at
`hits_chain == 1`: the test had recorded `L3 → M3` and expected a cycle
that needs `M3 → L3` (a thread holding M3 taking L3 blocks on the waiter,
and the signaller behind it on M3). Cases 1 and 2 had passed, including
the deferred report at the signaller's unlock. The case was corrected;
nothing in the checker changed.

`tools/lockdep-completion-probe.py` (throwaway worktrees, `gmake test`):

| Mode | Removes | Required failure | x86-64 | AArch64 |
|---|---|---|---|---|
| `no-wait` | the wait hook | case 1, `hits_signal_first == 1` | required failure | required failure |
| `no-signal` | the signal hook | case 1, `hits_signal_first == 1` | required failure | required failure |
| `no-signal-check` | the signal-side cycle check (edges still recorded) | case 2, `hits_wait_first == 1` | required failure | required failure |
| `no-self` | the discard of a thread's own pending complete() at its own wait | the boot panics with `lockdep: a completion wait holds a lock its signaller needs`, its detail naming `lockdep-cm-l5` and `lockdep-cm-self`, before the test's result line | panic: `complete() held 'lockdep-cm-l5'#0, and a wait for 'lockdep-cm-self' is recorded holding it`, chain `'lockdep-cm-l5'#0 -> 'lockdep-cm-self'#0`, 22 s in | the same, 27 s in |

Eight of eight. The two modes that fail at the same check remove the two
halves of one cycle (the wait's edge and the signal's edge), as the
callback probe's `no-wait` and `no-class` do; `no-signal-check` keeps the
signal's edge and so fails only where the signal is the side that must
report.

### 6. Capacity

The first boot of the change reached **382 of 384** classes, against the
332 a four-CPU x86-64 debug boot of `main` creates at the branch point
(the callback-classes report measured 324 on 2026-10-05): 38 completion
classes, and the new test's mutexes and spinlocks for the rest.
`LOCKDEP_MAX_CLASSES` is 512 now; the graph is 2048 nodes × 32 words
(512 KiB, `LOCKDEP=1` only), the dump's snapshot 573,448 bytes, and the
host `search-work` bounds follow the macros. A full four-CPU debug boot
reaches **408** classes on both architectures (353–354 at one CPU, 321 at
two, where the per-CPU run-queue classes and some tests differ); `main`
reaches 332.

## Part B: what the suite never drives

Debug builds record every callback function `timer_setup` registers (a
64-entry lock-free table in `timer.c`) and mark it when `run_expired` runs
it, and every completion class records whether a wait and a `complete()`
named it. The self-test runner prints both listings after the suite, after
the graph dump.

### Timer callbacks

The source sets up 23 distinct timer callback functions. Two are
AArch64's alone (`test_tick` in `kernel/arch/aarch64/timer.c`, and the EL2
guest UART test's `wake_by_typing`); a full x86-64 debug boot sets up the
other 21, and one of them never runs:

| Callback | Set up | Where | Why it never runs | Matters? | A test that would cover it |
|---|---|---|---|---|---|
| `delack_timer` (`kernel-services/network/tcp.c`) | 242 times, one per TCP control block created in the boot (`tcp_pcb_new`) | armed for 40 ms on every second in-order data segment (`tcp.c`, the receive path), cancelled whenever an acknowledgement goes out first | every TCP exchange in the suite and the harness acknowledges within 40 ms, by the next data segment or by the send side's own output, so the delayed acknowledgement is always cancelled before it fires | a protocol path, not an error or teardown path: the callback is `timer_kick(pcb, WORK_DELACK)`, the same function `rexmit_timer` (which runs) calls, so its locks are in the graph under the retransmit callback's class and not under its own; a wait across `timer_cancel_sync(&pcb->delack)` (`tcp_pcb_free`) is checked against the retransmit class's edges only by coincidence of code sharing | cheap: a loopback receiver that reads one segment and sends nothing for more than 40 ms (the `net-lo-tcp` fixture), then checks the acknowledgement arrived; recorded in `docs/plan.md` §1 |

The other twenty ran, grouped by subsystem: the scheduler's `sleep_fired`
(`thread_sleep_ns` and its killable form share it); the futex, poll, epoll
and aio timeouts (`timeout_fired`, three `alarm_fired`) and the timer
object's `timer_obj_fired`; the network's `age_timer` (ARP) and TCP's
`rexmit_timer`, `keep_timer` and `timewait_timer`; the e1000e watchdog;
and the tests' `cb_a`, `cb_b`, `cb_c`, `cont_timer`, `cm_timer_cb`,
`timer_probe_fn`, `rearm_cb`, `tagged_cb` and `storm_timer`. The paths one
might have expected to be quiet -- the TIME-WAIT and keepalive timers, the
poll family's deadlines -- all fired at least once in the suite. Error and
teardown callbacks in this kernel are not timers: device removal and
controller death run on threads (the block timeout thread, the USB
worker) or in interrupt handlers, which the callback-class mechanism does
not cover and which this listing therefore does not judge. The table has
64 entries and was not full (`timer: 21 callback functions set up this
boot, 1 never ran`); the AArch64 boot's listing is in the validation
section.

### Completion classes

Forty-four completion classes exist in a full x86-64 debug boot (the
eleven production names of the survey, nine of this unit's test, the rest
other tests'); five are not both waited for and signalled:

| Class | Listing | Judgement |
|---|---|---|
| `xhci-first-scan` | signalled, never waited for | polled with `completion_done` by the USB class drivers' module init under `g_controllers_lock`, bounded at 3 s; the worker that signals it takes no mutex, so the poll under the mutex is safe today and outside the graph; a `wait_for_completion_timeout` would put it in |
| `ct-none` (`completion-timeout` test) | waited for, never signalled | by design: the test's completion nobody completes |
| `chrblock-reader`, `chrblock-writer` (`vfs` tests) | signalled, never waited for | the test joins the threads instead of waiting on their completions; the completions are redundant with the join. Harmless |
| `lockdep-cm-sync-submit` (this unit's test, first boot) | signalled, never waited for | the self-signal discard returned before marking the wait; the wait is marked now and the class no longer appears |

The production classes the survey expected to be driven by interrupt
handlers only (`nvme-admin`, `xhci-cmd`, `usb-sync`, `ahci-sync`) are
listed as both waited for and signalled: the signal mark is made in any
context, so an interrupt-only signaller is not mistaken for a missing one
(the first boot of the branch had that mistake; the listing showed the
four as "never signalled" and was wrong).

## Release builds

Every hook is a `static inline` no-op without `CONFIG_LOCKDEP`, and the
callback table exists only with `CONFIG_DEBUG`. Release kernels of `main`
(`e3d9e6fe`) and of the branch (`e2eb847d`), both built in the same clone
directory so that `__FILE__` strings and paths agree, have **identical
defined-symbol tables** (name, size and type; `llvm-nm -S --defined-only`)
on both architectures, and neither contains a `lockdep_completion_*`,
`lockdep_dump_completion_coverage`, `timer_dump_callbacks` or
`callback_cover_*` symbol. `struct thread` is 40 bytes larger in every
build, which changes no symbol size.

A first comparison, of the branch's release built in the working tree
against `main`'s built in the clone, showed 8 differing lines on x86-64
and 74 on AArch64, all in functions this change does not touch (`crc32c`,
`lz4_decompress`, `sha512_block`, the PCI legacy accessors): the two trees
sat at different paths, and the AArch64 immediates that encode
`__FILE__`-derived lengths differ with the path. A control must differ in
one thing; the same-path build above is that control.

## Validation

On `e2eb847d`, one chain, one QEMU at a time at its default priority
(sampled: `pri 31 nice 0`), no `&` loops; the x86-64 four-CPU rows are
the two timing boots:

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test` (lockdep host models at 512 classes, 2048 nodes) | PASS (62 s) | -- |
| `make host-test-lockdep-tsan` | PASS | -- |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 139.3 s and 140.1 s, 430 self-tests, 408 classes | PASS 141.3 s, 430 self-tests, 408 classes |
| `make test-smp2` | PASS 148.1 s | PASS 142.1 s |
| debug boot, 1 CPU | PASS 131.9 s | PASS 131.2 s |
| `make test-chaos` | PASS 139.5 s | PASS 143.6 s |
| release build and boot | PASS 16.6 s | PASS 20.1 s |
| `tools/lockdep-completion-probe.py`, four modes | 4 of 4 required failures | 4 of 4 |
| `tools/neigh-down-race-probe.py --old`, four races (housekeeping) | 4 of 4 required failures | 4 of 4 |

`lockdep-completion` passed in every debug boot (22–29 ms). The AArch64
listing agrees with x86-64's: `delack_timer` the one callback of 22 set up
(the AArch64 tick adds one) that never ran, and the same four completion
classes.

**Self-test time against `main`**, x86-64, four CPUs, alternated boots
(`main`, branch, `main`, branch), `SELFTEST: timing total=`:

| Run | `main` (332 classes) | branch (408 classes) |
|---|---|---|
| 1 | 113.1 s | 111.0 s |
| 2 | 110.7 s | 111.9 s |

The difference is inside `main`'s own spread (2.4 s). Both trees run with
`LOCKDEP=1`, so the comparison is like with like; the absolute numbers are
the debug-with-lockdep ones (`docs/development.md`, "Benchmark runs"). No
x86-64 boot approached the 180 s budget (the longest, the two-CPU boot,
148.1 s); the flakes record is unchanged.

## Plan §1

The item is marked complete, on these grounds:

- **Completion waits are modelled**, soundly for every signaller context
  the kernel has: thread signallers through held-at-signal edges, deferred
  for the self-signalling shapes; interrupt and callback signallers by the
  argument that they cannot be on a lock cycle through the wait. What the
  model does not record is enumerated: the "lock, unlock, complete" shape
  (no production instance), the queued-worker dependence (the network
  worker's items take no mutex), and a self-signal through a poll (the
  NVMe fallback, not reached under QEMU).
- **Unexecuted callback paths are a listing**, printed after every debug
  suite on both architectures, with one entry today, judged, and the test
  that would remove it recorded in the plan.

A new §1 item carries what remains: the delayed-acknowledgement test, the
polled completions, and the two recorded shapes.
