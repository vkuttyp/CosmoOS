# NEXT SUBSYSTEM — the network receive path has no duplicate-frame fault injection

> Constitution §68 report. This PR adds the report and the probe
> (`tools/net-rx-dup-probe.py`); the `FI_NET_RX_DUP` fault-injection kind, the
> injection in `rx_common` and the `net-rx-dup` test described under "Design"
> and the edits in "Affected files" are planned work that lands in the
> implementation PR that follows, gated on CI. As committed here, no
> fault-injection kind can duplicate a received frame.

## Problem

The fault-injection framework (`kernel/core/faultinject.c`, Prompt #2 §47)
has nine kinds — `kmalloc`, `blk-submit`, `blk-complete`, `demand-page`,
`demand-copy`, `usb-csw`, `ahci-ci`, `hv-selfcheck`, `file-readpage` — and not
one of them touches the network. The receive path has a single choke point,
`netif_rx` → `rx_common` (`kernel-services/network/netif.c`), that every
interface funnels through — loopback, the tap device, virtio-net, e1000e — and
there is no way to make it deliver the same frame twice.

So the stack's behaviour under a duplicated frame is a correctness claim no
test makes. TCP must deliver a duplicated segment to the socket exactly once
(the sequence-space dedup in `tcp.c`: a segment at or below `rcv_nxt` is
dropped, `ooo_dropped`); ARP must stay idempotent under a duplicated reply (no
second cache entry, no churn); ICMP echo is stateless and a duplicate is
answered twice (correct). None of these is exercised, because nothing can
present the receive path with a duplicate.

The one adjacent hook does not fill the gap. The loopback loss filter
(`loopback_set_filter`, used by `net-lo-tcp-loss`) can *drop* a frame, but it
is called inside `lo_transmit` **before** that function completes the
loopback checksum and sets `M_CSUM_OK` (`loopback.c`:23–25). A copy made in
the filter therefore lacks `M_CSUM_OK`; the receiver recomputes the transport
checksum over the partial-form packet, it mismatches, and the duplicate is
dropped as a bad checksum. The filter can lose a frame but cannot duplicate
one the receiver will accept — and it is loopback-only, reaching neither the
tap nor a virtio-net interface.

Prompt #2 §47 names "packet duplication" among the fault-injection points
still to build (`docs/audit/2026-09-deferred-work-inventory.md`, the
fault-injection row: "open: ... packet duplication, CPU starvation, VM-exit
storms"). This unit builds the first of those.

### Measured

`tools/net-rx-dup-probe.py` adds a self-test, `net-rx-dup-gap`, that subjects
`rx_common` to a duplicated frame the only way possible today — a hand-patch,
armed only around the test, because no fault-injection kind exists — and runs
a loopback TCP transfer with every received frame delivered twice (one debug
boot, x86-64):

```
NETRXDUP: no faultinject kind duplicates a received frame; rx_common hand-patched to deliver 276 duplicates (loopback filter runs pre-csum, its copy dropped); TCP still delivered 262144 bytes once
```

(a kernel log line is capped at `KLOG_LINE_MAX`; the full argument — that the
one adjacent hook is loopback-only and runs before the checksum, and that no
counted per-thread knob exists — is in "Problem" above. The dup count varies
run to run with retransmission; it is non-zero, which is what the test
asserts.)

- **No network injection kind.** `enum fi_kind` has nothing for the receive
  path; `faultinject_should_fail` is never called from `netif_rx`/`rx_common`.
- **The adjacent hook cannot duplicate.** The probe has to patch `rx_common`
  by hand (gated on a probe-only flag) rather than reuse the loopback filter,
  because a copy made in the filter is dropped as a bad checksum — the gap the
  implementation closes by injecting after the frame's metadata, including
  `M_CSUM_OK`, is settled.
- **The idempotency holds but is unverified.** With duplication forced, TCP
  delivered the stream once; that this stays true is a claim CI does not make.

## Why it matters

- **Duplicate frames are real.** A link-layer retransmit, a switch flooding a
  segment, a virtio descriptor retried after a spurious completion, a NAT
  hairpin — the receive path sees the same frame twice in ordinary operation.
  The stack's correctness under that is asserted nowhere.
- **The fault-injection point cannot be built on nothing.** §47's "packet
  duplication" needs an injection site and a knob; there is none, and the one
  hook that could re-inject a frame (the loopback loss filter) produces a copy
  the receiver rejects, and only on loopback.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| fault-injection kinds | `enum fi_kind` (`faultinject.h`) | kmalloc / blk / demand / usb / ahci / hv / file — nothing for the network |
| the injection hook | `faultinject_should_fail(kind)` (`faultinject.c`) | per-thread, counted (`seen`/`hits`), suppressed in interrupt context; called from kmalloc, blk, demand-paging, usb, ahci |
| the receive choke point | `netif_rx` → `rx_common` (`netif.c`) | every interface's frames enqueue here onto a per-CPU queue; no injection |
| the only re-injection hook | `loopback_set_filter` (`loopback.c`) | can drop a frame; a copy made here is pre-`M_CSUM_OK` (dropped as bad checksum), and it is loopback-only |
| the copy primitive | `m_copypacket` (`mbuf.c`, exported) | a full independent copy, carrying `m->pkt` and `M_CSUM_OK` |

## Design

### 1. A network RX duplication kind

`enum fi_kind` gains `FI_NET_RX_DUP` (before `FI_KIND_COUNT`), named
`"net-rx-dup"` in `g_names`. It is armed like every other kind — by the
kernel API (`faultinject_set`) or the boot parameter
(`opt/cosmo/faultinject`, e.g. `net-rx-dup:1`), for one thread or all, with an
optional budget — and reported by `sysctl debug.faultinject`. No new
mechanism: it rides the existing rule table and hook.

### 2. The injection in `rx_common`

When `faultinject_should_fail(FI_NET_RX_DUP)` returns true for a frame,
`rx_common` makes a copy with `m_copypacket` and enqueues it on the **same**
per-CPU queue the original went to, right behind it, then wakes the worker as
usual. The worker delivers both copies through the ordinary path
(`input_one` → `ipv4_input`/`ether_input` → the transport). The copy carries
`m->pkt` and `M_CSUM_OK`, so — unlike a copy made in the loopback filter — the
receiver accepts it and the transport layer is the one that must dedup.

Two properties fall out of reusing the existing hook rather than inventing a
knob:

- **Interrupt context.** `faultinject_should_fail` returns false when
  `irq_depth != 0` (the framework's deliberate rule: fault injection describes
  thread work, and allocating a copy in a handler has no caller to report to).
  So the duplication fires only where `rx_common` runs in **thread** context.
  The loopback path is one (`lo_transmit` runs on the sending thread), and the
  test drives it there. The two hardware drivers are **not**: `e1000e` calls
  `netif_rx` straight from `e1000e_irq`, and `virtio-net`'s `vnet_rx_done` is a
  virtqueue interrupt callback, so the knob is a no-op on their receive paths —
  the same limitation every fault-injection kind has. This unit does not add a
  thread-deferred (NAPI-style) receive path, so it covers thread-context
  delivery only; the stack code under test (`input_one` → `ipv4_input` →
  `tcp_input`, and `arp_input`) is interface-agnostic once `rx_common` has
  handed the frame up, so loopback exercises the dedup logic in full.
- **Counting.** `seen`/`hits` on the rule count eligible frames and injected
  duplicates, so a test can assert that duplicates were *actually* delivered
  (`hits > 0`) and is not vacuous.

The site is `rx_common` — the one point all interfaces share, and after the
frame's metadata is settled — deliberately, not the loopback filter (pre-csum,
loopback-only) and not each driver's receive handler (interrupt context, and
one site per driver).

