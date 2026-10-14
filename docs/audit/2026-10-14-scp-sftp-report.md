# scp and sftp — 2026-10-14

The third of the owner's post-1.0 units (after the everyday commands and
`ping`): copying files to and from the installed system over ssh. Base:
main `ae94d281` (`ping`, PR #382). Goal: a stock OpenSSH client's `scp`,
in both of its protocols, and `sftp` work against the installed system.

## What a client needs from the server

| Client | What it runs on the server | Before |
|---|---|---|
| `scp -O` (the classic protocol) | `scp -t <dir>` or `scp -f <file>` through the session's shell | no `scp` in the image |
| `scp` (OpenSSH 9.0 and later: SFTP underneath) | the `sftp` subsystem | `DROPBEAR_SFTPSERVER 0`; dropbear ships no sftp-server |
| `sftp` | the `sftp` subsystem | the same |

## Changes

| Area | Change | Files |
|---|---|---|
| scp | dropbear's own `scp` built beside the server (`PROGRAMS="dropbear dropbearkey scp"`), installed as `/bin/scp`; the session's PATH finds it | `ports/dropbear/dropbear.mk` |
| sftp | a new pinned port, OpenSSH portable 10.6p1, building `sftp-server` alone (no OpenSSL, zlib, PAM or PIE), installed as `/usr/libexec/sftp-server`; dropbear's sftp subsystem on and pointed there | `ports/openssh/`, `ports/dropbear/localoptions.h`, `Makefile` |
| boot namespace | archive entries under `usr/` keep their path (`usr/bin/`, `usr/sbin/`, `usr/libexec/` executable) instead of landing under `/boot`: the first build put `sftp-server` at `/boot/usr/libexec/` and the subsystem answered "not found" | `kernel-services/vfs/ramfs.c` |
| Linux door | `prctl` with `PR_SET_DUMPABLE`/`PR_GET_DUMPABLE` (x86-64 157, AArch64 167), the flag kept on the process (fork inherits it, exec resets it) and reported; every other option `-EINVAL`. `sftp-server` makes itself undumpable at start and exited 255 ("unable to make the process undumpable: Function not implemented") without it | `compat/linux/syscalls.c`, `process.h`, `process.c` |
| release and CI | the port in the CI cache keys, `check-reproducible.sh` (both trees, `scp` and `sftp-server` compared), the release notes' pinned-source table | `.github/workflows/ci.yml`, `scripts/` |

## Found on the way: a scheduler corruption, two causes

The aarch64 user guide on macOS panicked at the ssh page's sftp step:
`assertion failed: list_empty(&t->rq_link)` in `rr_enqueue`, a thread
queued twice -- dropbear, preempted while it released an exited
`sftp-server`. A stress script (install, then sftp batches and `scp -O`
in a loop against the installed AArch64 system) reproduced it at the
fourth round; 200 rounds of plain ssh commands did not, so the data
relaying in dropbear's `select` loop is what exposes it. Both causes are
on main, older than this unit.

| Stress (AArch64, sftp + scp per round) | First panic |
|---|---|
| neither fix | round 4 |
| `waitqueue_prepare` fixed (S34) | round 170; round 29 |
| `poll` fixed | round 25 |
| both | none in 800 rounds (two runs of 400) |

- **S34.** `poll` arms one wait queue per descriptor; a wake from one
  armed earlier made the thread READY and queued it, and the next
  `waitqueue_prepare` wrote BLOCKED over that: a BLOCKED thread on a run
  queue, queued again by the next wake or preemption.
  `sched_prepare_block_current` now leaves READY alone, under the
  run-queue lock. `wait-multi-prepare` checks it; `tools/wait-ready-probe.py`.
- **`io_poll`'s timer.** The timer and its alarm live in `io_poll`'s
  frame; `timer_cancel` did not wait for a callback running on the CPU
  that armed it, so a poller moved elsewhere returned while it ran, and
  the callback wrote into the dead frame and woke a thread through a
  pointer read from it. `timer_cancel_sync` now, as epoll and the aio
  ring's wait already did. `poll-timer-outlives` checks it;
  `tools/poll-timer-probe.py` (with the fix reverted, AArch64 faulted in
  `alarm_fired` on a NULL thread read from the dead frame before the test
  ran -- the race itself).

## Tests

- `tests/boot/net_login_test.py` (`make BUILD=release test-net-login`,
  CI on both architectures): 256 KiB of random data to the installed
  system and back with `scp -O` and with `scp`, SHA-256 compared on both
  sides; an `sftp -b` batch doing `mkdir`, `put`, `ls -l`, `get` (the
  digest compared), `rename`, `rm` and `rmdir`, and the directory gone.
- The user guide's ssh page copies a file each way with `scp` and runs an
  `sftp` batch (CI runs the guide on Linux and macOS).
- `lxtest`: `PR_GET_DUMPABLE` is 1, `PR_SET_DUMPABLE` 0 then reads 0, 2
  is `-EINVAL`, an unknown option is `-EINVAL`.

## Validation

- **Matrix**, local, at 5e5beaf0: 36 of 38 steps pass. AArch64
  `test-entropy` failed before the kernel ran (QEMU could not bind its
  host-forwarding port: the collision `flakes.md` records) and passed on
  rerun. AArch64 `test-chaos` failed once in `lxpty` check 35 (recorded
  in `flakes.md`, not attributed) and passed four reruns, as main did
  four times.
- **`make BUILD=release test-net-login`** passes on both architectures.
- **The user guide on macOS**, both architectures: the ssh page's `scp`
  and `sftp` steps included (AArch64 panicked there before S34 and the
  `poll` fix).
- **Stress**, AArch64, sftp batch and `scp -O` per round: 1,200 rounds in
  three runs, no panic (round 4 before the fixes).
- **Probes**: `wait-ready-probe` and `poll-timer-probe`, both modes, both
  architectures.

## Not done here

- Port forwarding stays compiled out of dropbear.
- `prctl` options other than the dumpable flag.
- No `scp`/`sftp` client on CosmoOS pointed at other machines (dropbear's
  `scp` is the server half here; there is no `dbclient` or `ssh` client).
