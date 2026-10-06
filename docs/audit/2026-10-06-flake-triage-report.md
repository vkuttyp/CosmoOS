# Flake triage: `net-nicbench`'s UDP rate, two lower bounds, the shootdown deadline -- 2026-10-06

Three items from `docs/testing/flakes.md`, taken together as one unit on a
branch from `main` at `80f46d83`. Each section says what was measured,
what it showed, what changed, and what is still not attributed. Nothing
here widens a bound.

## 1. `net-nicbench`: the UDP send rate

### 1.1 The facts going in

The benchmark reports, per NIC, ARP round trips per second (requests
built by hand, replies counted at the driver boundary, 64 in flight) and
UDP sends per second (1 KiB datagrams to the gateway from an in-kernel
socket), and fails only on a broken path. Its history:

| when | what it read | where |
| --- | --- | --- |
| e1000e unit, mid-September | 20-23 k sends/s per NIC on x86-64, about 60 % of that on AArch64; test about 2.1 s | `docs/kernel-services/network/testing.md` |
| 2026-09-29, -30 | 8,039 and 8,116 ms against an 8 s budget, local AArch64, beside another boot | `flakes.md`, two sightings |
| 2026-10-04, PR #307 | AArch64 chaos CI: 6,272 and 2,082 sends/s, 8,360 ms; budget raised to 20 s | lockdep report, PR #307 follow-up |
| 2026-10-06 | 1,037 and 903 sends/s, ARP 7,378 and 7,022 rt/s, 21,523 ms against 20 s, local AArch64 four CPUs, unchanged scheduler | `flakes.md`, third sighting |

The third sighting is the odd one: ARP normal, UDP ten times under its
usual, on both NICs. About 1 ms per send.

### 1.2 The distribution

**Every reading in the local `out/` tree** (326 distinct boot logs,
2026-09-20 to 2026-10-06, both architectures, mostly four CPUs; the
`.vcon` copies deduplicated):

| | n | UDP min | p10 | median | p90 | max |
| --- | --- | --- | --- | --- | --- | --- |
| x86-64 | 344 | 4,626 | 8,031 | 12,134 | 17,706 | 47,391 |
| AArch64 | 266 | 2,082 | 8,501 | 12,400 | 16,530 | 28,634 |

By bucket: x86-64 2 readings under 5 k, 31 in 5-8 k, 135 in 8-12 k, 159
in 12-20 k, 17 at 20 k and above; AArch64 5, 16, 101, 127, 17. The second
interface (e1000e, `eth1`) reads 0.72 of the first's UDP rate at the
median (p10 0.63, p90 1.02) with the same ARP rate. UDP divided by ARP in
the same reading: p10 1.0, median 1.47, minimum 0.31 -- twelve readings
under 0.5, all on 2026-09-29 and 2026-10-04 and all in the 4.6-6 k band.
No local reading under 2 k is in the tree; the third sighting's log was
overwritten by the boots after it.

