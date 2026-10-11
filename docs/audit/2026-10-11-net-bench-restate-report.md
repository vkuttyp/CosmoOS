# `net-bench`: counters and fixed windows -- 2026-10-11

A blocker for roadmap M4, not one of its PRs. M4 PR 1 (#351, the DHCP
client) merged and main's aarch64 boot failed `net-bench`'s 8 s per-test
budget twice (run 38127318850), every assertion passing; #351 was reverted
(#356). The owner's plan: instrument `net-bench` (Step 1), run it with #351
re-applied on CI (Step 2), and by the data either fix a loopback/TCP defect
or restate the test (Step 3). The data said restate. The facts are in
`docs/testing/flakes.md`, "`net-bench` over its 8 s guard with #351's code".

## Step 1: the counters

Per round (TCP one flow, two flows, UDP), `net-bench` prints the wall time,
RTO fires and the largest backoff reached, fast retransmits, segments
retransmitted, the time senders waited on a retransmission timer, each
netrx worker's drops and loopback's drops (`bench_snap`, `bench_report` in
`kernel-services/network/nettest.c`). The TCP side (`tcp_stats`
`rto_fires`, `fast_retransmits`, `rto_backoff_max`, `rto_wait_ns`; the pcb's
`rexmit_armed_ns`) is counted in `CONFIG_SELFTEST` builds only: a release
kernel's TCP path is unchanged, and `tcp_test_reset_rto_backoff_max` is
absent from both release kernels (`llvm-nm`).

## Step 2: the data

Draft PR #362 (Step 1 plus #351) ran eight debug boots per architecture on
CI. In the slow aarch64 rounds (TCP 0.5-0.86 s, UDP 1.4-2.2 s): one RTO of
a bench flow in 16 boots, no fast retransmits, no drops in any TCP round;
UDP, which retransmits nothing, the slowest phase. The time is the host's,
which the bench measured because it was a fixed amount of work.

## Step 3: the restate

Each phase is a fixed 300 ms window. Each TCP client sets its own deadline
when it has connected and reports what it sent and how long it took; the
rate is the sum of each flow's bytes over its own time. The assertion is
each flow's integrity: no error, something sent, and exactly that
received; a failed or empty flow beside a good one fails the test rather
than lowering the rate. UDP sends for its window too and checks every
datagram arrived. The 8 s budget is a hang guard again
(`docs/testing/flakes.md`, the restate rule). Not a defect fix, so there is
no `--old` probe.

## Validation

Full matrix on both architectures (host-test, fuzz, analyze, debug boots
at `QEMU_SMP=1/2/4`, `test-smp2`, `test-chaos`, `test-harness-retry`,
release boot): PASS. `test-net-login` does not exist on this tree (#351
reverted). Local debug boots: `net-bench` 1.96 s aarch64, 1.92 s x86-64.

## Recorded, not chased

A bench flow's 200 ms RTO with no drop anywhere (twice in 16 boots), and a
connection left from an earlier test retransmitting at backoff 4-7 during
`net-bench` (deferred-work inventory, 2.5).
