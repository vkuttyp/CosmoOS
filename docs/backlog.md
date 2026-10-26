# Backlog

Written 2026-10-20 at v1.0.7, after the [1.1 roadmap](roadmap-1.1.md)
completed. This is every piece of work still open in
[`plan.md`](plan.md) and the
[deferred-work inventory](audit/2026-09-deferred-work-inventory.md), one
line each, grouped by subsystem. Those two files stay as the historical
record; each of their entries now says whether it is done (with the PR),
obsolete (and why), or open, and names the line here that holds it.
Nothing open was dropped: where several entries said the same thing (the
plan repeats most of the inventory, and some inventory sections repeat
each other) they are one line here.

The scope rules in [`AGENTS.md`](../AGENTS.md#scope-rules) still decide
what is worked: a line here is taken up only as part of a roadmap unit the
owner has chosen, or when it blocks that unit's acceptance test or turns
main red. A line is struck through with its PR when it is done. New
follow-ups are still recorded where `AGENTS.md` says (the inventory) until
the owner moves that rule here; each is then given a line here too, under
its subsystem with the next free number.

**Severity.** *correctness*: a reachable wrong result, leak, crash or
race, or a test that can fail for a reason other than the defect it
tests. *security*: a gap in trust, privilege or isolation. *usability*:
a missing feature or behaviour a user or a program notices.
*nice-to-have*: internal quality, measurement, an optional feature, or a
deferral by decision or measurement.

**Size.** S: about a day, one PR. M: a unit, two to five PRs. L: larger
than one unit.

Open lines (struck lines are not counted):

| | correctness | security | usability | nice-to-have | total |
|---|---|---|---|---|---|
| Block devices | 3 | 0 | 2 | 4 | 9 |
| Boot chain and verified boot | 0 | 6 | 2 | 1 | 9 |
| Build and tooling | 0 | 2 | 0 | 4 | 6 |
| cosmofs and storage | 4 | 1 | 7 | 13 | 25 |
| Documentation | 0 | 0 | 1 | 1 | 2 |
| Drivers and platforms | 3 | 0 | 2 | 12 | 17 |
| Kernel modules | 1 | 0 | 0 | 1 | 2 |
| Linux personality | 7 | 0 | 11 | 6 | 24 |
| Lock validator and concurrency | 3 | 0 | 0 | 7 | 10 |
| Memory | 1 | 0 | 1 | 7 | 9 |
| Network | 4 | 3 | 8 | 7 | 22 |
| Observability | 1 | 0 | 0 | 6 | 7 |
| Packages and upgrade | 2 | 0 | 2 | 2 | 6 |
| Quiescence | 0 | 0 | 0 | 3 | 3 |
| Scheduler and SMP | 0 | 0 | 1 | 5 | 6 |
| Security and isolation | 0 | 10 | 1 | 2 | 13 |
| Testing and CI | 9 | 0 | 0 | 9 | 18 |
| Commands | 1 | 0 | 5 | 1 | 7 |
| VFS | 3 | 0 | 5 | 3 | 11 |
| Virtualisation | 3 | 0 | 0 | 6 | 9 |
| **all** | **45** | **22** | **48** | **100** | **215** |

## Block devices

- **BLK-1** correctness, S. virtio-blk timeout against a submit parked between its dead check and the queue lock: prove with a test, fix as NVMe's was (#338).
- **BLK-2** correctness, S. USB storage: the timeout path clears `recovering`/`broken` after a failed BOT reset recovery; prove the state under a failed recovery and keep it.
- **BLK-3** correctness, M. No bounce buffers: a 32- or 44-bit DMA device refuses page-cache pages above its mask with `-EINVAL` once RAM is above 4 GiB.
- **BLK-4** usability, S. GPT: a disk whose primary or backup table alone is valid gets no partitions; recover from the good copy.
- **BLK-5** usability, S. No partition scan when a disk registers after boot (USB, hot-plug); only `/dev/blkctl` RESCAN. Roadmap 1.2 B3's third PR scans a disk that arrives with a PCIe hot-plug slot's device; a USB disk that arrives late is still not scanned until asked.
- **BLK-6** nice-to-have, M. AHCI NCQ, deferred until a real disk shows it pays (four streams reach 87 % of NVMe without it).
- **BLK-7** nice-to-have, S. USB storage scatter-gather beyond 16 segments, deferred (within noise of NVMe).
- **BLK-8** nice-to-have, L. The rest of the NVMe feature set, only for a demonstrated device or workload.
- **BLK-9** nice-to-have, M. No NVMe namespace management (create, attach, delete namespaces); a controller's namespaces are what it reports at probe (out of roadmap 1.2 B3's scope).

## Boot chain and verified boot

- **BOOT-1** security, M. KASLR: a relocatable kernel, a loader-chosen slide from `EFI_RNG_PROTOCOL` or the CPU, the slide in the boot protocol, symbolisation and `ksymtab` adjusted.
- **BOOT-2** security, M. The loader is not signed for UEFI Secure Boot (no db enrolment): replacing `BOOTX64.EFI` defeats verified boot.
- **BOOT-3** security, M. No measured boot: the manifest and boot files are not extended into TPM PCRs.
- **BOOT-4** security, L. The installed root filesystem is not verified (no dm-verity-like tree); verification ends at the boot archive. Roadmap 1.2 B5's encrypted root authenticates each metadata block, not the tree: a disk replaced wholesale by another pool still boots as that pool.
- **BOOT-5** security, M. No anti-rollback: any boot manifest or module ever signed by a trusted key verifies forever; needs signature versions and a recovery-safe policy.
- **BOOT-6** usability, S. init does not wait for a `root=` disk that registers late (USB storage); it stays on the live root.
- **BOOT-7** usability, S. The loader allocates below 4 GiB only (`LOADER_ALLOC_LIMIT`); a machine with all RAM above cannot boot.
- **BOOT-9** security, M. No TPM-sealed key: an encrypted root (roadmap 1.2 B5) unlocks unattended only through a key file on the unencrypted ESP; sealing the master key to measured PCRs (needs BOOT-3) is out of B5's scope.
- **BOOT-8** nice-to-have, S. `direct_map_size` covers the RSDP's tables, not those reached through the FADT (DSDT, FACS).

## Build and tooling