**CI** (`gh run view --log` over the last 60 runs; the serial log is only
in a failing job's output, so 20 readings from 10 jobs survive):

| run | job | eth0 arp / udp | eth1 arp / udp | test ms |
| --- | --- | --- | --- | --- |
| 37126012073 | aarch64 | 12,981 / 14,307 | 8,650 / 5,994 | 2,977 |
| 37135325697 | aarch64 | 7,872 / 7,191 | 1,954 / 2,138 | 7,570 |
| 37136093738 | aarch64 | 7,967 / 13,300 | 5,828 / 4,239 | 4,080 |
| 37139019840 | aarch64 | 7,283 / 7,757 | 4,177 / 3,068 | 5,600 |
| 37150697619 | aarch64 | 2,529 / 6,433 | 1,904 / 2,189 | 8,153 |
| 37150697619 | x86-64 | 7,929 / 7,079 | 2,447 / 4,180 | 5,135 |
| 37154202680 | aarch64 | 2,435 / 6,272 | 2,061 / 2,082 | 8,360 |
| 37185958348 | aarch64 | 14,998 / 15,406 | 8,507 / 5,400 | 3,151 |
| 37253412080 | aarch64 | 7,219 / 8,717 | 1,947 / 2,815 | 6,257 |
| 37391549559 | aarch64 | 5,321 / 7,560 | 1,688 / 1,681 | 9,366 |

On CI a slow reading is slow in both columns: the five `eth1` UDP
readings under 3 k have ARP at 1.7-2.1 k beside them. That is a slow
runner, the shape `flakes.md` already names, not the third sighting's.

**Twelve boots of unchanged `main` (`80f46d83`) today**, one at a time,
four CPUs, alternating architectures, 19:51 to 20:25, in a shared clone
so the working tree was free; the host otherwise idle apart from this
session's reading (load average 2.4-5.4 on eight cores):

| boot | eth0 arp / udp | eth1 arp / udp | test ms |
| --- | --- | --- | --- |
| x86-64 1 | 8,338 / 7,197 (5,838 of 10,000 accepted) | 2,861 / 2,752 | 5,617 |
| AArch64 2 | 9,030 / 1,586 | 2,673 / 1,291 | 15,257 |
| x86-64 3 | 7,945 / 7,055 (6,762 accepted) | 2,681 / 1,184 | 10,649 |
| AArch64 4 | 8,337 / 4,939 | 9,320 / 1,603 | 8,967 |
| x86-64 5 | 7,975 / 6,983 (6,719 accepted) | 2,689 / 1,250 | 10,187 |
| AArch64 6 | 8,686 / 1,651 | 8,651 / 4,306 | 9,062 |
| x86-64 7 | 7,619 / 7,078 (6,829 accepted) | 7,272 / 1,312 | 9,339 |
| AArch64 8 | 8,037 / 5,036 | 9,946 / 4,536 | 4,868 |
| x86-64 9 | 8,259 / 7,314 (6,026 accepted) | 8,381 / 4,576 | 3,705 |
| AArch64 10 | 7,814 / 1,874 | 7,117 / 1,537 | 12,604 |
| x86-64 11 (20:17) | 8,367 / 13,644 | 8,394 / 9,343 | 2,490 |
| AArch64 12 | 2,545 / 9,961 | 7,238 / 7,096 | 3,733 |

The first ten are the third sighting's shape: ARP at its usual 7-9 k on
at least one NIC, UDP at 1.2-7 k, the test at 3.7-15.3 s against a usual
2.1-2.5 s. The slow mode was **on** from 19:51 to about 20:16 and **off**
from boot 11 at 20:17: that boot and the next, and one more boot of the
same image at 20:28 (13,741 and 9,773 sends/s, ARP 8,489 and 9,148, the
test 2,425 ms), read what the two-week tally calls normal. So it is
bimodal in *time*, a host state that lasts tens of minutes and spans
QEMU processes, not a per-boot coin. In the local tree's two weeks of
logs it was on for at most the dozen readings under 6 k; today it was on
for ten boots in a row.

The x86-64 boots show a second thing: `5,838 of 5,838 frames left the
driver` means 4,162 of the 10,000 sends *failed* -- the virtio ring was
full and `vnet_transmit` returned `-ENOBUFS` -- which the summary line did
not say. The e1000e NIC accepted all 10,000 and took 360-840 us each.

### 1.3 Where a send's time goes

**The host.** An unconnected UDP `sendto` to a closed loopback port, from
a Python process on this Mac, 10,000 of 1 KiB:

| socket | rate | p50 | p99 | max |
| --- | --- | --- | --- | --- |
| unconnected (what slirp uses) | 8,413/s | 115 us | 185 us | 707 us |
| connected | 340,306/s | 3 us | 3 us | 56 us |
| unconnected, a listener on the port | 8,308/s | 111 us | 215 us | -- |
| unconnected, another port | 8,070/s | 112 us | 217 us | -- |

QEMU's user-mode network (libslirp 4.9.4 here) sends each guest datagram
through an unconnected host socket; it answers ARP in-process, with no
system call. So the host's per-datagram cost -- 115 us idle on this
macOS, and whatever it was during the slow half hour -- is in every UDP
send and in no ARP round trip. That is the asymmetry the third sighting
showed and the CI runners do not (there, the whole runner is slow).

**How each driver pays it.** The e1000e model transmits in the `TDT`
register write, synchronously, in the vCPU thread: the guest's send
returns when the host's `sendto` has. virtio-net's notify schedules a
bottom half on QEMU's main loop and returns; the guest runs ahead, the
ring fills, and the driver refuses with `-ENOBUFS` (the x86-64 `5,838 of
5,838` above) or the next notify waits for the main loop to release the
big lock. Either way the measured rate is the host's drain rate.

**In the guest.** The test now times each send and prints, per NIC, a
log2 histogram from 16 us to 16 ms, the share of the window spent inside
the driver's transmit (`netif_tx_probe`, around `nif->ops->transmit`),
the ring's peak occupancy (a new optional `tx_pending` op in both
drivers), how often the sender was switched out or moved CPU, and the
gateway's ARP entry and counters before and after. The first instrumented
x86-64 boot, with the slow mode off:

