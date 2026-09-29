# NEXT SUBSYSTEM — net-hostinput's window-update check races the zero-window probe

> **Status: built (PR #259).** As designed, with these specifics:
>
> - **§1's check is a helper,** `hin_recv_stream(u, port, base, covered,
>   total, &sg, have)`, in `nettest.c`. It asks that **every byte of the
>   range arrive, in any order**, not "no gap in arrival order" as the
>   report (#258) proposed. The rexmit work builds its probe under the pcb's lock but
>   sends it after unlocking (`batch_send` follows the unlock), so a
>   window update's data can reach the wire before the probe, and a
>   receiver reassembles. Greptile found this on #259. A segment
>   reaching outside the range fails, including a byte before `base`,
>   whose offset wraps. A SYN, FIN or RST fails, with or without data,
>   and a bare ACK is passed over. `net-hostinput` counts a probe it
>   read in the blocked phase as `covered = 1`.
> - **§2's test does not use the seam the report proposed**
>   (`tcp_test_arm_rexmit`). It waits for the probe the timer sends at
>   its own RTO. It needs no debug-only code, so no
>   release stub either; release boots run no self-tests, but the
>   release build compiles it. Each wait is for the probe, not a stopwatch. It
>   has four cases:
>   1. the probe;
>   2. an update that does not ack it;
>   3. an update that acks it;
>   4. the probe read off the tap before the update is sent, and passed
>      to the helper uncounted, as the run's first segment:
>      `net-hostinput`'s slow-host placement, made deterministic. A
>      first form sent the update once `retransmits` had risen. The
>      count is taken under the lock, but the probe goes out after it,
>      so that form could see the data first (Greptile, #259).
> - **§2 does not assert `challenge_acks`.** It is a machine-wide
>   counter, and the claim is about this connection. Case (3) asserts
>   the data.
> - **Mutations,** each alone on both architectures:
>
>   | mutation | result |
>   |---|---|
>   | the probe removed | fails at (1) |
>   | one probe keeping `snd_nxt` and `snd_max` | fails at (3), as predicted; (2) passes |
>   | the helper demanding the whole run in its first segment (the old rule) | fails at (2) |
>
>   The second mutation's first form only skipped `snd_nxt += seglen`,
>   so the output loop built probes until the batch was full. It failed
>   at (2) by the 8-segment bound, not by the mechanism, and was
>   replaced.
> - **Forced placements** against the new `net-hostinput` (a 300 ms or
>   700 ms pause before the update, 300 ms before the blocked check)
>   pass on both architectures.
> - **`tools/zero-window-probe.py`** patches the pre-fix check. On a
>   tree with the fix, its anchor is not found and it exits without
>   touching anything; its docstring says so.

## Problem

`docs/testing/flakes.md` ("`net-hostinput`: no data after a window
update, three times") records one check failing three times, all x86-64
debug:

```
SELFTEST: net-hostinput ... FAIL: check failed: hin_recv(u, IPPROTO_TCP, 40001,
    &sg, HIN_TRIES) && sg.paylen == 50 && sg.seq == iss1 + 101
