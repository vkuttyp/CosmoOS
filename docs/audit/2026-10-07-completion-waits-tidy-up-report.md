# Completion waits: the tidy-up

Date: 2026-10-07. Branch `completion-waits-tidy-up` from `main` at
`01c5f6aa`. Scope: the seven items the
[completion-waits report](2026-10-07-lockdep-completion-waits-report.md)
and its plan items (§1, §7) left, one commit each; nothing else changed.

## The items

| # | Item | Commit | Outcome |
|---|---|---|---|
| 1 | The report was dated 2026-10-08; the work was done 2026-10-07 | `docs:` rename | `git mv`, the date line, every link and date mention (plan, inventory, history, lockdep design, `lockdep_core.h`). No content change. |
| 2 | A shape the completion model does not record was unnamed | `docs:` blocked signaller | A signaller blocked in another wait (a completion, `thread_join`, `timer_cancel_sync`) before its `complete()` contributes no edge for that wait, so "waiter holds M and waits C1; C1's signaller waits C2; C2's signaller needs M" is not detected. Named in design.md and the report. Survey: the xHCI port worker, a USB removal and exiting threads that joined all wait before they complete; none of their second-level signallers takes a mutex, so the chain has no instance today. |
| 3 | `delack_timer` never ran in a boot | `net:` net-tcp-delack | The test found why: the timer could not fire (below). The output rule is corrected to the one the design document states, and a loopback receiver that accepts and stays quiet gets its odd segment acknowledged from the timer, after 44 ms (AArch64: 48 ms). The coverage listing now reports **0** timer callbacks never run on both architectures. |
| 4 | The NVMe no-vector fallback polled `completion_done`, outside the graph | `nvme:` fallback | The fallback waits with `wait_for_completion_timeout` (1 ms rounds, the same overall bound); `FI_NVME_ADMIN_POLL` forces the path on a machine with a vector and makes the interrupt handler leave the admin queue to the issuer; `nvme-admin-poll` runs Identify Controller through it under lockdep. Edges below. |
| 5 | `xhci-first-scan` was polled under `g_controllers_lock` | `xhci:` wait | `wait_for_completion_timeout`, the same 3 s bound per controller. The edge `xhci-controllers → xhci-first-scan` is in the graph (below); it is no problem: the worker that signals takes no mutex. |
| 6 | `fw.c`, `ipv4.c`, `nat.c` read `nif->flags` once per check | `net:` N26 | `ipv4_input` reads the ingress word once, `ipv4_forward` the egress's once, `ipv4_output` the egress's once; the readings travel as `in_flags`/`out_flags` into `fw_forward_verdict`, `nat_out`, `fw_output_verdict` and `fw_host_flow_of`. `net-netif-flags` parks a datagram between the anti-spoof check and the masquerade decision while the flag flips, and runs a stream under a toggling thread; `tools/netif-flags-probe.py --old` restores the live reads. |
| 7 | The class table filled to 382 of 384 unseen | `lockdep:` headroom | The post-suite dump prints `class table peak N of 512 (P%)` and warns at 90%. |

## The delayed acknowledgement that could not fire

The completion-waits report read `delack_timer`'s silence as a path the
suite never drove: every exchange acknowledged by output before 40 ms.
The test written to drive it failed twice, with the acknowledgement of a
lone segment arriving at once, and the reason is in `tcp.c`: the receive
path arms the delayed-ACK timer for an odd segment and then, a few lines
later, calls `tcp_output_locked` for the segment's acknowledgement and
window ("new window or ack may allow more data"), whose pure-ACK branch
fired whenever `delack_pending` was set -- sending the acknowledgement at
once and cancelling the timer it had just armed. The delayed ACK was dead
code in every boot, and the design document's rule ("a pure ACK is sent
at once when two segments are pending or after 40 ms") was not what the
code did.

The correction separates the two meanings `delack_pending` carried:
`ack_now` says a pure acknowledgement is wanted on this output (every
second segment, a gap behind, a window update, the handshake, the timer),
`delack_pending` says one is owed and the timer is armed. The output
routine sends a pure ACK only for `ack_now`; an owed one rides on the next
data segment, goes with the application's read, or waits for the timer.

The first version of the correction had only the segment and the timer,
and the one-CPU boots failed on `net-bench`'s budget: its loopback TCP
stream fell from 21 to 2 MiB/s (8.8 s for the test against 8 s) on both
architectures, while at two and four CPUs it had *risen* (x86-64: 14 to
34 MiB/s one flow, 14 to 39 two flows; fewer pure acknowledgements). On
one CPU the sender runs until its window is full and the receiver then
runs; a window that ends on an odd segment left the last one owed to the
timer, and the sender waited 40 ms every round. That is the classic
delayed-ACK stall against a small window, and the standard remedy is
the receiver's read: when the application takes the data and an
acknowledgement is owed, it goes at once (Linux's `tcp_cleanup_rbuf`
does the same). With that rule a receiver that reads keeps the sender
moving and a receiver that does not read still leaves the
acknowledgement to the timer, which is what `net-tcp-delack` checks. The
one-CPU numbers after the rule, and the two- and four-CPU ones, are in
the validation table.

## What the flag race looked like

