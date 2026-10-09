# Device lifecycle Unit 2: fault and interleaving sweep

## Scope and baseline

Started 2026-10-09 from main `2661385c14c388190d8becfe95388f80c23995d3`
(PR #337 merge, main CI run `37885005992` green on x86-64, AArch64 and litmus),
on branch `device-lifecycle-fault-sweep`. The targets are the Unit 1 audit's
NVMe late publication, AHCI probe rollback ordering, virtio-rng refill,
acknowledgement failures in NVMe/AHCI/xHCI/e1000e, and callback retirement
in xHCI and the network worker. Every finding will distinguish a proven
failure from an unreachable or unresolved suspicion.

## Evidence and dispositions

| Target | Verdict | Evidence | Action |
|---|---|---|---|
| NVMe submit across controller death | Proven | `selftest_submit_die_window` calls `controller_die` after the first dead check and before the queue lock. `tools/nvme-die-window-probe.py --old` on x86-64 and AArch64 reported `accepted=1 done=0 inflight=1`; each failed only the required proof marker, while all 451 self-tests and the 100-round network harness passed. With the fix, both architectures reported `accepted=0 done=0 inflight=0` and booted cleanly. | Added an acquire dead recheck under `q->lock`; rejects with `-EIO` and unmaps every segment before returning. |
| virtio-rng completion after reset | Proven | `vrng_completed` runs after the synthetic reset boundary and reaches `vrng_post`. `tools/virtio-rng-repost-probe.py --old` on both architectures reported `posts_after_reset=1`; the sole harness failure was the required proof marker, with 451 self-tests and the network harness passing. Fixed x86-64 and AArch64 boots reported zero posts after reset. | `vrng_post` checks `stopping` while holding the lock that serializes `virtq_add` and `virtq_kick`; remove closes this gate before reset. |
| AHCI probe rollback before IRQ synchronization | Refuted | The test fails probe after all ports start, issues COMRESET to latch status (`pending=0x3f`), and observes `GHC.IE=0`, `irqs=0`, no disk and no active command on x86-64 and AArch64. Both 452-test boots passed. | No ordering change: every reachable `fail_ports` path is before worker creation, `GHC.IE`, disk probing and bio publication; the pending event cannot dispatch the handler. |
| AHCI port-stop acknowledgement on remove | Proven | `tools/ahci-stop-ack-probe.py --old` on x86-64 and AArch64 reports `persistent_hits=12 dma_frees=12 rebound=1`; each old run has exactly the required `ahci-stop-ack` failure, 100/100 network service, and no other self-test failure. Fixed boots on both architectures pass all 453 tests; their marker reports `retry_hits=1 retry_frees=12 persistent_hits=12 dma_frees=0 rebound=1`. | Retry each port stop once. If the second acknowledgement also fails, remove masks and synchronizes IRQs, unregisters the disk, and retains controller, port DMA, outstanding mappings, and bios. |
| NVMe disable acknowledgement on removal | Proven | Fixed `tools/nvme-disable-ack-probe.py` boots pass all 454 tests on x86-64 and AArch64 with `NVME-DISABLE-ACK: PASS hits=1 queue_dma_bio_retained=1`; the shell snapshot passes and the network harness serves 100/100 rounds on both. Paired `--old` worktrees report `queue_dma_bio_retained=0` and fail exactly this one of 454 self-tests on both architectures; their shell snapshots pass and their network harnesses still serve 100/100 rounds. | If `CSTS.RDY` does not clear, mark the controller dead and refuse new I/O. After vector synchronization, retain queue DMA, PRP pages, active mappings and bios; the probe rollback path also retains its allocations when disable is unacknowledged (by review: no test drives a probe failure after the controller is enabled). |
| Network active-worker removal | Refuted | `netif-remove-worker` reported `entered=1 barrier=1 waited=1 release=1` on x86-64 and AArch64. In both 455-test boots unregister reached its barrier after grace-period synchronization and queue purge while the dequeued input worker remained parked, and returned only after release. The shell snapshots and network harnesses passed, with 100/100 rounds on both architectures. | Keep the existing per-worker barrier after purge; the deterministic test closes the test-coverage gap. |
| AHCI COMRESET acknowledgement (restart and recovery) | Proven | `tools/ahci-comreset-ack-probe.py --old` on both architectures reported `recovery_dead=0 recovery_failed=0 recovery_rejected=0` (the reissued bio was accepted after a failed COMRESET); fixed boots on both report `recovery_dead=1 recovery_failed=1 recovery_rejected=-19 restart_dead=1 restart_rejected=-19`, 456 tests passing. | A COMRESET whose status never acknowledges fails closed: the port is marked dead and the reissued bio fails with `-ENODEV`. |
| e1000e RX/TX disable acknowledgement | Proven | `tools/e1000e-stop-ack-probe.py --old` (x86-64, AArch64) reports `ring_frees=1 retained=0` for both kinds; fixed boots report `hits=2 ring_frees=0 retained=1 recovered=1 rebound=1` for RX and TX. | Each engine's disable is read back and retried once; on persistent failure the IRQ is retired but rings, mapped buffers, mbufs and the private state are kept (invariant E7). |
| xHCI Stop Endpoint / Disable Slot acknowledgement on disable | Proven | `tools/xhci-disable-ack-probe.py --old` reports `stop=0/1 slot=0/1` on both architectures (the fixture was freed after the refused command); fixed boots report `stop=1/1 hits=2 slot=1/1 hits=2`. Re-run after the cancel changes (probe re-anchored, `935ece18`): old and fixed pass on both architectures. | Retry once; on persistent failure detach the slot in software and keep the DCBAA context, rings and memory (U12). |
| xHCI halt acknowledgement on remove | Proven | `tools/xhci-halt-ack-probe.py` at `935ece18`: fixed `XHCI-HALT-ACK-SWEEP: PASS retained=1 hits=2 recovered=1` on x86-64 and AArch64; `--old` `FAIL retained=0 hits=1` on both, and (since the old helper reports every halt acknowledged) the cancel's quarantine case fails with it. | Retry the halt once; without HCH keep the BAR, every controller allocation and the driver data pointer, so a later probe cannot rebind (U13). |
| xHCI cancel when no stop is acknowledged | Proven | `xhci-cancel-ack` drives the four escalation outcomes on a synthetic controller. `tools/xhci-cancel-ack-probe.py --old` (whole fix commit reversed) on x86-64 and AArch64: `XHCI-CANCEL-ACK-SWEEP: FAIL stop=1 slot=0 halt=0 quarantine=0` -- with a refused stop the request came back `done=1 slot_at_done=1 hch_at_done=0`, and with every acknowledgement refused it came back too (`cancel=0`); `USB-SYNC-QUARANTINE: FAIL caller_buf_handed=1`; exactly these 2 of 462 failed. Fixed on both: `slot_at_done=0` after Disable Slot, `hch_at_done=1` after the halt, `cancel=-5 done=0 in_flight=1` when nothing acknowledges, `caller_buf_handed=0`. | Escalate Stop Endpoint -> Disable Slot -> halt; complete only after an acknowledgement; otherwise mark the controller dead, quarantine the TDs (mapped, ignored by events and flushes, counted) and return `-EIO`. Synchronous transfers bounce through an HCD-owned heap block; class drivers keep request memory and bios on `-EIO` (U10, U14). |
| xHCI cancel racing a running callback | Refuted | `xhci-cancel-retired` (two CPUs) cancels while a retired request's callback spins 20 ms on another CPU: `rc=-2 exited_at_return=1 cb_cpu=1 canceller_cpu=0` on x86-64 and AArch64 (`QEMU_SMP=2`). `tools/xhci-cancel-retired-probe.py --old` removes `xhci_gone`'s two waits: `FAIL rc=-2 exited_at_return=0` on both, the only failure in each boot. | No change: `xhci_gone` waits for the interrupt handler and the irqpoll worker before answering `-ENOENT`. The test closes the coverage gap. |

## The xHCI cancel contract

The rule, written as U14 in `docs/drivers/usb/invariants.md` and applying
to every driver: no DMA memory is freed or returned while a device that
has not acknowledged a stop may own it. For a cancel the acknowledging
steps are, in order, Stop Endpoint with Set TR Dequeue, Disable Slot, and
HCH after a halt; the request completes, with an error, only after one
of them. With none the controller is dead and the request is
quarantined. The quarantine changes `usb_cancel`'s public contract: `-EIO`
means the caller may never free or reuse the request or what it names
(U10). The decision to pay for this with a bounce in `usb_sync_msg` (so
the twelve synchronous call sites keep owning their buffers) and to leak
request memory and bios in usb-storage, usb-hid and the hub was taken
with the maintainer on 2026-10-09. No module ABI number changed: the
operation table and structure layouts seen by other modules are the
same; `usb_note_quarantine` is a new export.

While making the hub keep its request on `-EIO`, a pre-existing ordering
gap in `hub_remove` was closed rather than relied on: `stopping` is set
before the status request is cancelled, so a hub worker that was already
awake could exit and free the hub while `usb_cancel(&h->req)` was still
running. The worker now waits for `remove` to release the request. This
was found by review and is not separately reproduced.

## net-nicbench's eth1 failure

The x86-64 failure that paused validation (`the gateway's ARP entry is
still incomplete after 1502 ms (+0 requests sent since the warm-up began,
8 pending dropped)`) is attributed and is not a kernel defect.

- Address reuse was real and harmless. The rebound eth1 is allocated at
  the old interface's address, but unregister's flush runs after `NETIF_UP`
  is cleared under the interface lock, every entry creator checks that
  flag under the table lock, and no entry naming eth1 existed at the start
  of the benchmark (N25, "Address reuse").
- The rebind removes eth1's cached gateway entry (correctly). In 56 of 56
  local boots without the rebind the entry was reachable at the warm-up;
  in 8 of 8 with it the benchmark had to resolve it.
- 15 instrumented x86-64 boots named the trigger (boot 12): during eth1's
  ARP phase a 40-byte TCP segment from 10.77.8.1:2222 to 10.77.8.99:40001,
  sent by the network worker for a connection `net-hostinput` leaves
  behind, routed by default to eth1 and resolved the gateway there. The
  benchmark's receive hook took the gateway's reply as one of its own
  (2,001 replies arrived, 2,000 counted). The entry stayed incomplete
  until the age retry, 1-2 s later, which can fall past the warm-up's
  1.5 s bound.
- Fix (test): `nicbench_resolve_gateway` resolves each interface's gateway
  before the hook is installed, bounded over all three ARP requests. No
  budget changed. Evidence and the shape's history: `docs/testing/flakes.md`.

## Out-of-scope validation finding

The first AArch64 old-behavior NVMe proof run also failed the existing
`signal-stop` self-test at `userland/init/init.c` check 9. The serial log
shows the stopped child exited with status 7 before its parent returned
status 9. Check 9 expects `waitpid(..., WCONTINUED)` after sending
`SIGCONT`; the child returns immediately, and
`kernel/process/process.c:child_event_locked` gives `EXITED` priority over
`CONTINUED`. This explains how the continued event can be hidden. A repeat
old-behavior run and the fixed AArch64 boot passed `signal-stop`. The first
run is recorded in `docs/testing/flakes.md` and the deferred-work
inventory; it remains an out-of-scope finding and is not changed in this
unit.

The first fixed x86-64 AHCI stop-ack boot also failed `module-unload-busy`
at `kernel/module/modtest.c:720` (`waited >= 50000000ULL`), followed by
four module-zombie checks. The unload deadline uses quantized global ticks
when `clock_is_common()` is false, while the assertion measures elapsed
time from a per-CPU clock. The mismatch explains how 50 ticks can expire
before 50 ms on the assertion's clock. This is recorded in
`docs/testing/flakes.md` and the deferred-work inventory; it is outside
Unit 2 and remains unchanged.

`net-hostinput` leaves a server-side TCP connection (10.77.8.1:2222 to
10.77.8.99:40001) that keeps transmitting after the test has destroyed
its `hinu` tap; on 2026-10-09 one of its segments, routed to eth1, was
the trigger of the nicbench failure above. The test does not reset the
connection it builds, and the runner's network census does not see it.
Recorded in the deferred-work inventory; not changed in this unit.

## Branch-introduced defect found by the analyzer gate

The first matrix run's `make analyze` (after PR #337's gate) reported
five dead stores to `rc` in `nvme_probe`: the Unit 2 rollback stored
`controller_disable`'s result in `rc`, so an acknowledged disable made a
failed probe return 0 after freeing the controller. The disable's result
now only decides whether DMA may be freed (`e613edee`). It never reached
main. The same run found a stale baseline entry (`nvme_submit`, Apple
clang; the dead recheck rewrote that path) and two dead stores in
`netif-remove-worker`'s teardown (`5b5f49ed`).

