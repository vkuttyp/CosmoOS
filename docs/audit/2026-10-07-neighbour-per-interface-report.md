# Neighbour entries are (interface, address) -- 2026-10-07

Branch `neighbour-per-interface` from `main` at `b6b9393a`. The defect
the host-modes follow-up left open
(`docs/audit/2026-10-06-flake-triage-report.md`, §7.5): the IPv4 ARP
table was keyed by address alone.

## 1. Scope and baseline

`kernel-services/network/arp.c` kept 64 entries, each recording the
interface the resolution was for (`e->nif`), and found them with
`find(ip)`, which ignored it. Every operation went through that finder:

- `arp_resolve(nif, ip, mac, m)` returned another interface's MAC for the
  same address, or parked this interface's packet on another interface's
  incomplete entry -- sending no request of its own, with the entry's
  retries going out on the other interface, up or down;
- `arp_input(nif, m)` completed or refreshed the entry for a sender
  address whatever interface the frame arrived on, and sent the parked
  packet out of the arriving interface;
- `arp_lookup(ip, mac)` took no interface at all, so `net-nicbench`'s
  gateway wait (PR #318) could be satisfied by `eth0`'s entry while
  `eth1`'s resolution had not happened.

The IPv6 neighbour cache (`ipv6.c`, `nd_find(ip)`) had the same
shape, where it matters more: a link-local address is per link by
definition, and `fe80::1` on two links is the ordinary case.

The callers: `ipv4.c` (`output_on` -> `arp_resolve`), `ether.c`
(`arp_input`), `netif.c` (`arp_flush`/`nd_flush` at unregister), `ipv6.c`
(`ipv6_output` -> `nd_resolve`), and the tests. `tap.c`, `nat.c` and
`fw.c` do not consult either table; the firewall's host-flow cache and
NAT's bindings are keyed by address tuples by design (a host flow is a
host flow whichever interface carries it).

QEMU hid the defect: both user-mode backends answer `10.0.2.2` with the
same MAC (`52:55:0a:00:02:02`), so a frame sent out of `eth1` to `eth0`'s
answer still reached a gateway. On a real machine the same address on
two links -- two NICs each with a gateway at `.1`, or any two links with
`fe80::1` -- is ordinary, and the stack would have sent one link's
traffic to the other link's MAC.

## 2. Proof first: a test that fails on the old keying

`net-arp-per-interface` and `net-nd-per-interface`
(`kernel-services/network/nettest.c`): two fake interfaces whose transmit
hook records each frame's EtherType and destination MAC, one neighbour
address on both links (`10.75.0.254`; `fe80::1`), two MACs. The checks,
in order:

1. Each interface's resolution sends its own request out of itself. Under
   the old keying the second interface's transmit count stays 0: its
   resolution found the first's incomplete entry and parked its packet
   there. **This is the check that fails.**
2. A reply fed in as the first interface's completes the first's entry and
   sends its packet out of the first to the first's MAC; the second stays
   incomplete, nothing leaves it.
3. The second's reply, a different MAC, completes only the second's; the
   first's entry is unchanged.
4. The sighting's shape: both entries deleted, the first resolves (entry
   incomplete), the first interface goes down (its entry flushed, the
   parked packet counted dropped), a reply arriving on the down interface
   teaches it nothing, the second resolves: it must send a request of its
   own and complete from its own reply; nothing of the second's ever left
   the first.

The first version of the test also called `arp_age(now + 1.5 s)` to see a
retry leave each interface. Review pointed out that this moves the shared
table's clock -- the hazard of §3 -- so the step is gone; the retry's
interface is the entry's own by construction (`retry[i].nif = e->nif`,
unchanged, asserted by `net-arp-retry-unregister`).

The ND test is the same shape over `nd_resolve`/`nd_input_na`, with
`fe80::1`, and adds that the first interface going down starts its
resolution over without touching the second's entry.

`tools/arp-per-interface-probe.py --old` builds a clone with the
address-only match restored in both finders (`find`, `nd_find`), the two
tests wrapped to panic with both verdicts, and boots it; without `--old`
the committed keying:

| | x86-64 | AArch64 |
| --- | --- | --- |
| `--old` (address alone) | both FAIL: `check failed: b.transmits == 1` (lines 2052, 2163) | both FAIL, the same two checks |
| fixed (interface, address) | both PASS | both PASS |

Four boots of about 90 s to the verdict. The failing check is the first
one that distinguishes the keyings: the second interface's resolution
sent nothing because it found the first's entry. Under the old keying
the tests do not reach the later checks, so the probe does not show the
wrong-interface transmit directly; the test's steps 2-4 are what that
would have failed at.

## 3. The change