The forwarding path decided from `nif->flags` several times per packet.
With masquerade *off*, the anti-spoof admits any source on the tap's
subnet; with it *on*, only the guest's `.15`. A toggle to *on* landing
between that check and the masquerade decision gave a datagram from `.20`
both: admitted as a plain forwarder, then masqueraded as a guest -- a
source the masquerade's anti-spoof refuses, translated, with a NAT flow
made for it. Neither state produces that packet. `net-netif-flags` holds
the datagram at exactly that point with a debug hook (`ipv4_test_hold_forward`)
and flips the flag:

| | one reading (now) | per-check reads (`--old`) |
|---|---|---|
| `.20` parked, masquerade turned on | leaves intact, no flow | masqueraded (`src == u_ip`), a flow for `.20` |
| `.15` parked, masquerade turned off | leaves masqueraded (the state it entered under) | leaves intact |
| 300 datagrams from `.20` under a thread flipping the flag every ms | each forwarded intact or refused as spoofed; none masqueraded | -- |

The probe result on both architectures is in the validation table.

## The graph edges the new waits produce

Read from the post-suite graph dump of the four-CPU debug boots, the same
on both architectures:

| Edge | Where it comes from |
|---|---|
| `xhci-controllers → xhci-first-scan` | module init's new wait under `g_controllers_lock` (item 5) |
| `modules → xhci-first-scan` | the same wait, under the module loader's mutex |
| `nvme-admin → nvme-admin`, `devices → nvme-admin`, `modules → nvme-admin` | admin commands under the admin mutex and, at bring-up, the registries -- the edges the first unit saw |

No edge leaves `nvme-admin` or `xhci-first-scan`: the forced NVMe path's
own `complete()` under the admin mutex is discarded by the thread's wait
for the same object (the self-signal rule), which is what the test's
"no report" line says, and the xHCI worker holds no mutex when it
signals. `nvme-admin-poll` runs Identify Controller through the polled
path in 1.6 ms on both architectures; the evidence that the path was
taken is the injection point's hit, not the time (a controller that
answers before the first `queue_process` makes the wait return at once).

## Coverage after the unit

| Listing | x86-64 | AArch64 |
|---|---|---|
| timer callbacks set up, never run | 21, **0** | 22, **0** |
| completion classes, not both waited for and signalled | 44, 3 | 44, 3 |

The three completion classes left are the test ones the first report
judged (`ct-none`, completed by nobody by design; `chrblock-reader` and
`chrblock-writer`, whose test joins the threads instead). Every production
completion class is now both waited for and signalled; every timer
callback the source sets up ran.

## Validation

On the branch head, one chain, one QEMU at a time, no `&` loops; 433
self-tests in every debug boot:

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | PASS (63 s) | -- |
| `make host-test-lockdep-tsan` | PASS | -- |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 136.5 s and 137.5 s; peak 408 of 512 classes; 0 callbacks never run | PASS 148.2 s; 408; 0 |
| `make test-smp2` | PASS 147.1 s; 389; 0 | PASS 145.9 s; 389; 0 |
| debug boot, 1 CPU | PASS 131.6 s; 353; 0 | PASS 133.4 s; 354; 0 |
| `make test-chaos` | PASS 135.3 s; 404; 0 | PASS 145.2 s; 404; 0 |
| release build and boot | PASS 16.8 s | PASS 20.1 s |
| `tools/netif-flags-probe.py --old` | FAIL at `src == other`, as required | the same |
| `tools/netif-flags-probe.py` (fixed) | PASS | PASS |

The chain's QEMUs ran at nice 5: the second launch of the chain went
through a zsh `&`, which `BG_NICE` demotes (`docs/development.md`,
"Benchmark runs"; the first launch, at nice 0, was stopped after its
one-CPU boots failed on the stall described above). The verdicts stand;
the timing comparison was repeated at the default priority (sampled
`pri 31 nice 0`), alternating `main` and the branch:

| Run | `main` (01c5f6aa) | branch |
|---|---|---|
| 1 | 111.5 s | 107.9 s |
| 2 | 111.2 s | 111.4 s |

Inside `main`'s own spread; both trees at `LOCKDEP=1`.

**`net-bench`'s loopback TCP** (one flow, MiB/s; the figure the one-CPU
stall sank to 2):

| CPUs | `main` | branch |
|---|---|---|
| 1 (x86-64) | 21 | 22 (nice 5 run), 25 (default priority) |
| 1 (AArch64) | 23 | 25 |
| 2 (x86-64) | 24 | 25 |
| 4 (x86-64, alternated boots at default priority) | 12 and 26 | 29 and 31 |
| 4 (AArch64) | 30 | 28 |

The `main` figures are the previous unit's validation and this chain's
`main` boots; the branch's are this chain's and the default-priority
rerun. No x86-64 boot approached the 180 s budget locally (the longest,
the two-CPU boot, 147.1 s). CI (PR #322, run 37595714297): the x86-64 job's debug boots took
140.7–147.3 s against the 180 s budget (release 16.5 s); the AArch64
job's 166.7–178.9 s against its 240 s. No timeout; the flakes record is
unchanged.

## Plan

Plan §1's "paths the suite never drives" item is complete: the one
callback the suite never ran is driven (and was dead code, now alive),
the two polled completions are waits in the graph, and the shapes the
model does not record are named -- the "lock, unlock, complete"
signaller, the queued worker, and now the signaller blocked in another
wait -- each with the survey result that no production chain closes
through it today. Plan §7's netif flag item is complete (N26). Inventory
§7.4 and the history carry the unit.