- **BUILD-1** security, S. The kernel builds with `-fno-stack-protector`; enable the stack protector with per-thread canaries.
- **BUILD-2** security, S. `COSMOOS_RELEASE_KEY` is a repository secret any pushed workflow can read; move it to an environment with required reviewers (repository settings).
- **BUILD-3** nice-to-have, M. Coverage builds with line coverage of the self-tests, and an LTO configuration, each with its own build and boot validation.
- **BUILD-4** nice-to-have, L. A KASAN-like kernel sanitiser build.
- **BUILD-5** nice-to-have, S. `ports/busybox/README.md`'s config regeneration fails at `make defconfig` on the macOS host; the checked-in config differs from the recipe in three options.
- **BUILD-6** nice-to-have, L. Native programs are static: no shared native libc and no native `PT_INTERP` (deferred by the owner past 1.1).
- ~~**BUILD-7** correctness, S. An rc's release run reads `docs/releases/vX.Y.Z-rcN.md`, which no release has, so the final notes (`vX.Y.Z.md`) are first checked by `check-release-version.py` in the final tag's run: v1.0.11's failed on "CosmoOS 1.0.10" in its notes (run 39942569981) after v1.0.11-rc1 had passed (run 39938572061). Check `vX.Y.Z.md` in the rc's run, or in CI when `docs/releases/` changes.~~ Done by #446 (the rc's run builds and checks `vX.Y.Z.md`).

## cosmofs and storage

