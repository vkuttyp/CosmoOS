# NEXT SUBSYSTEM — a verdict TCP's callers can see

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This was that report; the unit is
now **implemented** (PR "A verdict TCP's callers can see"), and the design
below is as built — see `docs/kernel-services/network/design.md` ("A
refused segment is the connection's business") for the shipped description
and `docs/kernel-services/network/testing.md` (`net-tcpverdict`) for its
proofs. Six things came out differently and are marked where they arise:
the abort turns out **not** to be what tells the caller of a refused
`connect` (`tcp_connect`'s own return is, so the abort's job is the connect
*already waiting* — a case this report did not describe and the test now
covers); the SYN-cache drop **is** separately observable after all, by the
slot being reusable, where this report expected only its counter; a
recorded verdict reaches `recv` as well as `send`, which the sweep found
and this report had not stated; an application cannot send its way out of
one; and two of the eight bug-proofs are not the ones listed here, one
because the bug it named was not observable and one because a listed
assertion was proving nothing.

**Subsystem: the one hole the OUTPUT chain left in itself. That unit made
the host's own egress filterable and, alone among the four chains, made a
refusal *spoken*: `ipv4_output` returns `-EPERM` and `udp_sendto` and
`icmp_send_echo` hand it to their callers. TCP hears nothing.
`batch_send` — the sole emitter, and by design the only code that touches
the link after the per-connection lock is dropped — discards
`ipv4_output`'s return, so a rule that refuses a connection's segments is
invisible to `connect`, to `send` and to `poll`: the socket waits out
eight retransmissions with a doubling RTO (about three minutes) and is
told `-ETIMEDOUT`, which names a network that did not answer rather than
a machine that decided not to ask. This unit carries the verdict back into
the connection. `batch_send` returns the first output error; the flush
sites that own a connection apply it under a rule shaped like RFC 1122
§4.2.3.9's treatment of a hard error — **an opening connection is aborted**
(`connect` returns `-EPERM` at once), **a synchronized one records it** and
is left standing, because the peer's half of the conversation still
arrives and a rule the operator deletes a moment later should let the
connection resume — which it does, because a recorded verdict is cleared
by the next segment that leaves. A refused SYN-ACK belongs to a third
kind: a passive open's half-open lives in the listener's SYN cache and has
no PCB, so there is no caller to tell and the cache entry is dropped
instead of held for eight seconds for a connection the machine has decided
not to answer. Only `-EPERM` — the verdict, and the only `-EPERM` the
output path produces — is treated this way; a full queue or a missing next
hop stays what it is today, loss, because a route can appear and a rule's
refusal will be taken again.**

## Problem

The OUTPUT chain reports its refusals to everything except the protocol
most likely to be refused.

- **`connect` cannot be told.** A rule that refuses a connection's SYN
  drops it in `ipv4_output`, counts `ip_stats.tx_filtered` and
  `fw_stats.out_drop_rule`, and returns `-EPERM` to `batch_send`, which
  throws it away. The socket stays in `SYN_SENT` until the retransmit
  budget runs out: `TCP_RTO_INIT_NS` is one second, the RTO doubles,
  `TCP_RTO_MAX_NS` caps it at sixty, and `TCP_MAX_REXMIT` is eight — so
  1 + 2 + 4 + 8 + 16 + 32 + 60 + 60, about **three minutes**, and then
  `-ETIMEDOUT`. The operator's rule works; the application is told the
  wrong thing about why, three minutes late.
- **An established connection's refusal is invisible until the same
  timeout.** A rule added mid-connection stops every subsequent segment.
  The application's `send` calls keep succeeding (bytes enter the send
  buffer), `poll` keeps reporting the socket writable, and nothing reports
  a problem until the retransmit budget ends the connection with
  `-ETIMEDOUT`.
- **The machine holds state for connections it has decided not to
  answer.** A refused SYN-ACK leaves the listener's SYN-cache entry in
  place for `TCP_SYNCACHE_TTL_NS` (eight seconds). The connection cannot
  be completed while the rule stands — the client's ACK would be answered
  by a SYN-ACK the chain refuses again — so the slot is held for nothing,
  and there are `TCP_SYNCACHE_SIZE` (64) of them.
- **The machinery to report it already exists and is unused for this.**
  `struct tcp_pcb` carries `int error` (`tcp.h:144`); the state machine
  sets it in four places, all fatal; `tcp_send` returns it
  (`tcp.c:1072`), `tcp_recv` returns it when no bytes are available
  (`:1111`), `tcp_ready` turns it into `COSMO_IO_ERROR` (`:1193`,
  `:1201`), `take_error` reads it (`socket.c:144`) and all three of
  `ksock_connect`'s completion paths consult it. Nothing in the socket
  layer needs to change for a connection to report `-EPERM`; only the
  error has no way to get from the link back to the connection.
