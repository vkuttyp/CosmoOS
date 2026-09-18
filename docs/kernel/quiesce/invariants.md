# Kernel object lifetime and quiescence: invariants

Rules the quiescence subsystem and every converted object keep. Each has
a **Check** (what verifies it today) and, where honest, a **Gap**.
Changing a rule means changing this file and the code together.

**Q1. No object is freed while a CPU may still hold a reference obtained
in a read-side section.** The protocol is unlink → grace period → free,
where the grace period is `synchronize_quiesce`, `call_quiesce`,
`synchronize_irq`/`interrupt_unregister_sync` (handlers) or
`timer_cancel_sync` (callbacks). Check: `quiesce-grace` (a reader inside
`quiesce_read_lock` on another CPU for 30 ms holds the grace period, the
object is poisoned only after the section closed), `quiesce-stress`
(readers on every other CPU against a churning pointer, magic checked on
every read, ~200 reclaim generations), host `test_quiesce` (ASan reports
any use-after-free of the reclaimed object under real threads).

**Q2. A CPU reports quiescence only at the five points in `design.md`, and
the interrupt-return point requires `irq_depth == 0`, `preempt_count == 0`
and interrupts enabled in the interrupted frame.** A tick that lands inside
a spinlock or a `quiesce_read_lock` section records nothing. Check:
`quiesce-grace` runs its reader with preemption disabled through ~7 ticks
and the grace period lasts the whole hold (≥ 20 ms asserted); review of
`x86_trap_dispatch`, aarch64 `handle_irq`, `schedule_internal`, `idle_main`,
`sched_start_cpu`. Gap: no test forces a nested interrupt on the paranoid
path; that path never records by construction.

**Q3. Every barrier is documented and no weaker.** W1 `seq_cst` RMW; W2
acquire loads; Q1 acquire load; Q2 release store; the `>=` comparison
tolerates concurrent waiters. Check: `design.md` "The epoch algorithm and
its memory ordering" is the normative text; host `test_quiesce`
(`epoch-math`) exercises the arithmetic including two waiters and the
highest CPU slot; the threaded test runs under ASan/UBSan. Gap: no TSan
run (Apple clang lacks it for this target); no litmus-level model check.

**Q4. `synchronize_quiesce` is never called from interrupt context or
with a spinlock held.** Asserted on entry (`irq_depth`, `preempt_count`).
`call_quiesce` is the form for those contexts. Check: `quiesce-call`
submits half its callbacks from a preempt-disabled, interrupts-off region.

**Q5. `call_quiesce` callbacks run once, in submission order, in thread
context, after a grace period that began after the submission; a head is
never on the list twice.** A second submission before the callback ran
panics (`pending`). Check: `quiesce-call` (eight callbacks, order and
context asserted, one grace period for the batch); the double submission
is a panic path, checked by review.

**Q6. A CPU that never takes an interrupt is not counted forever: a
straggler is kicked.** After two ticks the waiter sends `IPI_RESCHEDULE`
to pending CPUs; a halted CPU publishes at that interrupt's return.
Check: review; `quiesce_stats.straggler_ipis` is observable. Gap: no test
drives a CPU into a state that needs the kick (a preempt-disabled loop
across ticks publishes at its end anyway).

**Q7. An interrupt handler's `arg` is freed only after
`synchronize_irq`.** `interrupt_unregister` merely unpublishes; the
`_sync` variants and the IRQ layer's release paths wait. Dispatch loads
the `{fn, arg}` record once (acquire) so a handler never runs with
another registration's argument. Check: `irq-sync` (a handler spinning 20
ms on another CPU: `interrupt_unregister_sync` returns only after it
finished, ≥ 10 ms asserted; the argument is poisoned and freed only then).

**Q8. A timer's memory is freed only after `timer_cancel_sync`, which
also defeats a callback that re-arms.** `struct timer_queue.running`
names the executing callback; the sync form spins on it and re-cancels.
Check: `timer-cancel-sync` (a callback spinning 20 ms on another CPU is
outlasted; a self-re-arming callback stops for good: no fire in the
following 30 ms). TCP frees pcbs through it (`pcb_free_locked`).

**Q9. Every kobject type has a release that frees the object, and nothing
else frees it.** `device_register`, `blk_register` and `netif_register`
refuse (`-EINVAL`) an object without a release; static objects use the
`*_release_static` helpers. Lookups (`device_find`, `blk_find`,
`netif_find`, `netif_default`, `netif_loopback`) return referenced
pointers. Check: `device` (refused without a release), `blk-lifetime`,
`net-netif-lifetime` (release runs exactly once, after the last holder,
never before). Gap: the vfs, process, socket, pipe, vm and vcpu types
were already release-owning and are unchanged.

