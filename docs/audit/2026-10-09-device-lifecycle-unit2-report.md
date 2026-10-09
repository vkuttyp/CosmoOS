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
| NVMe disable acknowledgement on removal | Proven | Fixed `tools/nvme-disable-ack-probe.py` boots pass all 454 tests on x86-64 and AArch64 with `NVME-DISABLE-ACK: PASS hits=1 queue_dma_bio_retained=1`; the shell snapshot passes and the network harness serves 100/100 rounds on both. Paired `--old` worktrees report `queue_dma_bio_retained=0` and fail exactly this one of 454 self-tests on both architectures; their shell snapshots pass and their network harnesses still serve 100/100 rounds. | If `CSTS.RDY` does not clear, mark the controller dead and refuse new I/O. After vector synchronization, retain queue DMA, PRP pages, active mappings and bios; the probe rollback path also retains its allocations when disable is unacknowledged. |
| Network active-worker removal | Refuted | `netif-remove-worker` reported `entered=1 barrier=1 waited=1 release=1` on x86-64 and AArch64. In both 455-test boots unregister reached its barrier after grace-period synchronization and queue purge while the dequeued input worker remained parked, and returned only after release. The shell snapshots and network harnesses passed, with 100/100 rounds on both architectures. | Keep the existing per-worker barrier after purge; the deterministic test closes the test-coverage gap. |

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
