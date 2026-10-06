# History: subsystem units (section 68 reports)

One entry per unit chosen from the deferred-work inventory
(`docs/audit/2026-09-deferred-work-inventory.md`) and built through a
section 68 report (`docs/audit/next-subsystem-*.md`), plus the lockdep
milestone that followed (PRs #302-#308). Entries moved here verbatim from
the README's former Status section; a new unit adds its entry at the end.
See the [history index](README.md).

- **A wake that preempts** (`docs/audit/next-subsystem-wake-preempt.md`).
  Every wake in the kernel happens under an interrupt-disabling spinlock,
  and `spin_unlock_irqrestore` re-enables preemption *before* interrupts,
  so the documented preemption point at `preempt_enable` never fired for
  a same-CPU wake: a woken higher-priority thread ran at the next tick,
  up to 4 ms later, at all fifty-three wake sites. `preempt_point()` is
  the same four-condition predicate at the other moment it can become
  true, called from each architecture's `arch_irq_restore` after the
  enable -- invariant S8's fourth point, covering the wait-queue wakes,
  the thirteen direct `sched_wake` callers and the bare interrupts-off
  regions alike, with no change at any unlock. Three kernel tests read
  the waker's flag from the waiter's first statement (the waiter now
  runs 20-77 µs after the wake, before that statement), and a debug
  sysctl whose *read* is the system call under test shows a wake made
  inside a call preempting before the call returns -- read, because this
  kernel's sysctl is read-only, one of the as-built differences the
  report records. Proved by removing the point (all four fail on both
  arches), placing it before the enable (the same), and putting it in
  the unlock only (exactly the bare-region test fails). **Then the
  decision milestone 8 left**: the network worker ran at 40, below
  default, which is why `net-bench` delivered 512 of 10 000 UDP sends on
  every boot; five boots per setting per architecture under the report's
  rule vetoed 31 (the worker preempting its feeder collapses the one-flow
  TCP figure, and one x86-64 boot hung with the feeder starved -- a
  latent spin now on the inventory) and chose the default: every TCP
  figure within the old spread or above it, and 9 300 to 10 000 of
  10 000 delivered.

- **A hang that names its program counter**
  (`docs/audit/next-subsystem-lockup.md`). The wake-preempt unit's
  measurement found a hang it could not diagnose -- the network worker
  one priority above its feeder, one x86-64 boot in five stopped in
  `net-steer` with the worker `running` for eight seconds -- and the
  watchdog's dump carried everything about that CPU but where it was.
  The kernel had the answer every tick and discarded it. Now every tick
  stores the interrupted PC and its time (two stores), and on request
  every other online CPU records its own frame and stack into its own
  per-CPU buffer, in its own handler, with no lock and no printing: an
  NMI on x86-64 (a new LAPIC delivery mode, answered on the paranoid
  path with any registered handler still dispatched), the ordinary
  `IPI_SAMPLE` on AArch64, where a CPU with interrupts masked is
  reported as "no answer, last tick N ms ago" rather than guessed at.
  One reporter at a time, claimed with a compare-and-swap and never
  spun for; one total wait bound. The scheduler dump prints each CPU's
  tick sample and age and a live `run_ms`; the self-test watchdog prints
  every CPU's sample. Two detectors run from the tick -- a soft lockup on
  a CPU's own tick (no switch while something is runnable) and a hard
  lockup seen by the next online CPU in the mask (no tick), each once per
  episode -- and the harness forbids a real report and symbolises a
  report's addresses with the kernel ELF. Seven kernel tests (a spinner
  in a known function whose sampled PC must lie in it; through an
  interrupt mask with the outcome stated per architecture; the
  single-reporter rule; both detectors at a lowered threshold; the quiet
  control) and a host test of the watcher rule over masks with holes,
  every one bug-proofed by injection. Found by building: an AArch64 leaf
  function had no frame record at `-O1`, so a walk from inside one
  skipped its caller -- the AArch64 kernel now keeps leaf frame pointers;
  and a CPU with interrupts masked cannot acknowledge a TLB shootdown,
  whose waiter panics after a second. Then the spin: with the worker's priority overridable from the command line, the hang reproduced on the seventh boot at 31 and the dump named it in one block -- eight samples of the worker, every one in its wait condition or its dequeue, none in a packet: the receive queue's count said non-empty over a list that was empty. The cause was a second enqueue of an mbuf already on the queue, from the reorder test's loopback filter (a held copy in a plain global that two CPUs could both take), which cut the list behind it; at priority 32 the spinning worker had gone unnoticed -- one boot in five burning a CPU since that test landed. Fixed at the stack (a queued mbuf is refused a second enqueue, counted and said once; `net-mbufq-double`) and in the test (its state under a lock). Five boots at 31 on each architecture: no hang. The five-boot check at 31 then found a second, older hang -- the keepalive test black-holing the handshake's last segment before the worker had sent it, leaving its server in `accept` forever -- fixed by waiting for the passive side to be established. Ten boots at 31 after both fixes, five per architecture: no hang.

