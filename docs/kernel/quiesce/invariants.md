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
