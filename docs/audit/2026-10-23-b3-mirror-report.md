# Roadmap 1.2 B3: mirror replacement and resilvering

Work dates 2026-10-22 and 2026-10-23. Baseline: main at `d45235c3`
(v1.0.10 published, B2 done). The housekeeping PR first: TEST-19, #438,
merged as `3c4c073b` (its own report:
`docs/audit/2026-10-22-test19-guide-report.md`).

B3's acceptance (docs/roadmap-1.2.md) is `make test-mirror`:

1. Install onto a two-disk mirror.
2. While a writer runs, remove one disk (QEMU `device_del`). The system
   keeps running degraded and says so.
3. Add a new disk (`device_add`). `cosmo-pool replace` resilvers it,
   and a scrub is clean.
4. The system reboots from the new pair.
5. The same again, with a spare taking over by itself.

It absorbs CFS-9, CFS-5, BLK-5 and DRV-6's PCI hot-removal.

Four pull requests, stacked:

| PR | What | Merged |
|---|---|---|
| #439 | copy states, degraded operation, the v13 copy record, self-tests | `38513322` |
| #440 | the resilver, replace, attach, detach, spares, `cosmo-pool` | |
| #441 | PCIe native hotplug, a late disk's partition scan | |
| #442 | `cosmo-install --mirror`, `make test-mirror`, the guide; ticks B3 | |

## The one stop

The brief says to stop if the label has no room for the copy state. It
has none: member 0 has no label, and its copies hold the superblock
slots at blocks 0 and 1. The owner chose a v13 `CFS_KIND_COPYSTATE`
block per degraded mirrored member, named by `cfs_member.state_root`
(the old `reserved` word).

No other stop condition was reached:

- Hotplug interrupts arrive on both architectures (below).
- `attach` adds a copy and needs no other on-disk change.
- The unit took four PRs.

## The design as built

The details are in cosmofs `design.md`: "Format version 13",
"Resilvering", "Replacing a copy", "Spares" and "CFS-5". The PCI part is
in `docs/drivers/pci/design.md`, "Hotplug". The invariants are CF25 to
CF29.

- **States.** Each copy of a mirrored member is `ONLINE`, `MISSING`,
  `OFFLINE`, `FAULTED` or `RESILVERING`.
  - A failed I/O on a removed device (`-ENODEV`) makes the copy
    `MISSING`.
  - Read and write errors past `fs.fault_errors` (16) make it
    `FAULTED`. The last `ONLINE` copy is never faulted.
  - A write that one copy failed and another took leaves a **hole** in
    the copy that failed. Reads skip its holes, and the member counts as
    degraded until the holes are filled.
  - With no good copy left, I/O returns -EIO; there is no panic.
  - Each transition prints one console line. `/proc/mounts` shows
    `rw,degraded` (`MOUNT_STATE_DEGRADED`).
- **What a copy missed.**
  - The pool keeps the regions written since the last durable root. A
    copy leaving `ONLINE` ORs those regions into its bitmap and records
    `since`; from then on, every write it does not take adds its region.
  - Each commit writes the record (7552 regions) into the member's
    `COPYSTATE` block. The pool's format is raised to 13 only when there
    is something to record.
  - A copy missing when the pool is mounted is marked for a whole
    rebuild (CFS-29).
  - A device behind the root is never read (CF27): the mount moves to
    the newest member-0 root.
- **The resilver.** One `cfs-resilver` thread per mount.
  - It walks the record's regions under `fs->lock` and copies their
    allocated blocks, paced by `cfs_pace` at `fs.resilver_rate` (8192
    blocks/s).
  - The copy being rebuilt is written by every write but is never read
    (CF28). It carries a `CFS_LABEL_RESILVER` label with the record's
    tag in place of superblocks.
  - The cursor is recorded every 2048 blocks; a mount finds the target
    by its tag and resumes from the cursor (CF29).
  - The superblocks, or the label, are written last. Only then is the
    copy `ONLINE`.
  - When a second good copy holds a block, the two are compared. If any
    block differs, the copy being replaced is kept for a scrub to
    decide.
- **Commands.**
  - `/dev/fsctl` `POOL_*` ops 13 to 20.
  - `cosmo-pool` status, offline, online, replace, attach, detach,
    add-spare and remove-spare. Each commits before it answers.
  - A spare takes over a `FAULTED` or `MISSING` copy by itself.
- **CFS-5.** A mount holds its pool's other devices, resilver targets
  and spares (`vfs_mount_set_held`). `vfs_bdev_mounted` and a new
  mount's claim see them.
