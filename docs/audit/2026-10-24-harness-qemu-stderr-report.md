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
  the guest's alone.
- `tests/host/test_boot_harness.py` (in `make host-test`): a fake
  `qemu-run.sh` writes half a guest line, a QEMU message to stderr, then
  the rest. Boot's text must hold the line whole and without the
  message, and the message must be in `boot-fake-qemu.log`. On the old
  harness it fails: `FAIL: a guest line QEMU wrote a message inside of is
  whole in the serial text`; with the change it passes.
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

Only the harnesses change, not the kernel or the image, so the rest of
the device-lifecycle matrix is not rerun; CI runs every boot on the
change.
