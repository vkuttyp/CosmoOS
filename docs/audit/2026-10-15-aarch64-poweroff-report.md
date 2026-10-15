# AArch64 power-off: PSCI SYSTEM_OFF and `poweroff` — 2026-10-15

The 1.1 roadmap's P1, chosen by the project owner on 2026-10-15. Base:
main `750c1780` (PRs #388 and #389 merged). Goal: a clean end of the
boot turns an AArch64 machine off where there is no semihosting --
Parallels Desktop above all, where `exit` halted and the VM stayed
running (inventory, found 2026-10-14) -- and a `poweroff` command
exists.

## Before

| What | Before |
|---|---|
| the end of a boot | `kernel_shutdown` called `arch_emulator_exit` (semihosting, QEMU's firmware only) and then halted: off QEMU, `shutdown: halting CPU` and a running VM |
| a QEMU without `-semihosting-config` | the semihosting `hlt #0xF000` is an undefined instruction there, and the kernel panicked at the end of every boot: `KERNEL PANIC: unhandled exception 1027 (undefined instruction)` (a defect, not in the inventory: every harness and the guide pass the option) |
| `poweroff` | none: BusyBox's `CONFIG_POWEROFF` off, no native command |
| PSCI | `PSCI_FN_SYSTEM_OFF` defined in `platform.h`, never called |

## Changes

| Area | Change | Files |
|---|---|---|
| shutdown | after the emulator exit returns, `arch_power_off()`; then the halt only where that returns | `kernel/core/shutdown.c`, `kernel/include/arch/shutdown.h`, `kernel/include/kernel/shutdown.h` |
| AArch64 power-off | `aarch64_psci_system_off`: PSCI `SYSTEM_OFF` through the FADT's conduit, read afresh (a one-CPU boot never ran `psci_probe`); logs `shutdown: power off through PSCI SYSTEM_OFF (HVC|SMC)`, or that there is no PSCI, or what the firmware returned | `kernel/arch/aarch64/smp.c`, `shutdown.c`, `include/aarch64/platform.h` |
| the semihosting HLT | in the exception table; `handle_sync` offers an EL1 `ESR_EC_UNKNOWN` the fixup before it classifies, so the exit returns on a QEMU without semihosting (A27) | `kernel/arch/aarch64/shutdown.c`, `trap.c` |
| x86-64 | `arch_power_off` is empty: ACPI S5 needs the DSDT's `\_S5` SLP_TYP values, so AML | `kernel/arch/x86_64/shutdown.c` |
| `poweroff` | BusyBox's applet (`CONFIG_POWEROFF`): it syncs and sends SIGUSR2 to pid 1 | `ports/busybox/busybox.config`, `applets`, `mkconfig.py` |
| init | a SIGUSR2 handler prints `init: power off requested`, syncs and `_exit(0)`s; done in the handler, so the signal cannot land between a flag's check and the `waitpid` it should end | `userland/init/init.c` |
| harness | `QEMU_SEMIHOSTING=0` leaves `-semihosting-config` out | `scripts/qemu-run.sh` |

The panic path is unchanged: it does not power off, so its message stays
on the screen (Parallels shows it).

## Tests

`make test-platforms` gains two variants, both `QEMU_SEMIHOSTING=0`:

- `psci-exit`: `exit` at the console; QEMU must exit by itself with
  status 0 within 60 s, and the log hold `shutdown: exit status 0` then
  `shutdown: power off through PSCI SYSTEM_OFF (HVC|SMC)`.
- `psci-poweroff`: the same, ended by `poweroff`, with `init: power off
  requested` first.

Without semihosting QEMU exits 0 whatever the boot's result, so the
status alone says nothing: the log lines are the check, and the
semihosting variants keep requiring 33.

**Not vacuous.** With the fixup line removed from `handle_sync` (the tree
committed first, the line deleted, the image rebuilt), `psci-exit`
fails: `QEMU still running 60 s after 'exit'`, the log ending
`shutdown: exit status 0` then `KERNEL PANIC: unhandled exception 1027
(undefined instruction)`. Restored with `git checkout --`, tree clean.

## Validation

- `make ARCH=aarch64 BUILD=release test-platforms` on the branch: all
  eight variants PASS (gicv2, gicv3, lowmem, rndr, nornd, psci-exit,
  psci-poweroff, noserial; 8.2-9.3 s each). QEMU's FADT names SMC (the
  machine has EL2), so the line reads `(SMC)`.
- **Parallels Desktop 27.0.3 (58680), by hand**, in a VM made for this
  (`CosmoOS P1 poweroff`: the branch's AArch64 release image as an NVMe
  plain disk, 2 CPUs, serial port 0 on a socket; the owner's own VM
  untouched). Two fresh boots, each to the `cosmo$ ` prompt (platform
  line `firmware OEM 'PRLS  '; ...; PSCI SMC`):
  - `poweroff`: `init: power off requested`, `shutdown: exit status 0`,
    `shutdown: power off through PSCI SYSTEM_OFF (SMC)`; `prlctl status`
    reported `stopped` 1.1 s after the command.
  - `exit`: `init: shell exited with status 0`, the same two shutdown
    lines; `stopped` 1.1 s after the command.
- Local debug boots on the branch: x86-64 PASS in 149.2 s, AArch64 PASS in
  166.0 s; `make host-test` passes; the x86-64 and AArch64 release builds
  compile.
- **The validation matrix**, on `7d6b53b3` (the review's commit), every
  step PASS; the boot times are the harness's own lines:

  | Step | x86-64 | AArch64 |
  |---|---|---|
  | `host-test`, `fuzz`, `analyze` | pass | pass |
  | debug `test`, `QEMU_SMP=1` / `2` / `4` | 135.3 / 150.9 / 153.2 s | 149.9 / 149.4 / 163.3 s |
  | `test-smp2` | 155.7 s | 158.9 s |
  | `test-chaos` | 154.1 s | 151.4 s |
  | `test-harness-retry` | 155.9 s | 154.1 s |
  | `BUILD=release test` | 18.1 s | 21.5 s |
  | `test-platforms` (8 variants) | -- | pass |

  The network harness passed in every debug boot (the boot test requires
  it).
- **`tools/semihost-undef-probe.py`** (AArch64 only: x86-64's exit is an
  I/O port write, which no machine takes as undefined): `--old`
  reverse-applies the fix commit's `kernel/arch/aarch64/trap.c` in a
  throwaway worktree and `psci-exit` fails with the undefined-instruction
  panic (`PROBE: PASS arch=aarch64 mode=old`); without `--old` it passes
  through PSCI (`mode=fixed`).
- Review (Qodo): test-platforms now also fails on a `KERNEL PANIC` after
  the closing command, which a semihosting failure exit (35) would
  otherwise only report as a status.

## Not done here

- `poweroff -f`, `halt`, `reboot`: no `reboot(2)` in the Linux
  personality and no reboot path in the kernel (inventory).
- x86-64 power-off off QEMU (ACPI S5 needs AML).
- `poweroff` from an ssh session is the same signal to pid 1, but no test
  runs it there.
