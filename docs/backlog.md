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
| Block devices | 3 | 0 | 2 | 3 | 8 |
| Boot chain and verified boot | 0 | 5 | 2 | 1 | 8 |
| Build and tooling | 0 | 2 | 0 | 4 | 6 |
| cosmofs and storage | 5 | 1 | 7 | 7 | 20 |
| Documentation | 0 | 0 | 1 | 1 | 2 |
| Drivers and platforms | 3 | 0 | 2 | 11 | 16 |
| Linux personality | 6 | 0 | 11 | 6 | 23 |
| Lock validator and concurrency | 1 | 0 | 0 | 7 | 8 |
| Memory | 1 | 0 | 1 | 6 | 8 |
| Network | 4 | 3 | 8 | 7 | 22 |
| Observability | 1 | 0 | 0 | 6 | 7 |
| Packages and upgrade | 2 | 0 | 4 | 2 | 8 |
| Quiescence | 0 | 0 | 0 | 3 | 3 |
| Scheduler and SMP | 0 | 0 | 1 | 5 | 6 |
| Security and isolation | 0 | 10 | 1 | 2 | 13 |
| Testing and CI | 8 | 0 | 0 | 9 | 17 |
| Commands | 1 | 0 | 4 | 1 | 6 |
| VFS | 3 | 0 | 5 | 3 | 11 |
| Virtualisation | 3 | 0 | 0 | 6 | 9 |
| **all** | **41** | **21** | **49** | **90** | **201** |

## Block devices

- **BLK-1** correctness, S. virtio-blk timeout against a submit parked between its dead check and the queue lock: prove with a test, fix as NVMe's was (#338).
- **BLK-2** correctness, S. USB storage: the timeout path clears `recovering`/`broken` after a failed BOT reset recovery; prove the state under a failed recovery and keep it.
- **BLK-3** correctness, M. No bounce buffers: a 32- or 44-bit DMA device refuses page-cache pages above its mask with `-EINVAL` once RAM is above 4 GiB.
- **BLK-4** usability, S. GPT: a disk whose primary or backup table alone is valid gets no partitions; recover from the good copy.
- **BLK-5** usability, S. No partition scan when a disk registers after boot (USB, hot-plug); only `/dev/blkctl` RESCAN.
- **BLK-6** nice-to-have, M. AHCI NCQ, deferred until a real disk shows it pays (four streams reach 87 % of NVMe without it).
- **BLK-7** nice-to-have, S. USB storage scatter-gather beyond 16 segments, deferred (within noise of NVMe).
- **BLK-8** nice-to-have, L. The rest of the NVMe feature set, only for a demonstrated device or workload.

## Boot chain and verified boot

- **BOOT-1** security, M. KASLR: a relocatable kernel, a loader-chosen slide from `EFI_RNG_PROTOCOL` or the CPU, the slide in the boot protocol, symbolisation and `ksymtab` adjusted.
- **BOOT-2** security, M. The loader is not signed for UEFI Secure Boot (no db enrolment): replacing `BOOTX64.EFI` defeats verified boot.
- **BOOT-3** security, M. No measured boot: the manifest and boot files are not extended into TPM PCRs.
- **BOOT-4** security, L. The installed root filesystem is not verified (no dm-verity-like tree); verification ends at the boot archive.
- **BOOT-5** security, M. No anti-rollback: any boot manifest or module ever signed by a trusted key verifies forever; needs signature versions and a recovery-safe policy.
- **BOOT-6** usability, S. init does not wait for a `root=` disk that registers late (USB storage); it stays on the live root.
- **BOOT-7** usability, S. The loader allocates below 4 GiB only (`LOADER_ALLOC_LIMIT`); a machine with all RAM above cannot boot.
- **BOOT-8** nice-to-have, S. `direct_map_size` covers the RSDP's tables, not those reached through the FADT (DSDT, FACS).

## Build and tooling

- **BUILD-1** security, S. The kernel builds with `-fno-stack-protector`; enable the stack protector with per-thread canaries.
- **BUILD-2** security, S. `COSMOOS_RELEASE_KEY` is a repository secret any pushed workflow can read; move it to an environment with required reviewers (repository settings).
- **BUILD-3** nice-to-have, M. Coverage builds with line coverage of the self-tests, and an LTO configuration, each with its own build and boot validation.
- **BUILD-4** nice-to-have, L. A KASAN-like kernel sanitiser build.
- **BUILD-5** nice-to-have, S. `ports/busybox/README.md`'s config regeneration fails at `make defconfig` on the macOS host; the checked-in config differs from the recipe in three options.
- **BUILD-6** nice-to-have, L. Native programs are static: no shared native libc and no native `PT_INTERP` (deferred by the owner past 1.1).

## cosmofs and storage