- **The limit is asserted, deliberately, so that this unit has a test to
  change.** `net-output` step 8 (`nettest.c:5787`) requires a nonblocking
  `connect` to a refused peer to return `-EINPROGRESS` "not `-EPERM`: TCP
  never sees it", with `out_drop_rule` rising and no SYN on the link. The
  OUTPUT report named the reversal of that assertion as this unit's first
  deliverable.

## Current implementation

**`batch_send` (`kernel-services/network/tcp.c:693`) is `void` on
purpose, and the purpose is the locking.** Segments are built under the
per-connection lock and sent after it is dropped, because nothing under
the TCP lock may consult the netif registry (invariant N5; `tcp.h:110`
records the same rule for `path_mss`). So the batch carries what the link
needs — up to sixteen mbufs with their source and destination addresses
(`struct tcp_batch`, `:67`) — and not what a refusal would need: the
connection the segments belong to.

```c
static void batch_send(struct tcp_batch *b)
{
    if (b->quiet) { /* the host chain's silent drop: free, count, return */ }
    for (unsigned i = 0; i < b->n; i++) {
        if (b->seg[i].src.family == COSMO_AF_INET)
            ipv4_output(m, ..., IPPROTO_TCP, IP_DEFAULT_TTL);   /* return discarded */
        else
            ipv6_output(m, ..., IPPROTO_TCP, IP_DEFAULT_TTL);   /* return discarded */
    }
    b->n = 0;
}
```

