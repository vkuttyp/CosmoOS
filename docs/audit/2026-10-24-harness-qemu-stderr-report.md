# QEMU's stderr out of the serial text (TEST-20), 2026-10-24

## Scope and baseline

Main at `097b6a19` (#449, B5's kernel part) went red: CI run
40077407190, `mirror test (x86_64)`: `test-mirror: FAIL: the removal: no
line matching 'pool: cosmo-root: member 0 copy \d+ \(\S+\) MISSING'
within 30s`. The job's artifact `test-mirror-x86_64` holds the kernel's
line whole in `vcon.log` (the virtual console's copy):

    [ WARN] pool: cosmo-root: member 0 copy 1 (vda2) MISSING (-19): the member is degraded

and split in `boot-replace.log` (the serial capture the test matches):

    [ WARN] pool: cosmo-root: mqemu-system-x86_64: wrong value for queue_enable 0
    ember 0 copy 1 (vda2) MISSING (-19): the member is degraded

QEMU printed `wrong value for queue_enable 0` (its virtio-pci device,
written after the hot unplug the test performs) to its stderr, and
`install_test.Boot` started QEMU with `stderr=subprocess.STDOUT`: both
streams in one pipe, so the message landed inside the guest's line. The
kernel and #449 are not involved; the race is between QEMU's two output
streams, so the same test passed on `7d8f40fd` and on #447's branch.

`tests/boot/platform_test.py` and `tests/boot/run_boot_test.py` merged
the two streams the same way.

## What changed

- `install_test.Boot`, `platform_test.py` and `run_boot_test.py` give
  QEMU's stderr a file of its own beside the boot's serial log:
  `boot-<name>-qemu.log` (`boot-test-qemu.log` for `make test`, which
  CI's `boot-test*.log` upload keeps). The serial text the tests match is
  the guest's alone. A boot's first attempt starts the file afresh and a
  firmware-stall retry adds to it, so a rerun into the same `out/` does
  not pile up earlier runs' messages (CosmoReview on #450).
- `tests/host/test_boot_harness.py` (in `make host-test`): a fake
  `qemu-run.sh` writes half a guest line, a QEMU message to stderr, then
  the rest. Boot's text must hold the line whole and without the
  message, and the message must be in `boot-fake-qemu.log`. On the old
  harness it fails: `FAIL: a guest line QEMU wrote a message inside of is
  whole in the serial text`; with the change it passes. It also leaves
  an earlier run's `boot-fake-qemu.log` in place first and checks the
  boot started it afresh; opened append-only, that check fails.
- Backlog TEST-20, struck by this change.

## Validation

On the branch, from `097b6a19`:

| step | x86_64 | aarch64 |
|---|---|---|
| `make host-test` (with `test_boot_harness.py`) | PASS | (host) |
| debug `make test` | PASS 175.1 s | PASS 171.2 s |
| release `test-mirror` | PASS 184.3 s | PASS 234.6 s |
| release `test-install` | PASS 31.7 s | - |
| release `test-platforms` | - | PASS |

In the x86-64 `test-mirror`, QEMU's `wrong value for queue_enable 0`
went to `boot-replace-qemu.log` and `boot-spare-qemu.log`, the two boots
with a hot unplug, and the serial logs hold no QEMU line.

The device-lifecycle matrix on `a2497be6` (exit status and seconds):

| step | x86_64 | aarch64 |
|---|---|---|
| `make host-test` | 0, 68 s | (host) |
| `make fuzz` | 0, 24 s | (host) |
| `make analyze` | 0, 239 s | 0, 225 s |
| debug `make test`, `QEMU_SMP=1` / `2` / `4` | 0, 245 / 166 / 167 s | 0, 257 / 173 / 181 s |
| `make test-smp2` | 0, 175 s | 0, 180 s |
| `make test-chaos` | 0, 200 s | 0, 204 s |
| `make test-harness-retry` | 0, 190 s | 0, 208 s |
| `make test-crash` | 0, 170 s | 0, 237 s |
| release `make test` | 0, 22 s | 0, 32 s |
| release `test-install` | 0, 32 s | 0, 60 s |
| release `test-mirror` | 0, 179 s | 0, 236 s |
| release `test-upgrade` | 0, 688 s | 0, 837 s |
| release `test-snapshot` | 0, 135 s | 0, 181 s |
| release `test-fsck` | 0, 210 s | 0, 284 s |
| `make reproducible` | 0, 475 s | - |

After the truncation change, on its commit: `make host-test` PASS; debug
`make test`, release `test-install` and release `test-platforms` exited 0
on both architectures (x86_64 176, 30, 17 s; aarch64 179, 48, 97 s), the
three harnesses that open a `-qemu.log`.

## Probe

`tools/qemu-stderr-probe.py` makes the race certain rather than waiting
for QEMU to print at the wrong moment: `QEMU_WRAP` runs QEMU under a
splitter that writes a QEMU-style message to stderr halfway through the
first `init: switched to the disk root` line, then runs release `make
test-install`. `--old` reverse-applies this change's `tests/boot` files
in a throwaway worktree.

    PROBE: PASS arch=x86_64 mode=fixed: test-install passes; QEMU's message is in a -qemu.log
    PROBE: PASS arch=x86_64 mode=old: the split line missed: test-install: FAIL: the installed boot: no line matching '^init: switched to the disk root (\\S+) \\(root=boot-disk\\)$'
    PROBE: PASS arch=aarch64 mode=fixed: test-install passes; QEMU's message is in a -qemu.log
    PROBE: PASS arch=aarch64 mode=old: the split line missed: test-install: FAIL: the installed boot: no line matching '^init: switched to the disk root (\\S+) \\(root=boot-disk\\)$'

The splitter's first version held back everything after a partial match
of the key, the shell prompt among it, so both modes hung at `install:
no prompt after the first prompt`; it now holds back only a tail that
could begin the key.
