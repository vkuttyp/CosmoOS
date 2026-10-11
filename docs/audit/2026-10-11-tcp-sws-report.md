# TCP sender silly-window avoidance -- 2026-10-11

Roadmap M3, PR 5 of 5. A defect on main that blocked the milestone: PR
#347's x86-64 CI boots failed `net-bench`'s 8 s budget and tripped the
hang watchdog, and a main with that layout would have gone red
(`docs/testing/flakes.md`, 2026-10-10). Merged before PR #347 and the
BusyBox PR.

## How it was found

| Step | Evidence |
|---|---|
| The failing boots | `net-bench` 9.8-24.6 s in 6 of about 27 x86-64 boots on PR #347; main 0 of about 20 the same day |
| Not the runner | a slow boot's compute tests read normal (`lockdep-graph-bench` 1.06x) while the network was slow; main's #346 merge run, rerun as a control at the same time, passed |
| Not PR #347's code | draft PR #348 -- main plus 9616 bytes of dead padding, PR #347's `.text` growth -- failed the same way (run 38087072382, `net-bench` 14.6 s, one flow at 1 MiB/s) |
| The mechanism | `net-bench` printing its TCP counters (run 38089469242): in the slow guard boot one flow took 2163 ms for 4 MiB with 13,869 segments in and 14,021 out -- about 300 bytes a segment -- and 1 retransmit, 0 timeouts, 0 receive-queue drops; a normal boot sent about 800 |

Nothing was lost. The stream fell into small segments and stayed there:
`tcp_output_locked` sent `min(queued, MSS, room)` whenever the room was
above zero, so a receiver that fell behind -- a slow boot, a layout that
makes the network path slower under TCG -- opened its window a sliver at a
time and every sliver went out as a segment; the per-segment cost then
kept it behind.

## The fix

RFC 1122 4.2.3.4's sender rule. With data in flight, a segment shorter than
the MSS and than the queued data waits unless it is at least half of
`max_sndwnd`, the largest window the peer has offered (`set_snd_wnd` keeps
it at every update). With nothing in flight it is sent, so a small window
never stalls the connection. Invariant N29;
`docs/kernel-services/network/design.md`, "Silly-window avoidance".

## Proof

`net-tcp-sws` (`docs/kernel-services/network/testing.md`): a world through
a tap offers 64240 bytes, then one segment; with more than two segments
queued and one in flight it acknowledges 200 bytes and then all of it; the
next data must be a full segment from where the first ended.

| `tools/tcp-sws-probe.py` | x86-64 | AArch64 |
|---|---|---|
| `--old` (the fix's `tcp.c` and `tcp.h` reversed) | PROBE: PASS -- `net-tcp-sws` fails: `next data at +1460, 200 bytes (mss 1460)` | PROBE: PASS, the same |
| fixed | PROBE: PASS | PROBE: PASS |

`net-bench` on a local x86-64 debug boot with the fix: 31 MiB/s one flow
and 31 two flows on steer 0, against 24-29 and 12-15 in this tree's earlier
boots.

## Validation (local, macOS host, QEMU, TCG)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 137.9 s | PASS 132.6 s |
| debug `test-smp2` | PASS 156.5 s | PASS 149.3 s |
| debug `test`, `QEMU_SMP=4` | PASS 153.6 s | PASS 159.1 s |
| `test-chaos` | PASS 172.0 s | PASS 146.1 s |
| `test-harness-retry` | PASS 169.3 s | PASS 154.7 s |
| `BUILD=release test` | PASS 16.9 s | PASS 20.3 s |
| `BUILD=release test-install` | first run: the fallback boot stopped in OVMF before the loader (flakes.md, the firmware hand-over, third sighting); rerun PASS 23.3 s | PASS 36.9 s |
| `test-crash` | PASS 139.2 s | PASS 131.6 s |
| `tools/tcp-sws-probe.py` | old and fixed PASS | old and fixed PASS |
