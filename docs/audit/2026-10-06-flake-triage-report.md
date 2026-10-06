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
the driver's transmit (`netif_tx_probe`, around `nif->ops->transmit`,
counting the benchmark thread's sends on the measured interface and no
others), the ring's peak occupancy (a new optional `tx_pending` op in
both drivers, read under the read-side section that keeps a ring alive
across removal -- review found the first version read it bare), how
often the sender was switched out or moved CPU, and the
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

**Nine instrumented boots** (the committed tree, one at a time, 20:33 to
20:56, the slow mode off throughout):

| boot | eth0 udp sends/s, driver share, ring max | eth1 udp sends/s, driver share, ring max | sends >= 1 ms | test ms |
| --- | --- | --- | --- | --- |
| AArch64 4 CPUs | 10,148, 15 %, 2 | 6,703, 43 %, 1 | 0 / 1 | 2,254 |
| x86-64 2 CPUs | 10,598, 16 %, 4 | 7,637, 39 %, 1 | 0 / 0 | 1,735 |
| AArch64 2 CPUs | 9,939, 15 %, 2 | 7,830, 41 %, 1 | 0 / 0 | 1,749 |
| x86-64 1 CPU | 8,928, 12 %, 6 | 9,165, 41 %, 1 | 1 / 0 | 1,697 |
| AArch64 1 CPU | 9,242, 20 %, 8 | 10,090, 45 %, 1 | 3 / 5 | 1,661 |
| x86-64 4 CPUs | 12,772, 17 %, 5 | 9,341, 40 %, 1 | 0 / 0 | 1,733 |
| AArch64 4 CPUs | 10,055, 15 %, 2 | 7,222, 42 %, 1 | 0 / 0 | 2,267 |
| x86-64 4 CPUs | 10,615, 16 %, 2 | 7,640, 39 %, 1 | 0 / 0 | 1,762 |
| AArch64 4 CPUs | 11,928, 16 %, 3 | 8,963, 45 %, 1 | 0 / 7 | 1,684 |

The driver's share is the same figure on every CPU count and both
architectures: 12-20 % for virtio-net, 39-45 % for e1000e. The sender was
switched out at most four times a window (one CPU) and never moved. The
ring never held more than 8 of 256. The gateway's entry was reachable
before and after every window with no ARP request during it. The one
excursion is the shape of the slow mode in miniature: the last boot's
e1000e window had 7 sends of 1 ms or more holding 40 of its 508 ms, the
longest 18.7 ms, with the driver's own maximum 8.6 ms -- the vCPU stood
in the `TDT` write while the host did something else -- and the virtio
window beside it had none. The slow mode did not return while the
instrumented tree was booting, so its full histogram is still to be
seen; what it will show is this excursion for most of a window.

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
and runs on every lock a send takes -- not the UDP path. **Settled in
§7.4 (2026-10-07)**: alternating `LOCKDEP=1` and `LOCKDEP=0` boots of
the same kernel read 12.7-13 k against 28-36 k UDP sends/s on x86-64,
with ARP, the irqrestore pair and the FPU switch all about twice as
fast without it. The drift is lockdep, by about half of every number.

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
removed). The data: with the window the test took 1,661-2,267 ms in the
ten instrumented boots (the one in §1.3 and the nine in its table; both
architectures, one to four CPUs),
against 3,705-15,257 ms for the count in the slow mode; its worst case is one NIC's ARP phase at the
slowest rate ever recorded (1.7 k rt/s: 1.2 s) plus 0.5 s plus a 3 s
drain that fails it, under 5 s, and the second NIC does not run after a
failure. The 20 s entry was sized for a count that no longer exists.

Probe: `tools/nicbench-host-cost-probe.py`. It boots a clone whose runner
panics right after the benchmark with its verdict and duration, with the
count-bound phase restored (`--old`) or not, and with or without the host
flood (`--load N`), and prints both NICs' rates, histograms and driver
shares. The flood is four processes doing the same unconnected `sendto`
in a loop; as §1.3 says it slows the whole host (ARP falls with it), so it
stands for a slow host rather than for the ARP-normal mode, which could
not be summoned. What it shows is the structural claim: the count's
duration follows the host's cost, the window's does not.