**Q9a. A refused registration leaves no kobject and no owner count.**
`device_register`, `blk_register` and `netif_register` record the owner
(and initialise the kobject) only after the registry accepted the object,
so a driver's failure path can free the storage directly. Check:
`net-netif-lifetime` (a duplicate `test0`: `-EEXIST`, type NULL, count 0,
owner NULL), `blk-lifetime` (the 27th `zy` device: `-ENOSPC`, likewise).

**Q10. A registry's reference and the creator's reference are distinct;
unregister drops the registry's, the creator drops its own.** `blkdev`
and `netif` gained the registry reference in this pass (previously
`blk_unregister` and `netif_unregister` dropped nothing and the driver
freed storage that `blk_find` holders could still name). Check:
`blk-lifetime`, `net-netif-lifetime` refcount assertions (2 after
register, 3 after find, 2 after unregister, release after both puts).

**Q11. After `blk_unregister` no bio reaches the driver, and no
`blk_submit` is inside the driver.** `gone` and `submitting` are
`seq_cst` on both sides (Dekker); `blk_submit` returns `-ENODEV`. Check:
`blk-lifetime`. Gap: the submit/unregister race itself is not driven by a
test (it needs two CPUs hitting a window of a few instructions).

**Q12. After `netif_unregister` no transmit or receive touches the
driver, no packet of the interface is queued or being input, and no ARP
or ND entry names it.** Steps: GONE flag → registry removal → grace
period (transmit and `netif_rx` are read-side sections) → receive-queue
purge → worker barrier → `arp_flush`/`nd_flush` → registry reference.
`ops->transmit` must therefore not sleep. Check: `net-netif-lifetime`
(transmit `-ENODEV`, `netif_rx` dropped, queue length unchanged); review
of `vnet_transmit` and `lo_transmit` (no sleeping call).

**Q13. A TCP child dequeued by `tcp_accept` is never without an owner.**
The socket is allocated first and attached under the TCP lock in the
same critical section as the dequeue. Check: `net-accept-race`
(`c->tcp->sock == c` on every accept against a peer that connects and
drops at once).

**Q14. A socket woken after a protocol lock is dropped is referenced
across the wake with `kobject_tryget`.** The socket's release clears
`pcb->sock` under the same lock but starts with a zero count, so a plain
get could panic. Check: review of `sock_ref` (TCP) and `udp_input`;
`net-lo-udp`/`net-lo-tcp` exercise the paths.

**Q15. A module is freed only after: GOING, `shutdown()`, one grace
period, and `live_objects == 0`.** Objects whose release code lives in
the module keep it mapped; after the timeout it becomes a zombie (name
reusable, memory kept, `-EBUSY`) that a later `module_unload` reaps.
`module_owner_of` raises the count inside its read-side section, so an
increment made under a section that saw the module is visible to the
unloader after its grace period. A zombie keeps its dependency pins until
it is freed, since its outstanding release code may call into them.
Check: `module-unload-busy` (including a zombie `cosmotest_dep` whose
release calls `cosmotest_answer()`: `cosmotest` cannot be unloaded until
the zombie is reaped).

**Q16. `struct kobject.owner` is set at `kobject_init` (from
`type->release`) or `kobject_track_code` (from the per-object callback),
and dropped after the release ran.** The release therefore runs with its
module still mapped. Check: `module-unload-busy` (`*released == 1` after
`kobject_put` on a zombie).

**Q17. The handle table hands out referenced objects under its lock and
drops references outside it.** Audited unchanged: `handle_lookup`/`handle_get`
take the reference under `t->lock`; `handle_close` puts after unlocking;
`handle_install*` take the reference before the lock and give it back on
failure. Check: review; `process-user` and the pipe tests exercise it.

**Q18. A grace period's waiter is woken when a CPU publishes, and never
waits past its own deadline to find out.** `synchronize_quiesce` blocks
on `g_gp_wq` with a `TICK_NS / 2` deadline instead of sleeping blind;
`quiesce_core_pending` returning zero is still the entire condition, and
the deadline is what makes the wake an optimisation rather than a
correctness dependency — a missed or spurious wake costs one re-check of
the condition the loop was going to re-check anyway. A defect in the wake
path is therefore a latency regression and cannot be a hang or a
premature return.

