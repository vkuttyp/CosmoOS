# Device lifecycle Unit 1: virtio-net removal

## Scope and baseline

Work started 2026-10-08 on main `561b913b` (PR #335). Branch:
`device-lifecycle-vnet-remove`. Contributor rules were added first in
`adecaa38`. Scope: prove and repair virtio-net removal, audit the named
drivers' remove/reset paths, and record unproven concerns. No independent
bind/unbind API or module ABI change is proposed.

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
unregister is prevented. `rx_posted` is currently modified by the RX
poll alone (irq_poll serializes its calls); the old remove never accounts
its drain. The unlocked decrement alone does not prove concurrent writers.

## Proposed ownership and ordering

Use private RX/TX buffer tables in `struct vnet`, protected by the driver's
IRQ-safe lock. Submission records ownership before exposing the cookie;
completion removes that ownership before releasing the mapping and mbuf.
After unregister has excluded/drained senders, mark stopping under the
same lock. Stop RX poll delivery before reset so no callback can repost;
reset stops DMA; free both queues (synchronous vector/poll teardown);
then reclaim all remaining table entries, including unused descriptors,
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

AArch64 reproduction, the remaining driver audit and validation are pending.
