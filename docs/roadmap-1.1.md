# CosmoOS 1.1 roadmap

Written 2026-10-15, after v1.0.3. The [1.0 roadmap](roadmap-1.0.md) is
complete (M1-M6, v1.0.0 on 2026-10-12). This document records what was
merged after 1.0, the gaps recorded since, and the units proposed next.
**No unit below is chosen yet**: the project owner picks the next unit
from the proposed list, and [`AGENTS.md`](../AGENTS.md#scope-rules)'s
scope rules then apply to it as they did to a 1.0 milestone.
[`plan.md`](plan.md) and the
[deferred-work inventory](audit/2026-09-deferred-work-inventory.md)
remain the record of everything else.

## Merged after 1.0

The post-1.0 queue the owner set on 2026-10-13/14, in the order it
merged. Each entry's detail is in
[`history/subsystem-units.md`](history/subsystem-units.md) and its report.

| Unit | PRs | Release | Acceptance in CI | Report |
|---|---|---|---|---|
| AArch64 platform discovery: the console, GIC, timer and PSCI from ACPI; Parallels Desktop boots to the live shell | #376 | v1.0.1 | `make test-platforms` (six QEMU variants, job `platforms`) | [acpi-discovery](audit/2026-10-13-acpi-discovery-report.md) |
| Parallels devices: firmware seed and time (boot protocol v10), timing-jitter entropy source, one-vector NVMe, legacy virtio transport, mutex handoff (S33) | #378, #379 | v1.0.2 (draft), v1.0.3 | `test-entropy` one boot per source; `mutex-handoff` | [parallels-devices](audit/2026-10-13-parallels-devices-report.md) |
| A readable screen console (8x16 face), RSA login keys, wall-clock file times | #380 | v1.0.2 (draft), v1.0.3 | `fb-console`; `test-net-login` RSA login; `lxtest` file times | [console-ssh-times](audit/2026-10-13-console-ssh-times-report.md) |
| Everyday commands: `vi`, `less`, `top`, `free`, `df`, `du`, `uptime`, `whoami`, `clear`, `md5sum`, `dd`, `cmp`, `hexdump`; procfs system files | #381 | v1.0.3 | `test-busybox` | [everyday-commands](audit/2026-10-14-everyday-commands-report.md) |
| `ping`: raw ICMP sockets over IPv4 (N31), `ITIMER_REAL` | #382 | v1.0.3 | `net-raw-icmp`; the networking guide pings the gateway | [ping](audit/2026-10-14-ping-report.md) |
| `scp` and `sftp`: dropbear's `scp`, OpenSSH's `sftp-server`; two scheduler fixes (S34, `poll-timer-outlives`) | #383 | v1.0.3 | `test-net-login` copies 256 KiB each way and runs an `sftp` batch | [scp-sftp](audit/2026-10-14-scp-sftp-report.md) |
| HTTPS for `wget`: mbedTLS 3.6.7 `ssl_client`, certificates checked against Mozilla's CA list | #384 | v1.0.3 | `test-net-login` serves good, untrusted, expired and misnamed certificates | [https](audit/2026-10-14-https-report.md) |
| virtio-gpu: a 2D driver; the screen console at the display's size, following a resize | #387 | after v1.0.3 | `make test-display` (a step of job `net-login`) | [virtio-gpu](audit/2026-10-14-virtio-gpu-report.md) |

Also merged: two test-budget records (#377, the AArch64 `blk` IRQ
budget; #385, the AArch64 entropy boots near 240 s) and #386 (the guide
names v1.0.3). v1.0.2 (tag at `7a87291b`, through #380) was published and
then returned to a draft; [v1.0.3](https://github.com/vkuttyp/CosmoOS/releases/tag/v1.0.3)
(`6011a5ec`, through #386, published 2026-10-15) replaces it and is the
first published release with the Parallels install, network
and ssh.

## Recorded since 1.0, not done

Open lines added to the
[inventory](audit/2026-09-deferred-work-inventory.md) since v1.0.0, by
area. None is scheduled; a proposed unit below names the ones it closes.

**Parallels Desktop and AArch64 platforms**

- SATA disks are not seen: Parallels' AHCI is a platform device described
  only in the DSDT, which needs an AML interpreter.
- No power-off off QEMU: `arch_emulator_exit` is semihosting only and
  nothing calls PSCI `SYSTEM_OFF`, so `exit` halts and the VM stays
  running.
- The debug self-test suite assumes QEMU's machine (`pci`,
  `net-firewall`, `net-tap-release-order`, `blk-submit-unregister` fail on
  Parallels).
- A firmware serial console at or above the loader's 4 GiB direct map is
  not used (no machine seen with one).
- PCI enumeration turns memory decoding off while it sizes BARs; a console
  write in that window stalls on Parallels' framebuffer (nothing logs
  there today).

**Commands and the Linux personality**

- `wget` refusing a bad certificate on Parallels ends with "Resource
  temporarily unavailable" after mbedTLS's message (cause not
  investigated).
- `sftp`'s `ls -l` shows `?` for the link count (cause not investigated).
- `/proc/<pid>/stat` reports VSZ and RSS as 0, so `top` and `ps` show no
  process memory (reading another process's space needs a reference on
  it, P34).
- A read of `/dev/zero` returns at most 64 KiB (`dd bs=1M` copies partial
  records).
- `prctl` is `PR_SET_DUMPABLE`/`PR_GET_DUMPABLE` only;
  `ITIMER_VIRTUAL` and `ITIMER_PROF` are `-EINVAL`.
- No `ping6` and no raw IPv6 socket; a raw ICMP socket has no `bind`
  (`ping -I`), `connect`, `IP_HDRINCL` or `ICMP_FILTER`.
- `wget`'s CA bundle is fixed at build time and nothing checks
  revocation.

**Storage, network internals and tooling**

- Every mount of a freshly installed cosmofs warns that the superblock's
  free block count differs from the bitmap's by 4-7 blocks.
- `mbufq_enqueue` returns `false` both when it freed the packet and when
  it did not touch it; callers assume the first.
- `vm_map_phys` maps large blocks that `vm_unmap_phys` cannot tear down
  across a 2 MiB block; its 14 callers are not audited for sizes.
- `ports/busybox/README.md`'s config regeneration fails at `make
  defconfig` on the macOS build host.
- The AArch64 boots' 240 s whole-boot timeout: three timeouts recorded
  in `docs/testing/flakes.md`; PR #389 measures six in the last 40 CI
  runs, all the entropy job's first boot, records them there, and widens
  the timeout to 300 s.

The 1.0 out-of-scope list ([roadmap-1.0](roadmap-1.0.md#out-of-scope-for-10))
stands, less "GPU and display", which the virtio-gpu unit began (a text
console only: no display interface for programs, no 3D or cursor).

## Proposed next units

For the owner to choose from; none is started. Each names its acceptance
test, which is the unit's proof under the scope rules (green on main, box
ticked in the same PR).

- [ ] **P1 Power-off on AArch64.** A `poweroff` command (BusyBox's
  `CONFIG_POWEROFF` is off today and nothing else provides one) and
  init's shutdown call PSCI `SYSTEM_OFF` when semihosting is absent (the
  conduit is already read from the FADT). Closes the "no power-off" line.
  (x86-64 has only QEMU's `isa-debug-exit` too; ACPI S5 needs the DSDT's
  `\_S5` values, so it waits for AML.) **Acceptance:** a `make
  test-platforms` variant with semihosting disabled boots twice, once
  ending with `poweroff` and once with `exit` from the login shell, and
  QEMU exits by itself within the boot budget each time; on Parallels
  Desktop the VM stops (manual, recorded in the report).
- [ ] **P2 Process memory in procfs.** `/proc/<pid>/stat` reports VSZ
  and RSS, and `/proc/<pid>/status` gains `VmSize` and `VmRSS`, from a
  reference on the target's space taken under the rule exit already
  follows (P34). `/bin/ps` is the native `userland/system/ps.c`, which
  takes no options and prints no memory: the unit gives it VSZ and RSS
  columns (or enables BusyBox's `ps`, `CONFIG_PS`, now off).
  **Acceptance:** `test-busybox` runs `ps` and `top -b -n 1` and requires
  a non-zero VSZ and RSS for a running `sleep`, and reads that process's
  `status` for non-zero `VmSize` and `VmRSS`; a self-test reads a
  process's `stat` and `status` while it exits, many times, with no fault
  or leak.
- [ ] **P3 The Parallels-test residue.** The four small defects the
  post-1.0 tests found: `wget`'s bad-certificate exit message, `sftp`'s
  link count, `/dev/zero`'s 64 KiB short read, and the installer's stale
  superblock free count. Each a defect fix with its own failing test and
  probe. **Acceptance:** `test-net-login` requires the refusal to end on
  mbedTLS's reason alone and `sftp ls -l` to show a numeric link count;
  `test-busybox` checks `dd if=/dev/zero bs=1M count=4` copies 4 full
  records; `test-install` requires the installed disk's first mount to
  log no free-count warning.
- [ ] **P4 Shared libraries.** musl's dynamic linker as `PT_INTERP`, with
  `libc.so` in the image; until now only a stand-in interpreter has run.
  **Acceptance:** a dynamically linked musl BusyBox (and a small
  dynamically linked test program using `dlopen`) passes the M3 scripted
  test on both architectures.
- [ ] **P5 Setting the clock.** `settimeofday`/`clock_settime` under a
  new operation privilege (`PRIV_TIME`; uid 0 alone grants nothing,
  `docs/kernel/security/design.md`), and BusyBox `ntpd`.
  **Acceptance:** `test-net-login` serves NTP from the host harness with
  a time a day ahead; `ntpd -q` sets `CLOCK_REALTIME` to within 1 s of
  it; `date -s` is refused both for a non-root user and for root with the
  bit dropped (`priv -d time -- date -s ...`), and applied with it.
- [ ] **P6 A network package repository.** `pkg` fetches an index and
  packages over HTTPS from a configured URL, checking the index's and each
  package's signature against the key ring, over the same certificate
  checks as `wget`. **Acceptance:** a test serves a signed repository
  from the host harness; `pkg update && pkg install` installs from it; a
  tampered package and an index signed by another key are refused; and
  the same repository behind an untrusted, an expired and a wrongly named
  certificate is refused before any index is read (a signature does not
  stop the replay of an older signed index, `docs/pkg/invariants.md`).

Larger items, not proposed as one unit until scoped: SATA on Parallels
(an AML interpreter, or a DSDT-described AHCI path with no generic AML),
IPv6 beyond the link (routing, `ping6`), and a display interface for
programs on virtio-gpu.