**There are ten flush sites, and they fall into three kinds.** Seven own
exactly one connection and already hold a reference to it at the flush —
`pcb_work` (`:927`, the work item's reference), `tcp_connect` (`:1064`),
`tcp_send` (`:1090`), `tcp_recv` (`:1113`), `tcp_shutdown_write`
(`:1140`), `tcp_input`'s `out:` epilogue (`:1928`, the lookup's or the
new child's) and `tcp_pmtu_notify` (`:1968`). Two own none: the
stray-segment path that answers an unmatched segment with an RST
(`:1645`) and the LISTEN path whose `listen_input` returned no child
(`:1656`), whose batch may hold a **SYN-ACK for a half-open the listener
put in its SYN cache**, or an RST. One, `tcp_close` (`:1275`), has a
caller that is going away and a batch that can hold a parent's and its
queued children's segments at once.

**`pcb->error` is set in four places and every one of them is fatal.**
The keepalive budget (`:852`), the retransmit budget (`:896`), a valid
reset of the host's own open (`:1680`, `-ECONNREFUSED`) and a valid reset
of an existing connection (`:1770`, `-ECONNREFUSED` in `SYN_RCVD` and
`-ECONNRESET` elsewhere). All four follow it with `pcb_end_locked`, so
today **a PCB carrying an error is always a PCB that has ended** — an
assumption this unit breaks on purpose, and the reason its sweep matters.

**`SYN_RCVD` is not the passive-open state here.** `listen_input`
(`:1451`) answers a bare SYN by adding a SYN-cache entry and building a
SYN-ACK; the child PCB is created ESTABLISHED when the completing ACK
arrives (through a cache slot or a cookie). The only way to a `SYN_RCVD`
PCB is a simultaneous open (`:1706`). So "the host's SYN-ACK to a guest
was refused" is a statement about a **cache entry**, not about a
connection — which is why the design below has three answers and not two.

**`-EPERM` is unambiguous.** `ipv4_output` (`ipv4.c:202`) returns it for
the OUTPUT verdict and nowhere else; its other refusals are
`-ENETUNREACH` (no route, `:181`) and whatever `output_on` returns
(`-EMSGSIZE`, `-ENOBUFS`). `ipv6_output` has no chain — IPv6 filtering is
a later unit — so it never produces one.

**A quiet batch never produces a verdict.** The host chain's silent drop
frees the batch before the loop (`:695`), so `M_FW_QUIET` segments are
counted as `quiet_dropped` and reach no chain.

## Why it matters

- **A policy that cannot be reported looks like a fault.** Every layer
  above TCP reads `-ETIMEDOUT` as "the network did not answer". An
  operator who has just written a rule is the one person who could have
  explained the failure, and the machine tells the application something
  else for three minutes.
- **It is the difference between a usable rule and an unusable one.** The
  OUTPUT chain's most-cited case is "the host may not answer guests on
  this port". With no verdict, a guest's connection attempt to a refused
  host service hangs rather than being refused, and the guest's own
  application waits out *its* retransmits.
- **The suite cannot even contain the current behaviour.** A blocking
  `connect` to a refused peer outlives the harness's eight-second
  per-test budget, which is why `net-output` step 8 had to use a
  *nonblocking* connect to assert the limit at all. Fixing this is what
  lets the case be tested the way an application would meet it.
- **Half-open state for a refused connection is a small, real surface.**
  Sixty-four slots held eight seconds each, refillable by a guest that
  sends SYNs to a port the host is not allowed to answer on.
- **It is the last follow-up the OUTPUT unit created for itself**, named
  in that report three times (§6 "Deliberately out of scope", the Tests
  bullet on TCP (now "TCP is told too"), the Risks bullet "TCP's
  asymmetry"), in `design.md` (the paragraph "A refused send is told, not
  hidden" and the section's "Named and deferred" line), in `testing.md`'s
  `net-output` step 8, and three times in the README. The other named follow-ups — per-interface chains,
  rate-limit and log targets, IPv6, full TCP state tracking — are new
  ground; this one is an incompleteness.

## Design (as built)

### A refusal is local, deterministic and hard

Every segment of one connection carries the same tuple: same protocol,
same source and destination address, same ports, same egress. A rule
matches on exactly that tuple (plus the OUTPUT scope), so **if a rule
refuses one segment of a connection it will refuse every segment of that
connection** for as long as it stands. That is what separates a verdict
from the errors TCP already ignores: a full transmit queue drains, an ARP
entry resolves, a route appears — retransmission is the right answer to
all of them. A verdict is a decision, and retransmitting into it is
waiting for a rule to change.

So the verdict is treated as a **hard error on the connection**, and the
shape is RFC 1122 §4.2.3.9's: a hard error on a connection that is still
opening aborts it, and one on a synchronized connection is advisory
(RFC 5927 argues the same caution, from the other direction — an
attacker's ICMP should not be able to kill an established connection).
Here the error is our own kernel's, not a stranger's, so the caution is
not about trust; it is that the connection may still be *useful* — the
peer's segments arrive regardless, unread data is still in the receive
buffer, and the rule may be deleted before the retransmit budget expires.

### Three kinds of refused segment, three answers

**1. A segment of a connection that has a PCB** — the seven owning flush
sites. The state decides:

| state | answer |
| --- | --- |
| `SYN_SENT` | abort: `pcb->error = -EPERM`, the PCB ends, the socket wakes |
| `SYN_RCVD` (simultaneous open) | the same |
| `ESTABLISHED`, `CLOSE_WAIT`, `FIN_WAIT_1/2`, `CLOSING`, `LAST_ACK` | record: `pcb->error = -EPERM`, the socket wakes, the state machine is untouched |
| `TIME_WAIT`, `CLOSED`, `LISTEN` | counted only (nothing is waiting on the outcome, and a `TIME_WAIT` ACK's refusal changes nothing) |

The abort is not a new mechanism: it is the same three lines the valid-RST
path already uses (`:1680`) — `pcb->error`, `sock_ref`, `pcb_end_locked`.
**As built, it is not what tells the caller of a refused `connect`**, and
this report said otherwise. `tcp_connect` returns the refusal itself (see
its row below), so that call is over before the abort could matter — which
a bug-proof demonstrated by removing the abort and watching step 1 pass
anyway. What the abort is actually for is the connect *already waiting*:
the first SYN left before the rule existed, the application is blocked on
the state (`ksock_connect`'s wait condition is "the state left
`SYN_SENT`/`SYN_RCVD`", `socket.c:290`, and not "an error appeared"), and
the refusal of the retransmission a second later is what ends it. That case
is now a test step of its own, and it is the step that fails when the abort
is removed. The abort also stops the connection spending its remaining
seven retransmissions on a SYN the chain has already refused.

**2. A SYN-ACK for a half-open in the listener's SYN cache** — the
owner-less LISTEN flush site. There is no PCB and no caller: the
accepting application has never heard of this connection. The answer is
hygiene rather than reporting — **the cache entry is dropped** (its
`ts_ns` cleared, the slot free at once) and counted, because the machine
has decided it will not answer and holding the half-open for eight
seconds serves nothing. A client that retransmits its SYN makes a new
entry and is refused again, at the cost of one slot for the length of one
flush.

**3. A stray segment's RST, or a refused segment on a PCB nobody waits
on** — counted, and nothing else. `tcp_close`'s flush is in this class
too: its caller is gone, and its batch can carry segments for several
connections at once, so it is given no owner at all.

### A recorded verdict is cleared by the next segment that leaves

This is the other half of the synchronized-state rule, and without it the
rule would be worse than the current behaviour: `pcb->error` is sticky
(`take_error` clears the socket's copy, never the PCB's), so a connection
that recorded a verdict would fail every later `send` even after the
operator deleted the rule.

So the same helper that records a verdict clears one: **a flush in which
at least one segment reached the link clears a recorded `-EPERM`** on that
connection. Delete the rule, the retransmit timer fires, the segment
leaves, the record is gone and the application's next `send` succeeds. Only
a recorded verdict is clearable, and only in a state that is not `CLOSED`
— `-ECONNRESET`, `-ECONNREFUSED` and `-ETIMEDOUT` always come with a PCB
that has ended, so they can never be reached by this path.

This is the discipline the host chain's `quiet` bit taught: one gate
that sets a flag, and an explicit, enumerated set of points that clear
it. Here there is one of each.

**As built, one consequence of that is worth stating and this report had
not:** an application cannot send its way out of a recorded verdict.
`tcp_send` reports the error before it would build anything, so the flush
that clears a record is always the retransmit timer's, or an
acknowledgment the peer's own traffic asks for — which is what the test
uses as its trigger.

### The first refused `send` still returns its byte count

`tcp_send` copies the caller's bytes into the send buffer, builds
segments, drops the lock and flushes (`:1068-1091`). By the time the
verdict is known the bytes are *queued* — they will be retransmitted, and
a retry by the application would send them twice. So the refused call
returns the count it accepted, and the error surfaces on the **next**
call, which is how a recorded socket error behaves everywhere else.
`poll` is immediate: `tcp_ready`'s default branch already turns a live
error into `COSMO_IO_READABLE | COSMO_IO_WRITABLE | COSMO_IO_ERROR`
(`:1201`), so an application that polls learns before it writes again.
The asymmetry is stated here, asserted in the tests, and not hidden.

### Only the verdict

The helper keys on `-EPERM` exactly, not on "any negative". The other
output errors keep today's behaviour — discarded, the segment treated as
lost — and the reason is the deterministic/transient split above. One
consequence is worth naming: this makes `-EPERM` from `ipv4_output` a
**part of the interface between the network layer and TCP**, so a future
chain, a future IPv6 verdict or any other refusal that wants TCP to react
must use that value and no other. `ipv4.c` gains a comment saying so.

### The mechanism

```c
/* The first output error, or the number of segments that reached the
 * link. Negative means the link refused one; 0 means there was nothing
 * to send. */
static int batch_send(struct tcp_batch *b);

/* After the flush, under the pcb lock, alone: apply a refusal or clear a
 * record. `rc` is batch_send's return; `wake` and `killed` are the
 * caller's unwind pair -- two sites already have one, the other five gain
 * a local pair (see the table below). */
static void output_result(struct tcp_pcb *pcb, int rc, struct socket **wake, bool *killed);
```

Only two of the seven owning sites have a `wake`/`killed` epilogue to
reuse — `pcb_work` and `tcp_input`'s `out:` — and there the call slots into
the existing unwind:

```c
    spin_unlock_irqrestore(&pcb->lock, s);
    int rc = batch_send(&b);
    output_result(pcb, rc, &wake, &killed);      /* may set either */
    sock_wake_after(wake);
    if (killed)
        pcb_put(pcb);   /* the state machine's */
    pcb_put(pcb);
```

The other five are syscall-context functions that today end with
`batch_send(&b); return ...;` and have neither variable. Each gains a local
pair and the two lines that unwind it, and each keeps its own return
contract — which is not the same answer in all five, so they are given one
by one rather than by a snippet:

| site | context | a refusal there |
| --- | --- | --- |
| `pcb_work` (`:927`) | timer worker | `output_result`; the existing `wake`/`killed` pair carries it |
| `tcp_input` `out:` (`:1928`) | network worker | the same |
| `tcp_connect` (`:1064`) | syscall | the caller is right here, so the refusal is **returned**: abort, record, and `return -EPERM` instead of 0. `ksock_connect` needs no change — it hands a non-zero `tcp_connect` straight back to the application, so a *nonblocking* connect fails immediately with `-EPERM` rather than reporting `-EINPROGRESS` and failing on the next call, which is what POSIX asks of a connect that fails outright. The record is kept as well as returned, so a second thread polling the same socket sees `COSMO_IO_ERROR`. The socket is left unconnected with an ended PCB — the state a reset-refused connect already leaves, so no new case appears |
| `tcp_send` (`:1090`) | syscall | `output_result`; returns the byte count it accepted, because those bytes are queued (above) |
| `tcp_recv` (`:1113`) | syscall | `output_result`; the return is unchanged — it reads `pcb->error` *before* the flush (`:1111`), so a verdict its own window-update ACK earns is reported on the next call, consistently with `tcp_send` |
| `tcp_shutdown_write` (`:1140`) | syscall | `output_result`; still returns 0 — the state moved to `FIN_WAIT_1`/`LAST_ACK` under the lock and the shutdown did happen; a refused FIN is recorded, not undone |
| `tcp_pmtu_notify` (`:1968`) | network worker | `output_result`; still returns true — the MSS was lowered whether or not the resend left |

Three properties make this the cheap version of the change:

- **No new ownership.** Every owning site already holds a reference to
  the connection at the flush point — the lookup's, the work item's, the
  socket's, or the new child's — so nothing needs to be kept alive and
  the batch gains no pointer and no reference. (A `struct tcp_pcb *` on
  the batch would have needed one, since the batch outlives the lock.)
- **No new lock order.** `output_result` takes one lock, the connection's
  own, with nothing else held, after the flush — the same acquisition the
  site had just released. No netif access happens under it, so invariant
  N5 holds unchanged.
- **The accepted path pays a compare.** `output_result` returns
  immediately when `rc >= 0` and the connection has no recorded verdict,
  which is one relaxed read of a field on a cache line the flush site has
  already touched. The lock is taken only when there is something to do.

`output_result` does nothing at all when `*killed` is already true (the
connection ended during the same flush; there is nothing left to tell) and
when the state is `CLOSED`, `LISTEN` or `TIME_WAIT`. It reuses an
already-set `*wake` rather than overwriting it, the way the FIN path does
(`:1905`), which arises only at the two sites that already have one.

For the SYN-cache case the LISTEN path keeps the listener's reference
across the flush instead of releasing it before (`:1653`), so a refusal
can re-take the listener's lock and clear the entry the SYN-ACK was built
for. The tuple is in `struct seg`, which the site still has.

### What the socket layer needs

Nothing — though what it *reports* changes in two places. A non-zero
`tcp_connect` is already handed straight back to the application, so the
refused connect returns `-EPERM` from both the blocking and the
nonblocking call without a line changing; `ksock_connect`'s three
completion paths already end with `take_error(s)` for the refusals that
arrive later (a keepalive or retransmission refused while the socket
waits); the blocking `connect` wakes on the state change the abort
causes; `ksock_sendto`'s stream loop already returns
`s->tcp->error` when it has written nothing and already waits on it
(`socket.c:349-353`); `ksock_recvfrom` drains buffered data first;
`tcp_ready` already anticipates a live error. That the socket layer needs
no change is the strongest evidence the design sits where the existing
one expected it to.

**The sweep found one semantic this report had not stated**, and it is
kept rather than coded around: a recorded verdict reaches `recv` as well
as `send`. `tcp_recv` returns the bytes it holds first and the error only
when there are none, so a refused *egress* eventually fails a *read* — and
that is what a pending socket error does in POSIX, which is not
direction-specific. Making it one would have meant teaching the socket
layer to tell a send-side error from a receive-side one, and the wait
conditions with it, for a connection that in this state cannot make
progress anyway. The test asserts the behaviour in both directions: the
peer's earlier bytes still read back, and the read after they are drained
is told.

### The §70 gate

**Correctness.** The rule is a function of one thing — the connection's
state at the moment of the refusal — and the states partition into abort,
record and ignore with no overlap. The clearing rule's precondition ("a
segment reached the link") is decided by the same return value, in the
same helper, so a record and its clearing cannot disagree.

**Concurrency.** The verdict is applied on whatever CPU flushed the batch:
a syscall thread (`tcp_send`, `tcp_connect`), the network worker
(`tcp_input`, `tcp_pmtu_notify`) or the timer worker (`pcb_work`). All
three already take this lock; the helper adds no second lock and no
ordering. Two CPUs can refuse segments of the same connection at once —
both write `-EPERM` under the lock, and the second write is idempotent.
A refusal racing with the connection's own teardown is the `*killed` and
`CLOSED` case, checked under the lock.

**Ownership.** The connection is owned by whoever holds a reference; the
flush site's reference is the one used, and none is created.

**Lifetime.** A connection aborted by a verdict ends exactly as a
reset-aborted one does, through `pcb_end_locked` and the caller's existing
`if (killed) pcb_put(pcb)`. A SYN-cache entry dropped by a verdict frees
its slot under the listener's lock. Nothing new outlives the flush.

### Statistics

`struct tcp_stats` gains four counters, because "which of the four things
happened" is the whole content of the unit and each is what a test needs
to assert:

| counter | meaning |
| --- | --- |
| `out_refused` | segments the chain refused (a retransmission counts again) |
| `out_aborted` | connections ended by a refusal (the opening states) |
| `out_recorded` | verdicts recorded on a synchronized connection |
| `out_cleared` | recorded verdicts cleared by a later segment that left |

`syn_refused` joins them for the SYN-cache entries dropped. The existing
`ip_stats.tx_filtered` and `fw_stats.out_drop_rule` continue to count the
chain's side of the same events, so a test can assert both ends.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/tcp.c` | `batch_send` returns `int`; new `output_result`; the seven owning flush sites call it; the LISTEN path holds the listener's reference across the flush and drops a refused SYN-cache entry; the `struct tcp_batch` comment records the new contract |
| `kernel/include/kernel/net/tcp.h` | four new `tcp_stats` counters plus `syn_refused`; the comment on `int error` records that a live connection may now carry one |
| `kernel-services/network/ipv4.c` | a comment at the OUTPUT verdict: `-EPERM` is the value TCP reacts to, and no other output error may use it |
| `kernel-services/network/nettest.c` | new selftest `net-tcpverdict`; `net-output` step 8 reversed — its nonblocking `connect` to a refused port now returns `-EPERM` on the *first* call, since `tcp_connect` returns the refusal itself |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | register `net-tcpverdict` |
| `docs/kernel-services/network/design.md` | "The OUTPUT chain": the paragraph "A refused send is told, not hidden" rewritten from "TCP does not" to the state rule, its pointer to this report replaced by what was built; the section's "Named and deferred" line; a new subsection for the rule |
| `docs/kernel-services/network/testing.md` | `net-tcpverdict`; `net-output` step 8's text (its sentence on TCP stalling) |
| `docs/audit/next-subsystem-output-chain.md` | the three places that say TCP's callers cannot see a verdict — §6 "Deliberately out of scope", the Tests bullet on TCP, the Risks bullet "TCP's asymmetry" — each already pointed here by this report and to be converted to as-built by the implementation |
| `README.md` | the OUTPUT Status entry's TCP sentence (`:1352`), the two "named next steps" mentions (`:1380`, `:1455`), and a Status entry for this unit |

## New APIs

No new syscall, no new uapi, no new device. Two internal functions
(`batch_send`'s changed signature and `output_result`), five counters in
`struct tcp_stats`, and one documented promotion of `-EPERM` from
`ipv4_output` into a value TCP interprets.

## Migration plan (followed as written)

Each step landed as its own commit with the tree green, in this order.

1. **`batch_send` returns.** Change the signature and the ten call sites
   to ignore the value. No behaviour change; the tree stays green. This
   step alone is the one that could regress everything, so it lands
   alone.
2. **`output_result`, abort only**, and `tcp_connect`'s return. Implement
   the opening-state abort, wire the seven owning sites (five of them
   gaining the local `wake`/`killed` pair), and make `tcp_connect` return
   `-EPERM`. `net-output` step 8 flips here — to `-EPERM` on the first
   call — and the blocking-connect case becomes testable.
3. **The synchronized record**, with `tcp_send`/`tcp_recv`/`tcp_ready`
   re-read against the new "a live PCB may carry an error" assumption —
   the sweep this unit turns on.
4. **The clearing rule**, and the test that a deleted rule lets a
   connection resume.
5. **The SYN-cache entry.** The LISTEN path's reference and the drop.
6. **`net-tcpverdict`** in full, then the bug-proofs.
7. **Docs and README**, including the nine claims listed above, swept by
   listing every occurrence of the terms rather than filtering for the
   phrasing I expect.

Steps 2–5 are each independently testable and each leaves the tree green.

As built, step 2's run is worth recording: the tree between step 2 and the
`net-output` edit failed on exactly the line that asserted the old limit,
which is the flip this report promised at this step and the first proof
that the verdict reaches the caller.

## Tests

**`net-tcpverdict`** (new), on the fixtures `net-output` already builds —
an "uplink" tap with a host route and a guest tap through
`/dev/net/tap`, so both scopes are available, plus a host listener:

1. **A blocking `connect` to a refused peer returns `-EPERM`**, and
   returns *promptly*: the old behaviour cannot return inside three
   minutes, so a test that finishes proves it. As built the wait is
   **bounded by the test itself** (two seconds on its own thread) rather
   than left to the harness's budget, so the step *fails* instead of
   hanging the suite. `out_refused` and `out_aborted` rise, `tx_filtered`
   rises, and no SYN reaches the tap.
1b. **A connect already waiting is woken by the abort** — the step this
   report did not foresee, added when a bug-proof showed step 1 passing
   without the abort. The first SYN leaves before the rule exists, the
   application blocks on the state, the rule appears, and the refusal of
   the retransmission ends the wait with `-EPERM`.
2. **A nonblocking `connect` fails outright**: the first call returns
   `-EPERM`, not `-EINPROGRESS`, because `tcp_connect` hands the refusal to
   `ksock_connect` directly; `tcp_ready` reports `COSMO_IO_ERROR` for a
   second thread polling the same socket. Delete the rule and the same
   sequence connects. This is the assertion `net-output` step 8 has to
   change, and the reason that step was written nonblocking to begin
   with.
3. **A refused SYN-ACK drops the half-open.** As built this runs on the
   **uplink** tap rather than a guest's, and the rule names the client's
   port: every property here is what the *connection* does with a refusal,
   the scope dimension is `net-output`'s, and a SYN from the world needs no
   INPUT seed, which is what makes the case deterministic. `out_refused`
   and `syn_refused` rise, `accept` has nothing, and `out_aborted` and
   `out_recorded` stay put, this connection having no PCB to abort or
   record on. **The drop is separately observable after all**, where this
   report expected only its counter: the same SYN sent again under the same
   rule caches *anew*, because `listen_input` answers a repeat SYN from a
   live entry by reusing its `iss` without touching `syn_cached`. The
   counter alone was proving nothing, which a bug-proof showed. Without the
   rule the same SYN is answered.
4. **A rule added mid-connection records, and does not tear down**: with
   an established connection, the first `send` after the rule returns its
   byte count, `out_recorded` rises, `tcp_state_of` is still
   `ESTABLISHED`, `tcp_ready` reports `COSMO_IO_ERROR`, the *next* `send`
   returns `-EPERM`, and data the peer sent before the rule is still
   readable — the recorded error must not swallow buffered bytes.
5. **A deleted rule lets the connection resume**: with the rule gone, a
   segment leaves, `out_cleared` rises, and `send` succeeds again. As
   built the trigger is the peer's own data — the acknowledgment the host
   owes it is the segment that clears the record — rather than the
   retransmit timer, because an application cannot send its way out of a
   record and the timer would cost the test a second it need not spend.
5b. **A refusal that records while a waiter is already held**, added
   because the bug-proof for keeping that waiter had nothing to catch
   otherwise: with the record cleared, the peer's next segment is the first
   thing refused, so `tcp_input` has taken a reference for the reader of
   that data before the acknowledgment it owes is refused. Replacing that
   waiter rather than keeping it leaks one socket reference, and the
   **socket count** at the end of the test is what sees it.
6. **A stray segment's RST is refused with no connection to tell**: a
   segment for no PCB, with a rule refusing the RST — `out_refused` rises
   while `out_aborted`, `out_recorded` and `out_cleared` do not, and
   nothing faults.
7. **Loopback is untouched**: a rule matching by every other field does
   not affect a `127.0.0.1` connection, which is what keeps the sixteen
   existing TCP selftests meaningful.
8. **UDP and ICMP are unchanged**: `-EPERM` is still immediate from
   `ksock_sendto` and `icmp_send_echo` (asserted here as well as in
   `net-output`, because this unit touches the value they depend on).

**`net-output` step 8** stops asserting the limit and asserts the fix:
its comment ("the documented limit: `batch_send` ignores output errors")
goes, and the nonblocking `connect` must return `-EPERM` on its first
call.

**Bug-proofs** — eight, each reintroduced against the shipped code, the
failure observed and named by **the assertion that actually caught it**,
and the source restored byte-identical every time. Two are not the proofs
this report listed, and the reasons are recorded rather than tidied away.

1. `batch_send` discards the error, as it did before this unit → step 1's
   connect is never told (and `net-output` fails with it, the same verdict
   read from the chain's end).
2. The abort applied to synchronized states too → step 4's `out_recorded`
   never rises, the connection having been torn down instead. This report
   expected the `ESTABLISHED` check to catch it; the counter notices
   first.
3. The record applied to an opening connection instead of the abort → step
   **1b** is never woken. This report said step 1 would hang; it passes,
   because `tcp_connect` returns the refusal whatever the abort does —
   which is what taught this unit what the abort is for.
4. The clearing rule removed → step 5's `out_cleared` never rises.
5. An already-taken `*wake` replaced rather than kept → the socket count
   at the end of the test is short by one. This report predicted "step 2's
   poll never reports", which was wrong twice over: the overwrite cannot
   lose a wake (both references name the same socket), it can only leak
   one, so the proof is a counting test — and on the first run it caught
   nothing at all, because the path was unreachable until step 5b existed.
6. The SYN-cache entry counted but **not dropped** → step 3's repeat SYN
   finds the live entry and `syn_cached` does not rise. This report's
   "the drop removed" could not compile (the helper became unused) and,
   more to the point, the faithful version of the bug *passed*, because
   the step asserted only the counter.
7. The half-open looked up with its tuple reversed → `syn_refused` stays
   zero. This replaces the listed proof "the owner-less sites given the
   looked-up connection", which is not observable: the listener is in
   `LISTEN`, where the rule does nothing, and a stray segment has no
   connection to pass at all. The reversed tuple is the mistake a reader
   of that call would actually make.
8. An empty flush counted as "a segment left" (`rc >= 0` rather than
   `rc > 0`) → the `recv` that drains the buffer clears the record, and
   step 4's post-drain `-EPERM` becomes a success. Not in this report's
   list; found while writing the helper.

**One proof this unit cannot produce, named in advance — and confirmed.**
The discrimination between `-EPERM` and every other output error is not
observable in this stack today: no TCP segment can reach a non-verdict
output error. There is no unroutable destination a socket can
name (the NIC carries a default route — the host-state unit established
this), segments never exceed the MSS so `-EMSGSIZE` is unreachable, and
ARP resolution queues a frame and reports success by design. So the
discrimination is argued from the code and from the comment in `ipv4.c`,
and not from a test. The OUTPUT unit's precedent applies: better a named
non-proof than a proof that does not prove what it claims. **It was run
anyway**, as a ninth experiment — `output_result` keyed on any negative
`rc` — and the whole suite passed, which is exactly what this report
predicted and the only evidence available that the prediction was right.

## Benchmarks

The accepted path gains one signed compare per flush and, for a
connection that has recorded a verdict, one relaxed read of a field the
flush site has already loaded. `net-nicbench` cannot resolve that — it
could not resolve the OUTPUT chain's own fast path, with medians of 15413
and 20626 sends/s across two trees and a 40% spread inside a single
build — so the measurement offered is the one that has worked twice: the
suite's own per-test timing across three consecutive runs on each arch,
with the claim being that no test's time moves outside its run-to-run
spread. As built the suite's total moved from 53.0 s to 58.1 s on aarch64
— and 2.3 s of that is `net-tcpverdict` itself, a test that did not exist
before, one second of which is a retransmit timeout it waits for on
purpose. The unit's real performance argument is the other direction: a
refused `connect` costs about three minutes today and a round trip to the
lock after this.

## Risks

- **`pcb->error` on a live connection is a new state.** Today every
  error comes with a PCB that has ended, and four consumers read the
  field. Each must be re-read against the new assumption, not just the
  one this unit's tests exercise — `tcp_send`, `tcp_recv`, `tcp_ready`,
  `take_error`, `ksock_connect`, `ksock_accept` and `ksock_sendto`'s
  wait. This is the unit's real hazard and the whole-rule-up-front
  decision it needs; the repository's own lesson (a rule stated once must
  be swept everywhere it governs) applies directly.
- **Aborting a `SYN_RCVD` child.** The simultaneous-open child can sit on
  a listener's accept queue, so the abort must use the same teardown the
  RST path uses and not a shortcut. One place, to be read twice.
- **A refused keepalive probe records a verdict the application did not
  ask for.** An idle established connection whose probe is refused
  reports `-EPERM` on the application's next `send`. Correct — the
  connection genuinely cannot send — but it arrives from traffic the
  application never generated. Named, asserted, not mitigated.
- **A second lock acquisition on the refusal path.** Bounded (one lock,
  nothing held, no netif access) but real; the guard that keeps it off the
  accepted path is the compare in `output_result`, which must be checked
  to be the first thing it does.
- **The LISTEN path's reference held longer.** A listener whose close
  races a refused SYN-ACK must still be freed exactly once; the reference
  moves, it is not duplicated.
- **Asserting a timing property.** Step 1's "promptly" is a bound, not a
  measurement, and the harness's budget is what enforces it. That is the
  right shape for this test — the failure it must catch is an unbounded
  wait — but it is the kind of assertion that goes flaky if it is written
  as a millisecond comparison instead of as "the test completes".

## Alternatives considered

- **A PCB pointer and a reference on the batch.** The obvious design, and
  it buys nothing the flush site's existing reference does not already
  provide, at the cost of a reference to acquire and release on every
  send and a batch that can outlive the connection it names.
- **Report the error to the caller of `tcp_send` instead of recording
  it.** Precise, and wrong: the bytes are in the send buffer before the
  flush, so a failing return would either lose them or invite a
  double-send. Recording is what the socket layer is already built for.
- **Abort synchronized connections too.** Simpler — no clearing rule, no
  new live-error state — and it throws away a connection whose peer is
  still talking because of a rule the operator may delete a second
  later. It also makes the machine's own filter the most effective way to
  kill established connections, which is a sharper tool than this unit
  wants to hand out.
- **Treat every output error as a verdict.** Would change the behaviour
  of `-ENETUNREACH`, `-ENOBUFS` and `-EMSGSIZE` paths this unit has not
  designed and, as the named non-proof records, cannot test.
- **Leave the refused SYN-cache entry to expire.** Eight seconds and one
  of sixty-four slots per refused SYN, refillable by the guest whose
  connections the rule exists to refuse. Dropping it is three lines.
- **A verdict on the receive side too** (telling a connection that an
  *inbound* segment was dropped by the host chain). Deliberately not this
  unit: the host chain's whole point is silence, and the connection has
  nothing to report to — the segment was the peer's.

Named and deferred, unchanged by this unit: per-interface chains, rate-limit
and logging targets, IPv6 filtering, full TCP state tracking in the filter,
hairpin/NAT reflection, and a listing of live host flows.