### 3. In-flight / ordering

The copy is an independent `m_copypacket` mbuf: it is not the original (no
double-free, no double-enqueue — `mbufq_enqueue` refuses an already-queued
mbuf and the copy is fresh), and it is enqueued directly behind the original
on the same queue so the duplicate is delivered immediately after it, the
deterministic order a test needs.

## Affected files

| file | change |
|---|---|
| `kernel/include/kernel/faultinject.h` | `FI_NET_RX_DUP` in `enum fi_kind` |
| `kernel/core/faultinject.c` | `"net-rx-dup"` in `g_names` |
| `kernel-services/network/netif.c` | the injection in `rx_common`: on `faultinject_should_fail(FI_NET_RX_DUP)`, `m_copypacket` and enqueue the copy behind the original |
| `kernel-services/network/nettest.c` | the `net-rx-dup` test |
| `kernel/include/kernel/selftest.h`, `kernel/core/selftest.c` | declare / register `net-rx-dup` |
| `docs/verification/design.md` | the new kind in the fault-injection list |
| `README.md` | Status entry |

## APIs

No user ABI changes. `FI_NET_RX_DUP` is a debug-only fault-injection kind,
armable through the existing `faultinject_set` kernel API and the
`opt/cosmo/faultinject` boot parameter (`net-rx-dup:every[:budget]`). Fault
injection is compiled out of release builds (`CONFIG_FAULTINJECT`), so the
injection site is a no-op there, as every other kind is.

