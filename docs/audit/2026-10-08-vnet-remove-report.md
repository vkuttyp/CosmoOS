# Device lifecycle Unit 1: virtio-net removal

## Scope and baseline

Work started 2026-10-08 on main `561b913b` (PR #335). Branch:
`device-lifecycle-vnet-remove`. Contributor rules were added first in
`adecaa38`. Scope: prove and repair virtio-net removal, audit the named
drivers' remove/reset paths, and record unproven concerns. No independent
bind/unbind API or module ABI change is proposed.

PR: [#336](https://github.com/vkuttyp/CosmoOS/pull/336).
Branch CI results: [PR checks](https://github.com/vkuttyp/CosmoOS/pull/336/checks).
Merge-commit CI is the required follow-up when the PR is merged.

## Initial source findings

`vnet_remove` unregisters the interface, resets, pops used RX/TX entries,
then frees the queues. `virtq_free` releases/synchronizes the vector and
disables the poll only at that last step. A callback can therefore run
while the drain runs. `vnet_rx_done` can refill after reset. The drain
does not unmap either direction, does not decrement `rx_posted`, and
cannot recover descriptors not in the used ring: `virtq_free` discards
the cookie table without returning its cookies. Thus outstanding buffers
need driver-owned tracking independent of the ring, as virtio-blk's slots
already provide.

`netif_unregister` marks GONE before its grace period and worker barrier.
`rx_common` rejects GONE and frees the packet before counting or queuing
it. Calling `netif_rx` late is possible; delivery to protocol input after
unregister is prevented. In main's baseline, `rx_posted` is modified by the RX
poll alone (irq_poll serializes its calls); the old remove never accounts
its drain. The unlocked decrement alone does not prove concurrent writers.

## Ownership and ordering implemented in `dc23c93d`

Use private RX/TX buffer tables in `struct vnet`, protected by the driver's
IRQ-safe lock. Submission records ownership before exposing the cookie;
completion removes that ownership before releasing the mapping and mbuf.
After unregister has excluded/drained senders, mark stopping under the
same lock. Stop RX poll delivery before reset so no callback can repost;
reset stops DMA; free both queues (synchronous vector/poll teardown);
then reclaim all remaining table entries, including buffers with no used entry,
unmap every mapped segment and bring `rx_posted` to zero. Apply the same
cleanup to partially initialized probe failures.

## Deterministic reproduction (original removal logic)

The debug-only device peer uses real `virtq_alloc`, DMA, irq_poll and
`vnet_remove`. It parks one completed RX, 31 outstanding RX buffers and
a two-segment TX completion. `vnet-remove-pending` holds completion
delivery through teardown; `vnet-remove-late` delivers the held RX and TX
at the reset status write, before the acknowledgement and vector teardown.
This models a still-deliverable interrupt, without scheduling assumptions.
A private ledger records maps, unmaps and ownership returns, and cleans
up residual fixture buffers after recording a failure. The independent
ledger does not repair the production removal logic.

x86-64 baseline (`out/vnet-baseline-x86/boot-baseline.log`, four CPUs):

| Check | Evidence | Verdict |
|---|---|---|
| Pending buffers | 34 maps, 0 unmaps; 34 buffers, 3 freed; `rx_posted=32` | `mapped buffers not unmapped` (21 ms) |
| Late completion | 35 maps, 3 unmaps; 35 buffers, 3 freed; 2 late callbacks, 1 post after reset | `receive buffers reposted after reset` (8 ms) |
| Accepted RX after unregister | 0 in both cases | GONE rejection works |

An initial test-integration boot is excluded: the debug peer referenced
unexported `arch_irq_save`; the module was rejected, both new tests failed
at `run != NULL`, and the network harness failed without eth0. The peer
now enters its synthetic interrupt through exported spinlock and irq_poll
APIs. That initial run's harness verdict was FAIL after 214.3 s, with
`net-harness` 72,439 ms. No test budget or assertion was changed.

AArch64 (`out/vnet-baseline-arm/boot-baseline.log`, four CPUs) produced
the same counters: pending failed on missing unmaps (20 ms); late failed
on reposting after reset (19 ms). The harness failed in 152.8 s, with
only those two of 449 tests failing. x86-64's corrected baseline failed
in 145.5 s with the same two failures. Both network harnesses passed.

The fixed x86-64 boot (`out/vnet-fixed-x86/boot-fixed.log`) passed all
449 tests in 149.5 s. Both cases recorded 34 maps/unmaps, 34 buffers
returned, zero posted RX, zero callbacks/posts after reset and zero
accepted RX. The network harness passed in 1,033 ms.

AArch64's fixed four-CPU boot passed 449 tests in 158.6 s; its two
removal tests took 52/18 ms and recorded the same balanced counts, with
`net-harness` passing in 870 ms. Logs: `out/vnet-fixed-arm/boot-fixed.log`
and `out/vnet-fixed-arm-result.log`.

## Audit of other drivers

The verdict is a source audit of ownership/order, not a claim of fault
coverage or real-hardware DMA cessation. A live reset may keep the IRQ
and poll running when completion and recovery claim each record under
the same lock and retain the record/ring storage. Whole-device teardown
must synchronize both before that storage is freed. `dma_alloc` buffers
need `dma_free`, not a separate `dma_unmap`.

| Driver | Path | Actual order | Verdict and limit |
|---|---|---|---|
| virtio-blk | `vblk_remove`, `vblk_reinit` | Block unregister/reset gate drains submitters → device reset → `virtq_free` (vector sync, poll disable) → slot claims/unmaps under `vb->lock` → bio completion → pool free/rebuild | Correct ownership teardown; PR #334's removal test covers the retained slot table. |
| virtio-blk | `vblk_timeout`, probe rollback | Timeout: set dead → reset → locked slot claim/unmap → complete; queue/table remain. Rollback: reset → queue free → pool/private free | Correct locked completion claims; timeout concurrent submit guard needs a fault/interleaving sweep. |
| virtio-rng | `vrng_remove`, rollback | Reset → queue vector sync/poll disable → persistent entropy buffer `dma_free` → private free. Rollback has no successfully allocated queue at its failure sites | Correct IRQ/poll-before-free lifetime order; pre-reset refill rule is not met. A callback may repost its persistent buffer after reset; incorrect observable effect is unproven. |
| virtio-console | `vcon_remove`, rollback | Unregister/synchronize sink → reset → free polled queues → TX/RX DMA buffer free → private free; rollback omits unpublished sink | Correct; queues have no completion callback/vector. |
| NVMe | `nvme_remove`, probe rollback | Unregister namespaces → `controller_die`: disable/wait → locked command claim/unmap/complete → vector release/sync → `queue_free` poll disable → SQ/CQ/PRP/table free → MMIO/private free. Rollback releases IRQs → disable/wait → poll disable/free | Correct storage lifetime and mutually exclusive claims; disable acknowledgement failures are not injected. |
| NVMe | `nvme_timeout` / `controller_die` | Abort first; otherwise mark dead → disable/wait → locked command claim/unmap/complete; queues/polls retained | Claim serialization is correct; submit checks dead before mapping but does not recheck under the queue lock. Lost late submissions are a suspicion, not reproduced. |
| AHCI | `ahci_remove`, `disk_detach` | Stop/join worker → detach disk under port lock, stop engine, locked slot drain/unmap/complete, unregister block → mask controller/ports, stop engines → release/sync IRQ → command/FIS/table DMA free → MMIO/private free | Correct table lifetime/locked claims if stop acknowledgements succeed; no deferred poll. |
| AHCI | `port_restart`, `port_recover` | Set recovering under port lock → stop command engine → locked slot claim/unmap/complete or retain for reissue → optional COMRESET → restart → clear recovering | Correct mutual exclusion; ignored stop/reset failures require injection. |
| AHCI | `fail_ports` | Stop port → port DMA free → release/sync IRQ → MMIO/private free | Needs proof: atypical free-before-IRQ-sync order. Probe state may make it safe; no published active bio established in this audit. |
| e1000e | `e1000e_remove`, `fail_irq` | Unregister interface → cancel/join watchdog → mask IRQ and disable RX/TX → vector release/sync → RX/TX drain/unmap/free → ring DMA free → MMIO/netif put. Rollback starts at hardware quiesce | Correct ownership order; no deferred poll. Hardware stop fault coverage absent. |
| e1000e | `e1000e_watchdog` | Driver IRQ-safe lock → reclaim completed TX → disable TX → drain/unmap/free remaining TX → program ring → unlock/rearm timer | Correct locked claims and retained ring storage; disable semantics depend on hardware. |
| xHCI | `xhci_remove`, `fail_irq` | Join worker → unregister USB children → halt controller → vector release/sync → poll disable → DMA tables/rings free → MMIO/private free. Rollback releases IRQ/synchronizes/disables poll before tables | Correct storage order; halt acknowledgement failure is ignored. |
| xHCI | `xhci_cancel`, `xhci_reset_endpoint`, `xhci_disable_device` | Stop/reset endpoint and set dequeue (disable slot on disconnect) → locked unlink/TD claim/unmap → callback outside lock → free endpoint context/rings on disconnect. Already-retired cancel synchronizes IRQ and current poll | Correct claims on successful commands; failed stop/disable commands and an already-running retired callback need deterministic coverage. |
| USB storage | `usbs_remove`, `usbs_timeout`, BOT recovery | Block unregister (remove) / recovering gate (timeout) → locked detach bio from phase machine → HCD cancel (endpoint stop/drain/unmap + callback synchronization) → reset/clear halts (timeout) → clear current request → bio complete → final blkdev release frees CBW/CSW/private | Correct layer ordering on successful cancellation; HCD command failure propagation and reset failure require injection. No class-owned IRQ/poll. |

No additional defect was reproduced or fixed. The concerns above are
deferred to device-lifecycle fault/interleaving coverage; an atypical
source order is insufficient to claim a use-after-free.

PR review identified a stale network lifetime coverage note, now updated
to name both synthetic removal checks while retaining the module-unload
and active-worker gaps. Qodo also flagged the cost of scanning up to 256 TX
ownership slots under the IRQ-safe driver lock. The review revision uses
a private free list and passes each ownership record as the queue cookie.
Submission, completion and failed-publication rollback are constant-time;
full TX table walks occur only at initialization and teardown. No transport API or mbuf layout
changes. This removes the scans by construction; no measured throughput
improvement is claimed.

Both removal cases first fill the synthetic TX queue twice. The first
round exhausts ownership records; the second uses a two-segment chain to
exhaust descriptors while one record remains free, exercising failed
`virtq_add` rollback. Reverse-order completions return all records and
mappings for reuse. Balanced warm-up ledger entries are cleared before
the original removal checks, preserving their 34-buffer baseline. The
old-behavior probe translates the private TX cookie to its mbuf while
retaining main's missing unmap and callback/drain ordering.

Additional CosmoReview findings were checked against source:

| Finding | Disposition and evidence |
|---|---|
| Callback still running after poll disable; unlocked final table walk | Not reproduced: `kernel/core/irqpoll.c:irq_poll_disable` marks disabled and waits on `idle_now` until neither running nor scheduled. Cleanup holds no driver lock during that wait. Both polls and transport vectors are synchronized before the walk; submitters were drained by unregister. A source comment now states this exclusivity. The existing active-worker test gap remains recorded. |
| DMA-address aliases cause a false double-unmap panic | The device-private ledger appends one record per successful map; unmap searches for a still-mapped matching address. Equal addresses therefore consume separate records, and previously unmapped records are skipped. Real devices have a NULL ledger. No failing alias case was established. |
| `m_getcl` may sleep under the driver spinlock | `kernel/include/kernel/kmalloc.h` explicitly declares allocation non-blocking and interrupt-safe. The actual path uses `kmem_cache_alloc` → `slab_grow` → `pmm_alloc_pages`, with IRQ-safe slab/zone locks and allocation failure returning NULL. It contains no sleeping allocation/reclaim path. The generic assumption about `kmalloc` does not apply here. |

## Validation

Review found a race in the first debug ledger implementation: a real
device callback could load the global fixture pointer before disarm and
read its device pointer after fixture reclamation. `47ea8581` replaces
that shared pointer with a debug-only private `vnet::test` field, initialized
before the synthetic queues exist and cleared only after their callbacks
stop. Real devices keep it NULL and never read fixture memory. The unarmed
hook uses a plain load, no lock or atomic RMW. Debug boots/probes affected
by this instrumentation change were repeated on the final implementation.
The first full matrix used `47ea8581`'s code. After Qodo's TX lookup
revision `71322638`, the entire matrix and both old-behavior probes were
repeated again. The table below reports that review revision. CI run
[37835499625](https://github.com/vkuttyp/CosmoOS/actions/runs/37835499625)
passed both architectures and the memory-order litmus job for `71322638`.
Qodo's summary for that commit lists no findings.

Host: macOS ARM64; Apple clang 21; QEMU 11.1.1; GNU make. QEMU runs
sequentially at default priority: sampled x86-64 PID 91630 and AArch64
PID 95489 each showed priority 31, nice 0. The review reruns sampled
x86-64 PID 23935 and AArch64 PID 43293 with the same priority/nice values.
No zsh background boot loop.
The remote main SHA was rechecked via GitHub and remains `561b913b`.

The first sandboxed host-test attempt failed at `NetTest.free_port`
with `PermissionError: [Errno 1] Operation not permitted` on loopback
bind. The permitted rerun completed successfully. This attributed
environment failure is excluded from the matrix; no assertion/budget
was changed. Sequential matrix commands, return codes, durations and
logs are recorded in `out/vnet-validation/results.json` and
`out/vnet-final-validation/results.json`. Review reruns are recorded in
`out/vnet-qodo-validation/results.json`.

The full x86-64 analyzer emitted 28 diagnostics, including an unused
assignment in the new fixture. That assignment was removed in
`4e1178c4`; the changed-source incremental analyzer then emitted none.
The other 27 are the existing scheduler/list/guest-memory/epoll/syscall/
test/network/NVMe diagnostics recorded by the October 3 lockdep report,
not a warning-free source audit. The target returns success despite
diagnostics. Full initial log: `out/vnet-analyze-x86.log`. Fresh final runs
confirmed 27 x86-64 and 28 AArch64 diagnostics, all outside the changed
driver/fixture. The fresh review reruns retained those same counts; logs:
`out/vnet-qodo-validation/{x86_64,aarch64}-analyze.log`.

All entries below passed (command wall time in seconds, including builds).
Use `gmake` on this host, `ARCH=aarch64` for AArch64. Debug OUT directories
are `out/vnet-fixed-x86` / `out/vnet-fixed-arm`; release directories are
`out/vnet-release-<arch>`. Review-validation logs are in `out/vnet-qodo-validation/` on both
architectures; CPU-count serial logs are `boot-qodo-final-<cpus>.log`
inside the debug OUT directories.

| Matrix item | x86-64 | AArch64 |
|---|---|---|
| `host-test` (ASan/UBSan and harness tests) | PASS, 60.5 | PASS, 60.7 |
| `fuzz` (default 20,000 mutations per target, seed 1) | PASS, 14.4 | PASS, 14.7 |
| Fresh `analyze` and vector-register check | exit 0, 53.1; 27 existing warnings | exit 0, 52.0; 28 existing warnings |
| `reproducible` | PASS, 14.2 | PASS, 15.4 |
| `vnet-remove-probe.py --old --arch=<arch>` | PASS, 157.9; exact two target failures | PASS, 155.0; exact two target failures |
| Debug `test QEMU_SMP=1` | PASS, 144.5 | PASS, 142.2 |
| Debug `test QEMU_SMP=2` | PASS, 154.9 | PASS, 139.5 |
| Debug `test QEMU_SMP=4` | PASS, 146.0 | PASS, 154.9 |
| `test-smp2` | PASS, 145.5 | PASS, 151.5 |
| `test-chaos` | PASS, 146.1 | PASS, 144.7 |
| `test-harness-retry` | PASS, 153.7; attempt 2 | PASS, 156.2; attempt 2 |
| `test-crash` | PASS, 124.4; expected page-fault panic | PASS, 122.8; expected page-fault panic |
| `BUILD=release test` (build and boot) | PASS, 17.2 | PASS, 20.8 |
| `llvm-nm` release kernel and virtio-net module | test entries/ledger/peer absent | test entries/ledger/peer absent |

Every normal debug boot reports `SELFTEST: PASS (449 tests)` and completes
the TCP/UDP and write-write-read network exchange. CPU-dependent existing
tests keep their documented skips. The intentional panic runner omits
the host back-connection, so its net-harness entry logs a skip; it passes
the self-test suite before the expected panic. No budget was widened and
no assertion weakened. The final removal cases record 34 maps/unmaps,
34 buffers returned, zero posted RX and zero reset-time callbacks/posts.
Release symbol logs: `out/vnet-qodo-validation/<arch>-release-{kernel,module}-symbols.log`.

## Recommendation for Unit 2

Run a fault/interleaving sweep first, using existing debug bind/unbind
helpers. The audit leaves reachable acknowledgement failures and
callback-retirement boundaries to prove before expanding the public
lifecycle API. Each proven defect should keep its own deterministic
test and old-behavior probe. Independent bind/unbind can follow once
these contracts are established. Existing module symbol resolution,
lockdep and lockup state were sufficient for this unit; observability
work in plan §11 need not block that next sweep.
