# Power and clock: S5 — 2026-10-16

The 1.1 roadmap's S5, the fifth unit of the shell-completeness track,
which absorbs P5 (setting the clock). Base: main `4faa1c8f` (S4 merged).
Goal: the system can be restarted and halted from a shell, its clock set
by hand, from the RTC and over NTP, and the RTC written back.

## Before

| What | Before |
|---|---|
| Restart | none anywhere: `kernel/include/kernel/shutdown.h` said "There is no reboot"; no PSCI `SYSTEM_RESET`, no FADT reset register, no 0xCF9 |
| `reboot(2)` | not in the Linux personality (`-ENOSYS`); BusyBox `reboot`, `halt` not built, `poweroff -f` failed |
| Init's signals | SIGUSR2 alone (power off); SIGTERM or SIGUSR1 -- what BusyBox `reboot` and `halt` send -- killed init, ending the boot as a failure; any root process could send them |
| Wall clock | set once at boot from the RTC (or the firmware), never again; no `settimeofday`, `clock_settime` |
| RTC | read once at boot; never written; no `/dev/rtc` |
| BusyBox `date -s`, `hwclock`, `ntpd` | `date -s` failed (`ENOSYS`); `hwclock`, `ntpd` not built |
| QEMU | always `-no-reboot`: a guest reset ended the run |

## Decisions

**One kernel function, two doors.** `kernel_reboot(cmd)` restarts, halts
or powers off; the Linux personality's `reboot(2)` reaches it after
Linux's checks in Linux's order (privilege, magic numbers, command), and
a native `SYS_reboot` (105) after the privilege alone. Init, a native
program, uses the native door. Power-off is `kernel_shutdown`'s success,
the same as init's exit, so the harness reads the same emulator exit.
Nothing is synced in the kernel, as on Linux: init and BusyBox's
`reboot -f` sync first.

**Restart, per architecture.** AArch64: PSCI `SYSTEM_RESET` on the
FADT's conduit, beside the existing `SYSTEM_OFF`. x86-64: what Linux's
x86 reboot tries, each given about 50 ms -- the FADT's reset register
(`RESET_REG_SUP`, system I/O space), the PCI reset control at 0xCF9, the
keyboard controller's pulse, and a triple fault. Each logs the path it
takes.

**The privilege.** `PRIV_SYS_ADMIN`, in place of Linux's `CAP_SYS_BOOT`
and `CAP_SYS_TIME` (P5 had proposed a `PRIV_TIME`; the roadmap entry
chose `sys_admin`). `/dev/rtc0`'s `RTC_SET_TIME` refuses with `EACCES`,
as Linux's does; the system calls with `EPERM`.

**Signals to init (security S20).** BusyBox's `reboot`, `halt` and
`poweroff` without `-f` signal init (SIGTERM, SIGUSR1, SIGUSR2), as they
signal any init that is not BusyBox's own. Under the ordinary rule a
process of uid 0 may signal init, so root with `sys_admin` dropped could
reboot through `kill 1` while `reboot(2)` refused it. A signal to init
other than the probe now needs `PRIV_SYS_ADMIN` on top of the ordinary
rule: `process_may_signal`, which the native `kill`, the Linux `kill`
(and so process groups) and `tgkill`/`tkill` all ask. Init handles all
three: a sync, then `SYS_reboot` for restart and halt, its exit for power
off.

**Setting the clock.** `clock_set_realtime_ns` stores the new offset
from the monotonic clock in one atomic store, which the reader loads
atomically. The monotonic clock, and every deadline armed on it, stay
put; an absolute `CLOCK_REALTIME` wait already converted fires at the
old wall time (inventory). `adjtimex` stays out: BusyBox `ntpd -q` steps
with `settimeofday` and exits before any `adjtimex`.

**The RTC device.** `/dev/rtc0`, made where boot read an RTC, answers
`RTC_RD_TIME` and `RTC_SET_TIME` through a new optional `ioctl` member of
`chrdev_ops`, which only the Linux personality's `ioctl` calls. The CMOS
gains a spinlock: every access is an index write then a data access, and
`hwclock` now reaches it after boot from any CPU.

