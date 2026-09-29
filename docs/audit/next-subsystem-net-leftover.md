# NEXT SUBSYSTEM — a failed network test keeps its taps, and the tests after it fail for them

> **Status: built (PR #257).** As designed, with these specifics:
>
> - **The release list:** `selftest_defer`, `selftest_release` (run one
>   early, by argument) and `selftest_forget` (drop one without running
>   it, for a test whose own release is under test, as `irq-route`'s
>   line is). It holds 64 entries per test, is locked, and a full list
>   fails the test by name.
> - **Wrappers:** the network tests use `nt_` wrappers, applied by a
>   word-boundary rename across `nettest.c`: 23 tap creations, 3 service
>   starts, 53 socket creations and 14 accepts, 9 interface
>   registrations, and 24 file opens, with their releases.
> - **Hooks and settings:** the loopback filter, rx hook, steering,
>   keepalive and FIN_WAIT_2 wrappers register a restore. This was found
>   in the build: `net-steer`'s hook took a frame pointer, so a failure
>   left a hook that the next packet would call with a dead frame.
> - **Threads:**
>   - `tcp_server` (`tcp_sink_thread`, `holding_server`): published
>     sockets, and a reference taken for the shutdown.
>   - `hin_connect`: it abandons after 2 s, and one exchange decides who
>     puts.
>   - `race_client`: a stop flag.
>   - The DNS responder: stopped, unblocked and joined.
>   - The harness's echo threads put their own references.
>   - The tests that already released on every exit are unchanged: the
>     TCP timer, ARP and ND retry, steer-injector, both benchmark, and
>     tap-ready tests.
> - **The runner's census:** `nettest_census` (interfaces by name and
>   count via `netif_names`, `tapsvc_count`, `socket_count`), taken
>   before and after every test. After review, only what a test **adds**
>   is a leftover. An abandoned `hin_connect` puts its socket when its
>   connect times out, possibly during a later test, which must not be
>   failed for it. Interface counts are compared too, so a name that did
>   not fit the 512-byte buffer still counts. The sockets abandoned
>   connect threads still hold are left out of the count
>   (`g_nt_abandoned`), so one closing during a later test can't hide a
>   socket that test leaves. Both counts change only on the runner's
>   thread: an abandoned thread whose connect returns hands its socket to
>   a list, and the runner puts it and drops the count at the start of its
>   next census. (Two reviews' worth of alternatives failed first: plain
>   reads could be one low in a "before" census, a false addition after,
>   and a sequence broke with two threads closing at once.)
> - **Interface references:** after review, a fake interface's creator
>   reference, a `netif_find` reference and `net-csum-offload`'s kept
>   packet are released however the test returns, through static
>   `nt_netif_ref` holders. This covers the ARP and ND flush-count tests,
>   `net-netif-lifetime`, `net-steer` and `net-csum-offload`. The
>   forced-midpoint pass was rerun after the change: 50 of 50 on both
>   architectures, nothing left, no hang or panic.
> - **`net-dns`:** asserts `s1.dns_expired - s0.dns_expired >=
>   s0.dns_pending`.
> - **Forcing:** `tools/net-leftover-probe.py --count-checks` and
>   `--force last|mid` make every counted test (50) fail at a chosen
>   check, in one boot.
>   - At the last check: 50 of 50 forced on x86-64 and 49 of 50 on
>     aarch64 (`net-icmp-limit` passed fewer checks on that run), with no
>     leftover, hang or panic.
>   - At the midpoint: the first run hung both architectures in
>     `net-steer`. Its interface, like `net-csum-offload`'s, was on the
>     stack, and the runner's `netif_unregister` touched a dead frame.
>     Both are now static. The rerun forced 50 of 50 on both
>     architectures: exactly those failed, nothing was left, and nothing
>     hung or panicked.
> - **The sighting's forcing on the built tree** (`--slow-dns 600 --gap
>   50`): 56 and 58 queries landed in the pause, the aging reclaimed
>   all 128, `net-dns` passed, and nothing followed.
> - **Verified:** debug, release, the GIC boot and chaos boots pass on
>   both architectures, and `host-test` and `analyze` are clean. A debug
>   x86-64 boot failed `net-hostinput` twice, the third and fourth
>   sightings of one zero-window check (recorded in `flakes.md`), and the
>   tests after it passed.
>
> Mutations, each alone, with its boot confirmed:
>
> | mutation | result |
> |---|---|
> | `net-dns`'s aging removed | `net-dns` FAIL at the new check (x86-64) |
> | `irq-route`'s count made to fail, the sightings' shape | `irq-route` FAIL, and **`irq-affinity` passes** on both architectures (it failed with `-EBUSY` in every sighting) |
> | a forced `net-dns` whose tap's release is not registered | the runner names it: `net-dns left the network changed: interfaces (3) [lo,eth0,eth1] -> (4) [lo,eth0,eth1,dns]`, and no later test fails for it |
> | the forced-midpoint run with the stack interfaces (as found) | both boots hung in `net-steer`, the reason for the fix |

## Problem

One local aarch64 boot (2026-09-29, `docs/testing/flakes.md`) failed nine
self-tests:

```
SELFTEST: net-dns        ... FAIL: check failed: s1.dns_pending == 0 && s1.dns_expired > s0.dns_expired at line 4817
SELFTEST: net-multiguest ... FAIL: check failed: vfs_open(NULL, "/dev/net/tap", ...) == 0 ...
SELFTEST: net-firewall   ... FAIL: (the same open)
SELFTEST: net-input      ... FAIL: (the same open)
SELFTEST: net-hostinput  ... FAIL: (the same open)
SELFTEST: net-hoststate  ... FAIL: check failed: svc != NULL
SELFTEST: net-flows-fw   ... FAIL: (the same open)
SELFTEST: net-output     ... FAIL: (the same open)
SELFTEST: process-user   ... FAIL: check failed: status == 0
```

Two defects, one feeding the other.

**1. `net-dns`'s expiry check asserts a state the test does not
control.** Step (5) floods the service's pending table (128 entries)
with 400 queries. It waits for the first drop, ages every entry
(`tapsvc_dns_age(now + 10 s)`), then asserts that nothing is pending.
But the wait ends at query 129, while the rest of the flood may still be
queued for the service's DNS thread. A query that arrives between the
aging and the check takes a slot the aging just freed. The service is
right to answer it, and `dns_pending == 0` fails.

**2. A failed network test keeps everything it made, and the tests after
it fail for it.** `net-dns` makes a tap (`dns`), a DHCP/DNS service,
sockets and a responder thread, and releases them only on its last
lines. Every `CHECK` before those lines returns holding them. The
service holds one of the eight service slots (`TAPSVC_MAX`).
`net-multiguest` then opens eight guest taps (`/dev/net/tap`, whose open
starts a service). The eighth open fails for want of a slot, and that
`CHECK` returns holding the other seven. Now all eight slots are held
for good, and every later test that opens a tap fails, keeping whatever
it had made when it did.

### Measured

`tools/net-leftover-probe.py` logs, whenever they change across a test,
the network interfaces (by name: a swap that keeps the count shows
too), the live DHCP/DNS services and the socket count. At `net-dns`'s expiry step it logs the queries the service
had handled when the wait ended and at the check. It adds no delay of its
own: a first version slept 100 ms there, which is exactly the settle the
test lacks, and hid the failure. `--slow-dns US` makes the service's DNS
thread take US microseconds a query, as a loaded host does (a busy wait:
a sleep lasts until a later tick, and the socket overflows before the
table fills). `--gap MS` pauses between the aging and the check, as a
host that deschedules the test's vCPU there does. Debug builds, each
boot confirmed.

| run | aarch64 | x86-64 |
|---|---|---|
| unmodified, `net-dns` ×20 | pass; the flood fully handled when the wait ends in all 20; **no test leaves anything**: the counts never change across a test | the same |
| `--slow-dns 600` | pass; the flood **still draining** when the wait ends, but the aging and the check are microseconds apart and no query lands between them (+0, 3 of 3) | the same |
| **`--slow-dns 600 --gap 50`** | **`net-dns` FAIL at the same check** (55 queries landed in the gap; 55 pending), then **the same eight failures as the sighting, test for test**: 9 of 408 | **the same** |
| … after `net-output` | 16 netifs (`lo,eth0,eth1,dns,tap0`–`tap6,hinu,hstu,hstg,flwh,outu`), 8 services, 22 sockets | the same |
| an earlier form of the probe that slept 2 ms per query | `net-dns` FAIL earlier, at the first-drop check: a sleep lasts until a later tick, the socket overflows, and the first drop never comes; the same cascade from there | the same |

The sighting is reproduced exactly: its check, its eight followers, its
order. Each test in the chain adds what it made to what is held: the
interfaces go from 3 to 16.

In the same `--gap` boot, the aging reclaimed every entry pending before
it: `dns_expired` rose by 128, the pending count read before the aging.
That is the claim step (5) exists for, and it held.

### The same defect outside the network

`irq-route` and `irq-affinity` failed together in this report's own
probe boot (aarch64, 2026-09-29): `hits >= 5` (the interrupt-count
flake `flakes.md` records), then `irq_request(...) == 0` in
`irq-affinity`. `flakes.md` already names why: `irq-route`'s `CHECK`
returns before its `irq_disable` and release, so the line stays held
and the next test's request is refused with `-EBUSY`. It is the third
sighting of that pair (2026-09-20, 2026-09-24, 2026-09-29). `flakes.md`
left the repair, "a test that acquires a resource releases it on every
exit", to whoever next touched `kernel/interrupt/irqtest.c`, and cites
`irqtest.c:247`. Both are wrong: `selftest_irq_route` is in
`kernel/scheduler/schedtest.c` (the check is `schedtest.c:247`), and only
`irq-affinity` lives in `irqtest.c`. The implementation corrects
`flakes.md`.

`irq-route` holds two things a failed `CHECK` skips: its interrupt line
(`irq_request`, released with `irq_disable` and `irq_release`), and the
periodic source that drives it (`arch_test_periodic_irq_start`: the PIT
on x86-64, a timer on aarch64), stopped by
`arch_test_periodic_irq_stop`. Left running, the source keeps raising
the line after the test. The defer list is the repair, with two
releases registered in acquisition order: the source's stop right after
it starts, and the line's disable-and-release right after the request.
They run in reverse: the line, then the source, which is the order the
test's own last lines use. `irq-route` is in this unit's scope.

### How many tests can do this

In `kernel-services/network/nettest.c`, **42 test functions** make a tap,
a service, a network interface, a socket or open `/dev/net/tap`. **38 of
them have a `CHECK` after their first acquisition**, and any of those
returns holding everything made so far: `net-hostinput` has 191 such
checks, `net-hoststate` 112, `net-output` 106. The acquisition sites:
23 `tap_create`, 3 `tapsvc_start`, 17 opens of `/dev/net/tap`, 9
`netif_register`, 53 `ksock_create`, and 23 threads.

### Why it matters

- One transient in one test fails nine, and each follower's failure
  message points at its own test. The sighting read as nine defects.
- Each leak widens: `net-multiguest`, which was fine, failed for want of
  a slot, and in failing kept seven taps.
- P33 (the proc-settle unit) made a leaked *process* fail only the test
  that left it. Network resources have no such rule.

## Current implementation (before this unit)

- `CHECK(cond)` returns false from the test function at once.
- Each network test releases what it made in its last lines, in its own
  order (a service before its tap). A failed check skips all of it.
- The runner (`selftest_run_all`) checks, after every test, that no
  process is left (P33). It checks nothing about the network.
- `tapsvc_get_stats`'s `dns_pending` counts entries in use and not yet
  past their expiry.

## Design

### 1. `net-dns` asserts what the aging did

Step (5) keeps its setup and asserts:

- `s1.dns_expired - s0.dns_expired >= s0.dns_pending`: every entry
  pending before the aging was reclaimed by it.

It no longer asserts `dns_pending == 0` afterwards: a query from the
still-draining flood may take a freed slot, and the service is right to
serve it. If the aging does nothing, the check fails, since
`s0.dns_pending` is 128.

### 2. A test's releases run however it returns

The runner gains a defer list: `selftest_defer(fn, arg)` registers a
release, and the runner runs the test's releases after it returns, last
registered first, whether it passed or failed. It runs them before P33's
check and the network check below. The list is bounded (64 per test),
and a full list fails the test by name.

The network tests register a release right after each acquisition:

- `tap_create` → `tap_destroy`;
- `tapsvc_start` → `tapsvc_stop`;
- an open of `/dev/net/tap` → `file_put`;
- `netif_register` → `netif_unregister`;
- `ksock_create` → `ksock_put`;
- a thread → the test's own stop and join, as one release.

Their explicit teardown lines go. The order is kept by construction:
a service is registered after its tap, so it is released before it.

**A thread's argument must outlive its join.** A release runs after the
test function returns, when its frame, and any helper's, is gone.
Twenty `thread_create` calls, in 16 functions, pass a pointer to a stack
local: test functions (`net-tcpverdict` has three) and helpers such as
`tcp_transfer`, whose sink thread reads its `srv`. A thread started that
way can read a dead frame between the failed `CHECK` and its release.
Those twenty arguments move off the stack: allocated at the start, and
freed by the release that stops and joins the thread, after the join.
A release can then run after any frame has gone.

This covers all 38 functions. It is mechanical, one line per acquisition,
and each converted test is checked by the forced failure below.

### 3. The runner checks the network too

After a test's releases have run, the runner compares the interfaces (by
name), the live services and the socket count with their values before
the test. A test that leaves one fails by name ("left netif `dns`", "left
1 service", "left 4 sockets"). As with P33, it is blamed once: the
baseline moves to what is left, so the tests after it are not failed for
it.

### 4. The record

`flakes.md`: the sighting attributed and fixed. The network tests'
`testing.md`: the defer rule and the runner's check.
`docs/verification/`: the defer list, where the runner is described.

### 5. The §70 gate

**Correctness.** One check asserts what its mechanism does. The rest is
test infrastructure.

**Concurrency.** None in the kernel. Releases run on the runner's
thread, after the test.

**Ownership and lifetime.** Each resource a test makes has one release,
registered once and run once. The explicit teardown lines that would
release it again are removed.

**Security.** None.

**Failure.** A failed test releases what it made. A test that still
leaves something is named.

**Performance.** One list walk per test.

## Affected files

The scope, built in PR #257 (the banner says what was built).

| file | change |
|---|---|
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | `selftest_defer`, the runner running releases, the network check |
| `kernel-services/network/nettest.c` | 38 functions: a release per acquisition, explicit teardowns removed; the 20 thread arguments off the stack; `net-dns` (5) |
| `kernel/scheduler/schedtest.c` | `irq-route`: its interrupt line and its periodic source released through the defer list |
| `kernel-services/network/netif.c`, `tapsvc.c` | the counts the runner reads (interface names, services) |
| `docs/testing/flakes.md`, `docs/kernel-services/network/testing.md`, `README.md` | the record |

## APIs

`void selftest_defer(void (*fn)(void *), void *arg)`, test-only. Two
read-only counts for the runner. No syscall or ABI change.

## Migration plan

One PR. The conversion is mechanical and can be reviewed per test.

## Tests

- **The sighting, forced:** the probe's `--slow-dns 600 --gap 50` on the
  built tree. `net-dns` passes (the aging reclaimed all 128), and nothing
  follows.
- **A forced failure in every converted test:** a mutation that fails
  each test's last `CHECK`, one test at a time in a loop over the 38.
  Each must fail alone, leave nothing (the runner's check passes), and
  the next test must pass. Run as a script, not by hand.
- The runner's network check: a mutation that removes one release makes
  its test fail by name, once.
- `net-dns`'s new check: with the aging disabled, it must fail.
- `gmake host-test`, `gmake analyze`, and debug, release, GIC and chaos
  boots on both architectures.

## Benchmarks

None.

## Risks

- **A release that runs twice** (a test that both registers a release and
  keeps its own teardown). Every explicit teardown is removed in the same
  change, and the forced-failure loop runs each test's releases on the
  failure path, where a double release would show.
- **A release that depends on the test's stack.** Releases take
  heap or static arguments; a test's local that a release needs is
  passed by value or kept static, as `net-dns`'s responder already is.

## Alternatives considered

- **Blame once without releasing.** The next test would not be failed by
  name, but it would still fail: the slots stay held. The cascade is a
  resource problem, not a reporting one.
- **`goto out` in every test.** One label per test and every `CHECK`
  rewritten, 38 times, with the release order spelled out again in each.
  A registered release is written once, at the acquisition.
- **A larger service table.** It would move the cliff, not remove it.