## Validation

All at `3515d50c`, local, one QEMU at a time at default priority
(`out/unit2-matrix/verdicts.txt`, every serial log beside it):

| Item | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | PASS | PASS |
| `make fuzz` | PASS | PASS |
| `make analyze` | PASS (7 baselined, 0 unexpected, 0 stale) | PASS (7 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS 141.2 s, harness 100/100 | PASS 133.0 s, harness 100/100 |
| debug `make test`, `QEMU_SMP=2` | PASS 153.9 s, harness 100/100 | PASS 142.3 s, harness 100/100 |
| debug `make test`, `QEMU_SMP=4` | PASS 153.2 s, harness 100/100 | PASS 156.3 s, harness 100/100 |
| `make test-smp2` | PASS | PASS |
| `make test-chaos` | PASS | PASS |
| `make test-harness-retry` | PASS | PASS |
| `make BUILD=release test` | PASS | PASS |

Release symbols (`llvm-nm`): neither release `xhci.ko` has an `xhci_test_*`
or `g_cancel_test*` symbol (the debug module has seven) or the
`__ksym_usb_request_complete` export; the release kernels have no cancel
or sync-quarantine helper. Probes at `935ece18` or later, both
architectures, old and fixed: `xhci-cancel-ack`, `xhci-cancel-retired`
(`QEMU_SMP=2`), `xhci-halt-ack`, `xhci-disable-ack` all PASS. The first
fixed x86-64 `xhci-cancel-ack` run also hit `module-unload-busy` (the
recorded clock mismatch, `docs/testing/flakes.md`) and passed on rerun;
the earlier probes (NVMe, AHCI, virtio-rng, e1000e) ran at their own
commits on this branch. Of the code changed since, only `nvme_probe`'s
rollback (`e613edee`) is in their drivers, and neither NVMe probe
reaches it (they drive the submit window, `controller_die` and
`nvme_remove_queues`); that fix rests on review and the analyzer, and a
probe-failure injection after enable is recorded as a gap.
Branch CI and merge CI: in the PR.