- ~~**CFS-1** correctness, S. A read-only mount still writes the replay of a free or orphan record to the device.~~ Done by roadmap 1.2 B2's first PR (CF21: no commit, no read repair, no rollback switch; the replay runs in memory).
- ~~**CFS-2** correctness, S. New files are LZ4 on any format version, so an older-version image written here can hold v6 records.~~ Done by roadmap 1.2 B2's first PR (CF22: no record below format 6).
- ~~**CFS-3** correctness, S. Snapshot ids are unique per mount only: a deleted newest snapshot's id is reused after a remount (V22 says never).~~ Done by roadmap 1.2 B1's first PR (format version 12: the list head carries the id high-water mark, CF18); a pool older than format 11 keeps the old behaviour.
- **CFS-4** correctness, S. `cosmofs_rekey` leaves the key block in the open transaction until the next commit.
- ~~**CFS-5** correctness, S. `/dev/blkctl`'s mounted check does not see the other members of a mounted multi-device pool.~~ Done by roadmap 1.2 B3: a mount holds its pool's other copies (the first PR), resilver targets and spares (the second) (`vfs_mount_set_held`), which `vfs_bdev_mounted` and a new mount's claim see.
- ~~**CFS-6** correctness, M. CF17 abandons the transaction (`cfs_fail`) for failures a retry could survive; `extents_store` and `inode_sync` should allocate and write first so they fail cleanly.~~ Done by roadmap 1.2 B2's first PR (`cfs_prep_meta`, `cfs_inode_prepare`; `cosmofs-nomem`).
- ~~**CFS-7** security, M. Metadata is checksummed, not authenticated: a forger can recompute CRC32C; a keyed `csum_algo` would close it.~~ Done by roadmap 1.2 B5 (#447): on a format-14 pool with a key block every header, superblock and label carries a keyed BLAKE2b tag (CF30); plain pools keep CRC32C.
- **CFS-8** usability, M. Snapshots cannot be booted as environments (roadmap 1.2 B4). Rollback to a snapshot and writable clones are done by B1 (`cosmo-snap rollback`, `cosmo-snap clone`; CF19, CF20).
- ~~**CFS-9** usability, M. Mirrors: no device replacement, no resilvering of a member that missed commits, no hot spares.~~ Done by roadmap 1.2 B3: copy states and the v13 copy record (the first PR), the resilver, `cosmo-pool` replace, attach, detach and spares (the second), `cosmo-install --mirror` and `make test-mirror` (the fourth).
- ~~**CFS-10** usability, M. A file is capped at 516128 blocks (64516 encrypted) by the one-level checksum index (`-EFBIG`).~~ Done by roadmap 1.2 B5 (#447): a two-level index past those (CF32); test-encrypt writes and reads back 1 GiB on an encrypted root.
- ~~**CFS-11** usability, S. Scrub's `-EIO` discards the counts fsctl would report.~~ Done by roadmap 1.2 B2's first PR (the scrub returns 0 whenever its walk ran).
- **CFS-12** nice-to-have, L. Incremental snapshot send/receive: stream format, ancestry check, interruption recovery.
- **CFS-13** nice-to-have, L. Striping and RAID-Z-like parity layouts with failure, replay and repair coverage.
- **CFS-14** nice-to-have, L. Block deduplication, evaluated against its space, integrity and recovery costs.
- **CFS-15** nice-to-have, S. A cosmofs label cannot be changed after format; `mkfs.cosmofs` makes single-device unencrypted pools only. Roadmap 1.2 B2 added `mkfs.cosmofs --mirror A B` (a two-way mirror, no label); more members and encryption remain.
- **CFS-16** nice-to-have, L. A distributed filesystem (deferred: its own consistency and failure project).
- **CFS-17** usability, M. Rollback of a mounted root takes effect at the next mount (a reboot for the root); a live rollback of a mounted tree is not built (out of B1's scope, roadmap 1.2).
- **CFS-18** usability, S. No scheduled snapshots (taken on a timer and pruned by age); `cosmo-snap` is run by hand (out of B1's scope).
- **CFS-19** usability, M. No quotas: neither per snapshot nor per directory tree (out of B1's scope).
- **CFS-20** nice-to-have, S. A rollback's switch rebuilds every deadlist into new blocks while the old chains stay allocated until its commit, so a nearly full pool with long deadlists can refuse a rollback that would need no net space; the switch is abandoned and logged and the record stays pending (Qodo on #422). Since roadmap 1.2 B2 the reason is said: the snapshot listing's pool record carries the mount's error and init's line ends with it; the space problem itself remains.
- **CFS-21** nice-to-have, S. A file unlinked inside a writable clone while it is open is not on an orphan record (the live tree's is not the clone's), so its blocks stay the clone's until the clone is deleted; a clone cannot be snapshotted or cloned (B1's rules, design.md "Clones").
- **CFS-22** usability, M. No interactive fsck: what a repair cannot decide alone (two files claiming a block, a name twice in a directory, overlapping runs, an entry naming a free inode) is refused and named, never resolved with the operator (out of roadmap 1.2 B2's scope).
- **CFS-23** nice-to-have, S. A scrub runs one pool at a time and only when asked: no scrubs of several pools in parallel and none on a schedule (out of B2's scope).
- **CFS-24** usability, S. Only the root is checked at boot; a pool mounted later (`mount`, `/etc/rc`) is not.
- **CFS-25** correctness, S. `fsck.cosmofs --repair`'s writable mount replays the orphan record and applies a pending rollback before the check runs, and either can allocate: on a pool with a free block in use the allocator could hand that block out first, which the repair would then see as a cross-link (by reading the code, not observed).
- **CFS-26** nice-to-have, S. The scrub paces between inode blocks, data blocks and snapshots but reads one deadlist chain (up to 4096 blocks) without letting go of the lock.
- **CFS-27** nice-to-have, M. A mirror group holds at most four copies (`CFS_MAX_COPIES`, the copy record's four slots) and a replace of a good copy needs a free slot; wider mirrors need a format change (out of roadmap 1.2 B3's scope).
- **CFS-28** nice-to-have, M. No rebalancing across members: a member added later fills only from new allocations, and nothing moves blocks between members or off one being removed (out of B3's scope).
- **CFS-29** nice-to-have, S. A copy missing when a pool is mounted is marked for a whole resilver, even when its record said `ONLINE` at the root being mounted and only the commits after it were missed (`setup_copies`); a disk left out of one boot is rebuilt rather than caught up (roadmap 1.2 B3's `test-mirror`, stage `lagging`).
- **CFS-30** correctness, S. A scrub of the running root reports 53 to 77 `skipped` (visits to dirty buffers: inode-map, inode and checksum blocks), the same count after a `sync` and a 3 s wait (roadmap 1.2 B3's `test-mirror`, 2026-10-23); unexplained: either something re-dirties them at once or a sync leaves them dirty.
- ~~**CFS-31** correctness, S. The member table's seal was not checked at mount: it is read raw, before the buffer cache can check an address, and was used whenever its magic, kind and address were right.~~ Found and done by roadmap 1.2 B5's first PR (#447): checked with the pool's CRC32C or, on an encrypted pool, its MAC (CF30).
- ~~**CFS-32** correctness, M. A file LZ4 compresses is one run per 32 KiB record, and an inode's map is loaded only up to `CFS_MAX_EXTENTS` (4096) runs, so such a file stops at about 128 MiB with `-EFBIG` -- and the failure is not contained: every writeback commit fails with it, the dirty pages pile up until memory runs out, and they are lost (`vfs: N dirty page(s) ... lost`). Seen writing 1 GiB of binaries to an encrypted root (roadmap 1.2 B5's test-encrypt, 2026-10-24), and on a plain pool from the live shell the same day: 150 MB of text written to a fresh `mkfs.cosmofs` disk, every commit `-EFBIG`, `4344 dirty page(s) of inode 2 on cosmofs lost`, and `ls` still saying 150533400 bytes. Merge adjacent records' runs, or raise the bound with a map that is not loaded whole, and refuse the write itself rather than the commit.~~ Done in v1.0.14: a write the bound would refuse is refused at the write, `-EFBIG` with nothing dirtied (CF33, V39), and past three quarters of the bound new records are written plain so they merge; the largest compressible file on a 1 GiB pool went from 124 MiB (v1.0.13, with 512 dirty pages lost) to 1016 MiB, the pool full (`make test-large`, x86-64, 2026-10-25). A file synced after every record past the mark, or a sparse one, still meets the bound, at the write; no format change, and v1.0.13 reads the files (CFS-39 is the path whose charge is short).
- **CFS-33** usability, M. An existing plain root cannot be encrypted in place; `cosmo-install --encrypt` makes a new one (out of roadmap 1.2 B5's scope).
- **CFS-34** nice-to-have, M. One master key per pool: no per-user or per-directory keys a user unlocks apart from the pool (the per-file keys derive from the master; out of roadmap 1.2 B5's scope).
- **CFS-35** security, M. An encrypted pool's metadata MAC binds a block to its address and the pool, not to its generation: a block replaced by an older version of itself at the same address verifies (the tree is not hash-linked; roadmap 1.2 B5, design.md "The metadata MAC").
- **CFS-36** performance, M. Appending to a file costs more the larger it is: 32 MiB appended and synced took 3.2 s at the start of a file and 14-18 s past 768 MiB on a plain root (1 GiB in 303 s), 20 s at 1 GiB on an encrypted one (1 GiB in 386 s) (x86-64 under QEMU, 2026-10-24), so a large file's writing time grows with the square of its size. Not measured further: something in each commit is proportional to the file (candidates: its run map or checksum index written whole); find it and make a commit cost what it changed. Measured with v1.0.14's write policy (`make test-large`, a 1 GiB file in 4 MiB `dd ... conv=fsync` chunks, x86-64, 2026-10-25): 3 s per 32 MiB at 64 MiB, 6 s at 256 MiB, 8-9 s past 768 MiB; each commit writes the run map whole, but it stops at about 15 blocks once the file is past the mark, and a commit writes about 24 metadata blocks in all (extents 15, bitmap 3-4, checksum 1-2, index 1, inode 1, others 4) against 8192 data blocks per 32 MiB, flat past 256 MiB while the time still rises and the commits per 32 MiB fall from 24 to 8. So not the map or the checksum index written whole; the next candidate is CPU: each record write-back loads and stores the whole map in memory (O(runs)).
- **CFS-37** security, S. Argon2id's cost for a new slot is fixed (64 MiB, three passes), not calibrated to the machine: a guess takes about a second under QEMU but 0.16 s on an Apple-silicon Parallels VM (v1.0.13-rc1, 2026-10-24), so a passphrase there costs an attacker a sixth of what the guide first said. Time one derivation at install and raise the passes until it takes about a second, as `cosmo-key bench` already measures.
- **CFS-38** correctness, S. The check calls an inode's chain of extent blocks a cycle when it is longer than `CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2` blocks (`walk_extent_chain`, `cosmofs_check.c:373`, `chain_cycle`), so a chain that is only long is reported under the wrong name; detect a cycle by a block visited twice, and report a long chain as long.
- **CFS-39** correctness, S. A page in a compressed record is charged two runs at the write (CF33), but a write-back that leaves the record incompressible writes it again as eight plain blocks, which on free space of single blocks are eight runs: near the bound that write-back can fail `-EFBIG` (a `CONFIG_DEBUG` panic names it). Measured at about two runs a rebuild on a cut-up pool (`cosmofs-extent-rebuild`, 2026-10-25); charge such a page a record's worth, or rebuild it as one run.

## Documentation

- **DOC-1** usability, S. The Parallels guide does not say the live image's plain disk must be the image's own size (a larger disk's GPT is refused).
- **DOC-2** nice-to-have, S. `handle.h` says every function is non-blocking, but `handle_close` can block in the last put.

## Drivers and platforms

- **DRV-1** correctness, M. Non-coherent DMA: the virtqueue's used-ring reads, and e1000e, AHCI and xHCI in both directions, have no `dma_sync_*`.
- **DRV-2** correctness, M. Device interrupts and every AArch64 exception run on the thread stack; separate interrupt stacks.
- **DRV-3** correctness, S. PCI enumeration turns memory decoding off while sizing BARs; a console write then stalls on Parallels' framebuffer.
- **DRV-4** usability, L. No AML interpreter: Parallels' platform AHCI (SATA), x86-64 ACPI S5 power-off and the DSDT-described RTC are unreachable.
- **DRV-5** usability, L. No device-tree discovery on AArch64 (ACPI only).
- **DRV-6** nice-to-have, M. PCI rescan and runtime PCI hot-removal beyond the test hooks; device power management (suspend/resume). Roadmap 1.2 B3's third PR did PCIe native hot-removal and hot-add through a port's slot capability (attention button, presence, link; BARs assigned in the port's windows); left: a rescan of a bus that is not a hot-plug slot, bridges below a slot, ACPI hotplug, and suspend/resume.
- **DRV-7** nice-to-have, M. Per-device bind/unbind exists only in debug builds.
- **DRV-8** nice-to-have, L. IOMMU variants: interrupt remapping, AMD-Vi, huge pages, IOVA cache, PASID/ATS, stream ids above 4095 (roadmap 1.2 B3 raised the SMMUv3 table from 256 entries to 4096, buses 0 to 15), bridge aliases, the concatenated root for <= 42-bit SMMUs.
- **DRV-9** nice-to-have, M. No GICv3 pseudo-NMI on AArch64 (lockup sampling cannot interrupt a CPU with IRQs masked).
- **DRV-10** nice-to-have, M. Asynchronous errors: attribute through RAS records, and decide whether EL1 unmasks `PSTATE.A`.
- **DRV-11** nice-to-have, M. No CPU feature framework (`arch_cpu_has`) or errata table.
- **DRV-12** nice-to-have, S. ACPI: no host test or fuzzer of `acpi.c`, a second `acpi_init` duplicates entries, the XSDT/RSDT signature is not compared, GICC online-capable untested.
- **DRV-13** nice-to-have, S. Restart: a memory-space FADT reset register is not written; Ctrl-Alt-Del is not reported (`CAD_ON` changes nothing).
- **DRV-14** nice-to-have, S. `/dev/rtc0` has no alarms or update interrupts.
- **DRV-15** nice-to-have, S. QEMU `-accel hvf` stops on an MMIO access without ISV; use ISV-reporting accessors.
- **DRV-16** nice-to-have, L. A display interface for programs, cursor, 3D; Wi-Fi and Bluetooth; a desktop -- when hardware and a use case exist.
- **DRV-17** nice-to-have, M. Hot-removal of a device that is not on PCIe (a USB disk under a mounted pool is handled by its own driver's disconnect only); roadmap 1.2 B3 did PCIe (out of its scope).

## Kernel modules

- **MOD-1** correctness, S. An object a module makes while its `init` or `shutdown` runs belongs to no module: `kobject_init` asks `module_owner_of` (`kernel/object/object.c`), which searches only the published modules, and a module is published after `init` returns and unpublished before `shutdown` runs (`kernel/module/module.c`). So such an object raises no `live_objects`, `module_unload` does not wait for it, and if it outlives the unload its release runs from freed module text. No in-tree module hands one out (the self-tests make theirs after the load); found by Nova's C8 (its report, 2026-10-25), whose first zombie control unloaded with 0 while kmalloc's live objects grew by 2 a cycle. `module-cycle` (invariant M27) is the harness for its failing test.

- **MOD-2** nice-to-have, M. `module_cycle`'s counts are machine-wide (kmalloc's live objects and large pages, the VM's regions and anonymous pages), so other work that allocates for good during a run (a late DHCP lease, a neighbour entry, a route) fails a module that leaks nothing; the run only waits out allocations that are freed again (`MODULE_CYCLE_SETTLE_MS`, design.md, "Load and unload in cycles"). Count allocations by the module whose code made them instead, which needs a hook in `kmalloc`'s path that takes no lock and does no atomic read-modify-write when unarmed.

## Linux personality

- **LX-1** correctness, S. `waitpid(WCONTINUED)` can miss a continue when the child exits at once (`child_event_locked` tests EXITED first; flakes.md, three sightings).
- **LX-2** correctness, S. Linux fork copies SysV shm attach records after the space; a concurrent `shmat`/`shmdt` can split record and mapping.
- **LX-3** correctness, S. `nanosleep` restarted after a stop sleeps its whole duration again, not the remainder.
- **LX-4** correctness, S. `dup2` to a slot at or above `RLIMIT_NOFILE` succeeds where Linux returns `EBADF`.
- **LX-5** correctness, S. An object whose only reference is a handle in flight in an unread unix message loses its epoll registrations.
- **LX-6** correctness, M. Setting the wall clock does not re-arm absolute `CLOCK_REALTIME` sleeps, futex waits and timers, nor cancel `TFD_TIMER_CANCEL_ON_SET`.
- **LX-7** usability, M. No netlink (`AF_NETLINK` rtnetlink): BusyBox `ip` stays off and Linux programs cannot enumerate interfaces that way.
- **LX-8** usability, M. `setsockopt` is `-ENOPROTOOPT` for everything; `getsockopt` answers `SO_ERROR` and `SO_PEERCRED` only.
- **LX-9** usability, M. `mremap`: no relocation, `MREMAP_FIXED`, file-backed or sub-range cases; a `MAP_SHARED|MAP_ANONYMOUS` mapping is `-EINVAL`.
- **LX-10** usability, S. No memfd sealing (`F_ADD_SEALS`).
- **LX-11** usability, L. No Linux `/sys`; `/proc` is partial; no real distribution userland validated (Linux phase 4).
- **LX-12** usability, M. termios: most flags and control characters are accepted and ignored.
- **LX-13** usability, S. No nice values or `setpriority`/`nice` (`stat` reports 20/0 for every process).
- **LX-14** usability, S. `ITIMER_VIRTUAL` and `ITIMER_PROF` are `-EINVAL`.
- **LX-15** usability, S. A fork-like `clone` takes only `SIGCHLD` as exit signal (`__WCLONE` not built).
- **LX-16** usability, S. `fchownat`/`fchmodat` through a dirfd need the handle's write right; Linux needs none.
- **LX-17** usability, M. `adjtimex`/`clock_adjtime` are `-ENOSYS`: no slewing, `ntpd` runs only with `-q`.
- **LX-18** nice-to-have, M. SysV shm: `SHM_RND`/`REMAP`, `IPC_SET`, lock/info operations, owner/creator ids, huge pages, IPC namespaces.
- **LX-19** nice-to-have, S. `rseq` is `-ENOSYS`.
- **LX-20** nice-to-have, S. `prctl` is the dumpable flag alone.
- **LX-21** nice-to-have, M. `chroot` is refused to a multi-threaded process (P39).
- **LX-22** nice-to-have, S. `fchown` on a pipe or socket is `EBADF`; no time zone kept by `settimeofday`.
- **LX-23** nice-to-have, M. glibc's dynamic linker untried; no vDSO (`AT_SYSINFO_EHDR`).

- **LX-24** correctness, S. BusyBox `yes` with a 53-byte line, piped to `head -c N | md5sum`, never exits once `head` has its N bytes: `md5sum` prints, `head` exits, and `yes` runs on (state R, its stdout the pipe's write end) until killed, so the shell waits for ever. `yes abc` exits at once with `Broken pipe` at every size tried (1000 B to 200 MiB); the 53-byte line hangs at 10 MB and 200 MiB, every time. Seen on v1.0.14-rc1 and on v1.0.13 (Parallels, aarch64, 2026-10-26), so not v1.0.14's. `pipe_ring_write` returns `-EPIPE` once the readers are gone and no write raises SIGPIPE here; what `yes` gets back instead is not yet known.

## Lock validator and concurrency

- **LOCK-1** correctness, M. lockdep's writer under general NMI/#MC entry, and cross-CPU raw-lock wait cycles, are undefined (same-CPU re-entry fails stop).
- **LOCK-2** nice-to-have, M. No simultaneous global held-state snapshot across CPU spinlock and thread mutex stacks.
- **LOCK-3** nice-to-have, M. Kernel interrupt-entry and callback interleavings validated beyond the host models and the targeted IPI/NMI tests.
- **LOCK-4** nice-to-have, M. Measure contention, PI waits, graph-size sweeps, worst-case search latency and native-hardware cost.
- **LOCK-5** nice-to-have, S. Decide whether a spin wait on a stopped owner needs a diagnostic or recovery contract.
- **LOCK-6** nice-to-have, M. Kernel rwlock, only with a workload, ownership rules and a lockdep model.
- **LOCK-7** nice-to-have, M. Ticket or MCS spinlocks, only with measurements.
- **LOCK-8** nice-to-have, M. Completion edges not modelled: a signaller taking a mutex before `complete()`, one blocked in another wait, the network worker's barrier.
- **LOCK-9** correctness, S. Release boots warn `mutex PI: donation chain cycle -- a lock-order violation` (`mutex.c`) while `/etc/rc` starts services (`svc` starting `greeter`), in some boots of the installed system on both architectures since at least the B1 branches (2026-10-20); unattributed, no backtrace in release; a debug boot of the installed system would name the chain. Roadmap 1.2 B3's `test-mirror` (2026-10-23, x86-64 release) saw it 53 times in one boot, while a shell loop copied `/bin/busybox` and synced, before any disk was removed.
- **LOCK-10** correctness, S. Lockdep keeps a class for every address a callback ever had: a callback's class is named `callback %p` (`lockdep.c`, `callback_name`) and the table only grows, so each load of a module whose callback lands at a new address adds a class that stays, until the table is full ("lock class table full", and lockdep stops). Nova's C8 measured 136 -> 143 over 1,000 load/unload cycles and one per zombie (2026-10-25); an x86-64 debug boot already peaks at 481 of 512 without it (`lockdep: class table peak`, 2026-10-25). `module_cycle` reports the count and does not judge it.

## Memory

- **MEM-1** correctness, M. `vm_map_phys` maps large blocks that `vm_unmap_phys` cannot tear down across 2 MiB; its 14 callers are not audited.
- **MEM-2** usability, S. The native ABI has no memfd or `shm_open`.
- **MEM-3** nice-to-have, S. `VM_KALLOC_POPULATE` fills every page under the kernel space spinlock with IRQs off.
- **MEM-4** nice-to-have, L. User huge pages.
- **MEM-5** nice-to-have, L. NUMA: topology (SRAT), node-aware allocation and placement.
- **MEM-6** nice-to-have, L. Swap, compression and deduplication.
- **MEM-7** nice-to-have, M. x86-64 PCID, once a test path exists (TCG has none).
- **MEM-8** nice-to-have, S. Thread guard page by map-then-protect instead of reserve-and-replace (no benefit shown).
- **MEM-10** nice-to-have, S. There is no swap; when there is, it should be encrypted (an encrypted root's pages would otherwise reach the disk in the clear; out of roadmap 1.2 B5's scope).
- ~~**MEM-9** correctness, M. Dirty page-cache pages are not bounded by memory or by the filesystem's free space: `dd` to a file on a 256 MiB root wrote ~470 MB before it was killed by a fault, and every spawn after was `ENOMEM` (test-pkgtx's first fill, 2026-10-20). A write should meet `ENOSPC` when the pool cannot hold it, and writeback should throttle a writer. It also costs time: 1 GiB written to an encrypted root with one `sync` at the end took 1275 s on x86-64 under QEMU (256 MiB of memory), against 386 s with a `sync` after each 32 MiB (2026-10-24; test-encrypt now syncs every 32 MiB).~~ Done in v1.0.14: dirty pages of reclaimable mounts are bounded at an eighth of RAM, a writer at the bound writes its own file back and sleeps holding no lock (V40), and a page of a regular file is charged at the write against the pool's free blocks above the reserve, so a full pool refuses the write `ENOSPC` with no page lost (CF34): `make test-fill` (512 MiB with no sync on a 256 MiB guest, nothing killed; one `dd` filling a 256 MiB root stopped by `ENOSPC` after 170 MiB with 16 KiB left), both architectures, 2026-10-25.

## Network

- **NET-1** correctness, M. Blocking UDP `send` gets `-ENOBUFS` on a full transmit ring where Linux waits; decide with a two-personality test.
- **NET-2** correctness, S. `mbufq_enqueue`'s `false` means both "freed" and "untouched"; give callers a distinct result.
- **NET-3** correctness, S. virtio-net posts RX before `netif_register` with no purge on a failed registration; prove whether an escaped `rcvif` outlives the device.
- **NET-4** correctness, S. An interface's IPv4 configuration is published field by field and read with plain loads; packet paths need a consistent snapshot.
- **NET-5** security, M. The filter does not inspect IPv6.
- **NET-6** security, S. The SYN-cookie secret is drawn once at `tcp_init`, possibly before the pool seeds; rekey at seeding.
- **NET-7** security, M. `wget`'s CA bundle is fixed at build time and nothing checks revocation.
- **NET-8** usability, L. TCP window scaling, SACK, timestamps, ECN, fast recovery, sender Nagle.
- **NET-9** usability, M. No IP fragmentation or reassembly (fragments dropped); with it, MTU change and per-interface MSS (`SIOCSIFMTU`, `SIOCSIFHWADDR` take the current value only).
- **NET-10** usability, L. IPv6 beyond the link: routing, router discovery, ND against a real peer.
- **NET-11** usability, M. No `ping6` or raw IPv6 socket (`ICMP6_FILTER`, ancillary data); raw ICMP has no `bind`, `connect`, `IP_HDRINCL`, `ICMP_FILTER`.
- **NET-12** usability, M. No routing table or static neighbours: `route add/del`, `arp -s/-d` are `-ENOTTY`.
- **NET-13** usability, M. DHCP client: no ARP probe/DECLINE, INIT-REBOOT, RELEASE, option 121; `resolv.conf` not rewritten on renewal.
- **NET-14** usability, S. The control channel cannot set forwarding, masquerade, the resolver or tap up/down.
- **NET-15** usability, S. `ip` needs `PRIV_NET` (`/dev/net/tapctl`); Linux's `ip a` is unprivileged.
- **NET-16** nice-to-have, M. Filter: per-interface host chains, rate-limit and log targets, full TCP state tracking.
- **NET-17** nice-to-have, L. IPv6 NAT and DNAT, hairpin NAT, an L2 bridge, per-guest limits beyond the NAT quota.
- **NET-18** nice-to-have, L. A DHCP server, caching resolver, DHCPv6, DNS over TCP, DNSSEC.
- **NET-19** nice-to-have, L. Faster data paths (zero-copy, multiqueue, TSO/LRO, jumbo, NAPI, interrupt moderation, busy polling), only with measured benefit.
- **NET-20** nice-to-have, M. Routing and protocol lookups on the epoch read side (constitution §22).
- **NET-21** nice-to-have, S. Small forwarding and listing gaps: route tie between two interfaces, UDP length mismatch relayed, IP id/options rebuilt, IPv6 neighbours unlisted, `SIOCSIFFLAGS` only `UP`, `/proc/net` uid/inode 0 and UDP receive queues in datagrams, not bytes.
- **NET-22** nice-to-have, S. e1000e checksum offload (measured at about 2 %).
- **NET-23** correctness, S. `net-tcp-delack` asserts no retransmission with the host-wide `tcp_get_stats` counter, so another pcb's retransmission fails it (AArch64 SMP4, 2026-10-24, flakes.md); count the client pcb's own.

## Observability

- **OBS-1** correctness, S. `p->syscalls` is a plain increment shared by threads; `syscall_count` and two siblings have no callers.
- **OBS-2** nice-to-have, M. A kernel debugger or documented GDB remote workflow per architecture.
- **OBS-3** nice-to-have, M. Kernel-side symbolisation and crash-state preservation (host-side resolution exists).
- **OBS-4** nice-to-have, M. Per-CPU structured tracing and performance counters, and operator tools over them (`ktrace`, `kstat`, `cosmo-prof`).
- **OBS-5** nice-to-have, L. eBPF-like tracing (verification, isolation, attachment lifetime).
- **OBS-6** nice-to-have, M. Benchmarks for kmalloc, page faults, `mmap`, IPI round trip, fsync, metadata and VM exits, and baseline-versus-change tracking.
- **OBS-7** nice-to-have, S. `/proc` fidelity: `environ` from exec's record not memory, `cpuinfo` the boot CPU's for all with no `flags`/`Features` or BogoMIPS, no user/system CPU split.

## Packages and upgrade

- **PKG-1** correctness, S. `pkg`'s whole-database checks (ownership, `depends`, `list`, `upgrade`) pass over installed records beyond 128 silently.
- **PKG-2** correctness, S. On v1.0.3, `pkg install` of `cosmo-upgrade.cpk` without `-n` leaves a package a rollback then removes.
- ~~**PKG-3** usability, M. No multi-package transactions, interrupted-install recovery or per-package rollback.~~ Done by roadmap 1.2 B4's first PR: each command a root snapshot undone by init after an interruption, `pkg rollback NAME` (PK11, PK12).
- ~~**PKG-4** usability, M. `cosmo-upgrade --rollback` keeps the packages `pkg upgrade` changed and `/etc` files the new release added.~~ Done by roadmap 1.2 B4: boot environments (`cosmo-upgrade --rollback` returns the root's snapshot with the set) for upgrades from v1.0.8's tool on.
- **PKG-5** usability, S. `cosmo-upgrade --fetch` runs the old binary's rules; a new directory (`lib/`) needs a second run. (Roadmap 1.2 B4 named it but did not take it: the base pass could run the new set's own `cosmo-upgrade`, which only two releases whose tools differ can test.)
- **PKG-6** usability, L. Libraries by name only: no versions, no provides/requires in the INDEX, no solver.
- **PKG-7** nice-to-have, M. SBOMs, dependency locking, build sandboxing, multi-architecture packages.
- **PKG-8** nice-to-have, M. `cosmo-upgrade`'s FAT writer refuses long names, a multi-sector `\cosmo` and FAT12/16.
- ~~**PKG-9** usability, S. `pkg install -f NAME=VERSION` to an older version says `NAME will be upgraded from 1.1 to 1.0`; a downgrade should say so (seen on Parallels, 2026-10-21).~~ Done by PR #431: `NAME will be downgraded from 1.1 to 1.0`.
- ~~**PKG-10** correctness, S. `cosmo-upgrade` trusted the root's record of its base system: an older tool's roll-forward left it naming the set rolled back to, and v1.0.8 refused every later upgrade.~~ Done by PR #430: the record is checked against the root's files and corrected.
- ~~**PKG-11** usability, S. On a mirrored root (`cosmo-install --mirror`, roadmap 1.2 B3) `cosmo-upgrade` writes the new release to the ESP of the disk the system booted from only; the other disk's ESP keeps the release it was installed with, so booting from it after the first disk fails starts an older kernel and loader.~~ Done by #446 (every copy's ESP written and read back; `--esp-check` at boot, `--repair`; U15).

## Quiescence

- **QUI-1** nice-to-have, M. Measure grace periods at 16-256 CPUs and find long preemption-disabled sections.
- **QUI-2** nice-to-have, S. Widen the straggler-kick sample and attribute its publications; then delete, bound or prove it.
- **QUI-3** nice-to-have, M. The idle-CPU tick floor on grace-period latency (needs a quiescent-point design).

## Scheduler and SMP

- **SMP-1** usability, L. Fixed-priority round-robin only: no fair, real-time or deadline policy, CPU isolation, or policy syscalls.
- **SMP-2** nice-to-have, L. More than 64 CPUs: one-word `cpumask_t`, xAPIC only, one cross-call slot, sequential AP bring-up, NVMe-only IRQ spreading.
- **SMP-3** nice-to-have, L. CPU hotplug.
- **SMP-4** nice-to-have, M. Migrating a thread preempted in user mode (S26 need not hold there); no measured stranding yet.
- **SMP-5** nice-to-have, S. `pick_cpu`'s 2:1 tie rotation, and a CPU whose own sleeper expires every tick never balancing as idle.
- **SMP-6** nice-to-have, M. Lazy FPU switching (deferred by measurement).

## Security and isolation

- **SEC-1** security, M. A root session with fewer privileges can rewrite `/etc/svc` and init's scripts, which run with all of them at the next boot.
- **SEC-2** security, M. No audit logging of privileged operations, logins or denials.
- **SEC-3** security, M. A confined walk's `..` can pass the root under a concurrent rename (needs a rename sequence count).
- **SEC-4** security, S. The Linux signal trampoline is executable at the fixed `0x7FFFFFFF1000` in every Linux process.
- **SEC-5** security, M. Native programs and static BusyBox are `ET_EXEC`: ASLR does not move their code (static-PIE).
- **SEC-6** security, S. No SP 800-90B health tests on RDSEED/RNDRRS, the firmware seed or virtio-rng.
- **SEC-7** security, M. AArch64 UAO, E0PD, BTI and PAC unused.
- **SEC-8** security, M. No documented speculative-execution policy or tested mitigations.
- **SEC-9** security, S. musl leaves `libc.so`'s own RELRO writable on AArch64 (`PAGE_SIZE` 0 when recorded).
- **SEC-10** security, L. Network and IPC namespaces, cgroups-style accounting, capabilities beyond handle rights.
- **SEC-11** usability, L. Container tooling over the existing primitives (with syscall filtering); Docker/Kubernetes compatibility after it.
- **SEC-12** nice-to-have, S. No supplementary groups at login (`initgroups`).
- **SEC-13** nice-to-have, M. No setuid/setgid bits or file capabilities (so `AT_SECURE` follows the credentials only).

## Testing and CI

- **TEST-1** correctness, S. `wait-multi-prepare`'s guard misses `schedule()` picking the same thread again.
- **TEST-2** correctness, S. `module-unload-busy` compares a tick-quantised deadline with `clock_since_ns` on a machine without a common clock.
- **TEST-3** correctness, S. `net-neigh-down-race`'s park hooks park the first caller on any interface.
- **TEST-4** correctness, S. `net-hostinput` leaves a transmitting TCP connection the census does not see.
- **TEST-5** correctness, S. CI's failure upload for `test-display` includes QMP sockets and uploads nothing.
- **TEST-6** correctness, S. Unexplained: AArch64 `syscall-fuzz` over its budget (8,240 ms of 8,000); the AArch64 virtio-console file missing its last line.
- **TEST-7** correctness, S. Test timing that stands in for durations: `cwdtest --held`'s 200,000-yield bound, remaining `thrtest`/`cwdtest` waits.
- **TEST-8** correctness, S. `net-bench`: a loopback TCP flow's 200 ms RTO with no drop, and an earlier test's connection still retransmitting during it.
- **TEST-9** nice-to-have, M. Fuzzing for PCI configuration, ACPI tables, VFS paths and the NIC descriptor/tap/worker paths; property-based tests.
- **TEST-10** nice-to-have, M. Fault injection: CPU starvation, device-reset faults, VM-exit storms; injection in interrupt context.
- **TEST-11** nice-to-have, S. CI boots 2 and 4 CPUs only; one firmware build; no documented minimum EDK2/AAVMF; PSCI variations untested.
- **TEST-12** nice-to-have, L. A physical hardware matrix (AMD, Intel, Apple Silicon).
- **TEST-13** nice-to-have, M. Linux-guest demonstrations (rootfs, writable root, real-NIC reach) as CI gates.
- **TEST-14** nice-to-have, M. The slirp defect behind the network harness's retry (outside this kernel).
- **TEST-15** nice-to-have, M. The debug suite assumes QEMU's machine (`pci`, `net-firewall`, `net-tap-release-order`, `blk-submit-unregister` fail on Parallels).
- **TEST-16** nice-to-have, M. Paths proved by review only: retained-DMA sites, `nvme_probe` after `CC.EN`, the usb-hub worker, CF17's other sites, cosmofs chain cycles. Since roadmap 1.2 B2 the cosmofs chain cycles are covered on the read side (CF24: a `test-fsck` class per walker; the four write-side walkers B2 bounded are checked by review) and `extents_store`/`inode_sync` by `cosmofs-nomem`; CF17's checker, create, symlink, bitmap and deadlist sites remain review-only.
- **TEST-17** nice-to-have, S. Untested refusals and paths: `switch_root`, partitions removed with bios in flight, halt, `cosmo-upgrade` rerun after `--stop-after=set` and an interruption inside a FAT-chain write, virtio-net probe faults.
- **TEST-18** correctness, S. Self-tests that wait for the machine-wide thread count (`threads_settle`, `kernel/scheduler/schedtest.c`) fail when a thread elsewhere exits in the window, and the dump cannot name it; `sched-restore-loop` twice on AArch64 (docs/testing/flakes.md). Baseline the identities.
- ~~**TEST-19** correctness, S. The guide test's `networking.md` fetches `http://example.com/` (and HTTPS), a public site that answered 503 to a CI runner twice on 2026-10-22 and failed main's run and the v1.0.10 release run (docs/testing/flakes.md); the guide can show the command while the test checks a site the project controls, or retries a 5xx.~~ Done by PR #438: the guide checks the project's own site (`https://vkuttyp.github.io/CosmoOS/releases/LATEST`), `example.com` is an `optional` block that only warns, and a 5xx answer is retried three times (5, 10, 20 s).
- ~~**TEST-20** correctness, S. The boot harnesses (`install_test.Boot`, `platform_test.py`, `run_boot_test.py`) read QEMU's stderr into the serial stream, so a message QEMU prints inside a guest line splits it: main's `test-mirror` on `097b6a19` (run 40077407190) failed `pool: cosmo-root: member 0 copy 1 (vda2) MISSING` with `qemu-system-x86_64: wrong value for queue_enable 0`, from the hot unplug, written in its middle.~~ Done by the PR that found it (QEMU's stderr to `boot-<name>-qemu.log`; `tests/host/test_boot_harness.py`).
- **TEST-21** correctness, S. x86-64 OVMF sometimes never reaches the loader after a guest-initiated reset inside one QEMU (`reboot: restart through the ACPI reset register`, then only screen clears; three sightings, flakes.md), and the boot harnesses retry a stalled firmware only at launch (`firmware_stall.py`).

## Commands

- **USR-1** usability, S. The live image's native shell expands `$?` for a whole line before running it.
- **USR-2** usability, S. `/dev/log` is root-writable only (no native `chmod` for `syslogd`).
- **USR-3** usability, S. No `useradd`; a second user is made by editing `/etc/passwd`.
- **USR-4** usability, S. `/etc/fstab` cannot name `LABEL=` or `UUID=`.
- **USR-5** nice-to-have, S. No `xz` compressor; `syslogd` keeps no file or ring; `lsblk` has no `MAJ:MIN` or options; `blkid` knows no FAT or `PARTLABEL`.
- **USR-6** correctness, S. `cosmo-sh` hands the terminal to a foreground job as its last stage is spawned (COSMO_SPAWN_FOREGROUND), so the group holds every stage when it takes ^C; the earlier stages of a pipeline start in the background for the moment the spawns take, and one that checks the terminal at its first instruction can still stop itself. A start barrier (spawn stopped, continue the group) would close it (Codex on #424).
- **USR-7** usability, S. No `/dev/urandom` or `/dev/random`: a shell script wanting random or incompressible bytes reads nothing (`head -c N /dev/urandom` gives at most the error); tests use awk's `rand()` or gzip output instead (test-pkgtx, test-fsck).
- ~~**USR-8** correctness, S. On the installed system `/run/svc` is on the root disk and nothing clears it at boot, so a pid file outlives its service; `svc`'s `running()` trusts any pid `kill(p, 0)` can signal, and a boot where another process has that pid says `svc: dropbear is already running` and never starts it. Seen on the Parallels VM 2026-10-23: `dropbear.pid` held 13 from 2026-10-22, pid 13 was `syslogd` at every boot, and ssh was refused through several boots and upgrades (cleared by hand). Clear `/run` at boot (or mount a ramfs on it), or check the pid is the service's supervisor.~~ Done by #446 (a fresh ramfs on `/run` at every boot; a pid file records the start time and is checked against the supervisor's cmdline; U14).

## VFS

- **VFS-1** correctness, S. `vfs_setattr` checks the owner before the vnode lock and keeps a mode the filesystem failed to store.
- **VFS-2** correctness, S. `chown`/`chmod` on procfs or devfs changes only the cached vnode.
- **VFS-3** correctness, M. A read whose copy-out faults loses what the object took from its source (pipes, console, pty), except signalfd.
- **VFS-4** usability, M. No hard links (`link`/`linkat`).
- **VFS-5** usability, M. Mount options: `mount(2)` takes `MS_RDONLY` only (no remount, bind, move, `nosuid`...) and no option string; `umount2` refuses `MNT_DETACH`, `MNT_EXPIRE`, `UMOUNT_NOFOLLOW`; no overlay.
- **VFS-6** usability, S. The native ABI has no `chmod`, `chown`, `chroot`, `utimes` or `umask`; the Linux umask is not applied to native creation.
- **VFS-7** usability, M. Devices have no numbers (`st_rdev` 0), so `ps` cannot name a terminal and `lsblk` has no `MAJ:MIN`.
- **VFS-8** usability, S. No `/proc/filesystems` (BusyBox `mount` without `-t` fails).
- **VFS-9** nice-to-have, M. A dentry cache and an `(ino, generation)` identity.
- **VFS-10** nice-to-have, S. No access times; `/proc/<pid>/cwd` and `root` are `ENOENT` across roots.
- **VFS-11** nice-to-have, M. A native `/sys` (deferred: sysctl holds it).

## Virtualisation

- **VIRT-1** correctness, M. The VMX backend has never executed; it needs Intel hardware or nested KVM.
- **VIRT-2** correctness, M. x86 guest fidelity: MOV-DR and RDPMC exits enabled and unhandled, WBINVD a no-op, string I/O, TSC, CPUID pass-through.
- **VIRT-3** correctness, M. The device models are tested with one guest vCPU only.
- **VIRT-4** nice-to-have, L. Dirty tracking and ballooning.
- **VIRT-5** nice-to-have, L. Full vCPU and device state in the UAPI, then VM snapshots and live migration.
- **VIRT-6** nice-to-have, L. Device passthrough, with IOMMU DMA and interrupt isolation, assignment lifetime and reset.
- **VIRT-9** nice-to-have, L. Nested virtualisation (after the VM state UAPI, VIRT-5).
- **VIRT-7** nice-to-have, M. VM operations as submittable async-I/O.
- **VIRT-8** nice-to-have, M. vGIC on GICv2 hosts.
