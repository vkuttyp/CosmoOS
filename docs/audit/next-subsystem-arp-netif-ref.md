# NEXT SUBSYSTEM — the interface an ARP retry still points at

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This report is a design, not
an as-built.

**Subsystem: a `struct netif *` that outlives the lock protecting it.**
ARP and ND entries hold a **bare** interface pointer and take no
reference (`kernel-services/network/arp.c:43`). Their retry paths copy
that pointer out from under the table lock, release the lock, and then
**dereference it** — `send_arp` reads `nif->mac` and `nif->ip4.addr`,
`nd_send` the same. Meanwhile `netif_unregister` flushes those tables
and then drops the registry's reference, which can be the last one.

The inventory files this as one of §4's "small debts" —
"ARP and ND entries hold bare interface pointers and rely on the flushes
in `netif_unregister`". Reading it, it is not small: **the flush does not
close the window, and the barrier that looks like it does is documented
for something else.**

## The window

`arp_age` (`arp.c:238-282`), and `nd_age` identically
(`ipv6.c:262-300`):

```c
    retry[nr_retry].nif = e->nif;      /* bare pointer, under g_lock */
    ...
    spin_unlock_irqrestore(&g_lock, s);
    for (unsigned i = 0; i < nr_retry; i++)
        send_arp(retry[i].nif, ...);   /* dereferenced with no lock, no reference */
```

`netif_unregister` (`netif.c:246-287`) tears down in numbered steps:

- **3.** `synchronize_quiesce()` — every in-flight transmit and `netif_rx`
  has left its read-side section.
- **4.** `rxq_purge` and a `net_work` barrier through every CPU. Its own
  comment says what it is for: *"every worker has finished any
  `input_one` it had started"* — the **receive** path.
- **5.** `arp_flush(nif); nd_flush(nif);`
- **6.** `kobject_put(&nif->obj)` — possibly the last reference.

The barrier at step 4 does drain any `age_work` already queued. It does
not stop a **new** one: `age_work` re-arms itself on a timer every
`ARP_RETRY_NS`, one second (`arp.c:19`, `arp.c:288-301`). So between the
barrier completing and step 5 running, a fresh `arp_age` can take the
lock, copy the doomed `nif` into `retry[]`, release the lock — and then
step 5 clears the table it no longer matters to, step 6 frees the
interface, and the retry loop reads `nif->mac` out of freed memory.

Narrow, and real. Nothing in the tree says the ARP retry depends on that
barrier, because it does not depend on it — it survives by the timer not
having fired.

## Why the existing rule does not cover it

`netif.h:81` states the rule for one case: *"Lookups return a referenced
pointer (`netif_put` when done)"*, with `netif_get`/`netif_put` beside
it. Every `netif_find` caller obeys it. **ARP and ND never looked the
interface up** — it arrives as an argument to `arp_resolve`, is stored,
and outlives the call. The rule is stated where pointers are *returned*
and enforced nowhere for pointers that are *kept*, which is the shape
this tree keeps paying for.

## The second debt, in the same lines

`arp_flush` frees every pending mbuf for the interface:

```c
    m_freem(g_table[i].pending);
    memset(&g_table[i], 0, sizeof(g_table[i]));
```

`arp_age`'s timeout path counts the same event
(`g_stats.pending_dropped++`); the flush path does not count it and
tells nobody. So a packet queued behind an unresolved address vanishes
when its interface goes, with no counter moved and no error returned —
while the socket-verdict unit exists precisely so that TCP's refusals
reach their callers. One line of the same function is careful and the
other is silent.

## Why it matters

- **It is a use-after-free of a kernel object, read in a transmit
  path.** Not a leak, not a latency bug.
- **`netif_unregister` is not rare.** Every tap teardown runs it —
  `/dev/net/tap` close, `vmctl` exit, guest detach — and module unload
  and `netif_unregister` are two of the five synchronous
  `synchronize_quiesce` callers the quiesce work just measured.
- **It is exactly the class the lifetime-windows unit was built to
  find** (`docs/audit/next-subsystem-lifetime-windows.md`), and it went
  unfound: that unit raced `blk_submit`/`blk_unregister` and the TCP
  timer callback, and never looked at the ARP retry.

## Design

**The rule: a `struct netif *` that outlives the lock that found it
holds a reference.** Stated once, swept everywhere, and enforced by
construction rather than by timing.