```
eth0: udp window 500 ms: 6627 sends attempted, 6627 accepted, 0 refused by the driver (ring full), 0 failed otherwise, 6627 frames left the driver
eth0: udp ring occupancy max 2 of 256; sender switched out 1 times, moved CPU 0 times; gateway arp reachable -> reachable, +0 requests, +0 pending dropped
eth0: udp us per send: <16:0 <32:0 <64:162 <128:6412 <256:52 <512:1 <1024:0 ... >=16384:0; max 504
eth0: udp 0 sends of 1 ms or more held 0 of 500 ms; in the driver 17% of the window over 6627 transmits (max 74 us)
eth1: udp us per send: <16:0 <32:0 <64:0 <128:3836 <256:515 <512:15 <1024:1 ...; max 537
eth1: udp 0 sends of 1 ms or more held 0 of 500 ms; in the driver 43% of the window over 4367 transmits (max 328 us)
```

virtio (`eth0`): 75 us a send, 17 % of it in the driver, the ring never
above 2 of 256. e1000e (`eth1`): 114 us a send, 43 % of it in the
driver's register write -- the synchronous host path, at the idle host's
115 us -- the ring never above 1. The sender was not sleeping, yielding
or moving: one switch in 6,627 sends, none in 4,367. The gateway's entry
stayed reachable and no ARP request was sent during either loop. There
is nothing periodic in the guest; the 1 ms of the third sighting is on the
other side of the register write.

The same instrumented tree under a host-side load (four processes
flooding the same loopback port, `tools/nicbench-host-cost-probe.py
--old --load 4`): the count-bound phase took 1,817 and 2,286 ms for its
two 10,000-send rounds (5,503 and 4,373 sends/s), 104 and 110 sends of
1 ms or more, maxima 22.8 and 3.9 ms, the driver's share 19 % and 44 %,
the virtio ring up to 82 of 256. But the flood is CPU load as much as
network load: ARP fell to 2.3 k and 2.2 k with it, and the whole suite
before the benchmark took 136 s instead of about 60. It reproduces a slow
host, not the ARP-normal mode; the mode's trigger was not found
(§1.6).

TODO instrumented loop at 1/2/4 CPUs, both architectures, and any slow-mode boot it caught.

### 1.4 Regression or mode?

A mode. Boots 10 and 11 above were the same image on the same host a few
minutes apart, 1.9 k and 1.5 k then 13.6 k and 9.3 k. No merged
unit selects it; it was on and off with the same kernel. The ARP rate did
not move with it, which rules out the scheduler, the workers and the
receive path, which ARP exercises harder than UDP does (2,000 replies
counted at the boundary against 10,000 datagrams nobody answers). The
driver's share and the ring occupancy in §1.3 put the time in the
transmit register write and the host behind it.