- **CFS-1** correctness, S. A read-only mount still writes the replay of a free or orphan record to the device.
- **CFS-2** correctness, S. New files are LZ4 on any format version, so an older-version image written here can hold v6 records.
- ~~**CFS-3** correctness, S. Snapshot ids are unique per mount only: a deleted newest snapshot's id is reused after a remount (V22 says never).~~ Done by roadmap 1.2 B1's first PR (format version 12: the list head carries the id high-water mark, CF18); a pool older than format 11 keeps the old behaviour.
- **CFS-4** correctness, S. `cosmofs_rekey` leaves the key block in the open transaction until the next commit.
- **CFS-5** correctness, S. `/dev/blkctl`'s mounted check does not see the other members of a mounted multi-device pool.
- **CFS-6** correctness, M. CF17 abandons the transaction (`cfs_fail`) for failures a retry could survive; `extents_store` and `inode_sync` should allocate and write first so they fail cleanly.
- **CFS-7** security, M. Metadata is checksummed, not authenticated: a forger can recompute CRC32C; a keyed `csum_algo` would close it.
- **CFS-8** usability, M. Snapshots cannot be booted as environments (roadmap 1.2 B4). Rollback to a snapshot and writable clones are done by B1 (`cosmo-snap rollback`, `cosmo-snap clone`; CF19, CF20).
- **CFS-9** usability, M. Mirrors: no device replacement, no resilvering of a member that missed commits, no hot spares.
- **CFS-10** usability, M. A file is capped at 516128 blocks (64516 encrypted) by the one-level checksum index (`-EFBIG`).
- **CFS-11** usability, S. Scrub's `-EIO` discards the counts fsctl would report.
- **CFS-12** nice-to-have, L. Incremental snapshot send/receive: stream format, ancestry check, interruption recovery.
- **CFS-13** nice-to-have, L. Striping and RAID-Z-like parity layouts with failure, replay and repair coverage.
- **CFS-14** nice-to-have, L. Block deduplication, evaluated against its space, integrity and recovery costs.
- **CFS-15** nice-to-have, S. A cosmofs label cannot be changed after format; `mkfs.cosmofs` makes single-device unencrypted pools only.
- **CFS-16** nice-to-have, L. A distributed filesystem (deferred: its own consistency and failure project).
- **CFS-17** usability, M. Rollback of a mounted root takes effect at the next mount (a reboot for the root); a live rollback of a mounted tree is not built (out of B1's scope, roadmap 1.2).
- **CFS-18** usability, S. No scheduled snapshots (taken on a timer and pruned by age); `cosmo-snap` is run by hand (out of B1's scope).
- **CFS-19** usability, M. No quotas: neither per snapshot nor per directory tree (out of B1's scope).
- **CFS-20** nice-to-have, S. A rollback's switch rebuilds every deadlist into new blocks while the old chains stay allocated until its commit, so a nearly full pool with long deadlists can refuse a rollback that would need no net space; the switch is abandoned and logged and the record stays pending (Qodo on #422).
- **CFS-21** nice-to-have, S. A file unlinked inside a writable clone while it is open is not on an orphan record (the live tree's is not the clone's), so its blocks stay the clone's until the clone is deleted; a clone cannot be snapshotted or cloned (B1's rules, design.md "Clones").

## Documentation

- **DOC-1** usability, S. The Parallels guide does not say the live image's plain disk must be the image's own size (a larger disk's GPT is refused).
- **DOC-2** nice-to-have, S. `handle.h` says every function is non-blocking, but `handle_close` can block in the last put.

## Drivers and platforms

- **DRV-1** correctness, M. Non-coherent DMA: the virtqueue's used-ring reads, and e1000e, AHCI and xHCI in both directions, have no `dma_sync_*`.
- **DRV-2** correctness, M. Device interrupts and every AArch64 exception run on the thread stack; separate interrupt stacks.
- **DRV-3** correctness, S. PCI enumeration turns memory decoding off while sizing BARs; a console write then stalls on Parallels' framebuffer.
- **DRV-4** usability, L. No AML interpreter: Parallels' platform AHCI (SATA), x86-64 ACPI S5 power-off and the DSDT-described RTC are unreachable.
- **DRV-5** usability, L. No device-tree discovery on AArch64 (ACPI only).
- **DRV-6** nice-to-have, M. PCI rescan and runtime PCI hot-removal beyond the test hooks; device power management (suspend/resume).
- **DRV-7** nice-to-have, M. Per-device bind/unbind exists only in debug builds.
- **DRV-8** nice-to-have, L. IOMMU variants: interrupt remapping, AMD-Vi, huge pages, IOVA cache, PASID/ATS, stream ids above 255, bridge aliases, the concatenated root for <= 42-bit SMMUs.
- **DRV-9** nice-to-have, M. No GICv3 pseudo-NMI on AArch64 (lockup sampling cannot interrupt a CPU with IRQs masked).
- **DRV-10** nice-to-have, M. Asynchronous errors: attribute through RAS records, and decide whether EL1 unmasks `PSTATE.A`.
- **DRV-11** nice-to-have, M. No CPU feature framework (`arch_cpu_has`) or errata table.
- **DRV-12** nice-to-have, S. ACPI: no host test or fuzzer of `acpi.c`, a second `acpi_init` duplicates entries, the XSDT/RSDT signature is not compared, GICC online-capable untested.
- **DRV-13** nice-to-have, S. Restart: a memory-space FADT reset register is not written; Ctrl-Alt-Del is not reported (`CAD_ON` changes nothing).
- **DRV-14** nice-to-have, S. `/dev/rtc0` has no alarms or update interrupts.
- **DRV-15** nice-to-have, S. QEMU `-accel hvf` stops on an MMIO access without ISV; use ISV-reporting accessors.
- **DRV-16** nice-to-have, L. A display interface for programs, cursor, 3D; Wi-Fi and Bluetooth; a desktop -- when hardware and a use case exist.

## Linux personality

- **LX-1** correctness, S. `waitpid(WCONTINUED)` can miss a continue when the child exits at once (`child_event_locked` tests EXITED first; flakes.md, two sightings).
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

## Lock validator and concurrency

- **LOCK-1** correctness, M. lockdep's writer under general NMI/#MC entry, and cross-CPU raw-lock wait cycles, are undefined (same-CPU re-entry fails stop).
- **LOCK-2** nice-to-have, M. No simultaneous global held-state snapshot across CPU spinlock and thread mutex stacks.
- **LOCK-3** nice-to-have, M. Kernel interrupt-entry and callback interleavings validated beyond the host models and the targeted IPI/NMI tests.
- **LOCK-4** nice-to-have, M. Measure contention, PI waits, graph-size sweeps, worst-case search latency and native-hardware cost.
- **LOCK-5** nice-to-have, S. Decide whether a spin wait on a stopped owner needs a diagnostic or recovery contract.
- **LOCK-6** nice-to-have, M. Kernel rwlock, only with a workload, ownership rules and a lockdep model.
- **LOCK-7** nice-to-have, M. Ticket or MCS spinlocks, only with measurements.
- **LOCK-8** nice-to-have, M. Completion edges not modelled: a signaller taking a mutex before `complete()`, one blocked in another wait, the network worker's barrier.

## Memory

- **MEM-1** correctness, M. `vm_map_phys` maps large blocks that `vm_unmap_phys` cannot tear down across 2 MiB; its 14 callers are not audited.
- **MEM-2** usability, S. The native ABI has no memfd or `shm_open`.
- **MEM-3** nice-to-have, S. `VM_KALLOC_POPULATE` fills every page under the kernel space spinlock with IRQs off.
- **MEM-4** nice-to-have, L. User huge pages.
- **MEM-5** nice-to-have, L. NUMA: topology (SRAT), node-aware allocation and placement.
- **MEM-6** nice-to-have, L. Swap, compression and deduplication.
- **MEM-7** nice-to-have, M. x86-64 PCID, once a test path exists (TCG has none).
- **MEM-8** nice-to-have, S. Thread guard page by map-then-protect instead of reserve-and-replace (no benefit shown).

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
- **PKG-3** usability, M. No multi-package transactions, interrupted-install recovery or per-package rollback.
- **PKG-4** usability, M. `cosmo-upgrade --rollback` keeps the packages `pkg upgrade` changed and `/etc` files the new release added.
- **PKG-5** usability, S. `cosmo-upgrade --fetch` runs the old binary's rules; a new directory (`lib/`) needs a second run.
- **PKG-6** usability, L. Libraries by name only: no versions, no provides/requires in the INDEX, no solver.
- **PKG-7** nice-to-have, M. SBOMs, dependency locking, build sandboxing, multi-architecture packages.
- **PKG-8** nice-to-have, M. `cosmo-upgrade`'s FAT writer refuses long names, a multi-sector `\cosmo` and FAT12/16.

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
- **TEST-16** nice-to-have, M. Paths proved by review only: retained-DMA sites, `nvme_probe` after `CC.EN`, the usb-hub worker, CF17's other sites, cosmofs chain cycles.
- **TEST-17** nice-to-have, S. Untested refusals and paths: `switch_root`, partitions removed with bios in flight, halt, `cosmo-upgrade` rerun after `--stop-after=set` and an interruption inside a FAT-chain write, virtio-net probe faults.

## Commands

- **USR-1** usability, S. The live image's native shell expands `$?` for a whole line before running it.
- **USR-2** usability, S. `/dev/log` is root-writable only (no native `chmod` for `syslogd`).
- **USR-3** usability, S. No `useradd`; a second user is made by editing `/etc/passwd`.
- **USR-4** usability, S. `/etc/fstab` cannot name `LABEL=` or `UUID=`.
- **USR-5** nice-to-have, S. No `xz` compressor; `syslogd` keeps no file or ring; `lsblk` has no `MAJ:MIN` or options; `blkid` knows no FAT or `PARTLABEL`.
- **USR-6** correctness, S. `cosmo-sh` hands the terminal to a foreground job as its last stage is spawned (COSMO_SPAWN_FOREGROUND), so the group holds every stage when it takes ^C; the earlier stages of a pipeline start in the background for the moment the spawns take, and one that checks the terminal at its first instruction can still stop itself. A start barrier (spawn stopped, continue the group) would close it (Codex on #424).

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