**The wake is not taken at every quiescent point**, and the difference is
not a detail. `quiesce_note_quiescent` is called from inside the
scheduler — `sched.c`'s AP bring-up publishes while holding a run-queue
lock with interrupts disabled — and waking from there reaches
`schedule_internal`, which asserts it is not called with a spinlock held;
the machine dies five seconds into boot. So the publish is unchanged and
`quiesce_note_quiescent_preemptible` is what publishes *and* wakes, from
the three places that hold nothing and can already schedule: both trap
returns and the idle loop.

The idle loop matters as much as the trap returns. An idle CPU is halted
in `arch_cpu_wait_for_interrupt`, so on an otherwise idle machine the
publish that completes a grace period comes from `idle_main` after an
interrupt, not from the interrupt's own return.

**Checked by** `quiesce-wake`, whose assertion is that the wake **fires**
— `quiesce_stats.gp_wakes`, wakes delivered to a queued waiter — and not
that a grace period was fast. That distinction cost three wrong tests
before it was got right, and each wrong one was a timing claim wearing a
counter's clothes:

- *"`gp_timeouts` did not move"* reads a machine-wide counter, so another
  thread's grace period fails it;
- *"ten grace periods cost fewer than ten deadline-ends"* looks like a
  counting argument and is not one. With the wake a grace period usually
  still reaches its first deadline — the other CPUs' tick is 4 ms away
  and the deadline is 2 ms — and is then woken; without the wake the
  timer's own wake re-checks the condition, finds it true, and counts no
  deadline-end at all. The number records where the ticks fell.

`gp_wakes` is identically zero unless the wake path runs. The duration is
reported in the same line and not asserted, because that is the part a
loaded host changes: 10 grace periods, 60 wakes delivered, 3.9 ms each.

**What this does not change**, measured rather than assumed: on a
four-CPU idle machine a grace period takes about 3.9 ms with the wake and
4.3–7.5 ms without it. The remaining 3.75 ms is not overhead this
rule removes — it is how long it takes the other CPUs to reach a
quiescent point, which on an idle machine means their next tick. The
wake removes the polling overshoot, roughly halving the latency and
collapsing its spread; it does not make a grace period cheap.

**Q19. A straggler kick is attributed only by the publish in its own trap
return, and the flag that carries that cannot outlive the trap.** The
kick (`quiesce.c`, after `2 * TICK_NS` and at most eight rounds) sends
`IPI_QUIESCE_KICK`, whose handler sets `pc->quiesce_kicked` and does
nothing else. Each architecture's interrupt tail reads that flag **and
clears it unconditionally** — before, and independently of, the
three-condition test that decides whether this CPU may publish — and
counts a `kick_publishes` only when the same return also published.

The unconditional clear is the invariant, not an optimisation. A kick
delivered while `preempt_count != 0` does not publish; if the flag
survived that trap, the next unrelated interrupt return or idle
iteration would clear it and count a publish the kick did not cause.
That would attribute falsely in **exactly** the case the kick is known
not to help, and would silently break `quiesce-kick-spinner`, which is
the test that gives the counter its meaning. A nested trap clearing the
flag without publishing under-counts, which is the safe direction.

**Why it is a kind of its own and not `IPI_RESCHEDULE`.** That kind's
contract is "target re-evaluates `need_resched` on interrupt return",
and its sender sets the flag under a run-queue lock. The kick sets no
flag and holds no lock; it wants the trap tail and nothing else. Sharing
the kind would leave the kick exposed to a send suppressed *because* the
target's `need_resched` is clear — the natural "nothing to reschedule
there" optimisation — which would disable every kick with no test to
notice. (A handler-side early return would not: the tail runs regardless
of what the handler did.)

**What the counters say, measured rather than argued.** `straggler_ipis`
counts kicks sent and `kick_publishes` counts kicks that worked; before
the second existed, no number in this tree would have changed if the
kick were replaced by a no-op. Over six boots, three per architecture:
**161 kicks sent, 7 publishes attributed — about four per cent.** The
kick is therefore kept, and `quiesce-kick-population` records why the
obvious candidate population is *not* the reason: a CPU whose tick keeps
landing inside a short read-side section publishes anyway, because
`schedule()` publishes at entry and the covered tick still sets
`need_resched`, so the `preempt_enable` ending that section publishes a
moment later. The publish is not confined to the trap return.