**Keying** (`arp.c`, `ipv6.c`): `find(nif, ip)` and `nd_find(nif, ip)`
match the interface and the address. `arp_resolve`, `arp_input`,
`arp_lookup`, `arp_delete`, `nd_resolve`, `nd_input_ns` and
`nd_input_na` go through them, so a lookup, an insertion, a completion
and a removal are all one interface's. `alloc_entry`/`nd_alloc` already
recorded the interface; ageing already retried on the entry's own.
`arp_lookup` and `arp_delete` take the interface; the one-argument forms
are gone and every caller names it (eleven sites, all in `nettest.c`).

**Down** (`netif.c`, `arp.c`, `ipv6.c`): `netif_set_up(nif, false)`
flushes the interface's ARP and ND entries, as `netif_unregister` step 5
does, and `arp_input`/`nd_input_*` learn and answer nothing on an
interface that is not up (review: a frame queued before the down and
input after the flush would otherwise carry a MAC across the transition). The reasons, now
invariant **N25** in `docs/kernel-services/network/invariants.md`: a MAC
learned over a link that is down is not known to be there when it comes
back; a packet parked on an incomplete entry of a down interface waits
for a link that is not there, and its retries could only fail at
`netif_transmit`'s `NETIF_UP` check; and the routing lookup never
chooses a down interface, so a down interface's table state is dead
weight. The flush takes only the table locks, so it is safe from every
context `netif_set_up` is called from; a transmit or receive already
past the flag check finishes on the live interface under its read-side
section (N-L1), which the flush does not wait for and need not.

**What the tests no longer need.** `net-second-nic` and `net-nicbench`
aged the whole table forward by an hour (`arp_age(now + 3600 s)`) to
make the second interface's resolution observable past the first's
entry. That hack had a hazard of its own: `arp_age` stamps every
*incomplete* entry it retries with the `now` it is given, and
`clock_delta_ns` clamps a future stamp to zero, so an incomplete entry
present at that call stopped retrying for an hour. Both tests now use
the first interface's down (which flushes it) and `arp_delete(second,
gw)`; no test moves the table's clock.

**Smallest correct change:** no change to the table's size, locking,
eviction, learning rules (N13) or ageing; no change to `ipv4_route`; no
interface added to the stats. The diff is the finders, the two
signatures, the down flush, the callers, two tests and a probe.

## 4. `net-nicbench`: an unresolved gateway is a failure again

With the entry the interface's own, the ways the warm-up's 1.5 s wait can
expire are a reply lost twice (the request and its 1 s retry) on a link
whose ARP phase has just completed 2,000 round trips, or a defect. Neither
is a thing to report and carry on from: the test now fails with `the
gateway's ARP entry did not resolve within 1.5 s on this interface`, and
the line before it says how many requests the warm-up sent (`+1` is a
request without an answer; `+0` would again mean an entry was found,
which N25 makes this interface's own, so an incomplete entry of its own
from an earlier test). The "measured nothing" guard from PR #318's review
is gone with the branch it guarded. Evidence that the failure does not
fire: in the eight debug boots of §6 (one, two and four CPUs, chaos, both
architectures) both interfaces read `gateway arp reachable (after 0 ms)`
on every boot -- `eth1`'s entry now being its own, resolved by
`net-second-nic` a few tests earlier and no longer aged away by the
hour-forward hack, the warm-up finds it already reachable (`+0 requests`
with a reachable entry is the entry already there; the failure line
would carry `+1`).

**What the sighting turns out to have been.** Not attributed to the
keying with certainty, and the three logs cannot be: all three show the
second interface's own ARP phase completing with every one of its 2,000
replies counted, so the link was answering; the old code took its
request count after the warm-up, so `+0` says nothing about the warm-up's
request; and whether a stale `10.0.2.2` entry on `eth0` existed at that
moment is not recorded. The address-only keying is a mechanism that
produces exactly the sighting (the second interface parked on the first's
entry, the retry out of the downed first) and it is gone; a lost reply is
the other, and the new line distinguishes them if it recurs.

## 5. Other global-by-address state, surveyed

- **`ipv4_route(dst)`** picks the egress by destination: a connected
  subnet by longest prefix, else the default interface; both skip down
  interfaces. Two up interfaces on the same subnet tie on prefix and the
  first registered wins -- deterministic, and a policy question (which
  link for a shared subnet), not a defect; the boot test's two backends
  are such a pair and route through `eth0` until it goes down. Left alone.
- **The pending packet** was per entry, so per address; it is per
  (interface, address) now by the keying. Still one packet per entry.
- **The ICMP rate limiter** (`net-icmp-limit`) is per host, as a limit on
  what this host emits should be. Left alone.
- **`netif_owns_ipv4`/`_ipv6`** answer for the host across interfaces --
  a datagram to any of our addresses is ours whichever interface it
  arrived on (weak host model). Left alone.