- **Hotplug.**
  - The ports' slot capability: an attention button unbinds what is
    behind the port and powers it off. A presence or link change powers
    it on, waits for the link, assigns BARs in the port's windows and
    enumerates.
  - Interrupts are MSI-X (QEMU's `pcie-root-port` has only MSI-X), set
    up after the IOMMU by `pci_hotplug_start`. A port with neither MSI-X
    nor MSI is polled every 500 ms.
  - The `pciehp` thread reads a hot-added disk's partitions (BLK-5).
- **Boot.**
  - A degraded root boots and init says so.
  - A lagging device is resilvered after the switch to the disk root.
  - A mount refuses a member with no complete copy (`member N of the
    pool has no current device`, -ENODEV). `cosmofs-mirror-stale` gained
    that case in #442.
  - With no good copy left at run time, I/O is -EIO
    (`cosmofs-degraded`).

## Hotplug findings per architecture

Both machines deliver the port's events by MSI-X, so the polled fallback
was not needed on either. Three defects stood between the first draft
and that, all CosmoOS's own:

- **`order`:** the ports' interrupts were set up in `pci_init`, before
  the IOMMU. An MSI is a DMA write, and behind the SMMU a port with no
  domain faulted, so its first event never arrived (AArch64).
- **`vtd`:** VT-d took a DMAR sub-hierarchy scope to cover the bridge
  alone. A device behind a root port got no domain, and a hot-added NVMe
  faulted on its first DMA (x86-64).
- **`smmu`:** the SMMUv3 stream table had 256 entries (bus 0); stream
  ids behind a root port start at 0x100 (AArch64).

`tools/hotplug-iommu-probe.py --case NAME --old` fails
`test-pci-hotplug` at that case's step:

- `vtd`: `NVMe add: no line matching ... blk: (nvme\S+)`.
- `smmu`: `add: no line matching ... part: (\S+) registered after boot`.
- `order`: `QMP: no DEVICE_DELETED for hot0dev within 15s`.

q35 runs with `-global
ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off`, so that the port's
native hotplug is used rather than ACPI's.

| | `DEVICE_DELETED` after `device_del` | add, a disk with partitions | `test-pci-hotplug` |
|---|---|---|---|
| x86-64 (q35) | 0.02 to 0.07 s | 0.21 to 0.32 s | 9.5 to 10.4 s |
| AArch64 (virt) | 0.05 s | 0.32 s | 12.6 to 12.7 s |

The test was named `test-hotplug` until the roadmap's C3 (CPU hotplug)
was seen to name its own acceptance `make test-hotplug`. It is now
`test-pci-hotplug`, and its CI job `pci-hotplug`.

## The acceptance test's defects (#442)

The first runs of `test-mirror` found two defects in #439's held
devices, which are on main.

| Case | Defect | `test-mirror` with `--old` |
|---|---|---|
| `check` | The checker's "another device of this pool is mounted" asked `vfs_bdev_mounted`, which sees the checker's own detached mount's hold. A mirrored root's boot check refused itself, and init mounted the root unchecked. | fails at `replace: the boot's check of the root: no line matching '^cosmofs: root checked, 0 findings, 0 repaired$'` (x86-64 and AArch64) |
| `gone` | `bdev_related` matched a partition to its disk by name. A mount holding the pulled `vda2` held the new disk `vda`, and the replace was refused `-EBUSY`. It now compares the partition's disk object (`blk_part_disk`). | fails at `cosmo-pool replace: no line matching '^replace-status=0$'` (x86-64 and AArch64) |

`tools/b3-mirror-probe.py --case NAME --arch ARCH --old` restores the
old line in a throwaway worktree and runs `make BUILD=release
test-mirror`. The `check` defect did not fail the first draft of the
test: it only made init's line say the check could not run. Every
installed boot is now required to say `cosmofs: root checked, 0
findings, 0 repaired`.

The runs also found two faults in this unit's own code, which is not on
main yet, so neither has a probe:

- `cosmo-install --mirror` called `blkdevs_format_mirror` without
  `/dev/blkctl` open, and got `EBADF`.
- `cosmo-pool status` freed its buffer, then read the pool's flags from
  it. Once #440's review made that buffer 80 KiB, the free unmapped it,
  and every status call ended with signal 11 after printing. The test
  missed this, because it checked only what was printed. Every status
  call must now exit 0 or 1, and no process may fault.

### Review of #442 (2026-10-23)

Codex made three P2 findings, all fixed in `ea830da7`:

- **The first `gone` fix:** it returned "unrelated" for any removed
  device. While a disk is unregistered, its partitions go first, so for
  a moment the disk itself was not seen as mounted, and a `/dev/blkctl`
  write could pass the guard. Partitions and disks are now compared by
  object. The probe was re-anchored to the new line and still fails at
  the replace on both architectures.
- **`cosmo-install --mirror`'s size check:** it compared bytes and left
  out each partition's last sector. It now compares the 4096-byte blocks
  the pool uses.
- **The reboot check of the writer's file:** `cmp ... || ls` passed any
  file that existed. The file must now equal the same-length prefix of
  `/bin/busybox`.

After the fixes, on both architectures: `test-mirror` passes (63.0 s
and 87.9 s), `analyze` is clean, and the debug boots pass.

## test-mirror

Release builds, 2026-10-23, on the tree #442 opens with. Each stage's
time is in brackets, and none is asserted.

| Stage | x86-64 | AArch64 |
|---|---|---|
| `install` | `cosmo-install vdc --mirror vdd` 4.2 s [10.7 s] | `vdd --mirror vde` 4.7 s [14.1 s] |
| `replace` | `MISSING` 0.2 s after `device_del`; resilvered in 1.2 s; scrub clean in 1.6 s [14.8 s] | 0.0 s; 1.5 s; 1.5 s [17.7 s] |
| `reboot` | the root from disks a and c, whole [6.6 s] | [9.4 s] |
| `spare` | the spare took over 0.1 s after `device_del`, `ONLINE` at 0.9 s; scrub 1.4 s [12.1 s] | 0.2 s, 1.5 s; 1.6 s [16.2 s] |
| `reboot-spare` | [6.0 s] | [9.3 s] |
| `degraded` | disk a alone; init's line; a copy `MISSING` [5.8 s] | [9.0 s] |
| `lagging` | disk d resilvered after the switch, `ONLINE` 0.4 s after the prompt; scrub 1.4 s [7.9 s] | 0.4 s; 2.2 s [12.7 s] |
| total | 64.0 s | 88.4 s |

After the analyzer fix (below), it was run again: 64.2 s and 89.1 s.

A resilver copies about 3600 to 3800 blocks, the pool's allocated blocks
on a 48891-block member, in 550 to 720 ms. That is well under the 8192
blocks/s bound.

## Other observations

- **LOCK-9:** the release boot's `mutex PI: donation chain cycle`
  warning came 53 times in one replace-stage boot, while the writer
  copied `/bin/busybox` and synced, and before any disk was removed.
  The same warning is in main's own `test-be`, `test-install`,
  `test-upgrade` and `test-pkgtx` logs from 2026-10-17 on, so it
  predates B3. A sighting was added to LOCK-9.
- **CFS-30:** a scrub of the running root reports 53 to 77 `skipped`
  visits to dirty buffers: inode-map, inode and checksum blocks. The
  count is the same after a `sync` and a 3 s wait. The cause is not
  established, so the test does not assert zero.

## Validation

Each PR ran the AGENTS.md matrix on x86-64 and AArch64 before it
opened: `make host-test`, `fuzz` and `analyze`; debug `make test` at
`QEMU_SMP` 1, 2 and 4; `test-smp2`, `test-chaos` and
`test-harness-retry`; and the release build and boot. Each step waited
for the host's one-minute load to fall below 8, because Docker's VM had
been taking 630% of the 8-CPU host (flakes.md, 2026-10-23).

- **#439:** 20 of 20 steps passed. A first attempt was restarted while
  the host was loaded.
- **#440:** 20 of 20 steps passed on `f791de0f`, after Codex's review
  fixes. An earlier run on `d378d10f` had one timeout, the x86-64
  `QEMU_SMP=1` boot while Docker ran (`cosmofs-replay` 72.6 s against
  its usual 18.6 s). That step passed on a quiet host.
  - `3888f9da`, the `cosmo-pool status` fix, came after this matrix:
    two lines in a userland command. `test-mirror` runs it on both
    architectures.
- **#441:** 20 of 20 steps passed on `bb9db052`. After the rebase onto
  #440's fixes, which change only cosmofs, `test-pci-hotplug` passed on
  both architectures.
- **#442:** 18 of 20 steps passed on `2dfeeb20`. `analyze` failed on both
  architectures with one new `core.NullDereference` in
  `vfs_bdev_mounted_except`: comparing the list cursor with a NULL
  `self` told the analyzer the cursor could be NULL. With `self` checked
  first, `analyze` is clean on both architectures (7 baselined, 0
  unexpected), and `test-mirror` on both and a debug x86-64 boot pass.

## Backlog

- **Struck:** CFS-9 (#442) and CFS-5 (#440).
- **DRV-6 and BLK-5:** notes on what #441 did and what is left.
- **DRV-8:** stream ids above 4095.
- **New, out of B3's scope:**
  - CFS-27: wider mirrors.
  - CFS-28: rebalancing.
  - BLK-9: NVMe namespace management.
  - DRV-17: hot-removal of devices that are not on PCIe.
  - RAID-Z was already CFS-13.
- **New, found:**
  - CFS-29: a copy missing at mount is rebuilt whole.
  - CFS-30: the live scrub's `skipped` count.
  - PKG-11: `cosmo-upgrade` updates only the boot disk's ESP.
- **LOCK-9:** a sighting.