```

The step it ends (`nettest.c`, the zero-window block of `net-hostinput`):

```c
CHECK(ksock_sendto(a1, data, 50, NULL) == 50);
CHECK(!(hin_recv(u, IPPROTO_TCP, 40001, &sg, 15) && sg.paylen >= 50));   /* blocked by the zero window */
l4len = hin_mk_tcp(..., 1003, iss1 + 101, TH_ACK, 64240, NULL, 0);        /* window update */
CHECK(hin_send(...));
CHECK(hin_recv(u, IPPROTO_TCP, 40001, &sg, HIN_TRIES) && sg.paylen == 50 && sg.seq == iss1 + 101);
```

The peer has closed its window, the test queues 50 bytes, checks for 15
tries that they do not go out, opens the window, and expects the first
segment after that to be all 50 bytes at `iss1 + 101`.

The kernel does not promise that. A zero window with data waiting keeps
the retransmit timer armed (`tcp_output_locked`: `pcb->sndbuf.len &&
pcb->snd_wnd == 0` → `arm_rexmit`), and when the timer fires, the output
path sends a **one-byte zero-window probe** and advances `snd_nxt` past
it (the `seglen = 1` branch, "one-byte probe on the timer"). This is
RFC 9293 §3.8.6.1 behaviour and is correct. After a probe, the window
update releases the other 49 bytes at `iss1 + 102`. The first segment
the check reads is then either the probe (`+101`, 1 byte) or, if the
probe was already read, `+102` with 49 bytes. Either way the check fails.

So the check assumes the window update reaches the stack before the
timer's first expiry. The timer is armed at the send with the pcb's RTO,
which is 200 ms here. The blocked phase is 15 × `thread_sleep_ms(10)`,
which lasts about 180 ms. Only about 20 ms stands between a pass and a
fail, and a loaded host can take that. This is a test defect. TCP is
right.

### Measured

`tools/zero-window-probe.py` instruments the step. It logs the pcb's RTO
at the send, the blocked phase's length, what (if anything) arrived in
it, and the first segment after the window update. It then either keeps
the test's check or substitutes the candidate check (`--fix`).
`--stretch MS` pauses before the window update, and `--early MS`
pauses before the blocked check: that is a slow host at either point.

| run | arch | boots | ZWPROBE | net-hostinput |
|---|---|---|---|---|
| instrumented, unforced | both | 4 | `rto 200 ms; blocked phase 179–183 ms, saw nothing; after the update: seq +101, 50 bytes` | ok, 4/4 |
| `--stretch 300` | both | 2 | `blocked phase 180 ms, saw nothing; after the update: seq +101, 1 bytes` | **FAIL** at the check, 2/2 |
| `--early 300` | both | 2 | `blocked phase 301 ms, saw seq +101, 1 bytes; after the update: seq +102, 49 bytes` | **FAIL** at the check, 2/2 |
| `--stretch 700` | both | 2 | `blocked phase 180–201 ms, saw nothing; after the update: seq +101, 1 bytes` | **FAIL** at the check, 2/2 |
| `--stretch 300 --fix` | both | 2 | as `--stretch 300`; `--fix read 2 segments` | ok, 2/2 |
| `--early 300 --fix` | both | 2 | as `--early 300`; `--fix read 1 segments` | ok, 2/2 |
| `--stretch 700 --fix` | both | 2 | as `--stretch 700`; `--fix read 3 segments` | ok, 2/2 |

- **The unforced runs** place the whole blocked phase inside the RTO,
  with 17–21 ms to spare.
- **The two forced placements** are the two ways over it. The probe
  goes out either after the blocked check or inside it. The instrumented
  check fails on the same condition as the original each time.
- **`--stretch 700` passes a second timeout** (200 ms, then 400 ms after
  the doubling). The timeout rewinds `snd_nxt` to `snd_una` and sends
  the probe byte again. The candidate then reads three segments to
  cover the 50 bytes. The probe logs only the first segment's sequence
  and the count, so "the second is the repeated probe at `+101`" is
  inferred from the rewind, not logged. A check that demanded each
  segment start exactly where the last ended would reject it. That was
  the candidate as first written; Greptile found it on #258.
- **The unforced runs never saw a probe.** The mechanism is therefore
  established by forcing, and not observed in the three sightings
  themselves. The failed check's message cannot say which segment it
  read. The sightings are consistent with it: the same check each time,
  in boots that change no TCP code.

### Why it matters

A failed `net-hostinput` used to take four tests down with it. Since
the net-leftover unit (#257), it fails alone, but it still fails a boot
that has nothing wrong with it. The check also claims more than the
step can know: what the kernel owes is that the 50 bytes go out in
order from `iss1 + 101` once the window opens. It does not owe them in
one segment, or before the persist timer.

## Current implementation

- **`tcp.c`, `tcp_output_locked`:** the zero-window probe (one byte,
  only on `WORK_REXMIT`, only with nothing in flight). The rexmit timer
  stays armed while `sndbuf.len && snd_wnd == 0`.
- **`tcp.c`, the rexmit work:** it counts the probe as a retransmission
  (`rexmit_count`, `retransmits`) and doubles the RTO.
- **`nettest.c`, `net-hostinput`:** the step above. `hin_recv` returns
  the first TCP segment for the port, polling in 10 ms sleeps.
- **The probe is not asserted anywhere.** No test asserts that a
  zero-window probe is sent, or what follows it.

## Design

### 1. The window-update check asserts the bytes, not the segment

`hin_recv_stream(u, port, base, covered, total, &sg, have)` reads
segments until every byte of `[base, base + total)` has arrived:

- **Order is not asked for.** A byte may arrive in any order, and more
  than once: a probe sent again, a retransmission from `snd_una`. The
  rexmit work builds its probe under the pcb's lock but sends it after
  unlocking (`batch_send` follows the unlock), so a window update's data
  can reach the wire before the probe, and a receiver reassembles.
- **A segment reaching outside the range fails.** That includes a byte
  before `base`, whose offset `seq - base` wraps past `total`.
- **A SYN, FIN or RST fails,** with or without data. A bare ACK is
  passed over.
- **At most 8 segments are read.** 50 bytes one at a time is a
  regression (silly-window avoidance), not a delivery.
- **`covered` bytes from `base` are already seen,** and `have` says
  `sg` holds a segment the caller read and has not yet counted.

`net-hostinput` counts a one-byte probe at `iss1 + 101` that it read in
the blocked phase as `covered = 1`, and otherwise passes 0:

```c
bool zw_seen = hin_recv(u, IPPROTO_TCP, 40001, &sg, 15);
CHECK(!(zw_seen && sg.paylen >= 50));
uint32_t zw_covered = zw_seen && sg.seq == iss1 + 101 && sg.paylen == 1 ? 1 : 0;
/* window update */
CHECK(hin_recv_stream(u, 40001, iss1 + 101, zw_covered, 50, &sg, false));
```

- **The blocked check stays as it is.** It proves that nothing of 50
  bytes goes out through a zero window; a one-byte probe does not
  violate it.

### 2. A test of the probe itself: `net-zero-window-probe`

The step above tolerates the probe; this test proves it exists and
what follows it. It has its own uplink tap (`zwpu`, `10.77.12.1`, the
world at `.99`) and a listener on `:2230`. The world completes the
handshake by hand and closes its window. Each wait is for the probe the
retransmit timer sends at its own RTO, with no stopwatch and no seam.
So it needs no debug-only code, and the release build compiles it.
Release boots run no self-tests.

1. **The probe:** a 50-byte send draws one byte at `snd_una`, and the
   pcb's `retransmits` rises.
2. **An update that does not ack the probe byte:** every byte of the 50
   from `snd_una`, the probe counted.
3. **An update that acks the probe byte:** every byte of the other 49
   from exactly past it. A resend of an acked byte falls before `base`
   and fails.
4. **The probe not yet counted:** `net-hostinput` on a slow host, where
   the blocked check missed it. The probe is read off the tap before the
   update is sent, so it is on the wire first, and is handed to the
   helper uncounted (`have`).

It closes with a reset from the world at `rcv_nxt`. Every case sends
its update only after reading the probe off the tap: a count taken
under the lock does not order the probe's output before the update's
data.

It does not assert `challenge_acks`. That is a machine-wide counter,
and the claim is about this connection, so case (3) asserts the data.

### 3. The record

- **The report banner and a mutation table.**
- **flakes.md:** the entry gains a "fixed by" line.
- **README Status entry.**
- **The testing guide:** a line on the probe test.

## Affected files

The implementation's, as built (PR #259). The report (#258) added only
the probe and this report.

| file | change |
|---|---|
| `kernel-services/network/nettest.c` | `hin_recv_stream`; `net-hostinput`'s window-update check (§1); `net-zero-window-probe` (§2) |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | the new test registered after `net-hostinput` |
| `docs/audit/next-subsystem-zero-window.md` | as built |
| `docs/kernel-services/network/testing.md` | `net-hostinput`'s window update; a section on the new test and its mutations |
| `docs/testing/flakes.md` | the entry's "fixed by" |
| `README.md` | Status entry |
| `tools/zero-window-probe.py` | added by the report (#258); the implementation adds a docstring note that it targets the pre-fix check |

No TCP code changes.

## APIs

None.

## Migration plan

None; test-only.

## Tests

| test | proves |
|---|---|
| `net-hostinput` | every one of the 50 bytes from `iss1 + 101` arrives, with or without a probe (repeats and any order allowed); forced by the probe's `--stretch` (one probe and two) and `--early` |
| `net-zero-window-probe` (new) | a zero window with data sends a one-byte probe on the timer; data after a window update resumes past it, and no byte the peer acked is sent again |

**Measured by the report (#258):** the old check fails under `--stretch
300`, `--stretch 700` and `--early 300`, and the candidate passes all
three (the rows above).

**Mutations** (each alone, both architectures, boot confirmed):

| mutation | result |
|---|---|
| the probe removed (`seglen = 1` → `break`) | fails at (1): no one-byte segment |
| one probe that keeps `snd_nxt` and `snd_max` | fails at (3), and (2) passes. The ACK of `+52` is above `snd_max`, so `tcp_input`'s RFC 5961 check (`SEQ_GT(ack, pcb->snd_max)` → `challenge_ack`) drops it before the window opens, and no data follows. `net-hostinput` allows a repeated byte by design, so it cannot catch this |
| `hin_recv_stream` demanding the whole run in its first segment (the old check's rule) | fails at (2) |

A first form of the second mutation, which only skipped
`snd_nxt += seglen`, flooded probes: the output loop never advanced and
built one until the batch was full. It failed at (2) through the
8-segment bound, which is not the mechanism under test, and was
replaced.

**Forced placements** against the new `net-hostinput`: a 300 ms or
700 ms pause before the window update (one probe, or two), and 300 ms
before the blocked check. All pass on both architectures.

## Benchmarks

None; `net-hostinput` gains at most seven more `hin_recv` calls, and only
when a probe went out (two in the measured runs).

## Risks

- **§1's helper is bounded at 8 segments.** A stack that sent the 50
  bytes one byte at a time would fail it. The most measured is 3.
  That was two probes and the rest, under `--stretch 700`, which a
  host would need to stall for over 600 ms to reach. That would be a real
  regression (silly-window avoidance), so the bound is intended.
- **Wire order is not asserted.** Neither test checks that the stack
  sends in sequence order. That is deliberate: the probe's output
  follows its lock's release, so order is a race, and a receiver
  reassembles. A byte outside the range, or one never sent, still fails.

## Alternatives considered

- **Shorten the blocked phase** (fewer tries). This narrows the window,
  but the race remains: a slow host is always slower.
- **Raise the RTO for the test.** This moves the race, not removes it,
  and it tests a pcb that is not the one users get.
- **Send the window update before the RTO by stopwatch.** A stopwatch
  adversary is what this report exists to remove.
- **Change TCP not to probe until later.** The probe is correct. The
  kernel should not change to suit a test.