| run | UDP phases (ms) | eth0 / eth1 sends/s | sends >= 1 ms | driver share | test ms |
| --- | --- | --- | --- | --- | --- |
| x86-64 count, no load | 783 + 1,185 | 12,762 / 8,437 | 0 / 0 | 17 % / 39 % | 2,700 |
| x86-64 window, no load | 500 + 500 | 10,752 / 7,857 | 0 / 0 | 16 % / 39 % | 1,759 |
| x86-64 count, load 4 | 1,817 + 2,286 | 5,503 / 4,373 | 104 / 110 | 19 % / 44 % | 6,228 |
| x86-64 window, load 4 | 500 + 500 | 6,439 / 4,173 | 15 / 12 | 16 % / 41 % | 2,547 |
| AArch64 count, load 4 | 2,162 + 2,441 | 4,624 / 4,095 | 189 / 151 | 17 % / 45 % | 6,422 |
| AArch64 window, load 4 | 500 + 500 | 3,981 / 4,427 | 48 / 17 | 21 % / 46 % | 2,895 |

The count's UDP phases grew 2.3-fold and 1.9-fold under the load; the
window's stayed at 500 ms and the test under 3 s while reading the same
slowed rates, with the same histogram shape and driver share. The probe
asserts the count's phases exceed 1 s under load and the window's stay
under 600 ms with the test under 8 s; it exits non-zero otherwise.

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
| `--old` (clock from this thread's return) | both FAIL: `check failed: sync_ns >= MS(10)` (lines 956, 1305) | both FAIL, the same two checks |
| fixed (clock from the callback's entry) | both PASS: `unregister_sync returned 20 ms after it entered`, `cancel_sync returned 20 ms after it entered` | both PASS, the same two lines (CPU 1) |

Four boots, four CPUs each, 11-12 s to the verdict. The old measurement
reads about 5 ms with the delay in place (20 ms of hold, 15 of them
spent before the clock started); the new one reads 20 ms whatever the
thread did in between, because the callback's hold is what it measures.
The suite's own runs of the two tests (22 boots this session, one to four
CPUs, both architectures) all passed, as they did before: the fix is for
the loaded-host case the probe makes deterministic.

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
| forced | 1.5 s | the deadline passes; the report names CPU 1 and the panic ends `; not by cpu 1` (below) |
| control | 100 ms | every CPU acknowledges; the shootdown returns after 100 ms and the probe's own panic says so; no `did not acknowledge` line |

The forced run's report, 6 s into the boot, symbolized with
`llvm-symbolizer` against the probe build's kernel:

```
mmu: TLB shootdown of 0xffffffff80177000+0x1000: 2 of 3 CPUs acknowledged within 1 s
cpu 1: did not acknowledge; running thread 6 'idle', irq_depth 1, preempt_count 0, last tick 999 ms ago at pc 0xffffffff80084773
cpu 1: pc 0xffffffff80084756 sp 0xffffc0001041dea0 (nmi, 94 us ago)
  #0  0xffffffff80084756     arch_cpu_relax                   cpu.c:184
  #1  0xffffffff80086eef     arch_mmu_shootdown_ipi_handler   mmu.c (the probe's hold loop)
  #2  0xffffffff80085ba2     x86_trap_dispatch                trap.c:87
  #3  0xffffffff8008407f     isr.S:145
  #4  0xffffffff80018e35     idle_main                        sched.c:86
  #5  0xffffffff800169a5     thread_trampoline                thread.c:40
  held by cpu 1 (0):
KERNEL PANIC: mmu: TLB shootdown of 0xffffffff80177000+0x1000 acknowledged by 2 of 3 CPUs; not by cpu 1
CPU: 0  context: thread  thread: 1 'kmain'  irq_depth: 0  preempt_count: 1
```

That is the answer the three sightings could not give: which CPU, that
it was inside an interrupt handler (`irq_depth 1`) with its tick stopped
for the whole second (`last tick 999 ms ago`, at `arch_cpu_wait_for_
interrupt` -- it was idle when the IPI arrived), and the frames it was
in, taken by NMI through the masked interrupts. A real sighting's trace
will show whatever that CPU was really doing; the lockup unit's existing
`cpu N: no answer` fallback covers a CPU that cannot even take the NMI.

## 4. Watched, not acted on

Three items the unit was to watch and record only if they recurred. In
the 22 full debug boots of this session (12 of `main`, 1 more of it, 9 of
the instrumented tree at one, two and four CPUs, both architectures):

- **`prio-inversion`** (one sighting, 2026-10-05): passed every boot,
  52-59 ms on AArch64 at two and four CPUs, 316 ms on one, 368-420 ms on
  x86-64. No recurrence.
- **`syscall-fuzz` over budget** (plan §2): 3,182-3,423 ms on x86-64,
  3,663-3,884 ms on AArch64, every boot. No recurrence.
- **The x86-64 180 s boot budget**: the x86-64 boots took 131.8-148.4 s by
  the harness's clock; the longest was `main` boot 3 at 148.4 s, in the
  slow mode and beside this session's reading. No timeout. The second
  sighting that would be the case for raising it has not happened.

No `SELFTEST: FAIL`, watchdog report or panic in any of the 22 boots.

## 5. Validation

On the final tree, one QEMU at a time, this host (eight cores, four of
them efficiency cores), 21:07 to 21:40:

| step | x86-64 | AArch64 |
| --- | --- | --- |
| `make host-test` | passed (62 s, host-native) | -- |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 134.8 s, 427 self-tests | PASS 134.4 s |
| `make test-smp2` | PASS 140.5 s | PASS 135.0 s |
| debug boot, 1 CPU | PASS 130.7 s | PASS 132.9 s |
| `make test-chaos` | PASS 145.5 s | PASS 132.4 s |
| release build and boot | PASS 17.2 s | PASS 20.1 s |

The first release build failed: `nettest.c` is compiled without
`CONFIG_SELFTEST` in a release kernel and the transmit probe's API was
declared only under it. The header now carries no-op stubs for that
build (zeros to a reader), as the fault-injection seams do; both release
images then built and booted. `net-nicbench` in the eight debug boots:
1,661-2,267 ms (§1.3's table). The eleven probe boots (§§1.5, 2, 3) ran
on clones of the committed tree before the chain. No `SELFTEST: FAIL`,
watchdog report, lockdep report or panic in any boot of this session
other than the ones the probes ask for.

## 6. What remains unattributed

- The host condition that puts QEMU's UDP path into its slow mode
  (§1.6). The instrumented test will show its histogram and the driver's
  share the next time it is on; the test no longer fails for it.
- ~~The gradual drift of both benchmark columns since September (§1.4)~~
  -- settled by §7.4: lockdep, about half of every benchmark number in a
  debug boot.
- What demoted the sighting's QEMU for 25 minutes (§7.2): the demotion
  reproduces the mode, the cause of the demotion was not recorded.
- Why the second interface's gateway sometimes stays unresolved (§7.5):
  the report now carries the counter that decides between the candidates.
- A real TLB shootdown sighting with the new report: none has happened
  since the instrumentation was built. The probe shows the report's
  content on a CPU made silent; what a natural one shows is still to be
  seen.

## 7. Host modes: efficiency cores, foreground load, lockdep (follow-up, 2026-10-06/07)

Two unexplained per-boot "modes" on this macOS host (four performance
and four efficiency cores): the ARP-normal, UDP-slow mode of §1.2, and
the two-mode `irqrestore-bench`/`fpu-bench` readings in `flakes.md`. The
hypothesis under test: macOS places QEMU's vCPU or main-loop threads on
efficiency cores. The tooling gained `QEMU_WRAP`, a command QEMU is run
through (`scripts/qemu-run.sh`), so a boot can be pinned by policy:
`taskpolicy -b` (the background policy: efficiency cores and throttling)
and `taskpolicy -c utility` (the utility QoS clamp: efficiency cores
preferred, less throttling). Unchanged `main` (`1f2c8237`'s kernel, a
shared clone), four CPUs, one boot at a time. Baseline readings are the
22 boots of §1.2/§1.3 and §5 on the same host the day before.

### 7.1 `taskpolicy -b`: a uniform fourfold demotion, not a mode

| boot | irqrestore pair | fpu switch with / without | eth0 arp / udp / driver share | eth1 arp / udp / driver share | outcome |
| --- | --- | --- | --- | --- | --- |
| x86-64, 180 s budget | 1,691 | 16,989 / 14,014 | not reached | not reached | timed out before the network tests |
| x86-64, 600 s budget | 1,489 | 18,214 / 21,648 | 1,447 / 1,579 / 16 % | 2,279 / 1,077 / 74 % | 376 s of self-tests, 9 timing failures |
| AArch64, 600 s budget | 1,806 | 28,752 / 27,766 | 1,039 / 1,032 / 9 % | 1,435 / 831 / 76 % | 613 s, timing failures |

Against the baseline's 330-540 ns a pair and 4.3-6.9 us a switch, this
is every number four times slower, ARP and UDP alike (the baseline's
ARP is 7-10 k): QEMU as a whole demoted and throttled, with the
self-tests that assert a tick rate (`timer`, `smp-ticks`,
`lockup-soft`/`-hard`) failing as they would on any starved host. The
e1000e driver share rose from 40 % to 75 %: the synchronous host
transmit path got relatively dearer on the slow cores. Neither mode's
shape -- not the 1.3x of the benchmark modes, not ARP staying normal.

### 7.2 The utility clamp, niceness, and the two together

The first run of this loop was started from zsh with `&`, whose default
`BG_NICE` option gave every QEMU in it **nice 5** on top of whatever the
variant asked for -- noticed from `ps -o pri,nice` during a boot and
redone (the lesson is in the flake file's family of "a control must
differ in one thing"). The confounded run is kept because it is the one
that reproduced the mode:

**Utility clamp + nice 5** (`taskpolicy -c utility`, QEMU at pri 20 nice 5):

| boot | irqrestore pair | fpu with / without | eth0 arp / udp / driver share | eth1 arp / udp / driver share | nicbench ms |
| --- | --- | --- | --- | --- | --- |
| x86-64 1 | 510 | 6,135 / 5,854 | 6,479 / 6,569 / 17 % | 6,737 / **1,626** / **75 %** | 1,891 |
| x86-64 2 | 538 | 6,139 / 5,850 | 5,959 / 10,730 / 16 % | 2,562 / 6,803 / 42 % | 2,400 |
| x86-64 3 | 404 | 5,230 / 5,098 | 6,831 / 12,850 / 17 % | 6,841 / 7,797 / 42 % | 1,871 |
| AArch64 1 | 417 | 6,670 / 6,057 | 5,480 / **1,458** / 5 % | 6,541 / **1,437** / **74 %** | 2,149 |
| AArch64 2 | 274 | 4,735 / 4,459 | 5,650 / 11,121 / 15 % | 6,592 / 8,974 / 50 % | 2,166 |
| AArch64 3 | 275 | 4,689 / 4,595 | 5,695 / 11,551 / 16 % | 2,650 / 9,108 / 45 % | 2,571 |

Three of twelve NIC rounds, in two boots of six, read the §1.2
sighting's shape: UDP at 1.4-1.6 k with ARP at 5.5-6.7 k beside it and
the e1000e driver share at 74-75 % where the baseline reads 39-45 %. The
slow round's histogram on AArch64 1, `eth1`: 565 of 719 sends in the
512-1024 us bucket, 74 % of the window inside the `TDT` write -- the
host's synchronous transmit, running slowly. On `eth0` (virtio) in the
same boot the sends themselves stayed at 128-256 us with the driver
share at 5 %, yet only 730 fitted in 500 ms: the vCPU was losing its core
between sends, as it would to QEMU's main loop woken by each kick on a
shared efficiency core. The windowed test held its phases at 500 ms and
its totals at 1.9-2.6 s through all of it.

**Utility clamp alone** (nice 0, pri 20), four boots, and **nice 5 alone**
(`nice -n 5`, pri 31), six boots:

| arm | boot | irqrestore pair | fpu with / without | eth0 arp / udp / driver | eth1 arp / udp / driver |
| --- | --- | --- | --- | --- | --- |
| clamp | x86-64 1 | 403 | 5,509 / 5,074 | 6,557 / 12,400 / 17 % | 2,707 / (gateway unresolved: not measured, §7.5) |
| clamp | x86-64 2 | 428 | 5,073 / 4,792 | 7,272 / 13,107 / 17 % | 8,108 / 9,351 / 42 % |
| clamp | AArch64 1 | 276 | 4,721 / 5,009 | 5,567 / 11,581 / 16 % | 7,394 / 10,159 / 45 % |
| clamp | AArch64 2 | 420 | 6,475 / 6,260 | 6,015 / 9,959 / 15 % | 6,637 / 7,791 / 43 % |
| nice 5 | x86-64 1 | 500 | 6,374 / 5,982 | 7,954 / 10,657 / 16 % | 6,792 / 7,761 / 39 % |
| nice 5 | x86-64 2 | 501 | 6,223 / 5,957 | 2,637 / 10,314 / 16 % | 2,650 / 6,807 / 42 % |
| nice 5 | x86-64 3 | 407 | 5,084 / 4,806 | 7,906 / 13,196 / 17 % | 9,368 / 9,511 / 41 % |
| nice 5 | AArch64 1 | 272 | 4,630 / 4,373 | 8,279 / 11,851 / 16 % | 9,202 / 10,024 / 44 % |
| nice 5 | AArch64 2 | 279 | 4,851 / 4,617 | 10,298 / 11,972 / 16 % | 10,499 / 10,258 / 44 % |
| nice 5 | AArch64 3 | 273 | 4,681 / 4,411 | 9,214 / 11,565 / 16 % | 2,921 / 9,081 / 45 % |

Not one slow UDP round in ten boots. (The AArch64 clamp boots fail the
three tick-rate self-tests, `timer`, `smp-ticks` and `lockup-hard`, as
the demoted ones do; the x86-64 ones pass.)

**What this settles, and what it does not.** The UDP mode's fingerprint
is now known and reproducible: a QEMU demoted below the host's other
work -- the utility clamp *and* a lowered priority together -- reads it
in about a third of its NIC rounds, with the e1000e driver share at 75 %
and the virtio vCPU losing its core between kicks. Either demotion alone
did not, in ten boots. So "macOS put QEMU on the efficiency cores" is a
necessary part of the explanation of the 25-minute sighting but not the
whole of it; what demoted that session's QEMU, and whether it was this
combination, was not recorded. The check in `docs/development.md`,
"Benchmark runs" (QEMU's `pri`/`nice` before a measurement; a plain boot
is pri 31 nice 0) is what would have caught it.

**Not confirmed for the benchmark modes:** across the 24 boots above the
`irqrestore-bench` pair reads its two modes regardless of arm (x86-64 403-
538 in every arm, AArch64 272-279 or 417-420 in every arm), and the policy
that certainly puts QEMU on the efficiency cores (§7.1) moves the number
fourfold, far past the modes' 1.3x. Core type is not what separates them;
the earlier hypothesis -- where TCG's translation buffer lands per run --
stands, untested. A negative result.

### 7.3 Default placement, a CPU-heavy foreground job beside QEMU

Four `yes > /dev/null` processes started just before each boot and killed
after it, QEMU at its default pri 31 nice 0 (the confounded first run,
with QEMU at nice 5 under the same load, read 610-750 ns a pair and
timed out both boots; not tabulated):

| boot | irqrestore pair | fpu with / without | eth0 arp / udp / driver | eth1 arp / udp / driver | outcome |
| --- | --- | --- | --- | --- | --- |
| x86-64 1 | 566 | 10,249 / 10,459 | 4,325 / 4,933 / 17 % | 2,730 / 3,322 / 41 % | timed out at 180 s |
| AArch64 1 | 588 | 12,977 / 16,606 | 2,125 / 3,981 / 14 % | 1,827 / 5,845 / 48 % | timed out at 240 s |
| x86-64 2 | 905 | 9,765 / 17,385 | 3,795 / 4,611 / 16 % | 1,699 / 3,678 / 43 % | timed out at 180 s |
| AArch64 2 | 579 | 10,679 / 10,181 | 4,259 / 3,794 / 15 % | 3,172 / 3,361 / 41 % | timed out at 240 s |

Eight runnable threads for eight cores, four of them efficiency cores:
everything about twice as slow, ARP with UDP, the driver share unchanged
at 41-48 %, and the suite past its whole-boot budget. A loaded host of
the CI kind (§1.2's slow runners), not the selective mode. The same
reading as the host flood in §1.5: load lowers every number together.

### 7.4 The September-to-October drift: LOCKDEP=1 against LOCKDEP=0

Alternating four-CPU debug boots of the same `main`, the default
`LOCKDEP=1` against `LOCKDEP=0` built into its own output tree, QEMU at
its default placement, one boot at a time:

| boot | irqrestore pair | fpu with / without | eth0 arp / udp / driver | eth1 arp / udp / driver | nicbench ms |
| --- | --- | --- | --- | --- | --- |
| x86-64 L1 | 400 | 5,204 / 4,797 | 8,408 / 12,866 / 17 % | 8,663 / 9,084 / 41 % | 1,691 |
| x86-64 L0 | 216 | 2,772 / 2,490 | 15,481 / 33,476 / 21 % | 16,580 / 20,565 / 50 % | 1,493 |
| x86-64 L1 | 409 | 5,305 / 4,999 | 8,350 / 12,654 / 17 % | 2,831 / 7,884 / 43 % | 2,185 |
| x86-64 L0 | 167 | 2,334 / 2,026 | 15,717 / 35,925 / 23 % | 16,955 / 22,533 / 50 % | 1,479 |
| x86-64 L1 | 410 | 5,504 / 4,909 | 7,948 / 13,060 / 17 % | 8,371 / 9,333 / 41 % | 1,723 |
| x86-64 L0 | 211 | 2,730 / 2,488 | 16,406 / 28,348 / 25 % | 18,604 / 20,031 / 49 % | 1,468 |
| AArch64 L1 | 420 | 6,964 / 6,276 | 7,877 / 9,273 / 15 % | 2,541 / 6,481 / 44 % | 2,287 |
| AArch64 L0 | 250 | 3,730 / 3,581 | 15,035 / 16,174 / 21 % | 3,252 / 19,507 / 51 % | 2,013 |
| AArch64 L1 | 293 | 4,849 / 4,606 | 9,110 / 10,922 / 16 % | 2,894 / 8,599 / 45 % | 2,158 |
| AArch64 L0 | 183 | 2,870 / 2,557 | 15,649 / 19,876 / 24 % | 3,503 / (unresolved gateway, §7.5) | 1,966 |
| AArch64 L1 | 443 | 6,732 / 6,295 | 7,904 / 9,492 / 16 % | 7,204 / 6,287 / 39 % | 1,791 |
| AArch64 L0 | 172 | 5,618 / 3,294 | 16,154 / 21,325 / 26 % | 15,844 / 25,712 / 54 % | 1,490 |

**Confirmed: the drift is lockdep**, and more than the drift. Without it
the debug kernel reads roughly twice the rate on every column: x86-64 UDP
28-36 k against 12.7-13.1 k, ARP 15.5-16.4 k against 7.9-8.7 k, the
irqrestore pair 167-216 against 400-410 ns, the FPU switch 2.0-2.8 against
4.8-5.5 us; AArch64 the same shape. The e1000e unit's September table
(20-23 k UDP, 12-14 k ARP on x86-64) was measured before lockdep landed
in the debug build (PRs #302-#313, 2026-10-03 to -06) and sits between the
two columns, where a kernel with less lock instrumentation than today's
`LOCKDEP=1` and none of `LOCKDEP=0`'s saving would. Every lock a send
takes -- the socket mutex, the ARP table, the firewall's flow lock, the
driver's ring lock, the quiesce read-side section -- pays lockdep's
class and dependency checks in a debug boot, and the benchmark's rates
are debug-boot rates. The e1000e offload table's conclusion is
unchanged (the checksum's share is a few percent either way), and the
two benchmark modes are present at `LOCKDEP=0` too (x86-64 167 against
211-216; AArch64 183 against 250), so lockdep is the level, not the
modes. §1.4's "not bisected" is settled: no unit's code made the path
slower; the instrumentation that watches its locks did, by about half.

### 7.5 Found on the way: a UDP rate for frames that never left

Two x86-64 boots in this follow-up (one in §5's chain the day before,
one in the clamp arm above) printed, for `eth1`, `gateway arp incomplete
-> incomplete, +0 requests, +5299 pending dropped` and `0 frames left the
driver` beside a headline `udp 12563 sends/s`. Every datagram of the
window parked behind an ARP entry that never completed and evicted the
one before it; the stack's acceptance count became a rate. The two-week
tally has the same `0 of 10000 frames left the driver` five times in 325
x86-64 `eth1` rounds, never on `eth0` or AArch64. The warm-up slept
20 ms after its first send and checked nothing.

The test now waits up to 1.5 s (one ARP retry) for the gateway's entry,
prints how long it waited, and when the entry is still incomplete says
`udp not measured: the gateway's ARP entry is still incomplete after N ms
(+R requests sent since the warm-up began, ...)` and claims no rate.
Reported, not asserted: the ARP phase just before it proved 2,000 round
trips on the same link. Why the entry stays incomplete is not attributed;
the `+R` the report now carries decides between the two candidates next
time (`+0`: `arp_resolve` found an entry already there -- the table is
keyed by IP alone and both NICs' gateways are 10.0.2.2 -- and sent
nothing, its retry going out on that entry's interface, which for a stale
`eth0` entry is the one the test just took down; `+1`: a request left
`eth1` and no reply reached the table). `docs/testing/flakes.md` has the
entry.
