# Boot harness: one retry of a firmware-handover stall -- 2026-10-11

A blocker for roadmap M4, not one of its PRs (the owner's Step 4). OVMF
sometimes never hands over: QEMU runs, the firmware clears the screen, and
nothing more -- no `BdsDxe:` line, no loader banner, no kernel output.
Five sightings by 2026-10-11 (`docs/testing/flakes.md`, "firmware
handover"), two of them in the install-style boots (`test-install`,
`test-net-login`) that M4's acceptance test is built on, where nothing
retried it and the stage timed out.

## Baseline

Main 8f4f9775. `run_boot_test.py` had its own 30 s handover retry (the
first sighting was retried by it); `install_test.Boot` -- used by
`test-install`, `test-busybox` and, with M4, `test-net-login` -- had none.

## The change

`tests/boot/firmware_stall.py` holds the rule and both harnesses use it:

- a boot whose log shows the loader banner, or whose QEMU exits, within
  the bound has handed over and is never retried -- any kernel output comes
  after the banner, so a kernel that hangs or dies still fails;
- one that shows neither is killed, reported ("firmware stall, retried",
  carried into the PASS/FAIL line) and started once more; the install boots
  keep the first attempt's log as `boot-<name>-stall.log`;
- a second stall fails the boot.

The bound is 30 s, or the boot's own timeout if shorter; the last look is
at the bound itself. A retried boot is timed from its own launch. Each
attempt's output goes to its own buffer and log, and the killed attempt's
reader is joined before its log closes.

## Proof

`tests/boot/test_firmware_stall.py` (in `make host-test`), with a fake
clock, processes and logs: banner in time, QEMU exit in time, kernel output
ending the watch, one stall then a good boot, two stalls, a banner after
the bound, a banner during the last sleep before the bound (fails with the
first version's loop), and a caller's shorter bound. The stall itself
cannot be produced on demand -- its cause inside OVMF is unassigned -- so
there is no `--old` probe; the owner asked for this harness unit test.

## Validation

`make host-test` on both architectures; x86-64 debug `make test`;
x86-64 `BUILD=release test-install` and `test-busybox`, which run every
`install_test.Boot` path; CI on both architectures.