- **A read that fills its buffer, and an error that reaches close**
  (`docs/audit/next-subsystem-file-path.md`). The audit's two MEDIUM
  findings on the file path, still as found: `read` returned at most
  1 KiB per call through a stack bounce sized to a console line (a
  64 KiB read was sixty-four system calls, and the Linux personality
  inherited it), and `close` could not report a write-back error (the
  last-reference write-back dropped its result; the vnode's release
  dropped the pages themselves, uncounted). Now the bounce is sized to
  the request -- the stack for a kilobyte and less, the heap above it
  up to 64 KiB, degrading to the stack chunk when the heap refuses --
  one object call per read, so a pipe or tty keeps its semantics and a
  file fills its buffer; both personalities share it. A write-back
  failure is recorded where it is seen, by the page cache's own sync
  under its own lock; each file open at the time is told once, by
  `fsync` or by `close` (a `flush` hook on the I/O object type, called
  before the handle's reference goes; the handle closes regardless);
  a named file's pages dropped at its vnode's release are counted and
  said once, an unlinked file's neither written back nor counted --
  the rule `cosmofs-reserve`, which fills a disk and unlinks, decided.
  Seven kernel tests on cosmofs over the RAM block device with the
  block fault injection scoped to the test thread, each bug-proofed by
  injection, a user-mode read that fills 64 KiB in one call, and the
  audit's missing read/write bandwidth benchmark: the object path reads
  a ramfs file at 44 MiB/s with 1 KiB requests and 317 with 64 KiB on
  x86-64, the whole syscall from user mode at 38 and 110 (PR #138).

- **A guard that is proved, not assumed**
  (`docs/audit/next-subsystem-hardening.md`). The kernel's guard on its
  own access to user memory -- `stac`/`clac` with SMAP, PAN on AArch64
  -- was a no-op on both CI CPU models, so an unbracketed access passed
  every boot: booting the unchanged image on `cortex-a76` failed 5 of
  265 (four ASID tests reading user pages outside the bracket, and the
  `el2` test because the stage-2 walk started at a level the
  architecture forbids below 43 bits of physical address, the boot
  self-check disabled the backend, and the disabled backend kept EL2's
  vectors). Now `make test-guard` boots the same image on a
  protection-capable model per architecture and the harness requires
  the kernel's own `hardening:` line whole, the `uaccess-guard`
  self-test's "guard live" and, on x86-64, `usertest: umip: enforced`;
  the default boot stays the control and carries the `WARN` naming what
  is absent. The three faults are fixed: the bracket in the ASID tests;
  a pure, host-tested `hv_s2_layout` rule (level 0 above 42 bits, level
  1 from concatenated root pages at or below; the SMMU driver refuses
  what the IOMMU walker cannot build); and `arch_hv_disable`, which
  needed a call of the switch's own (`HV_EL2_CALL_HANDBACK`), since the
  stub's calls are gone once the switch owns EL2 -- proved by
  `hv-disabled` with an injected self-check failure. The native ABI's
  `mmap`, `mount`, `umount` and `open` refuse an unknown flag bit with
  `-EINVAL`. `SCTLR_EL1.WXN` is set on every AArch64 CPU from the
  kernel's own tables on, and `make test-wxn` executes a deliberate W+X
  page and requires that panic. 267 self-tests on both architectures on
  both CPU models, the `hv` suite included on `cortex-a76`; the guard
  instructions cost nothing measurable under TCG (PR #140).

- **A name that points somewhere else**
  (`docs/audit/next-subsystem-symlink.md`). The VFS had three vnode
  types and no way to offer a fourth: no `symlink` or `readlink` in
  `vnode_ops`, no call that would reach them, and no type for `stat` to
  report. The Linux personality was worse than absent -- `lstat` was
  aliased to `stat` and `newfstatat` read `AT_SYMLINK_NOFOLLOW` and
  dropped it, so a program that asks specifically not to follow a link
  was told about the target. Now a link is a node: the walk expands one
  the moment it meets it, with a budget of eight and the component count
  that already bounded it, carrying two path buffers from a single
  allocation that a walk without links never takes. A relative target
  resolves against the directory the link lives in; an absolute one
  restarts at the calling process's root, so a link cannot name its way
  out of a root the way a leading slash cannot -- proved by a child
  rooted at a jail writing through an absolute-target link and landing
  inside its own root. `open` is a loop, so `O_CREAT` creates the target
  of a dangling link and `O_NOFOLLOW` refuses a link named last while
  saying nothing about the ones in between. ramfs keeps the target in
  the node and cosmofs in the file's own block, written inside the
  transaction that publishes the entry and given back whole on any
  failure before it; format version 8 gates creation, because an older
  kernel would read a link as a regular file whose contents are a path.
  Three native calls (`SYS_COUNT` 89 → 92), six Linux entry points, and
  `ls` showing a link with its target. 273 self-tests on both architectures (PR #142).

- **The blocks nobody can reach**
  (`docs/audit/next-subsystem-fsck.md`). cosmofs could tell you every
  block was still what it wrote, and nothing could tell you the blocks
  added up. `cosmofs_check` walks the live tree, every snapshot, both
  allocation maps and the inode map under the mount's lock and compares
  what it reached with what the filesystem believes: ten finding
  classes, four repaired because each has one right answer, the rest
  reported because setting the bit of a block that is reachable and free
  may hand out a block in use, and choosing which of two inodes keeps a
  shared block is data loss. A snapshot shares blocks with the live tree
  on purpose, so the union map and the live-generation map are separate
  and only the second can report a cross-link. Pointed at the crash
  suite's replayed images, it found that **every crash strands space**,
  not only the unlinked-but-open file the report predicted: a block
  freed in a transaction keeps its bit until the commit after the one
  that made the new root durable, which is correct for crash safety and
  costs the previous generation's copy-on-write casualties. Measured
  across 199 replayed prefixes: 162 leaked, worst 18 blocks, 1912 in
  all, each reclaimed and clean afterwards. Repair is an argument from
  absence, so it runs only on a walk that is sure of itself: one
  unreadable directory block reports every file named inside it as an
  orphan whose blocks are leaked, and repairing that image would destroy
  them. 281 self-tests on both architectures; seventeen bug-proofs, two
  of which exposed tests that could not fail (PR #144).

- **The pass nobody can run**
  (`docs/audit/next-subsystem-fsctl.md`). cosmofs had two maintenance
  passes -- the scrub, which repairs a rotted mirror, and the structural
  check built the week before -- and every caller of either was a
  self-test. The check was compiled only into debug builds because
  nothing in a release kernel could have called it; the scrub had no
  such gate and shipped as dead code. Three things were missing, and
  none of them was the pass. A mount had no name: it carries no
  identifier, and a path is not one, because the same mount sits at
  different paths in different namespaces and a path names different
  mounts over time. Nothing pinned a mount: unmount infers busyness from
  the vnode hash, which a walk holding a reference does not appear in.
  And there was no channel. So: an id handed out in order and never
  reused; an unmount that **drains** a running pass rather than refusing
  or tearing down under it, terminating because the flag it sets first
  stops a new pass starting; and `/dev/fsctl` (0600), a versioned
  fixed-layout command whose result belongs to the open file that asked,
  with the listing scoped to the caller's mount namespace and the root
  filesystem emitted by name because it deliberately holds no namespace
  reference. Two optional entries on `struct fs_type` let the VFS learn
  that a filesystem has a pass without learning what one is. `fsctl(8)`
  lists, checks, scrubs and repairs; a check that *finds* something
  still exits zero, because a script cannot otherwise tell a broken
  filesystem from a question it could not ask. The first boot panicked
  on a lock-order inversion -- a device operation runs under the vnode
  lock, and the mount table is taken the other way round -- which is now
  a separate lock class and an inventory row. And the first real
  filesystem the tool was pointed at was not clean: **28 leaked blocks
  on the boot's own scratch disk**, because a clean unmount strands what
  its last transaction freed, exactly as a crash does, which nobody had
  measured because nothing could look. 286 self-tests on both
  architectures, debug and release; twelve bug-proofs, one of which
  passed and became an inventory row (PR #146).

- **The commit after the last one**
  (`docs/audit/next-subsystem-unmount-leak.md`). Two units in a row found
  this and neither fixed it. A cosmofs commit publishes its new root and
  *then* clears the freed blocks' bits in memory, marking those bitmap
  chunks for the next commit -- which is correct, because a block the old
  root still names cannot be freed before the new one lands, and which at
  an unmount means there is no next commit. So every unmount, on every
  filesystem, lost the space its last transaction freed: the fsck unit
  measured 1912 blocks across 199 replayed crash prefixes, and the fsctl
  unit found the sharper half when the first real filesystem an operator
  could point a tool at turned out to have 28 stranded blocks after a
  *clean* unmount -- and 41 more when that unit's test stranded some
  deliberately rather than depending on the residue it found. The obvious fix does not converge -- writing the
  bitmap frees the bitmap, so a second commit leaves a third's worth of
  work -- so format version 9 gives the superblock a `free_root` naming
  a chain of blocks that records what this root freed, written before
  the root and made true by it, and replayed by the next mount before
  the bitmap is trusted for allocation. The ordering is the design and
  both halves are load-bearing: everything that allocates must happen
  before the bitmap fixpoint, or the root publishes a bitmap that does
  not know about the record's own blocks and the allocator eats them;
  and the set to record is not final until the fixpoint has run, because
  it frees every chunk it copies. Reserve before, fill after. Two of the
  plan's claims were wrong and the build says so in the report: moving
  the snapshot filter ahead of the fixpoint corrupts a snapshot's member
  table, so the filter stayed and the *question* was extracted instead;
  and "a superseded record is freed outside the filter" was not true of
  the code, because a deferred free goes on the list phase 7 filters --
  found by writing the test that argument never had. Four tests passed
  their own bug-proof and were rebuilt before they measured anything,
  one of them three times. Review then found three failure paths where
  the happy path was right and the unhappy one was not: a commit that
  failed after reserving the record's blocks kept them, an over-reserved
  block had nowhere to be recorded when the last chain block came out
  exactly full, and a record whose count was past what a block holds was
  read as empty. The third corrected the report as written -- refusing a
  root is not refusing the filesystem, because cosmofs keeps two and
  falls back a generation to a whole one. The crash suite's
  stranded-block total is now zero and its weakened assertion is gone.
  297 self-tests on both architectures, debug and release (PR #148).

- **The list the root does not name**
  (`docs/audit/next-subsystem-snap-deadlist.md`). The record of what a
  transaction freed fixed the frees and not the holds. A block a
  snapshot still occupies was appended to that snapshot's deadlist from
  the commit's release loop -- *after* the root was published -- by a
  call that allocated a block the published bitmap did not know about
  and dirtied two blocks for a commit that an unmount never makes. The
  snapshot list was also the one metadata chain in the filesystem
  rewritten **where it lay**, so a crash between that write and the next
  root left the surviving root naming a list belonging to a transaction
  that never happened. The inventory row called for the free record's
  reserve-before-fill treatment; that alone would have traded a leak for
  a corruption, because an append moved in front of the root under an
  in-place update is an append the *old* root can see. So the snapshot
  list and its deadlists are now copy-on-write, published by the same
  superblock write as everything else -- `snap_root` names the list and
  nothing else does -- with the blocks taken from the commit's existing
  reservation, the superseded ones freed exempt on the rule the free
  record established, and the verdict "does a snapshot hold this" taken
  once per freed block instead of once for the record and again for the
  release loop, which now clears bitmap bits and nothing else. **The
  report's own analysis was wrong in one place and the crash suite said
  so on its first run**: the release loop's append *was* a crash hazard,
  not through its entries but through the pointer it wrote into the
  in-place list -- a deadlist head neither written nor allocated under
  the surviving root, `unreadable` and reachable-and-free at once, at
  prefix 125, block 23. A control with the change disabled failed
  identically, which is what said the defect pre-dated the unit. Nothing
  had ever tested it, because **the crash suite had never taken a
  snapshot**; it does now, and asks per prefix a question the structural
  checker cannot -- a deadlist's entries are claimed non-live, so one
  block on two lists is not a duplicate claim and not a finding. 334
  prefix images, 0 blocks stranded, 1312 deadlist entries examined, none
  duplicated. The previous unit's weakened bound, `alloc_not_seen <= 4`,
  is `== 0`. No format change: the on-disk shapes are unchanged and only
  where their blocks live differs, so `CFS_VERSION` stays 9. One of the
  new tests passed its own bug-proof and was rebuilt: comparing
  `snap_root` across two commits compares equal on a filesystem that
  copies perfectly, because the allocator hands the superseded block
  straight back, so the measurement is taken across one commit instead.
  (PR #150).

- **The name is gone and the handle is not**
  (`docs/audit/next-subsystem-orphan.md`). The last clause of a row two
  units in a row named and neither took. `cfs_unlink_common` removes a
  name and zeroes the link count; the blocks are released later by
  `cfs_evict`, when the VFS drops the last reference -- and a commit can
  land between the two, leaving a durable filesystem with an inode whose
  extents are intact and which no directory entry reaches. A crash or a
  forced unmount after that lost the inode and its blocks for good,
  because no mount reconsidered them: `cosmofs_check` found them and an
  operator with the repair flag got them back, which is the workaround
  with a person in it that the free-record unit had already refused. The
  tree carried a test whose comment called this "the leak the design
  admits by omission" and whose assertions described it. Format version
  10 gives the superblock an `orphan_root` naming a chain of inodes the
  root still owes, written before the root and replayed at the next
  mount by doing what `cfs_evict` would have done -- which means queuing
  the blocks and clearing the slot, so the space comes back when that
  mount's first commit publishes it, as an ordinary eviction's does. **It is a record, not a
  list**, and that is the design: the set is derived, so each commit
  writes it whole and nothing is edited on disk -- unlike the snapshot
  list, nothing here is copy-on-write -- and an ordinary unlink's add
  and eviction cancel in memory, so a filesystem holding nothing open
  across a commit writes no record at all. Review corrected the plan
  twice: **directories reach this state too**, because a working
  directory is a referenced vnode and an empty one can be removed under
  it, and the replay must not touch the parent's link count, which
  `rmdir` already decremented; and the cancel is not a guarantee,
  because the filesystem lock is dropped before the VFS drops the last
  reference. The build corrected it twice more: `cfs_inode_read` calls
  an inode with no links absent, so the first replay reclaimed nothing
  until it used the raw read the structural check already had; and the
  reclaim lands on the mount's first commit rather than on the mount,
  because the root still names those blocks -- and a replay that fails
  after queuing them must fail the mount *and* clear the queue, or the
  older-root fallback commits frees that root's inodes still name. Two test oracles measured
  the wrong thing before they measured anything -- one compared against
  a filesystem that had no record either, and the next asserted sixteen
  blocks for a 64 KiB file that compresses to five. The crash suite now
  holds a handle across a sync: 410 prefix images, 0 stranded, and with
  the replay disabled it fails at prefix 213 reporting one orphan, which
  is what says the workload is not vacuous. 312 self-tests on both
  architectures, debug and release (PR #152).

- **Four windows nothing has ever raced**
  (`docs/audit/next-subsystem-lifetime-windows.md`). The first unit in
  six that is not cosmofs. The lifetime and quiescence report states its
  ordering argument as a table of claims and then says how each was
  checked -- a host model under sanitizers, plus review -- and lists as a
  risk that ordering is "verified by review and sanitizers, not by a
  model checker". That is good evidence for the algorithm and none at
  all for the four places where the algorithm meets a driver, a socket
  or a device, because nothing had ever run the other side:
  `straggler_ipis` was incremented in one place and read in none;
  `blk_unregister` was called by two tests and in both the device was
  quiescent; `timer_cancel_sync` was tested on a probe, which is
  evidence for the primitive and not for four uses of it; `vpci_remove`
  ran only on module unload. Seven tests race them now, each holding its
  window open with a hook rather than a stopwatch. **Three of the four
  windows are closed and the fourth is narrowed**: removal is asserted
  to be the whole unbind -- the driver's hook *and* the model's
  bookkeeping, since the hook alone leaves a bound device holding a
  dangling `drvdata` -- but on a device of the test's own, because the
  machine's live virtio-blk is the scratch disk the filesystem tests run
  on and a boot-time suite that removes it destroys the run. Driving
  `vpci_remove` itself with real I/O outstanding needs a virtio device
  dedicated to removal in the test machine, and stays an inventory row.
  The numbers, from one run: 6 kicks from that waiter over a 32 ms
  wait with the spinner unhelped, 5749 units of work on a third CPU while
  one stalled the waiter, 15 accepted and 349784 refused across the
  unregister window, an unregister that spun 6118 times for a submitter
  parked inside the driver, a cancel that spun 96 times for a callback
  holding a pcb it had not yet taken a reference on. **The review was
  worth more than the run.** It caught three oracles that measured the
  wrong thing: a kick credited with a completion it could not have
  caused, a drain window no submitter ever occupied, and a hold placed
  after the callback takes its reference -- which would have measured
  reference counting and passed with `timer_cancel_sync` stubbed out --
  plus a removal hook that would have left a bound device with a
  dangling `drvdata`. **And the unit's own findings came before it
  ran**: a CPU publishes at interrupt return only when `preempt_count`
  is zero, so the straggler kick cannot help the spinner its comment
  named, and what it is worth for the population it *can* help is
  unproven because arranging that is a phase coincidence. The comment is
  fixed and the question is filed. No use-after-free, no bio reaching a
  detached driver, no hung unregister -- which the report said in
  advance it would report as such and keep the tests. Also corrected:
  the inventory row claiming nothing here could attempt an unprivileged
  open was stale, and what was left of it was two doors added after that
  suite. 319 self-tests on both architectures, debug and release
  (PR #154).

- **Two timestamps and no rule about subtracting them**
  (`docs/audit/next-subsystem-cpu-clock.md`). `clock_now_ns()` is a
  per-CPU clock on x86-64 that the whole tree treated as a machine-wide
  one, and `has_invariant_tsc` -- the one fact that says whether the
  counter is even usable that way -- had been detected at boot since this
  kernel had an x86 port and read by nothing. **The sweep was the unit.**
  Planned as step 6, a re-check of step 1, it was run first as a grep
  rather than a re-reading and changed the plan twice. The report called
  `sched_dump` "the one place with a hand-rolled guard"; there were
  three, written independently, each for the same reason and none
  referring to the others. And rewriting the block-timeout scan as
  `clock_since_ns(stamp)` re-reads the clock per bio *inside a spinlock*,
  against a `now` that moves underneath the comparison -- a regression
  step 1 had already written before the grep caught it -- so
  `clock_delta_ns(now, stamp)` is the primitive and `clock_since_ns` its
  fresh-read form. The classification rule is not the one the report
  implied either: a thread that sleeps between two clock reads wakes on a
  different CPU, so a plain `t0` in a local variable is a foreign stamp,
  and that is nearly every timing assertion in the suite (**that premise
  was itself wrong, and the thread-migration unit below says so: this
  kernel pins a thread to one CPU for life, so those stamps were
  same-CPU when they were swept; **and true again since the
  percpu-migration unit below, which is when threads began to move**) -- 42 such
  sites, 20 shared-state ones, 4 in userland the report had not noticed,
  1 deliberately left plain (the tick cost, where saturating would hide a
  counter going backwards on one CPU) and 4 that are not elapsed times at
  all; 66 changed in all. **Then the gate fired on the first machine it met.** QEMU's
  x86-64 TCG does not advertise an invariant TSC and refuses to be asked
  to (`-cpu ...,+invtsc`: "TCG doesn't support requested feature"), so
  the only x86-64 machine this project runs on is one where the kernel
  must decline to promise -- while its counters demonstrably agree, 0 ns
  outside the bracket over 2400 handshakes. It declines anyway, because
  the promise is about the hardware's contract and not about what happens
  to work today. The report's fallback for that case does not exist: PIT
  channel 2 is a one-shot calibration gate, not a free-running counter,
  and there is no HPET driver. So the kernel keeps the TSC, still
  monotonic per CPU, and gives up the cross-CPU claim instead. **The
  measurement runs anyway** -- gating it on the bit would have made it
  dead code on every machine here -- and reports what it found without
  acting on it. Its first run reported an uncertainty of ±0 ns, which is
  a promise no measurement can make: the narrowest bracket had width
  zero, the counter not having advanced across a handshake that certainly
  took real time. The bound is floored at one counter tick now, and a
  test keeps three cases apart permanently: 0 only when nothing was
  measured, unbounded only when the kernel has declined, otherwise at
  least one tick. **And the unit found a live dependency on the property
  it was defining**: `blk-unregister-drain` asserted an order between two
  events on two different CPUs by comparing their timestamps, which is
  exactly what the kernel had just stopped promising -- it uses an atomic
  sequence number now and depends on no clock at all. Ten new tests;
  the two cross-CPU oracles pass trivially on every machine here, so the
  injection that would fail them runs in CI rather than in a terminal.
  Review then spent five rounds on one finding the report had scoped out
  -- that a *deadline* is a timestamp too, so one built on one CPU and
  tested on another is unsound where the offset is unbounded -- and was
  right to keep asking. It is fixed: a machine-wide tick advanced by a
  designated CPU, with ownership taken over by another when its owner
  stops ticking, so the counter cannot stop while any CPU still ticks.
  Two earlier shapes were built and reverted first, and the test that
  ships with the third creates exactly the failure that killed the
  first. What does not run anywhere available: the applied correction,
  since no machine here both has a per-CPU counter and advertises it as
  invariant.
  329 self-tests on both architectures, debug and release (PR #156).

- **A thread that could never move, on a CPU chosen once**
  (`docs/audit/next-subsystem-thread-migration.md`). Half of this unit
  shipped and the half it is named for did not, which is the result
  rather than an excuse. `pick_cpu` compared with a strict `<`, so ties
  went to the lowest-numbered CPU — and because `nr_running` counts only
  what is runnable *now*, and a kernel thread is blocked almost all of
  its life, the queues had usually drained to zero between creations and
  every CPU tied. **So every thread created on an idle machine went to
  CPU 0, and nothing ever moved it.** Measured: 8 of 14 threads and 94%
  of context switches there. Ties rotate now, which fixes the placement
  half; the test had to be built around a surprise, since threads created
  back-to-back already spread (each raises its target's count) and only
  threads that *block* pile up. **The balancer was built, worked, and was
  removed.** It moved threads correctly — CPU 0 to 6 of 14, CPU 2 doing
  ten times the switches — and made three of four aarch64 boots fail,
  once with seven concurrency tests at once, against four of four passing
  without it. Seven together is corruption, not timing, and it was not
  found; what was ruled out is written down so the next attempt starts
  past it. Removed rather than left behind a flag, because a balancer
  switched on only by its own test closes nothing and reintroduces the
  instability wherever it is switched on. **The failure was worth more
  than the feature.** lockdep cannot check a two-run-queue lock order,
  because both are one class and the nesting annotation only says
  "deliberate" — an invariant claimed otherwise and is corrected.
  `rq->current` *can* be in a ready list, which the report and two
  comments called structurally impossible, caught by an assertion kept
  only because the property lived in another file. And the tree holds
  per-CPU assumptions nothing declares: `el2` asserts the hypervisor
  backend owns EL2 "on this CPU" from an unpinned thread, and was correct
  only while threads could not move. 330 self-tests on both
  architectures, debug and release (PR #158).

- **A writeback thread inside a mount that was still replaying.** Not a
  unit: the deferred-work inventory's one open defect, chased on
  request. `cosmofs-orphan-reserved` failed on three of three CI runs
  and none of three local ones, on a documentation-only branch, always
  as one metadata block that would not verify, at a different address
  each time. What opened it was the other architecture: the same test
  panicked x86\_64 with a kernel write at zero in `list_remove` <-
  `cfs_buf_get` <- `freelog_release_previous` <- `cosmofs_sync`, running
  on `cfs-wb`. cosmofs starts its writeback thread lazily, on the first
  dirty buffer, so that a read-only mount has none to join -- and a
  mount's own replay dirties buffers, so the thread was being started
  from inside the replay. It then takes only `mount.sync_lock`, which
  the mount path does not hold, finds `mnt->unmounted` false, and
  commits a half-built filesystem across an `fs->bufs` that the mount
  walks with `fs->lock` unheld, because until `cosmofs_mount` returns
  nothing else is supposed to be in the filesystem. Two threads on one
  intrusive list: an unverifiable metadata block on one architecture, a
  null dereference on the other. The same window let a *failed* mount
  reach `cfs_destroy`, which frees every buffer and the filesystem, with
  that thread still running. The rule is now that no autonomous
  committer exists before the mount is live: the spawn is gated on
  `mount_done` and `cosmofs_mount` starts the thread at the end if the
  replay left anything dirty, and `cfs_destroy` stops and joins for
  itself rather than trusting its caller. Whether that thread wins the
  race is timing and whether it exists during the replay is not, so
  `cosmofs-mount-no-early-writeback` claims the second: it counts the
  replay's dirty marks, which must not be zero or the test is asking
  nothing, and how many of those found a thread already running, which
  must be zero -- that count is the rule. With the gate reverted it
  fails on exactly that count, while `cosmofs-orphan-reserved` still
  passes here -- which is what the three local runs had been saying all
  along. 331 self-tests on both
  architectures, debug and release (PR #160).

- **An invariant written down thirteen times, checked once, and false in
  half the tree** (`docs/audit/next-subsystem-vcpu-regs-size.md`).
  `struct cosmo_vcpu_regs` is the VMState, and its size was stated in
  thirteen lines across eight files, all of which said 448 bytes. It is
  448 on x86-64 and **496 on AArch64**, and has been since the EL2
  backend landed. The one check lived in a host test that compiles
  whichever block the *build host* matches, so on the x86-64 CI runner
  it passed and could not fail: the invariant's own check never
  compiled the block that violated it. The first plan was to resize both
  blocks to a shared 512. Review asked what protects a caller built from
  the older header, and the answer was nothing -- `SYS_vcpu_regs` takes
  no size and no version and copies `sizeof` both ways -- which led three
  lines above the struct, to the header's own preamble: *"This is user
  ABI: numbers and structures here are stable."* x86-64's 448 is correct
  and stable; the AArch64 ABI is 496. **The documentation was what was
  wrong**, so the unit resizes nothing: it asserts each real size with
  `_Static_assert` in the UAPI header, where the check runs in every
  translation unit on every architecture instead of in one host binary,
  and corrects the twelve lines that were wrong. The cross-architecture
  equality rule is gone and bought nothing -- every caller and copy uses
  `sizeof`. Deleting the stale host assertion had a second effect worth
  more than the first: `test_hv` is ninth of twenty-three in
  `HOST_TESTS` and the target stopped there, so **fourteen host suites
  had never run on an arm64 machine**; they run now, and all pass.
  `hv-vcpu-regs-roundtrip` is new, because nothing had ever checked that
  a register file survives a set and a get: it asserts every field back,
  and asserts the four the backends normalise against their documented
  rules, so removing the `rflags` masking fails the test that documents
  it. 332 self-tests on both architectures, debug and release (PR #162).

- **A filesystem lock held across a device that sleeps**
  (`docs/audit/next-subsystem-chrdev-vnode-lock.md`). `file_pread` and
  `file_pwrite` took the vnode's mutex and dispatched to the character
  device inside it. The inventory row called that a lock-order problem
  and it was one; the larger half is that one of those devices sleeps.
  `tty_read` waits for a line with no timeout, and `ramfs_lookup` hands
  out **one vnode per device node**, so a process blocked at a terminal
  held the lock every other opener of that terminal needs — not slower,
  stopped, until somebody typed. Nothing had ever failed, and the reason
  is worth stating: kernel messages never touch the VFS, the shell
  harness has one reader whose background jobs print nothing while it
  waits, and no other character device sleeps. The one that does is the
  one nothing writes to concurrently, which stops being true the moment
  there is a second session. The `VNODE_CHR` arm now runs with **no
  filesystem lock** — it guards nothing there, and a device's other
  entry points already ran outside it (`ops->open` from `file_run_open`,
  `ops->release` from `file_release`), so this is read and write being
  brought into line with open and release rather than a new rule. It is
  **checked rather than stated**: `lockdep_assert_not_held` sits at both
  dispatches and fires for every character device in every debug build,
  including ones not written yet — a macro that was defined in the tree
  and called from nowhere, which is most of why the row survived. The
  `vnode-chr` lockdep class split, added after `/dev/fsctl` panicked on
  its first boot, is **removed rather than layered over**: it was sound
  and answered the wrong question, since what made a mount-table lookup
  an ordering at all was the lock being held across the device. Three
  proofs: the new `vfs-chr-write-during-blocked-read` fails at
  `writer_returned` in 511 ms before and passes in 17 ms after;
  reverting the fix with the split gone reproduces the original
  `mounts -> vnode` panic, which is what says the two were coupled; and
  with both in place lockdep reports only the six its own suite asks
  for. The test releases its blocked reader on every path before
  asserting, because a test that proves a deadlock by deadlocking is a
  hung boot. New invariant V32. 333 self-tests on both architectures,
  debug and release (PR #164).

- **A count published after the thing it counts.** Not a unit: an
  inventory row this session's VMState work turned up, fixed on request.
  `cfs_writeback_thread` incremented `fs->wb_commits` after
  `cosmofs_sync` returned — after that function had dropped `fs->lock` —
  while `cosmofs_stats` reads the counter and `sb.generation` together
  under it. For the few instructions in between, a reader saw a
  filesystem that never existed: the new generation with the old count,
  which is exactly the pair `cosmofs-writeback` asserts. It failed once
  on aarch64 CI and looked like the two genuine flakes beside it; the
  log is what separated them, because it failed after **80 ms** rather
  than at its 2-second deadline with `committed generation 2` printed
  above it, so the commit had happened and host load explains nothing.
  The count is now taken inside the same hold of `fs->lock` that
  publishes the generation, and the field has exactly two accesses in
  the tree with both under that lock. The race is not deterministic and
  the proof is: `lockdep_assert_held` at the increment means publishing
  it outside the lock panics on the first writeback commit of the boot
  rather than on the one run in a hundred that catches it. 333
  self-tests on both architectures, debug and release (PR #165).

- **A harness that could not say why its own exchange failed**
  (`docs/audit/next-subsystem-nettest-deadline.md`). `net-harness`
  failed seven times in a fortnight, on both architectures, on CI and
  locally, including on branches that add a single Markdown file — and
  two documents had concluded it was host-dependent and unbounded. It is
  neither, but **this unit does not claim to have found the cause, and
  an earlier draft of its report did**. The obvious candidate was a real
  defect: the harness armed a 120-second `accept()` deadline in
  `NetTest.__init__`, which runs *before QEMU is launched*, and closed
  the listener when it expired — so a guest connecting later had its
  connection completed by QEMU's user networking and answered by
  nothing, which is exactly the `ksock_connect` returning 0 with no echo
  that every sighting recorded. Measured, the back-connection lands at
  **72 %** of the boot, which projects onto CI's 140–146 second boots at
  101–105 seconds against a 120-second deadline: a margin of fifteen to
  nineteen seconds. Thin, and not shown to be crossed — across sixteen
  aarch64 jobs a 145.6-second boot passed and a 145.8-second one failed,
  so boot length does not predict the outcome. What the unit *did* find
  is why nobody could tell: **the harness recorded none of those
  numbers.** Every sighting produced `TimeoutError('timed out')` and
  nothing about when the guest connected, how much budget remained, or
  whether the port was open. So the deadline is fixed — bound early,
  accepted after the guest reports ready, one budget derived from the
  run's `--timeout` — and every run now prints its timings whether it
  passes or fails. `tests/boot/test_nettest_deadline.py` (in `make
  host-test`) holds four properties in under a second, including that an
  expired deadline leaves nothing listening, which is why expiry was
  fatal rather than late; restoring the deadline to the constructor
  fails it by name with `got 120.0`. **The inventory row stays open**:
  it is struck when a sighting with the new timings shows the deadline
  was the cause and the fix ended it, and not before (PR #167).

- **Twelve bytes that never arrive** (`docs/audit/next-subsystem-twelve-bytes.md`).
  The guest half of the instrumentation the previous unit added to the
  host. `net-harness` prints `client failed (%d)` with the **connect's**
  result, so every one of its failures has reported that the step which
  worked, worked — the same defect PR #167 removed from the other side
  of the same wire. It now says what `ksock_sendto` and `ksock_recvfrom`
  returned, and asks the connection rather than the global counters:
  `tcp_send_space` before the send, after it and after the read, because
  data sits in the send buffer until it is **acknowledged**, and that is
  per-connection where `tcp_get_stats` counts this connect's own SYN and
  every other socket's traffic. Four outcomes, and they are exclusive:
  never queued; queued and never acknowledged with retransmissions
  climbing; queued and never acknowledged with retransmissions **flat**,
  which would be a defect here whatever else is true; or the send buffer
  **drains** — the data was acknowledged and the host still saw nothing,
  which is what QEMU's user-mode networking being a *proxy rather than a
  wire* makes possible, since it acknowledges into its own buffer before
  writing onward. **This unit is step 1 and stops there.** Twenty-one
  local boots produced one failure — the rate is one in twenty-one, not
  the one in three an earlier draft claimed from a single observation,
  and six boots under CPU load did not raise it. So the instrument ships
  and the next failure reports itself, as #167's did within the hour —
  **and it did, on two of this unit's own CI jobs, one per
  architecture**: `sent -104`, which is `ECONNRESET`, with the send
  buffer untouched across all three samples, `segs_out +0` and the pcb
  `TCP_CLOSED`. So `ksock_sendto` failed and the twelve bytes were never
  written: the connection had already been reset, while the host had
  accepted it a second earlier. Every framing of this defect so far,
  this unit's own title included, describes a symptom of something that
  had already happened, and the question is now what resets an
  established connection between `ksock_connect` returning and the next
  statement. The aarch64 job narrows it one step further with
  `rsts_in +1` — an inbound RST accepted in sequence, so the reset came
  off the wire rather than from this stack — while the x86-64 job's
  `+0` for the same failure is the instrument's window starting after
  the connect, not a run without a reset. Who sent it is still not
  named, because `tcp_get_stats` is machine-wide and the pcb's own
  pending error is never read; that is the next unit's first step. The
  inventory row stays open, narrowed. 333
  self-tests on both architectures, debug and release (PR #169).

- **A socket can be asked what went wrong.** The stack has always known:
  `pcb->error` is set at six sites with four errnos — reset, refused,
  timed out, refused by the firewall — and `struct socket` carries a
  second field, commented *"pending asynchronous error"*, that **nothing
  ever wrote**. `sock_set_error` had zero callers while five sites tested
  the field, and behind that dead setter sat a real bug: `ksock_accept`
  read `return take_error(s) ? take_error(s) : -EINVAL`, and `take_error`
  clears as it reads, so the second call answered 0 — `accept` returning
  **success** with its out-parameter unassigned, live the moment anyone
  called the setter. Meanwhile `icmp_input` consumed only
  fragmentation-needed and echo, so this host **sent** ICMP
  port-unreachables and had never **received** one, and a connected UDP
  socket talking to a closed port waited forever. And no caller could ask
  for a verdict at all: `SYS_ioready` says *that* a socket is broken and
  never which way, the native ABI had no socket-option call, and the
  Linux door refused every `getsockopt` while `setsockopt` returned **0
  for every `SOL_SOCKET` option and did nothing** — so a program setting
  `SO_RCVTIMEO` was told it worked and then blocked forever. This unit
  makes the verdict a thing you can ask for: `ksock_error` reads it once
  (clearing by compare-exchange, so two readers cannot both be told the
  same error) and takes **no lock**, because three of its five callers hold `s->lock`
  and two do not and the writer runs in packet-receive context where a
  mutex cannot be taken — which is a rule the field never had and the
  reason the first draft of the design would have recursed on a
  non-recursive mutex. `udp_error_notify` gives the field its first
  writer, from an `icmp_input` branch that reuses the quoted-header parse
  `icmp_needfrag` already had and delivers only to a **connected** socket
  whose whole four-tuple the message quotes (RFC 5927: the bar N16 sets
  for a reset). `SYS_getsockopt` (92) carries `SO_ERROR` — one option,
  positive errno, cleared by the read — with the Linux door forwarding to
  the same kernel path rather than growing its own, and `setsockopt`
  refusing what it does not implement. Invariant N21. Five bug-proofs,
  each shown to fail for its stated reason: the double call makes
  `accept` return the wrong thing, cutting the delivery makes the UDP
  socket wait, dropping connected-only lets an unconnected socket take
  another flow's error, giving the accessor the mutex stops the kernel,
  and a delivery that commits by errno alone destroys a second verdict of
  the same value that nobody had been told — which is why the pending
  error is one 64-bit word carrying a generation as well as an errno, and
  why a syscall peeks, copies, and only then commits the clear rather
  than taking the verdict and putting it back. `net-harness` now prints the pending error and samples its
  counters *before* the connect, which is what PR #169's window was too
  late for. 337 self-tests on both architectures, debug and release
  (PR #171).

- **A hardware error the CPU corrected no longer kills the machine.** An
  SError on AArch64 and a machine check on x86-64 are the same class — a
  fault the CPU could not attribute synchronously — and both ended in
  `panic`. Every SError reached `aarch64_trap_entry`'s `default` arm,
  which **relabelled the frame `ARCH_TRAP_GENERAL_PROTECTION`**, so a
  machine stopped by an asynchronous abort reported a general protection
  fault; and vector 18 had no handler at all, so a machine check panicked
  through `arch_trap_unhandled`. Both now classify: `arch_async_error_class`
  answers *corrected* only for a syndrome that positively says so — on
  AArch64 `ID_AA64PFR0_EL1.RAS` non-zero with `ESR_EL1.IDS` clear and
  `AET = CE`; on x86-64 **at least one valid bank**, every valid bank
  `UC == 0`, no `PCC` or `OVER`, and `RIPV`, across all `MCG_CAP.Count`
  banks. That first x86 clause is not redundant: without it "every valid
  bank is clean" is true of *no banks*, and a machine check carrying no
  record would read as corrected. Everything else panics, naming the
  class and printing the syndrome. **No process is killed** — an
  asynchronous abort's frame names the context interrupted when the error
  was *delivered*, not the one that caused it, so nothing here may choose
  a victim; attribution needs the RAS error records and is a unit of its
  own. Invariant **I-ARCH-16**. Five bug-proofs, each shown to fail for
  its stated reason — the vacuous bank rule, the missing-FEAT_RAS rule,
  the SError vector back in the panic arm (`KERNEL PANIC: exception in an
  unsupported vector slot 7 (EC 0x2f)`, dead in 8.8 s), the unregistered
  machine-check vector, and a bank scan that stopped at 32 and pronounced
  on the prefix — which is the vacuous rule again in a different
  disguise, a check ranging over less than it claims. Two things the building found and
  the report had not: `HCR_EL2.VSE` is inert without `AMO`, since the
  host runs `HCR_EL2 = RW` and nothing else; and **EL1 runs with
  `PSTATE.A` masked for the kernel's entire life** (`daifset #0xF` at
  boot; the only unmask anywhere is `daifclr, #2`, which is IRQ), so the
  kernel never takes an asynchronous abort while running — EL0, entered
  with DAIF clear, is the live path. Whether EL1 should unmask `A` is
  recorded as I-ARCH-16's gap rather than settled here. A real corrected
  SError, injected through `HCR_EL2.VSE`, is taken at EL1 and execution
  continues on the guard boot (`cortex-a76`, which has FEAT_RAS;
  `cortex-a72` does not, and the test says so rather than passing
  quietly). 339 self-tests on both architectures, debug and release
  (PR #173).

- **A grace period ends when the last CPU publishes, not at the next
  sleep boundary.** `synchronize_quiesce` polled — `thread_sleep_ns(TICK_NS
  / 2)` in a loop — so nothing told the waiter that the last CPU had
  published one microsecond after it went to sleep, and a 2 ms request is
  serviced at the next 4 ms tick. It blocks on a queue with a deadline
  now, woken by a CPU passing a quiescent point: **about 3.9 ms a grace
  period against 4.3–7.5 ms**, on four idle CPUs, with the spread
  collapsing as well as the mean. Every synchronous caller gains it —
  `interrupt_unregister`, module unload, `netif_unregister`, the
  receive-hook removal, and the `call_quiesce` batch worker, which is why
  the deferred form never escaped the floor either.
  **The report's premise was half wrong and the unit says so**: a grace
  period is *not* over in microseconds. It is over when the other CPUs
  reach a quiescent point, which while they are halted in
  `arch_cpu_wait_for_interrupt` means their next tick — and that ~3.75 ms
  is untouched. What went is the polling overshoot on top of it.
  Two more of the design's claims died in the building. The wake cannot
  live in `quiesce_note_quiescent`: that runs inside the scheduler, where
  the AP bring-up path publishes holding a run-queue lock with interrupts
  off, and waking from there reaches `schedule_internal`'s assertion —
  the machine dies five seconds into boot. And the two trap returns are
  not enough, because an idle CPU is halted, so the publish that finishes
  a grace period comes from `idle_main`. Three wake sites, all of them
  contexts that hold nothing and call `schedule()` a line or two later.
  The deadline stays, and is the correctness argument rather than a
  hedge: `pending == 0` is still the whole condition, so a missed or
  spurious wake costs one re-check and a defect in the wake path can only
  make the wait longer. Invariant **Q18**, and a new `wait_event_timeout`
  — the tree's first timed wait — with its own three-arm test. The
  assertion is that the wake **fires** — wakes delivered to a queued
  waiter, identically zero unless the path runs — and not that a grace
  period was fast; three earlier versions asserted timing in disguise and
  none of them was sound. Found next door and fixed: a thread
  killed while sleeping cancelled its stack timer with `timer_cancel`,
  which only promises the callback will not *start*. 341 self-tests on
  both architectures, debug and release (PR #175).

- **The harness can say which connection it accepted.** `net-harness`
  failed repeatedly over three weeks, on both architectures, on CI
  and locally, several times on branches that change no code — and said
  only `TimeoutError`. The cause was on the *host* side, where three
  earlier units had not looked: `nettest.py` listened with a backlog of
  one and accepted exactly once, blindly, treating whatever it dequeued
  first as the guest's. It never checked. **The failure was reproduced
  deterministically**: occupying that single backlog slot before QEMU
  starts reproduced the signature on the first boot, and the packet
  capture shows slirp acknowledging the guest's twelve bytes into its
  own buffer, never delivering them, and resetting the guest ten seconds
  later — the guest correct from first SYN to final reset, which is the
  outcome `nettest.c`'s own comment predicted before it was ever
  observed. The rule now: **the guest's connection is the one that
  delivers `cosmo hello\n`, and every other connection is evidence that
  gets reported.** A backlog of eight, a select loop over every
  connection that arrives, a per-connection receive budget measured from
  its own accept, and a failure line carrying a roster — each peer, when
  it was accepted, bytes read, a thirty-two-byte preview — in place of
  "connection accepted at 90.9s", which was a time without an identity.
  A connection that arrives and never delivers the request is still a
  failure, so the deeper backlog cannot turn a real guest fault into a
  pass.
  Two side facts closed by measurement: a backlog of one makes a second
  connect **hang silently** on this host — the SYN dropped, no refusal —
  so a stale connection both won the accept and stalled the real one;
  and `free_port` is clean, zero collisions in three thousand triples.
  A baseline the thread never had: a healthy back-connection is
  SYN-ACKed in **150 µs** and completes in **52 ms**, which is what made
  sighting twenty-two's `connect 0 in 1031 ms` readable at all.
  **And it answered on its own CI, against the hypothesis that built
  it.** Sighting twenty-three landed on this pull request's aarch64 job
  and the roster said `1 connection(s): … 0 byte(s)` — **exactly one
  connection, carrying nothing** — and sightings twenty-four and
  twenty-five said it again, three times running. So the wild trigger is
  *not* a foreign connection: the stale-slot reproduction reproduces the
  symptom without being the cause. The guest's connect succeeds but
  takes 787 ms to 1116 ms against a 150 µs baseline, and a
  SYN-retransmission constant read off the first three sightings was
  **withdrawn** when the fourth answered the first SYN with
  `retransmits +0`. The locus is **slirp's own host-side connect**, and
  the guest's side is fully accounted for. Still unnamed: why that
  connect stalls and then fails. Host-side only: no kernel change, no new API, no
  self-test registry entry. Eight host tests that run in about six
  seconds without booting anything, and the bug-proof is that the
  stale-slot case fails against the old harness with exactly the wild
  symptom — `back_error: timeout('timed out')`, zero of twelve bytes,
  9.9 s (PR #177).

- **The straggler kick is worth something, and now there is a number.**
  After 8 ms of waiting, `synchronize_quiesce` sends an IPI to every CPU
  still pending, up to eight times. Nothing counted a kick that
  *worked*: `straggler_ipis` counts kicks sent, so **no number in the
  tree would have changed if the kick were replaced by a no-op**, and
  the code said as much — "an open question rather than a measured
  fact". A kick that worked is now defined as narrowly as it can be, a
  publish that happened in that kick's **own trap return**, and counted
  there: a kind of its own (`IPI_QUIESCE_KICK`), a per-CPU flag its
  handler sets, and each architecture's interrupt tail reading **and
  clearing it unconditionally** before deciding whether it may publish,
  so the flag cannot outlive the trap that set it and claim a later
  publish. Invariant **Q19**.
  **The answer: it works, but barely — one attributed publish in eight
  boots, about 220 kick IPIs, and that one on AArch64.** The decision
  rule was written down before the measurement, and it says a counter
  that rises means the kick stays; deletion, which the report called the
  likely outcome three times, is off the table — on evidence thin enough
  that the follow-up should widen the sample first. **A first version of
  this said four per cent and was wrong**: it counted publishes by a CPU
  that had *already* published the target epoch, which are correct,
  cheap, and advance nothing. Attribution now requires the publish to
  have **moved** this CPU's epoch, which took the rate from 7-in-161 to
  1-in-220.
  **And the population the kick's own comment named is not the reason.**
  The adversary was built as designed — phase-locking a short read-side
  section over the target CPU's tick — and showed the opposite of what
  it was built to show: that CPU publishes *without* a kick, because
  `schedule()` publishes at entry and the covered tick still sets
  `need_resched`, so the `preempt_enable` ending the section that hid
  the tick publishes a moment later. The publish was never confined to
  the trap return, which is the premise the story rested on.
  **That is a local result and CI does not reproduce it reliably** —
  there the covered CPU has been seen both publishing while its ticks
  were hidden and still pending after a hundred milliseconds, on both
  architectures — so the test now reports the outcome rather than
  asserting it, and whether the variation is the population or a
  non-portable adversary is unsettled (invariant **Q19**). Where the
  the one attributed publish came from is now a question the counter can
  answer and argument could not.
  The other half of the measurement is the half that makes it
  attribution rather than a tally: `quiesce-kick-spinner` takes eight
  kicks inside a read-side section and publishes **none** of them. A
  counter that only goes up is not attribution. 343 self-tests on both
  architectures, debug and release (PR #179).

- **The harness can say what the connection it accepted was doing.** The
  accept unit answered *which* connection arrived; it has answered the
  same way every time since — one connection, carrying nothing — and
  nothing was recorded about that connection itself, because the harness
  closed it in a `finally` without asking. Now every connection carries
  **how it ended**: `closed` (an orderly FIN), `error` **with its
  errno**, `deadline`, or `wrong-data`. Those were all `0 byte(s)`
  before, so no sighting could say whether slirp closed its end or the
  harness merely timed out — the most valuable bit the roster lacked.
  On Linux it carries the connection's TCP state too, separating slirp
  holding an open socket and never forwarding from slirp having closed
  it. On the failure path only, a timed probe through the **same slirp**
  to the guest's echo service asks whether that path was answering at
  all; the reading is asymmetric and the line says so, since a slow
  answer implicates slirp *or* the guest and cannot separate them.
  **Nothing is written to the accepted socket**: a write into
  `CLOSE_WAIT` succeeds, so it cannot tell an open peer from a closed
  one — that was the first design and it could not have discriminated.
  Neither could the second, which flattened a reset into an orderly
  close, in the one case this defect is known to involve. The bug-proof
  is that an open, a closed and a resetting peer must read **three
  different ways**, asserted rather than assumed.
  A host-side packet capture would be the better measurement and needs
  root, so it is left to a human with `sudo` rather than designed into a
  test.
  **It answered on its first outing.** Sighting thirty, on this unit's
  own CI: the guest's half was reset (`sent -104`) while slirp's
  host-side half was **`ESTABLISHED`, open and silent** — ended by the
  harness's deadline, not by a FIN or a reset — and a probe through the
  *same* slirp answered in **1 ms to connect and 1 ms to echo**. So
  slirp tore down one half of this connection and orphaned the other
  while remaining perfectly responsive to everything else. That is a
  **per-connection failure inside slirp**: not a stall, not a foreign
  connection, and not this kernel, whose side has been fully accounted
  for since the socket-verdict unit. It does not name a line of code —
  it names the component and the shape, which thirty sightings had not.
  343 self-tests on both architectures, debug and release (PR #182).

- **An ARP retry can no longer outlive the interface it points at.** ARP
  and ND entries held a **bare** `struct netif *` and took no reference.
  Their ageing passes copy that pointer out from under the table lock,
  release the lock, and then dereference it — `send_arp` reads
  `nif->mac`. Meanwhile `netif_unregister` flushes those tables and drops
  what can be the last reference. **Nothing closed that window**: the
  per-CPU barrier a step earlier is for the receive path, by its own
  comment, and `age_work` re-arms every second — so a retry could begin
  *after* the barrier and read the interface after it was freed. The code
  survived by the timer not having fired.
  Both retry lists now take `netif_get` as they copy and `netif_put`
  after the send, which is sound because the flush takes the same lock:
  an entry present under it means the unregister's flush has not run, so
  the registry's reference is still held. Invariant **N22**, with the
  sweep it came from — `tapsvc` holds one too and is safe by explicit
  ordering, which the invariant now records rather than assumes.
  Entries deliberately do **not** hold one each: the flush already
  clears them, so per-entry references would turn a missing flush from a
  dangling pointer into a leak without fixing the dangling pointer.
  Flushing now also **counts what it drops** — `pending_dropped` for ARP
  and a new `nd_pending_dropped` for ND, which keeps its statistics in a
  different struct, so one fix was invisible to the other.
  The tests park a retry in that one-unlock window and run
  `netif_unregister` to completion against it, rather than racing two
  threads: the driver's release must not have run, and the reference
  count must show the retry's hold. 347 self-tests on both
  architectures, debug and release (PR #184).

- **A module zombie is collected without being asked for.** A module
  whose objects outlive its unload keeps its whole image mapped **and
  its dependencies pinned** — `drop_deps` runs at the free, not the
  unload — so one left behind blocks unloading everything beneath it.
  The only reaper was `module_unload` of the zombie's own name: a
  request nobody had a reason to make, which a replacement loaded under
  that name hid entirely, and which took one call per zombie when a
  name had more than one. **It survived because the happy path was
  tested and passed** — the mechanism worked, and nothing invoked it.
  A sweep now frees every zombie whose objects have gone, by identity
  rather than by name, at the top of a load and on **every** exit from
  an unload — including the `-EBUSY` a pinned dependency returns, which
  is the one action someone takes on discovering the pin. The ordering
  is the point: an explicit `module_unload("name")` is a real request
  and must still find its own zombie, so the named paths run first and
  the sweep never steals it. Invariant **M24**, with
  the two properties such a walk needs — removal-safe iteration, and an
  **acquire** load of `live_objects` so module text is never unmapped
  ahead of the final release.
  A load also reserves its publish slot **before** `init()` runs, so
  exhausting `MODULE_MAX_LIVE` returns `-ENOSPC` instead of panicking
  after the module is already initialised, linked and counted — where
  returning an error would have been worse than the panic.
  352 self-tests on both architectures, debug and release (PR #186).

- **A slow section is named instead of the whole suite.**
  `process-user` was one `SELFTEST` line standing for the entire
  user-mode suite — nine sections, about 540 checks in them and 54 more
  outside any section — so when it was slow nothing said which part
  was, and its budget had to be widened twice blind. On CI it reached
  **8284 ms**, past the 8000 ms every ordinary test is held to,
  surviving only on its own 20 s composite budget.
  `init --selftest` now drives a **table** of sections and times each
  call, printing `USERTEST: section <name> <ms> ms` and a total; the
  harness parses them and names the slowest beside the per-test
  summary. The table rather than ten bracketed calls is the point: the
  driver is the only caller, so a section cannot be added without a
  line — where the suite's existing `usertest: … ok` prose lines were
  a convention two of the nine had already stopped honouring. **No
  per-section budget**, because rationing a section that got more
  thorough is the defect this replaces; the numbers are for
  attribution. The first measurement disagrees with the source: `svc`
  is two fifths of the suite from **34** checks and nine sleeps
  waiting on service state, while `proc` with 235 checks is smaller —
  time here is spawning and waiting, not checking. Invariant **F13**,
  34 host checks in `tests/boot/test_usertest_sections.py`.
  352 self-tests on both architectures, debug and release (PR #188).

- **The checker checks what the format promises.** `cosmofs_check`
  took three of the format's invariants on trust and had five
  reporting paths that nothing had ever made fire — code whose
  behaviour was unknown rather than merely uncovered. Runs are now
  verified to **ascend by `lblk` and not overlap**, checked as they
  stream past with one `prev` per inode and no new memory; ordering is
  tested first and the overlap test skipped for a pair that fails it,
  because every descending pair also begins inside its predecessor and
  the two are different repairs. The end of a run is computed in 64
  bits, so a run near the 2³²-block bound cannot wrap and hide a real
  overlap. A **name repeated in one directory** is reported through a
  fixed 4 KiB bitmap whose hit only means *maybe*: the directory is
  re-scanned to confirm, so a collision costs a re-scan and never a
  wrong finding — and the names pass completes **before** the walk can
  recurse, or a subdirectory clears its parent's sheet. The five
  unfired paths each gained a corruption and a test; all five worked
  first time, against the report's own prediction that one would not.
  Thirteen classes, every one of which a test can now make fire.
  360 self-tests on both architectures, debug and release (PR #189).

- **The two tables threads left behind are locked.** When native
  threads arrived, `malloc.c` and `stdio.c` took locks and `errno`
  became thread-local; `stdlib.c`'s environment and `atexit` list were
  left as they were, and invariant **L8** enumerated three safe tables
  and said "all three are done" while the library had five. `setenv`
  growing the environment calls `free(environ)` while `getenv` may be
  walking it — **a use-after-free in the allocator that same unit
  locked**, reached through a table it was locked to protect — and
  `atexit`'s `g_atexit[g_natexit++]` both lost handlers and could
  write past a static array, because the bound check and the
  increment were separate. One lock now covers both tables, `exit`
  never runs a handler while holding it, and `getenv`'s returned
  pointer stays valid because `setenv` **leaks** the string it
  replaces — deliberate, **unbounded** in the number of overwrites,
  and recorded with its cost so it is not tidied away; a case holds a
  returned pointer across an overwrite and reads it back after the
  heap has been reused at that block's size, so tidying the leak away
  now fails a test rather than nothing.
  Eight cases in `thrtest`, and **the use-after-free is reproduced
  rather than argued**: unlocked, with a thread churning the heap so
  the freed array is reused, the process dies with a `#GP` at the
  same address on three runs of three — a reliable reproduction
  rather than a deterministic one, since nothing forces the
  interleaving. Getting there took two
  corrections — the reader had to look up a name placed *after* the
  padding so it actually walks the array being reallocated, and the
  block had to be reused before the stale pointers in it could be
  wrong. `setenv` frees the array and never a string, so a reader on
  the stale copy otherwise reads correct pointers out of freed
  memory. Unlocked `atexit` separately accepts **33 registrations
  into a table of 32** and loses two handlers. Two fixes outside
  `stdlib.c` came out of building it, both recorded where they live:
  the shell's AND-OR lists were right-associative, so `A && B || C`
  with a failing `A` ran **neither** branch and `/etc/rc.test` could
  never print `SHTEST: FAIL n` (`docs/userland/invariants.md` U11);
  and `cosmo_thread_start` builds a stack by punching a hole in a
  reservation and re-mapping it `MAP_FIXED`, which another thread's
  `mmap` can take in between — `EEXIST` out of a thread start, three
  times on aarch64 CI, now retried while the real repair (a
  `MAP_FIXED` that replaces, as POSIX says) is filed in the
  inventory. 360 self-tests on both
  architectures, debug and release (PR #191).

- **`MAP_FIXED` replaces, as POSIX says, and the range is owned while
  it does.** `space_insert` refused any overlap, so a fixed mapping
  over live memory returned `-EEXIST` and a caller wanting to convert
  part of a range it already owned had to `munmap` a hole and `mmap`
  it back — two syscalls with the range belonging to nobody in
  between, which `vm_user_find_free` will hand to the next
  `mmap(NULL, …)`. `cosmo_thread_start` does exactly that to place a
  guard page below each stack and **lost the race three times on
  aarch64 CI**, getting `EEXIST` out of a thread start; the previous
  unit shipped a bounded retry and filed the real repair.
  `vm_user_map_anon_replace` is that repair: three critical sections,
  because the page-table teardown takes the space lock itself once
  per chunk, with the new region put in under the
  same lock that clears the old ones, marked `VM_REGION_QUIESCED`
  and still claimed across it, so **one region owns the whole
  interval — holes included — at every instant**. An earlier version
  claimed only the regions that existed and left a spanned hole for
  the allocator to hand out, which review caught as a route to a
  kernel panic. The
  claim is an ownership claim — a user fault on such a region
  installs nothing and retries, a kernel fault inside a copy reports
  `-EFAULT`, `munmap` refuses it with `-EBUSY`, and a per-space mutex
  serialises replacements — so the finishing swap cannot fail, which
  matters because every fallible step and the whole page accounting
  happen before the first change. Both doors use it: the native
  `mmap` now conforms, and the Linux personality, which already
  replaced but as an unmap followed by a map (**the same window, one
  door further in**), no longer has it. `COSMO_MAP_FIXED_NOREPLACE`
  keeps the old refusal, because "place this only if the range is
  free" is a real request and the self-tests assert it. libc drops
  the punch, the retry and `STACK_MAP_ATTEMPTS`. Invariant **M40**;
  `vm-replace` and `vm-replace-race`, each bug-proofed by four
  mutations — and one of those mutations found a vacuous case of my
  own, a refusal whose range could not split and so could not show
  that a failure changes nothing. 362 self-tests on both
  architectures (PR #193).

- **`mprotect`, which one personality already had.** The Linux
  personality has been able to change a mapping's protection since
  milestone 10 — `lx_mprotect` over `vm_user_protect`, which the ELF
  loader also uses — and the native ABI could not: this tree's usual
  second-door bug, inverted. `SYS_mprotect` (93) is the native door,
  keeping the native rules `mmap` and `munmap` keep (an undefined
  `prot` bit is `EINVAL` rather than ignored, `len` is a page multiple
  rather than rounded) and translating for the VM layer, which decides
  W^X, holes and claimed ranges. The part that only came out of review
  of the report: **user code cannot make freshly written bytes
  executable on AArch64**, because `dc cvau`/`ic ivau` need
  `SCTLR_EL1.UCI` and this kernel does not set it — so the kernel
  synchronises the instruction stream when a range gains `PROT_EXEC`,
  at both doors, for every JIT and not just a test (invariant M41).
  Honest about what QEMU can show: TCG invalidates translated code on
  write, so the write-then-execute self-test proves the path runs
  without trapping, not coherence — and removing the PAN bracket
  around the maintenance did not fault under the guard boot either, so
  the bracket is kept for the rule, not for a proof. Hardware is where
  both would be settled. `MAP_FIXED`
  replacement can never substitute for this call — it returns
  demand-zero memory — and libc's thread stacks deliberately keep the
  reserve-and-replace sequence #193 proved (PR #195).

- **The two thread calls the native door still lacked.** After
  `mprotect`, `README` still listed futex requeue and per-thread signal
  targeting as what native threads went without -- both built since
  milestone 10, both reachable only through the Linux personality.
  `SYS_futex_requeue` (94, the compare form only: the native ABI is new
  and need not carry the race glibc abandoned `FUTEX_REQUEUE` for) and
  `SYS_thread_kill` (95, `tgkill` with the process implied, so another
  process's thread is `ESRCH` by construction) close the gap. The unit's
  substance was in libc: `cosmo_cond_broadcast` now wakes one waiter and
  moves the rest onto the mutex, and the report's review found that a
  requeue is not a drop-in for a three-state mutex whose unlock wakes
  only when it finds 2 -- so the condition records its mutex (two words,
  published `SEQ_CST` against `seq`) and waiters relock through the
  contended path, which makes the one waiter a broadcast wakes the head
  of a chain that every unlock carries (invariant L10,
  `docs/libc/invariants.md`). The report's third rule, the broadcaster
  reading the word after the requeue, was removed by its own bug-proof
  passing, and with it the only load through the recorded pointer. The
  measurement libc's own comment
  had deferred: eight waiters, one broadcast, **no** sleep on the mutex
  word against seven for wake-all, on both architectures. Building it
  found a kernel bug: a requeue of a word onto itself re-pushed each
  waiter to the tail of the list being walked, an unbounded loop with
  interrupts off that any program could ask for; counted in place now,
  and that count is how `thrtest` knows its waiters are asleep rather
  than merely arrived. Eight `thrtest` steps, every rule bug-proofed by
  removal with a bounded join reporting the hang; the syscall fuzzer
  gets both calls, `thread_kill` with signal 0 only. Report:
  `docs/audit/next-subsystem-native-thread-door.md` (PR #197).
- **A device removed while it was busy, at last on a real one.** The
  lifetime-and-quiescence work named four windows nothing had ever
  raced; three got an adversary and the fourth — a virtio device
  removed with I/O outstanding — was narrowed to the *unbind
  transition* on a synthetic device, because the machine's only
  virtio-blk is the scratch disk every filesystem test runs on. The
  reason was a machine, not a mechanism: the machine now carries a
  second, 4 MiB virtio-blk that exists to be removed (`QEMU_RMDISK`,
  attached after every function the documentation numbers, so nothing
  moved). `virtio-remove-inflight` fills the driver's slot table by
  construction — a hook leaves the device's finished requests
  unconsumed, because a QEMU device answers in microseconds and the
  natural race finds **nothing** in flight, every run, on both
  architectures — removes the function while a submitter on another CPU
  keeps going, and asserts the protected object: every accepted bio
  completed exactly once with `0`, `-EIO` or `-ENODEV`, the `-EIO`
  count *equal* to what the remove found (64 of 64: no double
  completion, no stranded slot), and nothing completed after the
  boundary the removal stamps inside itself — a stamp the caller takes
  afterwards can be beaten by a callback that completes later and
  numbers itself earlier. Then it brings the disk back with
  `pci_test_rebind` and reads the sector written before the removal,
  which is what says the removal left the hardware sane and what lets
  the test leave the machine as it found it. Nine mutations, including
  `blk_unregister` dropped from the driver's remove (a kernel page
  fault) and a leftover slot completed twice. **And it found the defect
  it was built to find, through review rather than through the test:**
  `vblk_remove` read and cleared the driver's slot table -- with no
  lock, and *before releasing the queue's interrupt* -- then freed the
  ring and the DMA pool that a completion handler would be walking. A
  reset stops the device; it says nothing about a handler already
  inside the driver. The kernel has had the answer all along
  (`synchronize_irq`, and a handler is a quiesce read-side section):
  NVMe releases its vectors before freeing its queues, xHCI calls
  `synchronize_irq` by hand, AHCI disables its interrupt before tearing
  its ports down, and **virtio-blk was the only one of the four in the
  wrong order**. The fix is the order -- `virtq_free`, which releases
  the vector, now precedes the slot walk (invariant **Q11b**) -- and
  the third test pass holds a read-side section across the teardown and
  asserts the removal entered it, the section ended, and only then did
  the walk begin. A first attempt built a private barrier in the driver
  instead, on the mistaken belief that the kernel had no
  `synchronize_irq`; the review that found the defect found that too,
  and the mechanism it wanted was already there. Found on the way and
  repaired here because it blocked the gate: six checks in
  `lockuptest.c` asserted `thread_count() == before` the instant a join
  returned, and the count falls at the reaper, not at the join —
  `docs/testing/flakes.md`. Report:
  `docs/audit/next-subsystem-virtio-remove-inflight.md` (PR #199).
- **File-backed regions: the mappings the constitution requires.** The
  constitution's §14 *must* list has file-backed mappings, shared
  mappings and copy-on-write, and the VMM had two region kinds and none
  of the three: the native `mmap` refused every file mapping with a
  comment that had outlived the VFS ("file mappings arrive with the
  VFS"), and the Linux personality copied a file eagerly into an
  anonymous region, refusing `MAP_SHARED|PROT_WRITE` and handing a
  read-only `MAP_SHARED` mapping a snapshot no later `write()` ever
  reached. Now a file is mapped as a `VM_REGION_FILE` region over **the
  page cache's own frames**: a shared mapping and `read()`/`write()` are
  one frame, coherent in both directions with nothing to synchronise;
  a private mapping is copy-on-write, per page; every page is
  demand-paged. The fault runs in two phases under the order
  `vnode → pagecache → vm_space` with the install **under the cache
  mutex**, so a truncate or a write-back on the same file is serialised
  against it without a third mechanism, and the region is found again
  by what it maps -- vnode, index, sharing -- before anything is
  installed. Building it found that both filesystems trim the cache
  *before* they lower the size, so the cache keeps a bound for that
  window. Dirtiness is a write fault, not a hardware bit: a shared
  page's PTE gains write only in the fault handler and write-back lowers
  it before the bytes are read for the disk. A cache frame is referenced
  once per PTE; reclaim skips a referenced frame; truncate unmaps before
  it frees; and every process exit checks `file_pages == 0` as it
  checks `anon_pages`. `SIGBUS` past the end or on a read that fails;
  `maxprot` so a shared mapping of a read-only fd cannot be made
  writable; `msync` at both doors (`SYS_msync` 96, `LX_msync` 26);
  `COSMO_MAP_SHARED`/`COSMO_MAP_PRIVATE` with the native refusal rules
  (`SHARED|ANONYMOUS` is `EINVAL`: there is no `fork`, so nobody to share
  with). The dynamic linker's mappings became demand-paged copy-on-write
  mappings of the same files, with the existing `lxtest` dynamic cases
  as their regression suite. An `mmap` section of `init --selftest`
  proves the coherence both ways, **the first shared memory between two
  processes in this system** (a spawned child writes a byte the parent
  reads), copy-on-write per page, the rights, the native rules, `SIGBUS`
  past the end and under a truncate (children, by status), the
  address-space limit, a leak cycle whose exit runs the count check,
  and on cosmofs that `MS_SYNC` writes exactly the dirtied page and a
  write after it faults and is written again; three kernel tests hold a
  fault between its phases through an event-driven seam while another
  thread installs the same page, unmaps the range, or replaces it with
  another file (nothing installed, one frame, the other file's byte),
  make a cache miss's read fail under a mapping (`SIGBUS`), and pin a
  frame against a forced reclaim. Nine mutations, each run: the
  write-back not lowering PTEs and the fault marking nothing dirty fail
  the same section on *different* checks (the counter tells them
  apart); a frame installed without its reference is caught by the
  poisoner with the section's own byte in the dump; a truncate that
  frees before unmapping dies on the frame-count assertion; the bound alone survived, as the report declared in advance (the window is inside a filesystem's truncate and no seam sits there); the re-find by kind alone let a held fault install one file's page under another file's name, which only the third held-fault variant could show; `maxprot` ignored and `SHARED|ANONYMOUS` accepted each failed their probe. **And self-review found what the report had not**: `mprotect` on a private mapping raised a read-only cache frame to writable, so a write after it went through to the file; the rule is per frame now -- a cache frame's PTE never gains write from `mprotect`, only a copy-on-write copy does -- with its own check and its own mutation. Review of the build found three more, each fixed with a check: the copy-on-write path that replaces a present frame did not consult `COSMO_RLIMIT_MEM`; a `write()` into a page some mapping executes from changed instructions with no cache maintenance, so the cache now synchronises by the frame's kernel alias; and the Linux door validated no mapping type.
  Invariants **M42**--**M44**, **V33**; 366 self-tests on both
  architectures. Report: `docs/audit/next-subsystem-file-regions.md`
  (PR #201).
- **A futex keyed by what the word maps.** The file-regions unit gave
  two processes one page and left them no way to wait on it: the futex
  was keyed by address space, so a sleeper on a word in a shared page
  was invisible to a wake from another process on the same word --
  nothing refused, the wake found nobody -- and the Linux door masked
  out `FUTEX_PRIVATE_FLAG`, the one bit that says which kind a futex is,
  so a musl process-shared mutex in a shared page was the program that
  broke. Now a futex's identity is what the word maps: the space and the
  address for a word in the process's own memory, the vnode and the
  file offset for a word in a `MAP_SHARED` file mapping, with the one
  vnode reference the waiter holds for the vnode its current key names.
  The VMM classifies under the space lock and skips the walk entirely in
  a process with no shared mapping, so libc's mutex pays one load; the
  Linux flag is honoured both ways; the native calls always classify,
  on the native rule that the kernel does not ask the program what it
  can see. The whole change to the futex is its hash and two match
  lines; the sequences and the compare-then-enqueue that L4 rests on
  are untouched, and a requeue that changes the key exchanges the
  reference -- one add of as many references as waiters moved, under
  the bucket locks, the old ones put after them -- which three review rounds on the report
  sharpened from "the reference travels with the waiter" to a rule that
  survives an unbounded chain of requeues with one slot. The `mmap`
  section proves the two-process wait (**the first wait across two
  processes in this system**, the parent counting the child asleep
  through a requeue of the word onto itself, a count that crosses the
  boundary only if the key does), the wake before the sleep, private
  stays private, unmap under a waiter, requeue across kinds and onto a
  shared word that then goes, and a double requeue through two files
  with the first released between; `lxtest` proves the flag both ways
  on a shared page with clone threads. Building it found that the
  file-backed page fault ran with interrupts masked, as every trap
  enters -- the anonymous arm never minded, the file arm sleeps and
  shoots down, and the file-regions unit's own tests never contended
  the cache mutex, so the assert never fired until this unit's did:
  the arm now runs with interrupts as the interrupted context had them.
  Seven mutations, each run: the key by space alone and the counter's
  test inverted each lose the two-process wake (the child times out,
  the count never crosses); the flag still masked fails all four
  `lxtest` checks; the counter not decremented panics at the first exit
  of a process that shared; the old references not put after a requeue
  leak the first file's vnode, which the double-requeue's page count
  sees; the moved waiters' references not taken panics on a released
  vnode; and the reference not taken at classification survives, as the
  report declared in advance. Bench: a wake costs 2.0 / 3.9 us with no
  shared mapping on x86-64 / AArch64, 3.9 / 6.6 us with one, and a
  cross-process round trip 132 / 145 us. Invariant **I7**.
  Report: `docs/audit/next-subsystem-shared-futex.md` (PR #203).
- **The count that could wrap, and the hold that held only at the door.**
  `virtio-remove-inflight`'s held pass found **0** in flight twice in
  CI, on branches that touch no driver, with every bio completed `0`
  and the pass over in 30 ms. The cause was the test's own count: the
  submitter counted an accept after `blk_submit` returned while the
  completion callback on the other CPU had already counted the
  completion, so `accepted - completed` -- two unsigned words --
  wrapped for that instant, the wait for a full table exited at once,
  and the remove walked an empty table. The accept is counted before
  the submit now, and the pass asserts `completed <= accepted` on every
  turn of its loops, completed read first and both atomically, from
  the test thread while the submitter runs on the other CPU; the worst
  case of the old order fails that assertion within a millisecond
  (PR #206). Reading the first sighting also found a real hole in the
  test's seam, closed on the way (PR #204): the hold that parks a
  device's finished requests was checked once at `vblk_done`'s entry,
  so a handler already inside its pop loop when the hold landed kept
  popping. The check runs before every pop now, and a fourth pass,
  `held-inside`, builds that moment rather than racing for it -- every
  hold in it stored from a completion callback, the parked requests
  known to be finished at the device by an exact seam (`unconsumed`:
  used entries not yet popped) instead of a wait; with the check back
  at the door it fails deterministically. The hole was real and was
  first taken for the cause; the second sighting, on the driver with
  the hole closed, is what named the count. `docs/testing/flakes.md`;
  `docs/kernel/device/testing.md`; the #199 report's banner, items 11
  and 12.
- **Unix domain sockets: a name in the filesystem, and a handle that
  rides in a message.** Since the service manager was built the README
  has said "a named pipe and a unix socket — are both things this
  kernel does not have"; the second exists now, and the `mknod` the
  first needs with it. A third family in the socket object
  (`COSMO_AF_UNIX`, dispatched inside the socket layer, so no door or
  rights table learns a new kind): a stream connection is the pipe's
  bounded queue twice, a datagram socket one such queue, a listener a
  backlog-bounded queue of connections made at `connect` -- the client
  writes before anyone accepts and the bytes wait. A name is a
  `VNODE_SOCK` node made by the new `mknod` vnode operation (ramfs;
  cosmofs has no on-disk type and refuses), mode 0755, owned by the
  caller, the node's mode the access control for connecting, `open`
  answering `ENXIO`; or an abstract name keyed by the caller's **root**,
  so a jail sees only its own. A message carries bytes, a name and
  handles: a handle rides under spawn's transfer rule, factored into
  one function both callers use, and arrives with the rights the sender
  named, installed in order until the first refusal (`HTRUNC`); a unix
  socket does not ride (the cycle Linux garbage-collects is refused,
  and the collector named as the unit that would lift it). `socketpair`,
  `SO_PEERCRED`, `sendmsg`/`recvmsg`; the Linux door's `AF_UNIX`,
  `sockaddr_un` in its three forms, `SCM_RIGHTS`, `MSG_CTRUNC`,
  `socketpair`. **Building it found two things.** A waiter on another
  socket must hold no reference to it: a connector blocked on a full
  backlog that held the listener kept it alive past its last handle,
  and the wait depended on a release the wait prevented -- so such
  waiters sleep on one generation queue and resolve the name again
  (invariant **I8**). And the Linux door's `pipe2` and `openat`
  installed handles with no owner rights, so no Linux descriptor could
  ever have been passed; they carry them now. Six self-tests (a stream
  end to end, datagrams, names, handles, a two-CPU close race,
  readiness), a `unix` section of the user suite (a child on the other
  end of a pair, a file handle across it, `SO_PEERCRED` naming the
  child, a uid-1000 child refused by the node's mode, a jailed child
  reaching neither the abstract name nor the path), `lxtest` rows;
  nine mutations, each caught by a named test or, for the reference
  rule, by the deadlock it exists to prevent. Bench: a one-byte round
  trip to a child costs 121 / 147 us over a unix pair on x86-64 /
  AArch64 against 140 / 181 over two pipes. Invariant **I8**; 372
  self-tests on both architectures. Report:
  `docs/audit/next-subsystem-unix-sockets.md` (PR #207).
- **Named pipes: a pipe with a name, and files that can be waited
  on.** The other half of the sentence the unix-sockets unit left: a
  `VNODE_FIFO` node made by the new `SYS_mknod` (100) through the same
  `mknod` vnode operation (ramfs; cosmofs and procfs refuse), behind
  which sits the pipe's ring -- split from its two end objects, which
  became one client of it, with `ipc-pipe` unchanged as the proof. The
  ring is made by the first open and freed by the last release, and
  its reader and writer counts are the live **opens** of each side, so
  the pipe's own end-of-file and `EPIPE` rules follow from them
  (invariant **I9**). Open is POSIX's: read-only waits for a writer
  (one that opened since the reader joined, even if it has closed
  again by the time the reader runs: the other side's open generation,
  not its count, which lost exactly that writer),
  write-only for a reader, `O_NONBLOCK` (`0x0800`, new to the native
  `open`) makes the first return at once and the second `ENXIO`,
  `O_RDWR` never blocks; the wait is killable, and an open that fails
  -- killed, refused, out of memory -- takes back its count and any
  ring it made, because the VFS runs release only for an open that
  succeeded. Non-blocking mode is per open, as POSIX has it. For it,
  **a `struct file` can now say whether it would block**: three
  optional `vnode_ops` (`ready`, `poll_wq`, `set_nonblock`) that the
  file kobject type delegates to, so `ioready`, `setnonblock`, `poll`
  and the async ring work on a FIFO handle; `chrdev_ops` gained the
  same three and no device set them yet (the terminal and the tap do
  since PR #211). The Linux door's
  `mknodat` (`S_IFIFO`; `S_IFSOCK` `EINVAL`, the rest `EPERM`) and an
  `O_NONBLOCK` that reaches the kernel; libc `mkfifo`; a `mkfifo`
  coreutil and a FIFO in the shell test (a pipeline whose two commands
  meet on the name, since this shell runs `&` in the foreground without
  job control and opens redirections before it spawns). One kernel
  self-test (`ipc-fifo`: both open orders from a second thread, the
  non-blocking rules, per-open mode, two readers and two writers, an
  unlinked FIFO, and a real process killed inside its open, the ring
  counted before and after every case), a `fifo` section of the user
  suite, `lxtest` rows; nine mutations, each caught by a named check.
  Bench: a one-byte round trip to a child costs 167 / 197 us over
  two FIFOs on x86-64 / AArch64 against 150 / 182 over two pipes in
  the same runs: the same ring, plus the file layer. 373 self-tests on
  both architectures. Report: `docs/audit/next-subsystem-named-pipes.md`
  (PR #209).
- **Devices that can be waited on: readiness for the terminal and the
  tap, and `select` for the Linux door.** The named-pipes unit gave a
  `struct file` and `chrdev_ops` the three readiness operations and
  wired no device; this unit wires the two that block. `/dev/console`
  and `/dev/tty` answer what the console object answers
  (`tty_read_ready`, the readers queue), and `/dev/tty` for a caller
  with no controlling terminal answers `ERROR`, which is what its
  read's `ENXIO` looks like to a poller. `/dev/net/tap` gains a wait
  queue its transmit wakes and **a read that blocks unless the open is
  non-blocking** -- the one contract change, from "0 when none, the
  owner polls" to Linux's tun; `vmctl` opens it `O_NONBLOCK` and is
  otherwise unchanged. A device's per-open non-blocking mode is the
  open file's `O_NONBLOCK` flag through two small VFS helpers, so
  nothing is allocated per open. With that, an asynchronous `READ` or
  `POLL` on the tap parks until a frame is transmitted, which is the
  constitution's "async I/O must work for devices" shown for the first
  time. The Linux door gains `select` (x86-64) and `pselect6` over
  `io_poll` -- musl's `select` is `pselect6`, so every `select` caller
  had been getting `ENOSYS` -- with Linux's own set membership and one
  documented deviation: the except set (`POLLPRI`) is always clear,
  because no object in this tree reports a priority event. Two kernel
  self-tests (`tty-devready`, `tap-ready`: readiness against the read,
  a poll woken by a fed line and by a transmitted frame, per-open mode,
  a process killed inside the tap's read releasing the tap only after),
  a `devices` section of the user suite, `lxtest` rows including a
  `select` over the tap and a UDP socket; eleven mutations, each caught
  by a named check. Bench: a frame written reaches a `READ` parked on
  the tap in 315 / 165 us on x86-64 / AArch64, against the 2 ms
  poll interval, floored to a tick, that `vmctl`'s loop imposes on every
  host-to-guest frame today; `pselect6` costs 4371 / 4373 ns per call
  on one descriptor against `ppoll`'s 16423 / 14205. Invariants **V34**
  (vfs) and **N23** (network); 375 self-tests on both architectures.
  Report: `docs/audit/next-subsystem-device-readiness.md` (PR #211).
- **A migration that can land: declared per-CPU claims, a lock order
  lockdep can see, and a migrator that moves one thread.** Thread
  migration was built and removed once because the tree held per-CPU
  assumptions nothing declared; this unit is the prerequisite, not the
  balancer (`docs/audit/next-subsystem-percpu-migration.md`). The rule
  (scheduler S25): a per-CPU answer is kept only while the thread cannot
  move -- preemption disabled, interrupts off, interrupt context, or an
  affinity of one CPU -- and `preempt_disable()` is the migration
  barrier, since a thread with preemption disabled is never READY and
  only READY threads move. Debug builds check the rule in `this_cpu()`
  and `arch_cpu_id()` and panic naming the call site; `raw_this_cpu()` /
  `raw_cpu_id()` are for the current thread, an asserted-zero count, or a
  diagnostic, each with its reason; `PERCPU_WARN=1` lists every site
  once instead (the sweep's form). The sweep named 113 sites on x86-64
  and 133 on AArch64; seventeen were claims a migration would break,
  `schedule_internal` reading its own per-CPU block before the run-queue
  lock first among them -- the corruption that removed the first
  balancer, named -- and the EL2 hand-back on AArch64; all are fixed by
  making the read happen where the rule holds. Each run queue's lock is
  its own lockdep class (a static name table), so two of them in
  increasing CPU-id order is an order lockdep checks (S24, real now).
  `sched_migrate(t, cpu)` and `sched_migrate_from(from, to, &moved)`
  move a READY, non-current thread under both locks and say *which*
  check refused; the woken-before-blocked window (READY, queued, still
  `rq->current`) is refused by identity (S26). `make test-chaos` boots a
  `SCHED_CHAOS=1` debug kernel whose tick moves a ready thread to
  another CPU every fourth tick, on every CPU, through the whole suite
  and the user-mode sections, requiring its tally line; CI runs it on
  both architectures. Five tests (`percpu-claim`, `lockdep-rq-order`,
  `sched-migrate`, `sched-migrate-refuses`, `sched-migrate-stress`: about
  3,500 moves in 200 ms on x86-64, 8,000 on AArch64), 380 self-tests on
  both architectures. Not in this unit: the balancer, which is the next.
  (PR #213)
- **A balancer that pulls, and a load that can see the thread already
  running.** The migration primitive moved threads only when a test or
  the chaos migrator asked; nothing in a shipped build moved one. This
  unit is the policy (`docs/audit/next-subsystem-load-balancer.md`).
  Measured first: twice as many threads as CPUs, created one at a time
  so the rotation places them one per CPU, then every other one released
  -- the runnable set lands two-deep on half the CPUs, the other half
  stays idle for the whole run, and the machine does 53% of the work it
  could on x86-64 and 60% on AArch64 (40-47% lost across four boots).
  `sched_cpu_load(c)` is that queue's `nr_running` plus the thread it is
  running unless that is its idle thread (S29): `nr_running` counts the
  ready list and `schedule` dequeues what it runs, so a CPU saturated by
  one thread reported the same zero as an idle one, to `pick_cpu` as
  much as to anyone. Balancing is a **pull** (S27) from `sched_tick`
  after its own unlock: an idle CPU with an empty queue looks every
  tick, a busy one every `SCHED_BALANCE_TICKS` (16, 64 ms), and one
  thread moves when the busiest CPU is at least two ahead (S28) -- one
  is the steady state of an odd thread count, and a CPU running one
  thread with an empty queue is at load 1, so its thread is never
  dragged to an idle CPU to arrive cold. The scan reads other queues
  without their locks and is a hint; `sched_migrate_from` re-decides
  under both locks and picks the thread itself. **It cannot move a
  thread that is time-slicing**: two compute-bound threads on one CPU
  alternate by preemption, so the one in the queue always carries
  `THREAD_FLAG_PREEMPTED` and S26 forbids moving it -- found by the
  hysteresis test, which passed under a deliberately broken threshold
  because the thread it wanted moved could never move. So the balancer
  corrects an imbalance as work becomes runnable, not once it has
  settled into alternation. `SCHED_BALANCE=0` compiles it out and is how
  the tests prove it; the boot prints its tally and `sched_dump`'s
  per-CPU line carries the load. Four tests and a benchmark
  (`sched-load`, `sched-balance-pull`, `sched-balance-hysteresis`,
  `sched-balance-affinity`, `bench-balance`: the alternate round reaches
  99-108% of the *balanced* round -- the same threads unpinned, spread
  because creation order happened to do it, which differs from it in
  exactly one thing -- against the 53% the report measured without the
  balancer; the pinned figure is reported beside it and is not what the
  assertion is against).
  (PR #216)
- **A back-connection that survives one reset.** `net-harness` failed
  seven CI jobs on 2026-09-22 alone, across four pull requests and
  `main`, two of which changed only documentation -- and the defect is
  not in this kernel. QEMU's user-mode networking resets the guest's
  half of one connection while keeping its own half open and answering a
  probe through the same instance a millisecond later; the guest is
  correct from first SYN to final reset, verified against a packet
  capture. Three units localised it and what is left is in slirp's
  source (`docs/audit/next-subsystem-nettest-retry.md`). So the guest's
  back-connection now runs the exchange up to three times on fresh
  sockets. The bound is a **failure**, not a fallback: exhausting it
  fails the test exactly as one reset did, printing
  `client failed every attempt (3 of 3)`. Every attempt prints the full
  diagnostic block the earlier units built, and both outcome lines name
  the attempt -- `client ok (attempt 2 of 3)` -- so a recovered boot is
  still a sighting the same grep finds, and `docs/testing/flakes.md`
  counts it among the recovered ones rather than losing it. A retry that
  hid the flake would be worse than the flake. The host harness needed
  no change: since PR #177 it accepts eight connections and picks the
  one that delivers the request. `make test-harness-retry` builds a
  `HARNESS_BREAK=1` image whose first attempt is shut down from inside
  the guest after its connect, so the retry runs on every boot of that
  build instead of one in twenty, and the runner requires both halves --
  that an attempt was broken and that a later one carried the exchange.
  (PR #218)
- **A program's text belongs to the file, not to each process that runs
  it.** The loader mapped every `PT_LOAD` as anonymous populated memory
  and copied the image in, so two processes running one binary held two
  complete copies of its text and every page of every segment was
  allocated whether or not it was ever touched
  (`docs/audit/next-subsystem-elf-shared-text.md`). Measured on `init`
  by spawning copies and subtracting free-frame counts: **89 pages per
  copy, the fourth costing exactly what the first did**. A `PT_LOAD`
  that is not writable and has no zero tail now comes from the file's
  page cache, shared, so every process running the program maps the same
  frames -- with `maxprot` excluding `W`, which is what stops a later
  `mprotect` from making shared text writable. The segment's zero tail
  is demand-paged rather than populated, because an anonymous page
  arrives zero. **89 pages per copy became 40, then 16**, identically on
  both architectures. Shared text makes a running program's instructions
  the file's, so a write to a file being executed is `-ETXTBSY`
  (invariant **P30**) -- and *busy* is a property of the page cache's
  mapping list, not a counter beside it: the record is linked there when
  the mapping is made and unlinked before its vnode reference goes, and
  the check is taken under that list's lock by a writer already holding
  the vnode's, so the answer and the write are atomic against a mapping
  being created. `VM_MAP_TEXT` is passed by the loader and by nothing
  else, because a program that maps a file executable and writes to it
  deliberately is a different thing the page cache already serves. Four
  tests: one physical frame for two address spaces, the per-copy cost,
  `PROT_WRITE` refused on shared text with the zero tail reading as
  zero, and all three doors a file's contents can change through -- a
  write, a truncate, and a writable shared mapping -- refused while a
  program runs and allowed once it exits, with a private writable
  mapping allowed throughout as the control and the text mapping
  refused in the other order too. Demand-paging the zero tail also gave a multi-threaded program
  its first pages two threads could fault at once, and the anonymous
  fault panicked when they did: the flags a fault carries are the
  hardware's snapshot from when the trap was raised, so both threads
  believed the page absent and the loser mapped over the winner. The
  page table is the authority now -- asked under the space lock
  immediately before the install, exactly as the file-backed fault has
  always re-found its region (invariant **M45**, test
  `vm-anon-fault-race`). (PR #221)
- **A held walk: the working-directory race becomes a proof**
  (`docs/audit/next-subsystem-cwd-hold.md`). The cwd-ref unit fixed a
  use-after-free with a reference taken for the length of the walk, and
  guarded it with a test that passed when the fix was removed -- a
  window a few instructions wide against a whole path walk. Measured
  first: with the bug put back at the native `open` and every freed
  vnode poisoned, the test catches it in **one x86-64 boot of five** and
  no AArch64 boot of three, which corrects that unit's record that a
  poisoned run passes, and says why: nothing in the tree poisoned a
  freed vnode, so a walk that outlived its reference read a plausible
  directory. Now it reads `0x5a`, in debug builds, as freed frames
  already do. And the interleaving is no longer left to chance: a seam
  in `walk_parent`'s relative branch -- the one line every relative walk
  from every caller shares, since `vfs_open` never enters `resolve()` --
  holds a walk of the armed process with its pointer in hand until that
  process's `chdir` has published and put, with `chdir` waiting for the
  hold **before** it publishes so the held walk has necessarily captured
  the directory being replaced. Both waits are killable and bounded. The
  swapper records the old directory's count before its put: **two** in
  the pass that removed the directory (the process's and the walk's) on
  a correct kernel, one with the fix removed, where the walk then resumes
  on the poison and the kernel panics by name. Two racers, one per door
  -- the Linux door had no test of this at all -- each run once per pass
  with the seam rearmed, every claim a count the seam read rather than a
  flag the code under test set. **Two mutations survived the first round
  and both said the test was weak, not the rule dead**: the release
  order derived on the walk's resume could not tell a release a few
  instructions before the put from one after it, because the woken walk
  loses that race every time, so the releasing side now reads the count
  at the instant it releases; and no racer had the swapper arrive first,
  so a third pass starts the walker only once the seam itself says the
  swapper is waiting inside `chdir`. A `chdir` in a single-threaded
  process registers no swapper, because it has no walk to race.
  Invariant **V35**; P29 gains its proof. (PR #224)
- **`tcp-pcb-timer-free` could park its callback above its own armer,
  with nothing left to release it.** Twice on 2026-09-23, in the same
  slot after `net-lo-udp`: a hard lockup at ten seconds once, a TLB
  shootdown unanswered at one second once. The armer arms a 1 ms timer
  and must exit on its CPU before the next tick; when it did not, the
  callback spun in interrupt context above it, the test's join of the
  armer -- made before the releaser existed -- never returned, and the
  releaser was never made. Made deterministic by having the armer linger
  two ticks on purpose, which reproduced the hard lockup to the line on
  the first boot; fixed by creating the releaser before arming and
  joining the armer after the release, with the callback's CPU recorded
  and asserted. (PR #225)
- **A placement is inserted under the hold that chose it.** `mmap(NULL,
  ...)` at either door chose a range under one hold of the space lock and
  inserted it under another, and the range belonged to nobody in
  between. Measured by running the syscall's own two calls from kernel
  threads on one space: **about half of all concurrent placements lost**,
  on both architectures, with `-EEXIST` for a request that named no
  address -- which is how a `thrtest` thread start failed on a
  documentation-only rebase, once the `MAP_FIXED` unit had removed the
  libc retry that used to absorb it. Both doors now choose and insert in
  one hold (`vm_user_map_anon_free`, `vm_user_map_file_free`), two
  attempts from the hint and then the base with `-ENOMEM` the only
  answer that moves between them; the file form inserts its region
  claimed before its record goes on the vnode's list, because truncate
  reads the base from there. `vm_user_find_free` stays, advisory, for
  the two callers that may use it. Invariant **M46**, the fixed path's
  M40 made whole; `mmap-place-race` and a racer at each door, rate-based
  and said so, at a rate that cannot hide. (PR #227)
- **A directory descriptor names a directory.** Every `*at` call in the
  Linux personality answered a real directory descriptor with `ENOSYS`,
  on the stated premise that the VFS could not resolve from one -- it
  always could, every entry point takes a start -- and `fchdir` was not
  in the table. Measured before the change: `openat`, `newfstatat`,
  `faccessat`, `readlinkat`, `symlinkat`, `mkdirat`, `mknodat`,
  `unlinkat`, `renameat` and `fchdir` all refused, on both
  architectures, which is every tree walker failing the moment it
  descends. One resolver replaces the refusal at nine sites and
  references the descriptor's directory before letting go of the
  descriptor (V35), demanding the handle's `READ` right to look a name up
  and `WRITE` to change an entry, so a directory handle delegated
  read-only stays read-only -- including what is opened through it (a
  directory opened at either door carries both, masked by the rights of
  the descriptor it was opened through); `renameat` resolves its two names from two directories through
  `vfs_rename2`; a directory file remembers the path it was opened by, at
  both doors, **only if that name walked with no symbolic link reaches the
  same directory** (since the cwd-name unit, PR #232, the name its open's
  own walk took, so a directory opened through a link has one), and
  `fchdir` publishes the name with the vnode as `chdir` does. Review of the first build found the missing rights, a
  child directory opened through a narrowed one regaining them, the
  incoherent name and a failed `chdir` stranding the held-walk seam's
  swapper; each has a test. Invariant **P31**; `lxtest` checks every call
  against a real descriptor by where its effect lands, not merely that it
  succeeded. (PR #229)
- **`tcp-pcb-timer-free` holds only its own connection's callback.** Its
  test hook held the next TCP timer callback of *any* connection, and
  once, on the dirfd unit's branch, a connection an earlier test had
  left behind fired first and was held in the test's place: the test's
  close had nothing to wait for and the test failed after five seconds
  with its CPU's tick stalled. `tcp_test_hold_callback` now takes the
  pcb to hold, `timer_kick` holds only that pcb's callback and counts
  any other it lets through, and the test arms a decoy connection's
  timer one tick before its own on every boot, so the sighting's shape
  is certain rather than waited for. With the hook back to "any pcb",
  the decoy is held and the test fails. (PR #230)
- **A working directory's name is the path the walk took.** `chdir`
  published a lexical normalisation of its argument with the vnode the
  walk reached; through a symbolic link the two named different
  directories, and after `chdir` through a link and `chdir("..")`
  `getcwd` answered `/tmp` while the process stood in `/tmp/clp` --
  measured at both doors on both architectures
  (`tools/chdir-link-probe.py`). The walk now keeps the name it took
  when asked (`vfs_lookup_named`): each component entered as a
  directory, links replaced by where they led, `..` removing one. `chdir`,
  a spawn's `cwd` and a directory file's name for `fchdir` all publish
  it, so `fchdir` of a directory opened through a link now succeeds with
  its own path where the dirfd unit refused it. The same probe found
  `chdir("..")` from `/proc/<pid>` was `ENOENT`; procfs's process
  directories answer `..` now, and `/proc/self` is a symbolic link to the
  reader's pid, so no name in the tree means different directories to
  different processes. Invariant **P32**. (PR #232)
- **Mount and unmount name what the caller's path names.** Every path
  call resolves a relative path from the working directory except two:
  `mount` and `umount` resolved their target from the root, so from
  `/tmp` a mount on `mrel` covered `/mrel` -- measured on both
  architectures (`tools/mount-rel-probe.py`) -- and the matching unmount
  removed the same wrong mount, so return codes looked fine. Found while
  taking up an inventory row that wanted a test for the
  one-unmount-at-a-time guard "through a relative path from inside the
  mount": that path did not exist. `vfs_mount_at` and `vfs_umount_at`
  take a start and the two calls pass the cwd; `vfs-umount-once` then
  fires the guard for the first time, with a held maintenance pass
  keeping the first unmount in its drain and a second from inside
  refused `-EBUSY` at once. P27 names the two calls. (PR #234)
- **The balancer's test asserts what the balancer promises.**
  `sched-balance-pull` failed five CI runs in two days under the chaos
  migrator, on branches that did not touch the scheduler. Measured: every
  spread takes under 40 ms and a miss never recovers; the miss is two
  spinning workers alternating on one CPU, and a spinning pair built on
  purpose is never separated -- an idle CPU is refused some five hundred
  times a second, because S26 forbids moving a thread that may have been
  preempted between the two instructions of a per-CPU access -- while a yielding pair is separated in
  milliseconds. The test had been asserting a race: that the balancer
  pulls a spinner before its first preemption, which one chaos move lost
  by the rules. Its released workers now yield, so it asserts the
  contract, and `sched-balance-pair` asserts both halves of the
  mechanism. No kernel code changed. (PR #236)
- **`sched-balance-pair` read a worker that had not run as "on CPU 0".**
  After #236 merged, the test failed once on aarch64 with its spinning
  pair "separated": an instrumented run found three such rounds in 25,
  each after 0 ms and with no migration at all. A worker's recorded CPU
  started at zero and was first written in its loop, so a worker still
  on its way there looked like one on CPU 0. It now starts at a value no
  CPU has, the premise waits for both workers to have written the
  pair's CPU, and the pull test's counter skips an unrun worker (which
  had been a possible false pass). Sixty rounds then: none apart. (PR #237)
- **The lockup sampler's bound is a count of waits, not a stopwatch.**
  `lockup-sample-busy` asserted that two CPUs that cannot answer cost
  the sampler one 5 ms timeout, by timing it: under 7 ms. It failed ten
  times in eight days at 88-241 ms. Measured (`tools/lockup-busy-probe.py`):
  quiet, one sample takes 5.00-5.12 ms; under host load the excess is
  time the virtual CPU did not run, most of it in one gap between two
  clock reads. And on x86-64 the "masked" targets answered by NMI, so the
  claim had never been exercised there. The sampler now reports, per
  call, what it did -- claim attempts, waits armed, the interval armed
  (`lockup_sample_all_info`) -- and can be asked, for that call alone, to
  send the ordinary interrupt so masked targets cannot answer on either
  architecture; the test checks one wait of exactly the timeout, and the
  loser's "refused at once" as an order plus a single claim attempt. The
  remaining time bounds are 1 s hang guards. (PR #239)
- **The aarch64 console no longer stops taking input.** Three times on
  CI, an aarch64 release boot's shell stopped echoing mid-line right
  after a job event and never recovered. Provoked on purpose
  (`tools/console-stall-probe.py`), it stalled in 13 of 20 boots. Looked
  at from outside, the guest was idle and ticking, and the PL011's
  receive FIFO was full with its interrupt enabled but none pending. The
  receive handler drained the FIFO and then cleared the interrupt, so a
  character arriving between the two lost its interrupt, and QEMU raises
  none for the characters behind it. The handler now clears first and
  drains after (T16). A new self-test, `console-rx-clear`, uses the
  PL011's loopback to put a byte into the FIFO just after the drain, and
  fails with the old order on every boot. The shell harness's release
  boots also run six cycles of a background job exiting as a line is
  typed. (PR #241)
- **The operator can see why a guest's flows are refused.** A guest that
  fills its share of NAT's table or the firewall's has every new flow
  dropped. Before this, nothing an operator could read changed when that
  happened (`tools/net-visibility-probe.py`): a guest was refused 232
  flows while the control snapshot stayed byte-identical and the log
  silent, and the counters were read only by the self-tests. The
  `/dev/net/tapctl` snapshot, now version 6, lists every live NAT and
  firewall flow, opener to peer, with its NAT identity, established flag
  and time left, beside the shares and the refusal counters. `vmctl
  flows` prints them. The counters were split first, one cause each:
  three of them had lumped a full share with a full table, and one
  counted something that was not a refusal at all. Port-forwarded TCP
  flows can now become established, and only when the handshake completes
  in order: the guest's SYN-ACK, then the client's ACK. A client's
  unsolicited ACK can no longer hold a guest's share for the long timeout.
  Before, they never became established, and the table kept them for 30 s
  instead of 300. Invariant N24: a flow is listed
  exactly when its share counts it. (PR #243)
- **A completion's waiter no longer leaves while `complete()` is still
  inside it.** An aarch64 debug boot on `main` panicked in `spin_unlock`
  during NVMe probe: the driver's `admin_cmd` polled `completion_done` and,
  once true, returned without the handshake `wait_for_completion` does, so
  the next command reused the stack frame while the interrupt handler still
  held its completion's lock. `wait_for_completion_timeout` now does the
  handshake, and the four polling drivers (NVMe, xHCI, AHCI, USB) wait with
  it wherever an interrupt completes the command. The one poll left is
  NVMe's no-vector fallback, where the waiting thread completes the command
  itself, so no other CPU can be inside `complete`; the normal path never
  reaches it. Invariant S30; the
  `completion-timeout` self-test lingers a completion from another CPU and
  requires the lock free on return, and `tools/nvme-admin-probe.py`
  reproduces the exact panic on demand. (PR #245)
- **The vGIC queue test holds in both interrupt orders.**
  `el2-guest-irq-queue` failed on aarch64 CI's GIC boot five times. The
  host refills its one list register only at vCPU entry, so any exit
  between the guest's EOI and its heartbeat puts a pending interrupt
  first. Both orders are correct, and the test asserted one of them in two
  places, of which a fix had removed only the first.
  `tools/irq-order-probe.py` forces that exit in all nine guests that take
  an interrupt, and found the remaining assertion to be the only one of
  its kind in thirteen tests. Both halves now go through one helper that
  accepts a heartbeat while the interrupt is pending. The test also runs on
  two more guests, `guest_irq_exit` (the interrupt always first) and
  `guest_irq_hb` (the heartbeat always first). So both orders happen on
  every boot, and an assertion of either fails at once. (PR #247)
- **`sched-migrate-stress` asserts what migration promises.** It failed
  twice on aarch64 CI with "a worker made no progress under migration".
  `tools/migrate-stress-probe.py` named the worker: in every case it was
  READY and first ran after the 200 ms window closed, and no thread was
  lost. The test's migrator re-picks the thread it just moved, because the
  policy takes the tail and a move enqueues at the tail; one worker was
  moved 250 times before its first round. The test now runs in three
  phases: stress, stop the migrator, then require every worker to run
  again. A late starter makes the old window assertion fail on every boot.
  A worker that never leaves is named rather than joined, with its storage
  left to it. "Migrate at most once between runs" was measured and
  rejected. (PR #249)
- **A test's process is named, not counted, and nothing a test spawns
  outlives it (P33).** `run_module`'s `process_count() == before` failed
  in three tests (`dev-tty-none` on CI, `signal-group`, `tty-isatty`), and
  each time the cause was the test before. The reaper completes an exit
  before it drops the exited thread's reference, so a test could return
  with its process still in the table, and the next test counted it and
  watched it leave. `tools/proc-settle-probe.py` reproduced the CI failure
  on every boot by delaying one pid's reap, and named the twelve tests that
  return early. `run_module` now waits for its own pid (`process_present`).
  The runner waits up to two seconds after every test for the table to
  empty and fails, by name, a test that leaves a process.
  `process-gone-order` forces the CI order with a reap hold armed by pid.
  Also found, for the next unit: an exited process keeps its address space
  until its last reference drops, so `waitpid` can return while the
  child's binary is still busy. (PR #251)
- **An exited process has no address space (P34), and tearing one down
  steps over what was never used (M47).** `waitpid` reaped a child as
  soon as it was `EXITED`, but the child's address space, and the text
  mapping that held its binary busy, went only when its last reference
  dropped. Unforced, a program that waited for a child and rewrote its
  binary was refused `ETXTBSY` 10 times in 4,200. The space now goes
  first in `process_last_thread_gone`, before `EXITED` is published.
  Placed after, a parent already looking still reaped mid-teardown. That
  needed the teardown cheap: it queried every page of every region (the
  8 MB stack reservation is 2,048 of them) and shot down each chunk of a
  space nothing runs, 5.7-18.1 ms a process. A dying space's tag is now
  invalidated first; its teardown shoots down no chunk and steps over
  absent tables (`arch_mmu_absent_span`, both architectures).
  `exit-space-order` holds an exit between the teardown and the publish
  and finds the binary writable; `vm-teardown-absent` counts the pages a
  teardown queries. P33 now blames a leaked process once: one boot of the
  probe failed 19 tests for one leak. (PR #253)
- **Three scheduler tests wait for the spinner they rely on to run.**
  `sched-load`, `sched-migrate` and `sched-migrate-refuses` each start a
  pinned spinner and then rely on it running. Each waited for something
  a spinner that has only been placed already satisfies: a non-zero load,
  or its worker `READY`. `tools/placed-running-probe.py` found the spinner
  not yet running at 98-99 of 100 of `sched-load`'s checks, and at 1-14
  of 100 of the others'. `sched-load`'s 0 ms failure was a read inside the
  idle CPU's switch, where the thread is dequeued before it is current.
  The spinner now says it has run (`entered`), and the tests wait for
  it. `sched-migrate-refuses`' spinner outranks the reaper and `quiesce`,
  and a rival at the reaper's priority, made runnable on its CPU before
  the migrate, must not run. (PR #255)
- **A failed test releases what it made, and a test that leaves network
  state is named once.** One `net-dns` failure used to take eight tests
  with it. It returned holding its tap and one of the eight DHCP/DNS
  service slots, `net-multiguest`'s eighth tap then failed for want of a
  slot and kept its seven, and every later tap open failed.
  `tools/net-leftover-probe.py` reproduced that exactly.
  - The expiry check now asserts that the aging reclaimed everything
    pending before it, not an empty table, which a still-draining flood
    can refill.
  - Every acquisition in the network tests registers its release with
    the runner (`selftest_defer`), and the runner runs what a test still
    holds, last first. That covers taps, services, sockets, interfaces,
    files, hooks, settings and threads (whose arguments outlive their
    join). `irq-route` releases its line and its periodic source the
    same way.
  - The runner compares interfaces, services and sockets before and
    after every test, and fails a test only for what it added: fewer is
    never a leftover.
  - Every network test was forced to fail at its last check and at its
    midpoint, in one boot each. At the last check, 50 of 50 were forced
    on x86-64 and 49 on aarch64, where one test passed fewer checks than
    counted. At the midpoint, the first run hung both architectures: two
    interfaces were registered on the stack. With those fixed, 50 of 50
    were forced on both. Each time exactly the forced tests failed and
    nothing was left behind. (PR #257)
- **`net-hostinput`'s window update is checked as a stream, and the
  zero-window probe has a test.** The check failed three times. A zero
  window with data waiting sends a one-byte probe on the retransmit
  timer, and the test's blocked phase (~180 ms) runs just under the
  200 ms RTO. On a slow host the probe went first, and the data resumed
  past it, which the check (all 50 bytes in the first segment) refused.
  `tools/zero-window-probe.py` forced it both ways. The check now asks
  for every one of the 50 bytes, in any order (`hin_recv_stream`), and
  `net-zero-window-probe` proves the probe and what follows it: an
  update that acks the probe, one that does not, and one that arrives
  before it is read. No TCP code changed. (PR #259)
- **`sched-balance-hysteresis` judges the balancer by what it saw.** It
  failed twice with "the balancer moved a thread for a difference of
  one", which the balancer cannot do: it re-checks every pull under both
  locks. The test judged pulls by sampled loads, and that failed two
  ways.
  - A third thread briefly runnable on A is a real 3 against 1, a pull
    the rule allows, and a sampler every ~8 ms can miss it.
    `tools/hysteresis-probe.py` reproduced the message that way.
  - The load hint reads a yielding CPU one high mid-switch. So the
    sampled premise broke in nearly every boot, and a balancer lowered
    to a threshold of one passed as "not asserted".

  `sched_migrate_from` now records each balancer pull's locked
  difference on the thread it moves (`bal_pulls`, `bal_gap_min`, debug
  builds). The test fails any pull below two, and a third thread
  spinning beside the pair must draw a pull, at two or more. (PR #261)
- **`smp-wake` posts to an idle target and reads what the wake
  decided.** It failed once on CI with no reschedule IPI counted. The
  test posted as soon as the waiter read `BLOCKED`, which
  `waitqueue_prepare` sets before the waiter switches out. A post in
  that window finds the waiter itself running, and `sched_wake` rightly
  sends nothing. `tools/smp-wake-probe.py` reproduced it exactly. The
  test also counted any reschedule IPI during the whole block, so it
  could pass without this wake's.
  - It now waits for the target to read idle before posting.
  - `sched_wake` records on the woken thread whether it asked for a
    reschedule, and the target's handled count before it sent
    (`wake_resched`, `wake_ipi_base`, debug builds; `ipi_count_on`).
  - The test asserts both, and repeats a round whose target turned busy.

  (PR #263)
- **`thrtest` step 25 asserts what each phase of a broadcast allows.**
  It failed once on CI, reading the mutex word as something other than 1
  after the requeue. The requeue wakes one waiter, which relocks the
  held mutex at 2, and if it gets there before the probe reads, 2 is
  correct. `tools/cond-phase-probe.py` made that order certain and
  failed the check on both architectures.
  - Before the requeue, the word must read 1.
  - After it, 1 or 2, and the run logs which.
  - A third run forces the waiter-first order and asserts 2; a forced
    wait that times out fails.
  - The unlock-first order stays the usual one, and is not guaranteed.

  (PR #265)
- **The self-test hang watchdog is armed for each test, at that test's
  budget.** An arming fires once and only an arm clears it, and the runner
  armed once for the whole run, so `cosmofs-replay` (13-20 s, no kicks)
  fired it in every debug boot and the 229 tests after it -- every network,
  guest, process and signal test, `process-user`, `syscall-fuzz` -- had no
  watchdog. `tools/watchdog-spent-probe.py` showed a 9 s sleeper after it
  went unreported on both architectures.
  - The budgets are one table in the kernel, printed as `SELFTEST:
    budgets default=8000 process-user=20000 cosmofs-replay=40000`; the
    harness judges by that line alone, and a run with tests and no line
    fails.
  - The runner arms the watchdog before each test at its budget. A
    passing boot prints no `[WATCHDOG]` dump, and the harness forbids it.
  - `watchdog-spend` leaves a quiet arming fired; `watchdog-rearm`, next,
    finds the runner re-armed it.
  - Its first catch: a `net-lo-tcp` hang with the test thread blocked on
    a socket, now in `docs/testing/flakes.md`.

  (PR #267)
- **`net-lo-tcp`'s backlog step asks which client it accepted.** It
  connected two clients to a listener, accepted once, and waited for
  `hi` on the first. But TCP queues a connection when the listener
  processes the client's final ACK, on the network worker that flow
  hashes to, after `connect()` has returned. So the second client can be
  accepted first, and the step then hung with every CPU idle: the
  per-test watchdog's first catch. `tools/accept-order-probe.py`
  reproduced it on both architectures by holding the first client's ACK.
  - The step (`lo_tcp_backlog`) matches the accepted socket's peer to a
    client, receives on that one after a bounded readable wait, and
    expects the reset on the other.
  - `net-accept-order` runs the same step in the forced order:
    `tcp_test_hold_ack` (debug builds) holds the first client's ACK in
    `tcp_input` until the second is queued.
  - `unixtest`'s backlog step says why its order holds: AF_UNIX queues
    at `connect()`.

  (PR #269)
- **A spinner sampled mid-interrupt still names the spinner.**
  `lockup-sample` starts a spinner on another CPU with interrupts on and
  asserted the sampled PC was in `spin_here`. On x86-64 the sample is an
  NMI, which can land while an ordinary interrupt runs on the spinner's
  CPU, and the leaf PC is then the handler's -- three CI sightings, all
  x86-64, all on commits that change no kernel code
  (`docs/testing/flakes.md`, the second with the interrupt tail and
  `spinner_main` at frame #6). `spin_here` now records the exact address
  it returns to in `spinner_main` (`__builtin_return_address(0)`), and the
  test accepts the spinner by that frame: the leaf in `spin_here` with
  that return just above it (uninterrupted), or the leaf elsewhere with
  that return deeper in the trace (interrupted). The `MAIN_FN_BOUND` bound
  on `trace[1]` goes; the masked (`lockup-sample-irqoff`, `lockup-hard`)
  and tick-sampled (`lockup-sample-busy`) checks keep the leaf-PC form,
  correct for them. `tools/lockup-interrupted-probe.py --force` parks a
  cross call on the spinner's CPU and samples into it: with the old check
  that forced sample fails as the sightings did, with the new one it
  passes. (PR #271)
- **A cosmofs metadata block says how it is checksummed.** Every metadata
  header carried a CRC32C with the algorithm hardcoded, while data blocks
  self-describe (`cfs_inode.csum_algo`: CRC32C or Poly1305) -- so on an
  encrypted filesystem the data was authenticated and the metadata only
  CRC'd, and a metadata block written by any other algorithm was
  indistinguishable from corruption. The header's spare word becomes
  `csum_algo`, written `CFS_CSUM_CRC32C` and covered by the block's own
  CRC. `cfs_mhdr_fault_of` resolves it before the checksum: a pre-v11
  image's zeroed word and `CFS_CSUM_CRC32C` both read as CRC32C, so no
  format version is threaded to the verify site, and any other value is
  `MHDR_ALGO` -- a distinct fault named in the metadata-fault message,
  rather than a bad checksum. `CFS_VERSION` is 11; the superblock and
  label keep CRC32C as the fixed bootstrap. Authenticating metadata is the
  enabled future work (the wrapped-key block is verified before the key is
  unwrapped, so metadata checksums cannot be authenticated unconditionally).
  `cosmofs-metadata-csum-id` proves a sealed block names CRC32C, a legacy
  block reads as CRC32C, an unsupported algorithm is `MHDR_ALGO`, and a
  one-bit corruption is `MHDR_CRC` -- the two distinct. (PR #273)
- **A timer is a submittable I/O object.** The async I/O ring drove any
  object with a readiness operation -- files, sockets, devices -- but had no
  timer to submit: its only timer was `aio_wait`'s whole-call timeout, a bare
  return, not a completion, so "wake me after T" could not be multiplexed
  with I/O and told apart from "nothing was ready". `timer_create`
  (`SYS_timer_create`) returns a timer kobject (a timerfd): armed with an
  initial delay and an optional interval, it becomes `READABLE` once it has
  expired, a `READ` returns the expiration count and resets it, and it rides
  the existing `POLL`/`READ` path with no new AIO op -- so a submitted `POLL`
  completes with the entry's `user_data` when it fires, and it serves
  `poll`/`select` and a blocking read too. `release` marks it dying so the
  callback cannot re-arm, then `timer_cancel_sync` cancels and waits out a
  callback in flight; a submitted timer is kept alive by the ring's reference
  while parked, so it cannot be freed under the ring. `aio-timer` proves a
  20 ms one-shot submitted as `POLL` completes with its `user_data` after it
  fires and not before, `READ` returns the count then `-EAGAIN`, a periodic
  timer fires repeatedly, and a create-and-close without submitting cancels
  at `close`. (PR #275)
- **A bound device can be reset in place.** The device model had
  `match`/`probe`/`remove` and no reset: the only in-place re-init of a bound
  device was a full remove+reprobe, which replaces the device and its
  higher-level object (a virtio-blk becomes a fresh `blkdev`, invalidating
  every reference). `device_reset(dev)` + a `reset` op on `struct
  device_driver` close that: the device stays bound and registered while the
  driver re-initializes the hardware. `blk_reset` pauses submissions into the
  pending queue (the `recovering` gate the timeout path uses) and drains the
  submit path, then reopens; virtio-blk's reset mirrors its remove's teardown
  (`virtio_device_reset`, `virtq_free` releasing the interrupt, then the
  in-flight slots completed `-EIO` once) and its probe's rebuild
  (`virtio_device_init`, `virtq_alloc`, `virtio_device_ready`) on the **same**
  `blkdev`, re-reading the geometry and failing on an incompatible
  capacity/block-size or queue-size change. `device-reset` proves a bound
  virtio-blk reset in place keeps the same disk registered and its data
  intact, a driver with no reset is `-EOPNOTSUPP`, and an unbound device
  `-ENODEV`. (PR #277)
- **Fault injection for a duplicated received frame.** `FI_NET_RX_DUP` joins
  the fault-injection kinds: when armed, `rx_common` delivers a second copy of
  a frame -- made before the original is queued (a worker could otherwise free
  it mid-copy) and carrying `M_CSUM_OK` -- so the stack meets the same frame
  twice, a link-layer retransmit or a switch flooding. It fires only where the
  receive runs in a thread (loopback; a thread-deferred driver), a no-op in a
  driver's interrupt handler like every kind. `net-rx-dup` doubles a loopback
  TCP stream and the sink still receives the bytes once, in order (sequence
  dedup), and doubles one ARP request through the ethernet path -- two replies,
  one cache entry. (PR #279)
- **Priority inheritance in the sleeping mutex.** A mutex owner is boosted to
  the priority of the highest-priority thread blocked on it, and up the chain
  of owners, so a medium-priority thread can no longer starve a high-priority
  one that is waiting on a lock a low-priority thread holds. `struct thread`
  gained `base_prio`; `priority` is the effective value, raised while a held
  mutex has a higher-priority waiter and restored on release. One `g_pi_lock`
  serialises donation and restoration as the outermost lock (run-queue and
  wait-queue locks nest under it), and `sched_reprioritize` requeues a
  ready/running/blocked thread, re-checking its CPU against a migration.
  `prio-inversion` stages the classic three-thread inversion on one CPU and
  shows the high thread now acquires before the medium thread finishes. (PR #281)
- **`statx(2)` for the Linux personality.** The modern stat call (x86-64 332,
  AArch64 291) joins `fstat`/`newfstatat`/`stat`/`lstat`: `lx_statx` resolves
  the dirfd+path the same way `newfstatat` does and marshals a 256-byte
  `struct statx`. `stx_mask` reports exactly the fields the kernel keeps —
  type/mode, nlink, uid/gid, inode, size, blocks, block size, mtime and ctime
  — and omits `STATX_ATIME` and `STATX_BTIME`, which `struct cosmo_stat` does
  not record, so a caller is told they are absent rather than handed a value
  the kernel never kept. `lxtest` checks the fields against `fstat` on the
  same file. (PR #283)
- **`eventfd(2)` for the Linux personality.** A `uint64` counter and a wait
  queue as a kobject on the readiness template (`kernel/io/eventfd.c`, the
  timer object is the model): a `write` adds its 8-byte value and wakes
  readers, a `read` drains the whole count to zero — or returns 1 and
  decrements in semaphore mode — and wakes writers, with the all-ones write
  rejected and an overflowing write blocked, as on Linux. Carrying
  `read`/`write`/`ready`/`poll_wq`, it serves `poll`/`select` and blocking
  read/write; readiness and reads also ride the I/O ring, though a ring write
  of an all-or-nothing counter is not spin-safe and no eventfd door onto the
  native ring exists today. `eventfd2` is wired on both arches and the
  older `eventfd` on x86-64; `EFD_SEMAPHORE`/`EFD_NONBLOCK` are honoured and
  `EFD_CLOEXEC` is a no-op under the spawn model. (PR #285)
- **`timerfd(2)` for the Linux personality.** A waitable timer fd, built by
  giving the aio-timer unit's timer kobject (`kernel/io/timerobj.c`) the
  control surface it lacked: a disarmed create, `timer_obj_settime`
  (cancel-reset-rearm, reporting the old setting), and `timer_obj_gettime`
  (remaining). `timerfd_create` takes `CLOCK_MONOTONIC`/`REALTIME`/`BOOTTIME`
  (BOOTTIME aliased to monotonic), `TFD_NONBLOCK`/`TFD_CLOEXEC`, and a
  read-only fd; `timerfd_settime` disarms only on a zero `it_value` and fires a
  past `TFD_TIMER_ABSTIME` deadline at once; `timerfd_gettime` reports the time
  remaining. The periodic re-arm, the expiration count and the resetting 8-byte
  read ride the existing timer object, which already answers `poll`/`select`.
  (PR #287)
- **`memfd_create(2)` for the Linux personality.** An anonymous memory-backed
  file: `ramfs_anon_reg` makes an unlinked `VNODE_REG` on the root ramfs mount
  (born unlinked — `VNODE_PINNED` cleared, `nlink` 0 — so the open file owns
  the only reference and the vnode and its pages are freed on the last close),
  which `memfd_create` installs read+write (`MFD_CLOEXEC` a no-op; sealing and
  hugepages rejected). `ftruncate` is wired alongside — `vfs_ftruncate` calls
  the vnode `truncate` op for any fd open for writing — so a program sizes the
  memfd and `mmap`s it; the file reads, writes and maps through the existing
  page cache. (PR #289)
- **`epoll` for the Linux personality.** The scalable readiness interface, as
  a kobject holding an interest set (`kernel/io/epoll.c`): `epoll_wait` arms a
  wait entry on every member's `poll_wq` and the set's own queue and reports
  the ready ones — the async I/O ring's multi-wait with a persistent, dynamic
  set in place of a per-call array, composing the existing readiness operations
  with no new mechanism. `epoll_create1`/`create`/`ctl`(ADD/MOD/DEL)/`wait`/
  `pwait` are wired (create and wait on x86-64; create1, ctl and pwait on
  both), level-triggered, with `EPOLLONESHOT`. A finite timeout wakes the
  waiter directly; `epoll_ctl` wakes the set so a concurrent waiter
  re-evaluates. Nesting an epoll and auto-remove-on-close are deferred
  (explicit `EPOLL_CTL_DEL`); `EPOLLET` was deferred here and landed in
  PR #301. (PR #291)
- **`sysinfo(2)` for the Linux personality.** The coarse machine snapshot, no
  longer a stub: `lx_sysinfo` fills `struct sysinfo` from the stats the kernel
  already keeps — `uptime` from the monotonic clock, `totalram`/`freeram` from
  the buddy allocator's page counts (`pmm_get_stats`, `mem_unit = 1`, bytes),
  and `procs` from the process count. Load average, swap and high memory are
  reported zero (the system has none). (PR #293)
- **`mremap(2)` for the Linux personality.** In-place resize of a whole
  anonymous mapping: `lx_mremap` validates the lengths (rejecting a size near
  `UINT64_MAX` before rounding) and calls `vm_user_remap`, which finds the
  region, checks it is a whole anonymous mapping and grows or shrinks it — all
  under one hold of the space lock, so no concurrent `munmap`/`MAP_FIXED` can
  race the resize. It extends or trims the region record itself: a grow keeps
  the region's flags (guard pages, name) and never leaves two regions, and a
  grow with no room after it is `-ENOMEM`. `MREMAP_MAYMOVE` is accepted but
  never relocates (so `realloc` falls back on its own), and
  `MREMAP_FIXED`/`MREMAP_DONTUNMAP`, file/physical mappings, and sub-range
  resizes are `-EINVAL`. (PR #295)
- **System V shared memory for the Linux personality.** `shmget`/`shmat`/
  `shmdt`/`shmctl`, backed by the memfd mechanism: a segment is an anonymous
  ramfs file (`kernel/ipc/shm.c` holds the key/id registry), and `shmat` maps
  it `MAP_SHARED` — at the kernel's choice, or at an exact page-aligned address
  (`-ENOMEM` if occupied, no silent relocation), read-only under `SHM_RDONLY`.
  The segment record is reference-counted (the registry holds one reference,
  each live attach one), so `IPC_RMID` unlinks the id and drops the registry
  reference while live attaches keep the pages until the last `shmdt` — SysV's
  "destroyed on the last detach", straight from the refcount. `shmctl`
  `IPC_STAT` reports the size and attach count; `IPC_SET`, `SHM_RND`,
  `SHM_REMAP`, `SHM_HUGETLB`, resize and IPC namespaces are deferred. (PR #297)
- **`signalfd` for the Linux personality.** `signalfd`/`signalfd4` make a
  descriptor that reads blocked signals as `signalfd_siginfo` records, so a
  program can fold `SIGCHLD`/`SIGTERM` into a `poll`/`epoll` loop — the fourth
  fd-based event source after eventfd, timerfd and epoll. It is a readiness
  kobject (`kernel/io/signalfd.c`) holding just a mask, reading the current
  process's pending signals and polling a per-process `signalfd_wqh` the signal
  path wakes; the object has no process pointer, so a passed or inherited fd
  reports the reader's signals. The signal core gained a consume-by-mask
  dequeue and now keeps a *blocked* ignored signal pending (so a `signalfd` for
  `SIGCHLD`, whose default is ignore, sees child exits). `SIGKILL`/`SIGSTOP`
  are never reportable; non-RT coalescing (one record per signal number). (PR #299)
- **Edge-triggered epoll (`EPOLLET`).** The epoll object now honours `EPOLLET`
  instead of refusing it: an edge member is reported only on a transition into
  readiness, not on every wait while it stays ready. Each registration carries
  an `armed` flag beside the one-shot `disabled`; `collect` reports an edge
  member only while armed and disarms it on report, re-arming when the member is
  drained or on a real (non-timeout) wake, and the wait's sleep decision gates
  on `armed` so a disarmed-but-readable member sleeps rather than spinning.
  Level-triggered behaviour is unchanged; an undelivered edge is re-armed, not
  lost. (PR #301)
- **Lockdep hardening: bounded diagnostics and module-safe class names.**
  The start of the lock-discipline milestone
  (`docs/audit/2026-10-03-lock-discipline-audit.md`). A long dependency
  cycle could overread the diagnostic path buffer, and lock-class names
  pointed into module memory that unload could reclaim. Returned paths are
  now bounded, class names are copied into kernel-owned storage with an
  identity that survives a module reload, the shared class-cache, IRQ-usage
  and graph accesses are atomic, panic enters console mode before reporting,
  and `LOCKDEP=0/1` overrides the build default. ASan reproduced both the
  overread and the use-after-free before the fix; 416 self-tests on both
  architectures. Report: `docs/audit/2026-10-03-lockdep-report.md`.
  (PR #302)
- **Lockdep validates IRQ dependencies, restoration and timer
  cancellation.** Transitive IRQ-safe to IRQ-unsafe dependencies are
  caught whichever of the usage label or the edge arrives last, with IRQ
  trylocks classified apart from successful IRQ-enabled thread trylocks.
  Spinlock irqrestore is checked against the state its irqsave saved, and
  cancelling a timer while holding a lock its active callback needs is
  reported, from a bounded 64-slot table of locks learned from observed
  callbacks (unobserved paths and other callback waits remain open). The
  cwd and clock tests gained UP-safe controls without weakening their
  assertions; 416 self-tests, UP and SMP, on both architectures. Plan:
  `docs/audit/2026-10-03-lockdep-context-plan.md`. (PR #303)
- **Consistent lockdep diagnostics and concurrent-reader validation.**
  Normal graph dumps print from a private consistent copy, remote CPU
  held stacks are read through bounded atomic snapshots that never wait on
  the target, and panic reports the actual thread and IRQ/preemption
  nesting instead of always naming boot. A host model drives the real
  graph and snapshot helpers under TSan (`host-test-lockdep-tsan`), real
  x86 NMIs check that a snapshot is stable under the graph lock and refused
  while the interrupted writer is busy, and a matched `LOCKDEP=0/1`
  benchmark measures warmed spin paths. Review also cut the AArch64
  initial user-stack builder frame from 5,328 to 192 bytes. 417
  self-tests. (PR #304)
- **Serialised interrupt writers and consistent thread snapshots.**
  Registration and removal of an interrupt vector take a raw per-vector
  lock, so concurrent writers can no longer race the publication record
  while dispatch stays lock-free. Lockdep counter snapshots are taken
  under the graph lock and thread mutex stacks publish through a
  single-writer sequence, so panic diagnostics print a consistent stack or
  say it is unavailable. Host tests race the real interrupt source under
  TSan, a kernel test races writers against a real IPI, and graph searches
  are measured through all 1,280 class/subclass nodes. The lockup sampler's
  response/deadline ordering race is fixed. 419 self-tests on 1 and 4
  CPUs. (PR #305)
- **Unhandled-interrupt totals and lockdep search bounds.** Both
  architectures' unhandled-interrupt warning totals are now atomic, and
  `irq-unhandled` sends repeated IPIs to unregistered vectors on every CPU
  before checking handled reuse. Host tests cover invalid dispatch,
  oversized initialisation and failed synchronous removal. Host-only
  counters bound the graph search's queue, bitmap and reconstruction work
  at full capacity (compiled out of the kernel), and `lockdep-first-bench`
  times first public acquisitions against reuse. 421 self-tests with
  lockdep on and off. (PR #306)
- **Lockdep failure paths that cannot hang.** Re-entry into the
  validator's graph raw lock by the CPU that owns it now fails stop with a
  named diagnostic instead of spinning, exercised by direct probes and
  real x86 NMIs. Fatal output skips the log ring and the VirtIO console's
  tracked locks, keeping serial and framebuffer. The contention test
  drains its lock, timer and worker before reporting a missed callback,
  and `lockdep-mutex-bench` times verified queued mutex acquisitions
  after a controlled 1 ms hold. CI exposed three test assumptions, fixed
  without weakening them: `net-nat` waits for its flood outcomes before
  aging, `irq-route` waits for five counted deliveries, and the NIC
  benchmark and chaos boots get measured budgets. 422 self-tests.
  Probes: `tools/lockdep-contention-probe.py`,
  `tools/lockdep-reentry-probe.py`. (PR #307)
- **Observed spin contention and acquisition handoff.** The contention
  self-test no longer relies on a 5 ms callback landing inside a 20 ms
  hold: a self-test-only per-CPU observer publishes actual failed spin
  exchanges, and a nested IRQ wait must restore the outer observation.
  `lockdep-spin-bench` measures plain and irqsave cross-CPU handoff after
  an observed wait and a controlled 1 ms hold. The first CI boot found a
  startup dependency -- an IRQ-masked owner waiting for a runnable waiter
  whose CPU the reaper held, waiting on that owner's TLB acknowledgment --
  fixed by having both participants rendezvous with IRQs enabled, with no
  budget changed. 423 self-tests on both architectures; no two-CPU full
  suite pass is claimed. Report:
  `docs/audit/2026-10-04-spin-contention-report.md`; probe:
  `tools/spin-contention-probe.py`. (PR #308)
- **Two-CPU validation.** Unchanged `main` failed every two-CPU boot and,
  for `sched-spread`, every three-CPU boot; all three failures were test
  defects, with no kernel change. `virtio-remove-inflight`'s `irq-order`
  pass put its nonpreemptible holder and remover on one CPU, and then a
  submitter preempted inside `blk_submit` behind the holder kept
  `blk_unregister`'s drain waiting. The removal now runs on the test thread
  with the submitter parked, and five stamps prove the overlap.
  `sched-spread`'s bound predated S29, under which the running creator's
  CPU never ties; two CPUs skip with that reason. `sched-balance-pair`'s
  2 ms poll expired on every tick before `balance_tick` on the only
  receiving CPU; it now blocks on a move signal. Full suite at one to
  four CPUs on both architectures. Report:
  `docs/audit/2026-10-05-two-cpu-validation-report.md`; probe:
  `tools/two-cpu-probe.py`; runner: `tools/two-cpu-matrix.sh`.
- **Two-CPU CI boot.** `make test-smp2` boots the debug suite with
  `QEMU_SMP=2` into its own `boot-test-smp2.log`, and CI runs it on both
  architectures beside the four-CPU boots, so a self-test that needs a
  third CPU without saying so fails in CI rather than only on a local
  two-CPU run.
- **Quiescence memory ordering.** Six litmus tests check the epoch
  protocol against the C11 model with herd7 (`make litmus`): the grace
  period race-free, unlink visibility, the release sequence through two
  waiters, onlining and the grace-period wake, each with reachability
  witnesses and negative controls that weaken one order and must flip the
  verdict. The epoch-core host model runs under TSan
  (`make host-test-quiesce-tsan`) with two new models and two
  negative builds that must report a race. Both are in CI, the litmus
  tests in a `debian:sid` job where herdtools7 is packaged. The model found
  a real hole: a grace period read the online CPUs before W1 while a CPU
  coming online published before marking itself online, so a reader on
  the new CPU could keep an unlinked object past its free (allowed under
  RC11; seen 5 times in 2,000,000 on an M1). Two `seq_cst` fences, W1b and
  Q0, close it. Report: `docs/audit/2026-10-05-quiesce-memory-order-report.md`.
- **Lockdep callback classes.** Each timer callback function is a lockdep
  pseudo-class (`LOCKDEP_KIND_CALLBACK`): a callback holds it while it
  runs, and every `timer_cancel_sync` acquires it without holding it, so the
  ordinary cycle check reports a wait that could deadlock against any
  observed callback of the function -- on another timer, through a chain,
  or with the callback arriving after the wait -- where the per-object
  profile saw only the execution in progress. No existing code reported.
  The class table grew from 320 to 384 (a debug boot had reached 307
  before; callback classes add 17). `lockdep-callback` with
  `tools/lockdep-callback-probe.py`'s two negative controls; 424 self-tests.
  Report: `docs/audit/2026-10-05-lockdep-callback-classes-report.md`.
- **Lockdep raw interrupt-state pairing.** With `LOCKDEP=1`,
  `arch_irq_save`/`arch_irq_restore` wrap the architectures'
  `_hw` operations and keep each context's outstanding saves (the
  thread's, or the CPU's before it has one). A restore is reported if
  nothing is outstanding, if it is out of order, or if interrupts were
  enabled inside the region, and a thread may not exit with a save
  outstanding. The tree had no violation. The first version ran its checks
  inline and so stopped tail-calling the hardware restore. `schedule()`'s
  preempt-at-restore recursion is bounded only by tail calls, so a one-CPU
  boot double faulted on the idle thread's stack. The checks are now out of
  line, and `check-kernel-elf.sh` fails a kernel whose restore chain is not
  tail calls. `lockdep-irq-pairing` with four negative controls;
  425 self-tests. Report: `docs/audit/2026-10-06-lockdep-irq-pairing-report.md`.
- **Scheduler restore loop.** `schedule()` ended in `arch_irq_restore`,
  whose preemption point entered `schedule()` again when a reschedule was
  pending, bounded only by every link compiling to a tail call. Now the
  pass is `schedule_pass()` and `schedule_internal()` loops while
  `preempt_point_due(s)` -- the same predicate, asked with interrupts off
  before the restore -- then restores with `arch_irq_restore_nopoint`. S31:
  `schedule()` never re-enters itself on one thread's stack, asserted in
  debug builds; `check-kernel-elf.sh` checks the structure, and its four
  per-link tail-call checks are removed with reasons. Baseline: one-CPU
  x86-64 idle chains of 47–72 reschedules; a 192-byte forced call put the
  idle thread 13.8 KB deep on its 16 KB stack. `sched-restore-loop` (3,200
  trips) with `tools/sched-restore-loop-probe.py`'s three controls; 426
  self-tests. Report: `docs/audit/2026-10-06-sched-restore-loop-report.md`.
