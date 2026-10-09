# Device lifecycle Unit 2: fault and interleaving sweep

## Scope and baseline

Started 2026-10-09 from main `2661385c14c388190d8becfe95388f80c23995d3`
(PR #337 merge, main CI run `37885005992` green on x86-64, AArch64 and litmus),
on branch `device-lifecycle-fault-sweep`. The targets are the Unit 1 audit's
NVMe late publication, AHCI probe rollback ordering, virtio-rng refill,
acknowledgement failures in NVMe/AHCI/xHCI/e1000e, and callback retirement
in xHCI and the network worker. Every finding will distinguish a proven
failure from an unreachable or unresolved suspicion.

## Evidence and dispositions

| Target | Verdict | Evidence | Action |
|---|---|---|---|
| NVMe submit across controller death | Proven | `selftest_submit_die_window` calls `controller_die` after the first dead check and before the queue lock. `tools/nvme-die-window-probe.py --old` on x86-64 and AArch64 reported `accepted=1 done=0 inflight=1`; each failed only the required proof marker, while all 451 self-tests and the 100-round network harness passed. With the fix, both architectures reported `accepted=0 done=0 inflight=0` and booted cleanly. | Added an acquire dead recheck under `q->lock`; rejects with `-EIO` and unmaps every segment before returning. |

## Out-of-scope validation finding

The first AArch64 old-behavior NVMe proof run also failed the existing
`signal-stop` self-test at `userland/init/init.c` check 9. The serial log
shows the stopped child exited with status 7 before its parent returned
status 9. Check 9 expects `waitpid(..., WCONTINUED)` after sending
`SIGCONT`; the child returns immediately, and
`kernel/process/process.c:child_event_locked` gives `EXITED` priority over
`CONTINUED`. This explains how the continued event can be hidden. A repeat
old-behavior run and the fixed AArch64 boot passed `signal-stop`. The first
run is recorded in `docs/testing/flakes.md` and the deferred-work
inventory; it remains an out-of-scope finding and is not changed in this
unit.