- **The firewall's host flows and NAT's bindings** are address tuples;
  the egress is read where the rule needs it (`fw_output_verdict(nif, ...)`).
  Left alone.
- **ND's `nd_input_ns`** learned the asker without recording the
  arriving interface on an existing entry; keyed per interface it can no
  longer find another interface's entry, so the omission is moot.

## 6. Validation

On the committed tree, one QEMU at a time, QEMU at its default priority
(pri 31 nice 0, checked as `docs/development.md`, "Benchmark runs", asks;
no loop started with a zsh `&`):

| step | x86-64 | AArch64 |
| --- | --- | --- |
| `make host-test` | passed (62 s) | -- |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 139.0 s, 429 self-tests | PASS 133.2 s |
| `make test-smp2` | PASS 143.7 s | PASS 140.0 s |
| debug boot, 1 CPU | PASS 128.9 s | PASS 122.4 s |
| `make test-chaos` | PASS 130.5 s | PASS 133.3 s |
| release build and boot | PASS 16.7 s | PASS 20.2 s |

The network harness passed in every debug boot. `net-arp-per-interface`
and `net-nd-per-interface` passed in all eight (13-23 ms); every other
network test and budget is unchanged. `net-nicbench` 2.1-2.4 s. The four
probe boots are in §2.

## 7. Unattributed

- Whether the 2026-10-06 sightings were the stale-entry path or a lost
  reply (§4).
- The two-way tie in `ipv4_route` for two up interfaces on one subnet
  (§5) is a policy without a knob; recorded, not changed.

## 8. Addendum, 2026-10-07: the two windows left in "a down interface holds no entries"

PR #319 said a down interface holds no neighbour entries, and left two
windows in it:

1. `arp_input` and `nd_input_*` tested `NETIF_UP` *before* taking the
   table lock. An input that read "up", lost the CPU while
   `netif_set_up(false)` cleared the flag and flushed, then took the lock
   and learned the asker's MAC on the down interface.
2. `arp_resolve` and `nd_resolve` did not test the flag. A send racing the
   down allocated a fresh incomplete entry, packet parked, after the flush
   -- an entry whose retries could only fail at `netif_transmit`.

**The change.** Both tables read the flag under their lock, in input and
in resolve; a resolve on a down interface frees the packet, counts it
with the flush's drops (`pending_dropped`, `nd_pending_dropped`) and
returns `-ENETUNREACH`, which `output_on` hands back as it does any
resolve error. `netif_set_up` clears the flag with a release store under
`nif->lock` and only then flushes; the other writers of the flag word
(`netif_set_forward`, `netif_set_masquerade`, `netif_unregister` step 1)
publish with the same release store, since the data paths read the word
with lock-free acquire loads (`netif_register`'s clear of `GONE` is
before publication). The argument is in the design document and N25:
the two critical sections on a table lock are ordered, and whichever is
second sees the other's effect.

**Proof.** `net-neigh-down-race` (debug builds): a debug hook parks the
next `arp_input`/`arp_resolve` (`arp_test_hold_lock_entry`) or
`nd_input_ns`/`nd_resolve` (`nd_test_hold_lock_entry`) exactly between
its decision to proceed and its taking of the lock; the test takes the
fake interface down while the caller is parked, releases it, joins it,
and looks: no entry, `-ENETUNREACH` from the resolves with the packet
counted dropped. `tools/neigh-down-race-probe.py --old` restored the old
order everywhere at once (input checks before the park point, no resolve
checks) when the table below was made; it now takes one race at a time
(`--old arp-input|arp-resolve|nd-input|nd-resolve`, each failing at that
race's own check; results in the completion-waits report of 2026-10-08):

| | x86-64 | AArch64 |
| --- | --- | --- |
| `--old` | FAIL: `check failed: !arp_lookup(&d.nif, r.ip4, mac)` (line 2244): the asker learned on the down interface | FAIL, the same check |
| fixed | PASS | PASS |

Four boots of about 90 s to the verdict. The old order fails at the first
of the four races (ARP input); the other three (ARP resolve allocating
after the flush, ND input, ND resolve) are the same window and the test
does not reach them under the old order.

**Validation**, on the committed tree, one QEMU at a time at its default
priority (pri 31 nice 0), no `&` loops:

| step | x86-64 | AArch64 |
| --- | --- | --- |
| `make host-test` | passed (62 s) | -- |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 138.8 s, 430 self-tests | PASS 131.5 s |
| `make test-smp2` | PASS 137.8 s | PASS 141.0 s |
| debug boot, 1 CPU | PASS 121.1 s | PASS 122.7 s |
| `make test-chaos` | PASS 133.7 s | PASS 140.7 s |
| release build and boot | PASS 16.6 s | PASS 20.0 s |

The network harness passed in every debug boot; `net-neigh-down-race`
passed in all eight (4 ms), every other network test and budget
unchanged.
