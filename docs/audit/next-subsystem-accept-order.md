# NEXT SUBSYSTEM — net-lo-tcp assumes accept returns the first client to connect

> **Status: built (PR #269).** As designed, with these specifics:
>
> - **The hook is guarded by `CONFIG_DEBUG`,** as `tcp.c`'s other test
>   hooks are; the report's first version said `CONFIG_SELFTEST`, and
>   named two functions. The sections below are written as built. There
>   are three: `tcp_test_hold_ack(port)`,
>   `tcp_test_ack_held()` (the test's wait needs to see the hold), and
>   `tcp_test_deliver_held_ack()` (disarms, and returns whether anything
>   was held). The hook runs in `tcp_input` right after the header
>   pull-up, before any lock. One compare-and-swap on the port takes one
>   segment.
> - **A failed forced run leaves nothing half-open.** The hold is
>   released through `selftest_defer`, and the forced wait delivers the
>   held ACK whether or not the wait succeeded.
> - **Ports:** `net-lo-tcp` 6002, as before; `net-accept-order` 6004.
>   `net-accept-order` is registered right after `net-lo-tcp`.
> - **Measured**, one debug boot each, both architectures:
>
>   | boot | result |
>   |---|---|
>   | plain | PASS; `net-lo-tcp` accepted the first client; `net-accept-order` accepted the second, ok (96 ms x86-64, 106 ms aarch64) |
>   | the step back to receiving on `c1` | `net-accept-order` fails on the bounded readable wait in ~2 s (`wait_until(ready_has, ...)`), and the boot runs on; `net-lo-tcp` passes |
>   | `net-accept-order` without its hold | fails in ~2 s: "the forced order did not happen" |
>   | `tcp_accept` LIFO (`list_push_front` on queueing) | `net-accept-order` fails `!accepted_first`; `net-lo-tcp` passes, accepting the second client |
>
>   The first attempt at the `c1` mutation did not compile (`mine`
>   unused, `-Werror`); it was rerun with `(void)mine`, and both boots
>   were confirmed to start.
> - **The docs:** `docs/kernel-services/network/testing.md`, flakes (the
>   sighting explained), README.

## Problem

`docs/testing/flakes.md` ("`net-lo-tcp` hung, with the test thread
blocked on a socket") records one sighting. It came from a local aarch64
debug boot on 2026-09-30, the watchdog-spent unit's M2 mutation, which
changes nothing near the network. `net-lo-tcp` never returned, and the
per-test watchdog printed its dump at 8 s:

```
   1 kmain                blocked   32   3      28192     4918 socket
```

Every CPU was idle, and no other test thread existed. That is the first
hang after position 180 to carry a dump: before PR #267 the watchdog was
already spent by then.

**The step that fits.** `net-lo-tcp`'s listen-backlog step
(`kernel-services/network/nettest.c:1177-1197`) works like this:
1. Two clients, `c1` then `c2`, connect to a listener with backlog 2
   that has not accepted.
2. The test accepts once, sends `hi` on the accepted socket, and
   receives it **on `c1`** (`:1193`).
3. It closes the listener and expects the reset **on `c2`** (`:1197`).

That assumes the accept returned `c1`'s connection. `tcp_accept` returns
the first live connection in the listener's accept queue
(`tcp.c:1174-1190`). A connection joins that queue when the listener
processes the client's final ACK (`tcp.c:1767`). That happens on the
network worker the ACK's flow hashes to (`netif.c` `steer`: the hash
covers the ports, so `c1`'s and `c2`'s ACKs usually go to different
workers).

`ksock_connect` returns on the SYN-ACK, before the listener has
processed the ACK. So nothing orders `c1`'s queueing before `c2`'s. It
only happens to come first because `c1`'s ACK is queued on its worker
before `c2`'s SYN is sent. If that worker does not run until `c2`'s
whole handshake has finished on the others, `c2` is queued first. Then:
- `hi` goes to `c2`;
- `recvfrom(c1)` waits for data nobody will send: an established,
  idle connection with no timer armed;
- the test thread blocks on `socket`, and every CPU is idle.

That is the sighting's dump.

`net-lo-tcp` prints nothing until it ends, and the dump names only the
wait channel, not the blocked thread's stack. So the sighting's log
cannot say which of the test's waits it was in. This is the step that
fits and that the probe reproduces, not a finding read from that log.

### Measured

`tools/accept-order-probe.py` has three parts:
- **The instrumentation** prints which client the accept returned
  (`LTPROBE: accepted c1|c2`, from the accepted socket's peer port
  against `c1`'s local port).
- **`--force`** makes `c2` first, deterministically. `tcp_input` holds
  the first bare ACK to port 6002 (`c1`'s, since `c1`'s handshake is the
  first to that port) without processing it. After `connect(c2)` the
  test waits, bounded at 2 s, until that ACK is held and the listener is
  readable (`c2` queued). It then delivers the held ACK through
  `tcp_input`. A wait that times out fails the step instead of passing
  untested.
- **`--fix`** is the candidate (§1).

| run | arch | accepted | `net-lo-tcp` | the boot |
|---|---|---|---|---|
| instrumented | x86-64 | `c1` | ok (2773 ms) | PASS |
| instrumented | aarch64 | `c1` | ok (2730 ms) | PASS |
| `--force` | x86-64 | **`c2`** (c1's ACK held, c2 queued first) | never returns: `[WATCHDOG] no progress for 8016 ms`, `kmain blocked ... socket` | timed out at 180 s |
| `--force` | aarch64 | **`c2`** | never returns: `no progress for 8003 ms`, `kmain blocked ... socket` | timed out at 180 s |
| `--force --fix` | x86-64 | `c2` | ok (2821 ms) | PASS |
| `--force --fix` | aarch64 | `c2` | ok (2772 ms) | PASS |

Each row is one debug boot, run one at a time. The forced runs
reproduce the sighting's dump on both architectures: the test thread is
blocked on `socket`, every CPU is idle, and the thread table is the
sighting's: `kmain` and the idle, reaper, quiesce, network-worker and
driver service threads, nothing else.

### Why it matters

- **A correct stack hangs the boot.** The check asserts an order the
  stack does not promise. When the order goes the other way, the boot
  does not fail a check; it hangs until the harness's 180 s timeout, and
  every test after `net-lo-tcp` (the 273rd of 411) goes unrun.
- **The documentation already says the right thing.**
  `docs/kernel-services/network/testing.md` describes the step as "a
  listener with backlog 2 accepts **one of** two queued connections".
  It then names `c2` as the one reset, which is the code's assumption
  again.

**No other TCP test makes the assumption.** Every test that connects
more than one client before it accepts was checked:
- `net-accept-race` (`nettest.c:2024-2076`) checks each accepted socket
  only in general terms.
- `net-tcp-syncache` (`:2266-2301`) has one client that completes a
  handshake; its 300 injected SYNs never do.
- The host firewall test (`:6735-6893`, the `hin_*` segments) completes
  at most one handshake per accept.
- `bench_tcp` (`:3431-3460`) gives each flow its own listener.

**One AF_UNIX test does make it, and there it holds.**
`kernel/ipc/unixtest.c:128-146` connects `c` and `c2`, accepts once, and
assumes it got `c`. There that is guaranteed: `unix_connect` queues the
connection itself, synchronously (`list_push_back`, `unix.c:641`), and
`unix_accept` pops the front (`list_pop_front`, `unix.c:665`). Connect
order is queue order. This unit does not change that; it adds a comment
to the test saying it depends on it.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| the step | `nettest.c:1177-1197` | connect `c1`, `c2`; accept `a1`; `hi` from `a1` received on `c1`; close the listener; `c2` reset |
| accept | `tcp.c:1174-1190` | the first live connection in `accept_queue` |
| queueing | `tcp.c:1744-1770` | on the listener's processing of the client's final ACK: `list_push_back` |
| steering | `netif.c` `steer` | a flow's packets go to `hash % ncpu`'s worker; the hash covers addresses and ports |

## Design

### 1. The step asks which client it accepted

After the accept, the step reads `c1`'s local address. The accepted
socket's peer port says which client it is:
- `mine` is that client;
- `other` is the one still queued.

`hi` is received on `mine`, and the reset is expected on `other`. The
step logs which it was (`net-lo-tcp: accepted the first|second client`),
so a run shows its order.

No stack change. The stack's behaviour, first-completed first-accepted,
is what TCP promises: accept order follows handshake completion, not
`connect()` calls.

### 2. The other order, made certain

**The backlog step becomes one helper, run in both orders**:
`lo_tcp_backlog(port, force)`. `net-lo-tcp` runs it unforced, as today.
A new self-test, `net-accept-order`, runs **the same helper** forced, so
the step that hung is the step under test (Greptile, #268). A separate
test of the other order would pass while the step itself regressed.

The forcing seam is a debug hook in `tcp_input`, the probe's hold made
permanent under `CONFIG_DEBUG` (as `tcp.c`'s other test hooks are). Armed with a listener port, it holds
that port's first bare ACK and hands it back to the test. Forced, the
helper:
1. connects `c1` and `c2` with `c1`'s ACK held;
2. waits, bounded, for `c2` to be queued;
3. delivers the held ACK, even when the wait timed out, so a failed run
   leaves no half-open connection;
4. asserts the forced order happened (`c2` queued first);
5. then runs the step's own checks, identical in both orders.

**The step's receive is bounded.** Before it receives `hi`, the helper
waits, with a deadline, for the client it chose to become readable. A
step that picks the wrong client then fails on that wait ("hi did not
arrive on the client it chose") instead of blocking until the harness's
timeout. Both orders, one code path, and a regression fails in seconds.

In addition, `net-accept-order` asserts what the forced run shows about
the stack: the accept returned `c2`, completion order and not
`connect()` order.

## Affected files

| file | change |
|---|---|
| `kernel-services/network/nettest.c` | the backlog step as `lo_tcp_backlog(port, force)`, which asks which client it accepted and bounds its receive (§1, §2); `net-lo-tcp` calls it unforced, `net-accept-order` forced |
| `kernel-services/network/tcp.c` | the ACK hold hook, `CONFIG_DEBUG` only |
| `kernel/include/kernel/net/tcp.h` | the hook's declaration |
| `kernel/core/selftest.c` | registers `net-accept-order` |
| `docs/kernel-services/network/testing.md` | the step's description; `net-accept-order` |
| `kernel/ipc/unixtest.c` | a comment: the backlog step relies on `unix_connect` queueing in connect order, which `unix.c` guarantees |
| `docs/testing/flakes.md` | the sighting explained |
| `README.md` | Status entry |

## APIs

- Kernel, debug builds only:
  - `tcp_test_hold_ack(port)` arms the hold;
  - `tcp_test_ack_held()` says whether a segment is held;
  - `tcp_test_deliver_held_ack()` disarms, delivers the held segment, and
    returns whether there was one.

  Nothing outside the self-tests calls them.

## Tests

| test | proves |
|---|---|
| `net-lo-tcp` | the backlog step passes in either order and says which it took |
| `net-accept-order` (new) | the same backlog step, forced into the order that hung: it passes, and the accept returned the later-connecting client whose handshake completed first |

**Planned mutations** (each alone, both architectures, boot confirmed):
- the backlog step back to receiving on `c1`: `net-lo-tcp` still passes
  (unforced, `c1` is accepted), and `net-accept-order` fails on the
  bounded readable wait, because `hi` went to `c2`. It fails in seconds;
  it does not hang.
- `net-accept-order` without its hold: the order is not forced, the
  test's wait times out, and it fails ("the forced order did not
  happen"), not a silent pass.
- `tcp_accept` taking the queue's tail instead of its head:
  `net-accept-order` fails on its first accept.

## Benchmarks

None.

## Risks

- **The hold hook sits in `tcp_input`,** the receive path of every
  segment. It is compiled only with `CONFIG_DEBUG`, and when it is
  not armed it costs one atomic load. The hook takes the segment before
  the TCP lock, so holding it blocks no other connection.
- **A held ACK is a real segment kept out of the stack for up to 2 s.**
  The client is established, and the listener's SYN cache keeps the
  half-open entry (TTL 8 s). Nothing retransmits a bare ACK, so the only
  thing the hold delays is that client's queueing, which is the point.

## Alternatives considered

- **Accept both connections, then match.** This also removes the
  assumption, but it changes what the step tests: "a listener that
  never accepts" becomes one that accepts twice, and the reset of a
  still-queued connection on close would no longer be exercised.
- **Serialise the handshakes by steering both flows to one worker.**
  That makes the test pass by making the machine less like itself. The
  order is not a property the stack has, so the test should not
  manufacture it.