**The harness restarts a machine.** `QEMU_REBOOT=1` leaves out
`-no-reboot` (`scripts/qemu-run.sh`); every other boot keeps it, so a
reset by mistake still fails. `test-net-login`'s reboot boot uses it, so
the RTC QEMU holds survives the restart -- a fresh QEMU would read the
host's time again and prove nothing.

## A defect the restart found

The first AArch64 run restarted (QEMU's firmware printed its banner
again) and then hung before loading any boot option, every time: three
`test-net-login` runs and one hand run, with 4 CPUs. With 1 CPU, or with
QEMU's own `system_reset` issued at the same point, the firmware came
back. A variant that did not stop the other CPUs first came back too, so
the parked CPUs were the difference. `ipi_halt` never returns, so the
dispatcher never EOIs the IPI that parked each CPU; Linux EOIs before it
parks one. With `ipi_halt` EOIing the IPI, four restarts in four came
back, and `test-net-login` passes on AArch64. The defect was not
reachable before this unit -- nothing reset the machine with CPUs parked
-- so the acceptance test is its proof; no probe.

## Changes

| Area | Change | Files |
|---|---|---|
| shutdown | `kernel_reboot`; `arch_restart` (PSCI `SYSTEM_RESET`; FADT reset register, 0xCF9, 8042, triple fault) | `kernel/core/shutdown.c`, `kernel/include/kernel/shutdown.h`, `kernel/include/arch/shutdown.h`, `kernel/arch/{aarch64,x86_64}/shutdown.c`, `kernel/arch/aarch64/smp.c`, `kernel/arch/aarch64/include/aarch64/platform.h` |
| IPI | the halt IPI EOIs before it parks the CPU | `kernel/interrupt/ipi.c` |
| clock | `clock_set_realtime_ns`; the civil-date helpers shared; `arch_rtc_write_epoch` (CMOS under a lock, PL031 `RTCLR`) | `kernel/timer/timer.c`, `kernel/include/kernel/timer.h`, `kernel/include/arch/timer.h`, `kernel/arch/{aarch64,x86_64}/timer.c` |
| RTC device | `/dev/rtc0`; `chrdev_ops.ioctl` | `kernel/timer/rtcdev.c`, `kernel/include/kernel/vfs.h`, `kernel/core/main.c`, `kernel/kernel.mk` |
| signals | `process_may_signal` (security S20) at every user-sent signal | `kernel/process/process.c`, `kernel/include/kernel/process.h`, `kernel/syscall/native.c`, `compat/linux/signal.c` |
| native ABI | `SYS_reboot` 105, `COSMO_REBOOT_*`, `cosmo_reboot` | `kernel/include/uapi/cosmo/syscall.h`, `kernel/syscall/native.c`, `libc/include/cosmo/syscall.h` |
| Linux personality | `settimeofday`, `clock_settime`, `reboot`; `ioctl` reaches a character device's hook | `compat/linux/syscalls.c`, `compat/linux/nr_{x86_64,aarch64}.h` |
| init | SIGTERM restarts, SIGUSR1 halts | `userland/init/init.c` |
| BusyBox | `reboot`, `halt`, `hwclock`, `ntpd` (and `FEATURE_WAIT_FOR_INIT`, NTP server, conf, auth: defconfig's choices) | `ports/busybox/{applets,busybox.config,mkconfig.py}` |
| harness | `QEMU_REBOOT`; `test-net-login` stage j and its NTP server | `scripts/qemu-run.sh`, `tests/boot/net_login_test.py` |
| tests | `ash.sh`'s S5 section; `lxtest`'s power and clock section | `tests/busybox/ash.sh`, `tests/linux/lxtest.c` |

## Tests

The acceptance, in CI on both architectures:

- **`test-net-login`** (job `net-login`) stage j, on the reboot boot
  (e), which runs with `QEMU_REBOOT=1`: with `sys_admin` dropped,
  `reboot` (init's signal) and `reboot -f` (`reboot(2)`) fail, `date -s`
  says "can't set date: Operation not permitted" (BusyBox exits 0 there,
  as on Linux) and `hwclock -w` "Permission denied"; `date -s` is refused
  to uid 1000; the clock has not moved. `ntpd -q -n -p 10.0.2.2:PORT`
  against the harness's NTP server, a day ahead, exits 0 and leaves the
  guest's `date +%s` within 1 s of the server's clock. Then `date -s`
  two days ahead, `hwclock -w`, and `reboot`: the kernel logs its restart
  path, the machine boots again in the same QEMU to the installed
  system, and `date +%s` reads the RTC's time (to 2 s). The boot then
  powers off as before.
- **`test-busybox`** (job `busybox`): `kill -TERM` to init refused with
  `sys_admin` dropped (that process keeps `PRIV_PROC` and uid 0, so only
  S20 refuses it) while `kill -0` passes; `reboot`, `reboot -f` and
  `halt` refused the same way; `date -s` refused without `sys_admin` and
  to uid 1000, the clock unmoved, and as root a day ahead and back;
  `hwclock -u` reads `/dev/rtc0`, `hwclock -w` without `sys_admin` is
  "Permission denied".

In every debug boot, `lxtest`'s power and clock section:
`clock_settime(CLOCK_MONOTONIC)`, a nanosecond field of 10^9, a negative
second, a microsecond field of 10^6 and a time zone beyond 15 hours are
`EINVAL`; `settimeofday(NULL, NULL)` succeeds for root; the clock moves
an hour ahead and back and reads so; `reboot` with a bad first or second
magic number or the kexec command is `EINVAL` and `CAD_OFF` succeeds;
`/dev/rtc0`'s `RTC_RD_TIME` gives a plausible date, `RTC_SET_TIME` of
month 13 is `EINVAL`, another request `ENOTTY` and a read `EINVAL`; and
to uid 1000 `reboot`, `clock_settime` and `settimeofday` are `EPERM`.

Checked against their bugs by mutation:

- `process_may_signal` without the init rule: `test-busybox`'s
  `kill -TERM` to init with `sys_admin` dropped restarted the machine
  (`init: restart requested`, then the ACPI reset register), and QEMU,
  under `-no-reboot`, exited mid-script.
- The x86-64 `arch_rtc_write_epoch` returning 0 without writing:
  `test-net-login` stage j failed after the reboot, the guest two days
  short of what `hwclock -w` had written.
- `ipi_halt` without its EOI: the AArch64 restart hung in the firmware,
  four times in four (above).

New features are proven by their acceptance test; no probe (AGENTS.md).

The restart on Parallels Desktop is the owner's to run (the roadmap
entry); it is recorded below when run.

## Validation

- **The validation matrix**, on `afb80f37`, every step PASS; the boot
  times are the harness's own lines:

  | Step | x86-64 | AArch64 |
  |---|---|---|
  | `host-test`, `fuzz`, `analyze` | pass | pass |
  | debug `test`, `QEMU_SMP=1` / `2` / `4` | 147.3 (rerun, below) / 161.1 / 159.1 s | 152.3 / 164.4 / 174.3 s |
  | `test-smp2` | 154.4 s | 161.0 s |
  | `test-chaos` | 149.0 s | 159.2 s |
  | `test-harness-retry` | 159.5 s | 160.0 s |
  | `BUILD=release test` | 18.6 s | 21.2 s |
  | `BUILD=release test-busybox` | 47.6 s (176 checks) | 57.8 s (176 checks) |
  | `BUILD=release test-net-login` | 76.3 s | 91.3 s |
  | `BUILD=release test-install` | 26.4 s | 40.8 s |

  The x86-64 `QEMU_SMP=1` boot first failed with the firmware-handover
  stall on both attempts -- the firmware never handed over, so no kernel
  ran -- and passed on the rerun; recorded as the stall's eighth sighting
  (`docs/testing/flakes.md`). The network harness passed in every debug
  boot (the boot test requires it).
- **Branch CI**: recorded in the PR.
