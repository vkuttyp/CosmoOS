# be-space's missed deadline attributed: a slow runner, restated, 2026-10-24

The first of v1.0.14's four PRs (#453). The brief put the attribution
before anything else in the unit touches the page cache, because the
MEM-9 and CFS-32 changes alter the evidence.

## Scope and baseline

Main at `5cfd1ffe` (#452), CI run 40117055622 green on every job.

`test-be`'s `be-space` boot on an encrypted root missed its 240 s
per-boot deadline twice on CI's x86-64 runners, at the same command
(`docs/testing/flakes.md`, "`test-be`'s `be-space` boot on an encrypted
root out of time, 2026-10-24"):

| run | PR, commit | job | result |
| --- | --- | --- | --- |
| 40057006013 | #448, `00ca2724` | `encrypted root test (x86_64)` | `be-space: no prompt after "cosmo-snap list \| grep 'KiB free'" before the deadline` |
| 40079613201 | #448, `3035ded4` | `encrypted root test (x86_64, mirror be)` | the same, after 21 fill rounds took the root to 6712 KiB free |

Neither serial log had times, so a slow runner and one stuck command
could not be told apart. MEM-9's mechanism (a list blocked behind
write-back that cannot finish on a nearly full pool) was the first
hypothesis to test.

## Evidence

**The failing runners were slow before be-space too.** From the job
logs' timestamps to the be-space boot's own firmware clock, the 16 boots
before it took 474 s (run 40079613201: test-mirror's PASS at 09:00:57,
be-space's firmware at 09:08:51) and about 464 s (run 40057006013:
test-be ran 708 s, be-space 240 s of it). Both runners' test-mirror took
295.9 s and 296.7 s, the two slowest of eight x86-64 samples (the others
156.6 to 237.4 s).

**The CI runner's own timing log.** The first commit of this PR uploads
`test-be-encrypted/*-times.log` on a pass. Run 40123417299 (this PR,
x86-64 `mirror be`): test-mirror 220.7 s, test-be 572.7 s, the 16 boots
before be-space 346 s (19:10:19 to 19:16:05), and be-space itself:

| what | count | each, CI | each, local |
| --- | --- | --- | --- |
| `cosmo-snap list \| grep 'KiB free'` | 22 | 6.2-6.5 s | 2.7-5.4 s |
| a fill round (`cat` x 32, `sync`) | 21 | 1.1 s | 1.1-1.2 s |
| `cosmo-upgrade /tmp/b` (the refusals) | 2 | 1.4, 6.3 s | 2.1, 6.7 s |

Every command answered, and none grew as the pool filled: the last list
answered `244 KiB free` in 6.2 s, typed at 174.2 s; the boot ended at
187.3 s. `cosmo-snap list` costs what it does because `snap_list_sized`
(`cosmofs.c`) walks every snapshot's tree and the live tree under the
mount's lock to size them, once per list.

**The two together.** The failing runners took 1.37 and 1.34 times this
runner's 346 s over the same boots. Scaled by 1.35, this boot types its
last list at about 235 s and has it answered at about 244 s: past the
240 s deadline, at the command both sightings name, with every command
before it answered. Nothing is stuck; the boot is longer than its
deadline on a runner of that speed.

**The prediction, met on a slow runner.** The job re-run on this PR's
first commit (run 40123417299 attempt 2, the old harness) landed on a
runner of the failing kind: test-mirror 305.5 s, the 16 boots before
be-space 472 s (19:42:51 to the firmware's 19:50:43). It failed as the
two sightings did, `be-space: no prompt after "cosmo-snap list | grep
'KiB free'" before the deadline (240 s from launch; the last line at
238.5 s)`, and its timing log (artifact `test-be-times-x86_64-2`) shows
how: all 21 lists of the fill answered, at 8.4-8.9 s each, 1.37 times
the faster runner's 6.2-6.5 s; every fill round 1.6-1.7 s; the last
list, at 6684 KiB free, answered in 8.8 s at 237.1 s; the next fill
round in 1.3 s; and the final list typed at 238.5 s, 1.5 s before the
deadline. Every command answered; the last one was late.

**Locally, no hang in CI's end state.** `be-space` took 149.0 s in a
full local `test-encrypt-b1b4 ENCRYPT_B1B4_TESTS='mirror be'` (x86-64,
`QEMU_SMP=4`). Five replays from a disk saved just before be-space (a
scratch hook in `be_test.py`), four of them with the first fill round
104 KiB short so the fill ends where CI's did (240 KiB free; the fifth
132 KiB), took 107.3-131.7 s, two of them under the host slow mode
(`QEMU_WRAP="taskpolicy -c utility nice -n 5"`), every list 2.7-3.5 s to
the last. MEM-9's hypothesis is not the mechanism: the last fill's
`sync` exits, the list after it answers as fast as the first, and on CI
it answered at the same 6.2 s as the rest.

CI's write throughput is not what differs: its fill rounds run at the
local 1.1 s, though its 1 GiB encrypted write in test-encrypt took 555,
819 and 890 s on main's last three runs against 386 s locally. The
list, a CPU-bound walk, is what runs at half speed there.

## Classification and change

`docs/testing/flakes.md`'s three-way rule: **restate**. The per-boot
deadline was a hang guard standing for "every command answers"; for a
boot of forty sequential commands its total is the runner's speed times
their number.

- `install_test.Boot.per_command(seconds)`: from then on each command
  typed (and the closing `poweroff`) moves the boot's deadline to at
  least that many seconds after it was typed. A command that never
  answers still fails within its own budget; the boot's deadline is
  never shortened.
- `be_test.py`: be-space is `stage_space`, and takes `BE_SPACE_COMMAND_S`
  = 65 s per command, ten times its slowest on CI, with the
  classification at the site. `--only space` (`BE_ONLY=space`, also in
  test-encrypt-b1b4's run of test-be) runs that boot alone after an
  install.
- `tests/boot/test_boot_deadline.py` (in `make host-test`) drives Boot
  against a fake shell: ten 0.15 s commands miss a 1 s boot deadline
  without a budget and pass with a 0.5 s one each; a command that never
  answers fails 0.5 s after it is typed; a budget shorter than the boot's
  remaining time leaves its deadline alone.
- CI uploads test-be's timing logs from the encrypted `mirror be` job on
  a pass too (`test-be-times-<arch>-<attempt>`).

Nothing in the kernel changes, and be-space asserts what it did.

## Probe

`tools/be-space-probe.py` puts this host where those runners were: a
release `make test-encrypt-b1b4 ENCRYPT_B1B4_TESTS=be BE_ONLY=space`
with `BOOT_TIMEOUT` at 45 s: under be-space's length run alone after an
install (about 70 s on both architectures, fewer trees to walk than in
test-be's sequence), and well over any one command (8.2 s at most), the
install's boots (16 s) and a boot to its first prompt (10 s). `--old`
reverse-applies the fix commit whole in a throwaway worktree.

    PROBE: PASS arch=x86_64 mode=fixed: test-be passes; be-space took 62.2 s against a 45 s boot deadline
    PROBE: PASS arch=x86_64 mode=old: the deadline missed: test-be: FAIL: be-space: no prompt after "cosmo-snap list | grep 'KiB free'" before the deadline (45 s from launch; the last line at 44.6 s)
    PROBE: PASS arch=aarch64 mode=fixed: test-be passes; be-space took 180.9 s against a 45 s boot deadline
    PROBE: PASS arch=aarch64 mode=old: the deadline missed: test-be: FAIL: be-space: no prompt after "sh -c 'i=0; while [ $i -lt 32 ]; do cat /tmp/rnd; i=$((i+1)); done > /fill12; sync'" before the deadline (45 s from launch; the last line at 45.0 s)

The old mode misses its deadline wherever the boot is when 45 s run
out, a list on one architecture and a fill round on the other, with the
last line a fraction of a second before it: the shape of CI's sightings,
a boot still answering. (The AArch64 fixed run's 180.9 s is the host:
its load average was 26-34 on 8 cores while the probes ran.)

The first round used 80 and 120 s, under be-space's length inside
test-be (107-150 s) but not alone (70.3 s, 69.8 s): every mode passed,
the old ones too, and the deadlines were lowered.

## Validation

The device-lifecycle matrix on `9a8ee222`, local, plus `test-install`, `test-be` and
`test-encrypt-b1b4 ENCRYPT_B1B4_TESTS=be` in release builds, since the
change is to `be_test.py` and the boot harness:

| Step | x86-64 | AArch64 |
|---|---|---|
| `host-test` (host) | pass, 96 s | |
| `fuzz` (host) | pass, 76 s | |
| `analyze` | pass, 338 s | pass, 239 s |
| debug `test`, `QEMU_SMP=1` | pass, 178 s | pass, 400 s |
| debug `test`, `QEMU_SMP=2` | pass, 162 s | pass, 185 s |
| debug `test`, `QEMU_SMP=4` | pass, 175 s | **fail**, 270 s: `net-tcp-delack`, 1 of 507 |
| `test-smp2` | pass, 162 s | pass, 206 s |
| `test-chaos` | pass, 202 s | pass, 214 s |
| `test-harness-retry` | pass, 211 s | pass, 202 s |
| release `test` | pass, 18 s | pass, 58 s |
| release `test-install` | pass, 35 s | pass, 48 s |
| release `test-be` | pass, 622 s | pass, 595 s |
| release `test-encrypt-b1b4`, be | pass, 505 s | pass, 569 s |

The AArch64 failure is in a kernel self-test this PR does not touch (no
kernel file changes): `net-tcp-delack` asserts no retransmission with a
host-wide counter, and `net-lo-tcp` just before it logged 3. It is
recorded in flakes.md with its log facts and in the backlog as NET-23;
the network harness passed in every other debug boot.

Branch CI: run 40128407985 failed once on `encrypted root test (x86_64,
snapshot pkgtx fsck)`, `test-snapshot`'s rollback boot stalled in the
firmware after a guest-requested reset (flakes.md, third sighting;
backlog TEST-21). The re-run of that job passed, and the run is green.

## What is left

Nothing for this unit. be-space's length is 24 tree walks; a cheaper way
to read the root's free space in `fill_root` would shorten the boot, and
is not needed for it to be correct.