## Tests

Planned for the implementation.

| test | proves |
|---|---|
| `net-rx-dup` | with `FI_NET_RX_DUP` armed (`every=1`) for the test thread, a loopback TCP transfer still delivers the byte stream to the socket **exactly once** in order (the sink's `bytes_seen == bytes` and the pattern check), and `faultinject_stats` shows `hits > 0` (duplicates were injected, so the assertion is not vacuous); a duplicated ARP reply leaves one cache entry, not two |

**Planned mutations** (each alone, boot confirmed):
- the injection not enqueuing the copy (a no-op): `hits` stays 0, so the
  test's "duplicates were injected" check fails — the injection does nothing.
- TCP's at-or-below-`rcv_nxt` drop removed (duplicates re-delivered): the
  sink's `bytes_seen` overshoots the bytes sent, or the pattern check fails at
  the first duplicated offset.
- the rule armed for all threads rather than the test thread: frames on the
  network workers' own paths duplicate too, perturbing concurrent tests —
  which is why the test arms it for `only` its thread.

## Benchmarks

None.

## Risks

- **Allocating on the receive path.** `m_copypacket` allocates; mbuf
  allocation already happens on the receive path, and the framework's
  interrupt-context suppression keeps the injection out of handlers.
- **Load.** Duplicating every frame doubles receive traffic for the armed
  thread; TCP absorbs it through flow control and retransmit, and the test
  uses a bounded transfer rather than the full 1 MiB.
- **A copy the receiver rejects.** The reason the site is `rx_common` and not
  the loopback filter: the copy must carry `M_CSUM_OK`, which is set before
  `rx_common` and not before the filter.

## Alternatives considered

- **Extend the loopback loss filter to duplicate.** It is loopback-only and
  runs before the checksum is completed, so the copy is dropped as a bad
  checksum; it is also not a counted, per-thread, release-compiled-out
  fault-injection knob. The filter stays a loss filter.
- **A per-protocol dedup unit test with a crafted segment.** It would not
  subject the real receive path to a duplicate — the point of a §47
  fault-injection point is to drive `netif_rx` → the stack end to end, which
  only an injection at the choke point does.
- **Inject in each driver's receive handler.** One site per driver, and for
  the two hardware drivers (`e1000e`, `virtio-net`) in interrupt context,
  where the framework (rightly) suppresses injection — so this would not fire
  there either. `rx_common` is the single point all interfaces funnel through,
  and the injection fires on the frames that reach it in thread context
  (loopback, which the test drives); the stack it then feeds is the same for
  every interface. That is strictly more than a per-driver IRQ-context site
  could inject, for one site instead of one per driver.
