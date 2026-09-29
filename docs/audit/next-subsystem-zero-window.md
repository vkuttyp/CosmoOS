# NEXT SUBSYSTEM — net-hostinput's window-update check races the zero-window probe

> **Status: proposed.** Report and probe (`tools/zero-window-probe.py`)
> only; nothing in the kernel changes in this PR.

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
| `--stretch 300 --fix` | both | 2 | as `--stretch 300` | ok, 2/2 |
| `--early 300 --fix` | both | 2 | as `--early 300` | ok, 2/2 |

- **The unforced runs** place the whole blocked phase inside the RTO,
  with 17–21 ms to spare.
- **The two forced placements** are the two ways over it. The probe
  goes out either after the blocked check or inside it. The instrumented
  check fails on the same condition as the original each time.
- **The unforced runs never saw a probe.** The mechanism is therefore
  established by forcing, and not observed in the three CI sightings
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

### 1. The window-update check asserts the stream, not the segment

The check reads segments until it has 50 bytes, and each must start
where the last ended, from `iss1 + 101`. A one-byte probe seen in the
blocked phase counts as the first byte. This is the probe's `--fix`
block, bounded at 4 segments:

```c
uint32_t covered = (blocked_seen && bseq == 101 && blen == 1) ? 1 : 0;
/* then: each segment must have seq == iss1 + 101 + covered and fit in 50 */
CHECK(ok_run && covered == 50);
```

- **The blocked check stays as it is.** It proves that nothing of 50
  bytes goes out through a zero window. A 1-byte probe is not a
  violation of it.

### 2. A test of the probe itself: `net-zero-window-probe`

The step above tolerates the probe, but nothing proves the probe exists.
A new test closes the window, queues data, and fires the persist timer
itself with the existing seam `tcp_test_arm_rexmit(pcb, 1 ms)`. That
makes the adversary the mechanism, not a stopwatch. The test then
asserts:

- the probe: one byte at `snd_una`, and `retransmits` counted once;
- for a window update that does not ack the probe byte, the rest in
  order from `+1` after it, as `net-hostinput` now expects;
- for a window update that acks the probe byte, the rest from there,
  with no byte sent twice.

The seam is `CONFIG_DEBUG` only (`tcp.c`, the `#if CONFIG_DEBUG`
block that holds `tcp_test_arm_rexmit`). The test either registers only
in debug or has a release stub, and `gmake BUILD=release image` must
build before the push.

### 3. The record

- **The report banner and a mutation table.**
- **flakes.md:** the entry gains a "fixed by" line.
- **README Status entry.**
- **The testing guide:** a line on the probe test.

## Affected files

| file | change |
|---|---|
| `kernel-services/network/nettest.c` | `net-hostinput`'s window-update check (§1); `net-zero-window-probe` (§2) |
| `docs/audit/next-subsystem-zero-window.md` | as built |
| `docs/testing/flakes.md` | the entry's "fixed by" |
| `README.md` | Status entry |
| `tools/zero-window-probe.py` | (this PR) the probe |

No TCP code changes.

## APIs

None. `tcp_test_arm_rexmit` exists.

## Migration plan

None; test-only.

## Tests

| test | proves |
|---|---|
| `net-hostinput` | the 50 bytes arrive in order from `iss1 + 101` with or without a probe; forced both ways by the probe's `--stretch` and `--early` |
| `net-zero-window-probe` (new) | a zero window with data sends a one-byte probe on the timer; data after a window update resumes in order past it |

**Mutations** (each alone, boot confirmed):

- **The old check in place of §1's, under `--stretch` or `--early`:**
  fails, the rows above.
- **The probe removed** (the `seglen = 1` branch disabled):
  `net-zero-window-probe` fails, because no probe arrives.
- **The probe does not advance `snd_nxt`:** the resumed data starts at
  the probe byte. The test must see the byte twice and fail. If it
  cannot, this mutation shows that the in-order rule is weaker than
  stated.

## Benchmarks

None; `net-hostinput` gains one to three more `hin_recv` calls only when a
probe went out.

## Risks

- **§1's loop is bounded at 4 segments.** A stack that sent the 50
  bytes one byte at a time would fail it. That would be a real
  regression (silly-window avoidance), so the bound is intended.
- **The seam arms the timer on the calling CPU.** Its comment says so.
  The test is single-CPU in what it asserts, so this is harmless. The
  implementation should confirm that the probe cannot race the test's
  own window update. The update is sent only after the probe has been
  read.

## Alternatives considered

- **Shorten the blocked phase** (fewer tries). This narrows the window,
  but the race remains: a slow host is always slower.
- **Raise the RTO for the test.** This moves the race, not removes it,
  and it tests a pcb that is not the one users get.
- **Send the window update before the RTO by stopwatch.** A stopwatch
  adversary is what this report exists to remove.
- **Change TCP not to probe until later.** The probe is correct. The
  kernel should not change to suit a test.