1. **The retry lists take references.** `arp_age` and `nd_age` call
   `netif_get` as they copy each entry, and `netif_put` after the send.
   That is the whole fix for the window, and it cannot be defeated by a
   timer: the interface cannot be freed while the retry holds a
   reference, and `netif_unregister`'s step 6 becomes a `put` that is
   simply not the last one.
2. **The entries themselves take references, or the report says why
   not.** Holding one per entry is the stronger property — an entry can
   then never name a dead interface — at the cost of a flush being the
   only thing that lets an interface go. This report proposes **the
   retry lists only**, and states the reason: the flush at step 5
   already clears entries, so a per-entry reference would keep
   interfaces alive until `arp_age` next ran rather than fixing
   anything, and would turn a missing flush from a dangling pointer into
   a leak. The dangling pointer is the defect; the flush is not.
3. **Sweep every holder of a `netif *`.** The two retry lists are what
   this report found; the sweep is part of the unit, not an afterthought,
   because "a rule stated in one place" is how this arrived.
4. **The silent drop gets a counter.** `arp_flush`/`nd_flush` move
   `pending_dropped` like the timeout path does, so the tally of packets
   that vanished because an interface went is visible in
   `arp_get_stats`.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/arp.c` | `netif_get`/`netif_put` around the retry list; `pending_dropped` in `arp_flush` |
| `kernel-services/network/ipv6.c` | the same for `nd_age` and `nd_flush` |
| `kernel/include/kernel/netif.h` | the rule stated where the reference API is, not only for lookups |
| `docs/kernel-services/network/design.md` | the ownership rule beside the interface lifetime |
| `docs/kernel/quiesce/invariants.md` or the network invariants | the rule as an invariant, with what enforces it |
| `README.md` | the Status entry |

## New APIs

None. `netif_get`/`netif_put` already exist and are what the fix uses;
this unit is about a rule and its sweep, not new machinery.

## Tests

| test | asserts |
| --- | --- |
| `net-arp-retry-unregister` | the race, constructed deterministically: a pending entry, an `arp_age` stopped between the unlock and the send, `netif_unregister` run to completion, then the send released. The interface is still alive and the send touches live memory |
| `net-arp-flush-counts` | a flush with pending packets moves `pending_dropped`, so the drop is visible rather than silent |
| `net-nd-retry-unregister` | the same race on the ND side, because the defect is in both and a fix in one is half a fix |

**The bug-proof.** With the references removed, `net-arp-retry-unregister`
must fail — and the frame poisoner the rare-crash unit added
(`docs/audit/...`, poisoned freed pages) is what turns "it read freed
memory" from a hope into a check, because an unreferenced netif that
happens to still hold plausible bytes proves nothing.

**The adversary is built from the mechanism, not a stopwatch.** The
window is between an unlock and a send, so the test stops the retry
*there* — a hook in the retry path, the way `tcp-pcb-timer-free` parks a
timer callback — rather than racing two threads and hoping. A test that
runs `netif_unregister` next to `arp_age` and waits for a crash is the
family `docs/testing/flakes.md` exists to keep out.

## Risks

- **A reference held across a send could delay teardown.** It is one
  send, and `netif_unregister` already waits for a grace period and a
  per-CPU barrier; this adds strictly less than either.
- **The sweep may find holders this report did not.** That is the point
  of doing it as a sweep; if it finds none, the rule is still worth
  stating where the reference API is, since its absence is what allowed
  two of them.
- **The window is narrow enough that no test will catch it by racing.**
  Which is why the design does not try: the hook is the test.

## Alternatives considered

- **Send under the lock.** Removes the window and takes a spinlock into
  a transmit path that allocates. Rejected.
- **Re-validate the entry after re-taking the lock.** Checks that the
  *entry* still exists, which is not the question — the pointer was
  copied out and the interface can be freed whether or not the entry
  survived.
- **Rely on the step 4 barrier and document that.** It is not true: the
  timer re-arms, so a fresh `age_work` can start after the barrier. A
  comment asserting a safety that depends on a one-second timer not
  having fired would be worse than the silence there now.
- **Leave it as a "small debt".** It is a use-after-free in a transmit
  path reached by every tap teardown. The inventory's own framing is
  what this report disagrees with.
