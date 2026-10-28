# test-net-login: the firmware's century write moves the RTC, 2026-10-28

Main's CI run 40739284519, on the merge of #480 (`4c711c66`, a
documentation-only backlog line), failed `net-login test (x86_64)` in
stage j (job 122996579922):

```
test-net-login: FAIL: after reboot the guest reads 1793382913, the RTC should read 1793382913..1793382917
```

The bounds are printed rounded: the lower one was between 1793382913 and
1793382913.5, so the guest read under it. Every other job passed,
`net-login test (aarch64)` among them. Main red, so fixed now rather than
recorded.

## Scope and baseline

Branch `rtc-century` from main at `4c711c66`. One harness check and a
probe; no kernel, firmware or library change. `tests/boot/net_login_test.py`
runs only in `make test-net-login`, not in a matrix step: a test-only PR
(`AGENTS.md`).

## What the log shows

Stage j sets the clock two days ahead (`date -s @T && hwclock -w`),
reboots inside the same QEMU, and requires `date +%s` to read T plus the
host time since, from 2 s below to 2 s above. The run's
`boot-reboot-times.log` (seconds since the boot's start):

| time | line |
|---|---|
| 8.28 | `date -s @1793382908 >/dev/null && hwclock -w` |
| 8.49 | `hwclock -u`: `17:55:08` |
| 8.61 | `reboot: restart through the ACPI reset register` |
| 12.72 | `timer: wall clock 1793382911 s since 1970 from the RTC` |
| 15.49-15.60 | `date +%s`: `1793382913` |

4.4 s after `hwclock -w` wrote 908, the kernel read 911. Whole-second
reads cannot lose that much: the RTC itself came back short.

## The mechanism

At every boot edk2's `PcRtcAcpiTableChangeCallback`
(`PcAtChipsetPkg/PcatRealTimeClockRuntimeDxe/PcRtc.c`, present in CI's
Debian trixie `ovmf`, edk2-stable202502) writes the century register
(CMOS 0x32, the FADT's CENTURY) with a plain `RtcWrite`, the clock running.
QEMU's MC146818 (`hw/rtc/mc146818rtc.c`, `cmos_ioport_write`) takes a time
register written while the clock runs as a new time: `rtc_set_time` sets
the whole seconds from the registers and keeps `offset`, the sub-second
phase recorded at the last set. The RTC therefore drops back by the time
since that set, modulo a second.

In stage j the last set before the reboot is `hwclock -w`
(`arch_rtc_write_epoch`, under SET), and the firmware's century write
comes 2-3 s later, so the RTC loses a fraction of a second the reboot's
length decides. That loss, the boot's whole-second RTC read
(`timer_init`, phase zero) and `date +%s` together exceed the 2 s the
check allowed below when all three are large.

Seen directly:

- A QMP event log of a local `test-net-login` (QEMU 11.1.2, `QEMU_EXTRA`
  `-qmp`): every boot has `RTC_CHANGE` events from the firmware, and after
  the reboot the offset stays at the two days `hwclock -w` set.
- A port trace of one boot (`-d trace:memory_region_ops_*`): the
  firmware's century write `w 0x32 0x20` with register B at `0x02` (SET
  clear), 0.25 s after its own write-back of the time.
- `tools/rtc-century-probe.py` drives QEMU's RTC over qtest, with no
  firmware or guest: set the time, wait, write the century register, time
  the next rollover. The lag is the time since the set modulo a second,
  within 5 ms (below).

The upper bound needs no change: the century write can also leave the
clock ahead (QEMU kept the old clock's phase at `hwclock -w` too), by
less than the 2 s allowed above. AArch64's PL031 is a seconds counter
with no century register.

## The fix

The check stays a keep (`docs/testing/flakes.md`, "The rule for joining
the list"): its bound is the RTC's resolution across a reboot, not a time
budget, and the comment at the site now counts the losses below the
reading: the boot's RTC read, `date +%s`, and on x86-64 the firmware's
century write. The x86-64 allowance below is 3 s; AArch64 keeps 2 s. The
regression the stage exists for, an RTC write that does not persist,
reads two days short and fails either way (the S5 mutation,
`docs/audit/2026-10-16-power-clock-report.md`).

## Proof

`tools/rtc-century-probe.py` (x86-64; the mechanism does not exist on
AArch64):

| mode | rewinds after 1.3 / 1.6 / 1.9 / 2.4 s | worst reading | bound | result |
|---|---|---|---|---|
| fixed | 0.303 / 0.605 / 0.904 / 0.401 s, predicted 0.303 / 0.605 / 0.903 / 0.400 | -2.901 s | -3 s | admits; PASS |
| --old | 0.300 / 0.605 / 0.905 / 0.405 s, predicted the same | -2.903 s | -2 s | rejects; PASS |

The worst reading is the largest measured rewind plus just under a second
for each whole-second reading. `--old` reverse-applies the fix commit's
change to `tests/boot/net_login_test.py` in a throwaway worktree and
evaluates the stage's own bound lines. A boot cannot show this
deterministically: the two truncations are phases nothing in the test
controls. The probe proves the mechanism and the arithmetic instead.

## Validation

A test-only PR: exempt from the matrix (`AGENTS.md`). Release
`make test-net-login` on the branch's head (`98622c20`): x86-64 PASS in
121.5 s, AArch64 PASS in 99.6 s. Branch CI runs both.

## Follow-ups

- QEMU's `cmos_ioport_write` could keep the running clock's phase when a
  time register is written outside SET. Not reported upstream from here.
- Residual: edk2's `PcRtcInit` writes back the time it read when register
  B differs from its initial value; a second boundary between that read
  and the write loses a whole second. Locally the gap was about 1 ms.