What the longer trend says: the medians have drifted from 20-23 k (the
e1000e unit, September) to 12 k (the two-week tally) with ARP drifting
the same way (12-17 k to 8-10 k). Both columns moving together is the
debug kernel getting heavier -- lockdep landed in October (PRs #302-313)
and runs on every lock a send takes -- not the UDP path; it was not
bisected because it is not the sighting's shape and the per-NIC rate
lands inside its own day-to-day spread (the tally's p10-p90 is 8-17.7 k).

### 1.5 The fix, and its classification

The test's structure was the exposure. Its UDP phase was a count --
10,000 sends -- so its duration was 10,000 times a cost outside the
guest. At 115 us that is 1.2 s a NIC; at the third sighting's 1 ms it is
10 s a NIC, 21.5 s the test, over a budget raised two days earlier to
hold it. A budget is a hang guard; the test had not hung, it had been
told to do a fixed amount of someone else's work.

The phase is now a **500 ms window**: send until the clock says stop,
report sends per second over what was sent. Same rate, same frames-left
check (now with the driver's refusals counted separately), bounded time.
Under `flakes.md`'s rule this is **Restate** (rule 2): the property --
the stack's send rate through each NIC -- is observed as a rate over a
window, and the count that stood in for it is gone. Nothing is widened;
the test is not on the list and does not join it. The ARP phase was
already a closed loop with its own 200 ms and 500 ms give-ups; the
receive drain already fails the test at 3 s rather than waiting.

**The budget returns to the default 8 s** (the 20 s entry PR #307 added is
removed). The data: with the window the test took 2,180 ms on x86-64 in
its first boot and TODO; its worst case is one NIC's ARP phase at the
slowest rate ever recorded (1.7 k rt/s: 1.2 s) plus 0.5 s plus a 3 s
drain that fails it, under 5 s, and the second NIC does not run after a
failure. The 20 s entry was sized for a count that no longer exists.

Probe: `tools/nicbench-host-cost-probe.py`. It boots a clone whose runner
panics right after the benchmark with its verdict and duration, with the
count-bound phase restored (`--old`) or not, and with or without the host
flood (`--load N`), and prints both NICs' rates, histograms and driver
shares. TODO results table (old/fixed × load/none, both architectures).

### 1.6 Not attributed

What put the host in its slow mode for twenty-five minutes today, and
on the afternoon of the third sighting, is not established. During it the
Python `sendto` above still measured 116 us from a fresh
process while the guest's e1000e send was taking 700 us, so it is not the
host kernel's UDP path in general but something about QEMU's: its main
loop and vCPU threads' placement (four performance and four efficiency
cores here), the big lock, or slirp's own socket. The guest-side
instrumentation will say so the next time -- a slow-mode boot now prints
the driver's share, which for e1000e should read near 100 % -- and the
test no longer fails for it. The twelve-boot table is the record of what
the mode looks like from inside.

## 2. `irq-sync` and `timer-cancel-sync`: the lower bound

Both tests hold a callback on another CPU for 20 ms (an interrupt
handler; a timer callback) and assert that the synchronous unregister or
cancel took at least 10 ms: the wait spanned the callback. Both started
their clock when *this* thread returned from `wait_flag(&p->entered)`.
`flakes.md` recorded two local failures of `timer-cancel-sync`'s bound
(2026-09-21 and -28, both on a loaded host, 23 ms into the test) and
named the fix: a host that holds this vCPU for more than ten of the
callback's twenty milliseconds between that return and the clock read
makes a correct sync look short.

**The change** (`kernel/core/quiescetest.c`): each probe records
`entered_ns = clock_now_ns()` in the callback itself, before it publishes
`entered`; the test measures `clock_since_ns(p->entered_ns)` after the
sync returns. The bound stays `>= MS(10)`. A delay of the test thread
anywhere between the callback's entry and the measurement now lengthens
the span; nothing a host does can shorten it. The property is unchanged:
a sync that returned mid-callback would return within the hold -- under
20 ms from entry -- and still fails `done == 1` on the next line, which
the old code also checked. Under `flakes.md`'s rule this is **Restate**
(rule 2): the span was measured from the wrong end.

**Proof** (`tools/sync-lower-bound-probe.py`): a clone with a 15 ms spin
inserted between `wait_flag(&entered)` and the sync in both tests, so the
callback's hold is three quarters over when the thread reaches it -- the
host delay the sightings described, made deterministic -- and a panic
after the second test carrying both verdicts. `--old` restores the clock
read at the point this thread reaches.

| | x86-64 | AArch64 |
| --- | --- | --- |
| `--old` (clock from this thread's return) | TODO | TODO |
| fixed (clock from the callback's entry) | TODO | TODO |

## 3. The TLB shootdown deadline's report

`arch_mmu_shootdown_cpus` (x86-64) sends `IPI_TLB_FLUSH` to every other
online CPU and spins up to one second for their acknowledgements; it
panicked three times in September with `acknowledged by 2 of 3 CPUs` and
no word on which CPU or why. The deadline is a defect's bound -- a flush
that never completes is a kernel bug -- so it is unchanged. AArch64 has
no such wait: its invalidations are broadcast (`tlbi ... is`) and the
`dsb` that completes them is every CPU's acknowledgement, so this section
is x86-64's alone.

**The change** (`kernel/arch/x86_64/mmu.c`): the acknowledgement is a
per-CPU bit (`g_shootdown_acked`, `fetch_or`) rather than a count. At
the deadline, for each CPU whose bit is clear the report prints its
current thread (tid and name), `irq_depth`, `preempt_count`, its last
tick's age and PC, then takes the lockup unit's sample of it
(`lockup_sample_cpu`: an NMI on x86-64, which a CPU with interrupts
masked still answers) and prints the frames through a new
`lockup_print_sample`, and `lockdep_dump_held_cpu`'s bounded snapshot of
the locks it holds. The panic then names them:
`acknowledged by 2 of 3 CPUs; not by cpu 1`. Nothing in the report waits
on the silent CPU beyond the sample's own 5 ms bound. A last
acknowledgement that lands exactly on the deadline is taken, not
reported.

**Proof** (`tools/tlb-shootdown-diag-probe.py`): a debug-only knob,
`arch_mmu_shootdown_test_hold(cpu, ns)`, makes the next flush handler on
that CPU spin for `ns` before acknowledging -- a CPU that does not answer
in time, inside its interrupt handler, exactly the state the report must
describe. The probe's clone arms it for CPU 1 before the first self-test
and issues one shootdown of a kernel page.

| | hold | result |
| --- | --- | --- |
| forced | 1.5 s | TODO |
| control | 100 ms | TODO |

## 4. Watched, not acted on

TODO

## 5. Validation

TODO
