# CosmoOS: where we stand, and where the docs have drifted (2026-10-28)

A read-only audit of main at f87a808 (the merge of PR #477; its tree is
identical to the audited `a5-fair` tip 13c0cd88), 1,870 commits since
2026-09-04, against the master prompt in `prompts/`
and the documentation in `docs/`. Nothing was built or booted; every
claim below was checked against the tree by grep or by reading the code,
and the high-severity ones were re-checked by hand a second time.

Scale: about 300,000 lines of C/asm/Nova/Python, 444 Markdown documents
(about 153,000 lines), about 480 in-kernel self-tests, 15 fuzz targets,
136 probes. Latest release: v1.0.14 (2026-10-26).

---

## 1. Summary

- **The system works and is well tested.** All of 1.0 (M1-M6) and 1.1
  (P1-P6, S1-S7) are ticked; 1.2's direction B (storage depth, B1-B5)
  and N1 are done; direction A (self-hosting) has A1 done and A2-A6 open.
  The code is unusually clean: no TODO/FIXME/XXX/HACK, no `#if 0`, no
  orphan `.c` files, every `CONFIG_` flag both defined and tested.
- **Against the original vision it is a working hybrid kernel that leans
  monolithic.** The weakest areas are native virtualization (VMX has
  never run), zero-copy I/O (not started), a capability model that sits
  beside ambient privileges and uids, mirror-only storage pools, and a
  thin native userland (BusyBox and musl do the work).
- **The docs are accurate at the deep layer and stale at the summary
  layer.** Each subsystem's `design.md` was usually updated when a unit
  landed. What was not updated: `docs/README.md`'s Layout table, each
  `architecture.md`'s file map and "non-responsibilities", the "Known
  limitations / Future extensibility" sections, count lines (`SYS_COUNT`,
  Linux table size, format versions), and the user guide's lists. About
  25 findings mislead about behaviour or ABI; about 60 are stale names
  or counts.

---

## 2. Progress against the original goals

The goal: a Unix-philosophy OS combining the best of FreeBSD and Linux,
on a hybrid (not micro, not unrestricted monolithic) kernel. The
scorecard below follows the master prompt §1 and the per-area sections.

| Area (master prompt) | Status | Evidence in code |
|---|---|---|
| Kernel objects and handles with rights (§10-11) | Done | `kernel/object/{object,handle}.c`; `HANDLE_RIGHT_*` used at 159 sites |
| Physical memory, buddy, slab, heap (§13, §16) | Done | `kernel/memory/{pmm,buddy,slab,kmalloc}.c` |
| Virtual memory, demand paging, CoW (§14) | Done | `kernel/memory/vmm.c`; no huge pages, no KASLR |
| Preemptive SMP scheduler (§20) | Done, capped | Per-CPU queues, migration, pull balancer, PI; a weighted fair share at the default level (PR #477, merged in f87a808, `kernel/scheduler/fair.nova`); `CONFIG_MAX_CPUS 64` |
| Epoch / RCU (§22) | Partial | Quiescence (`synchronize_quiesce`, `call_quiesce` in `kernel/core/quiesce.c`); no RCU-style read-side API |
| Loadable, signed modules (§23-24) | Done | `kernel/module/{modelf,modsig,ksym}.c`; 14 driver modules (`build/module.mk`); module ABI 11 |
| Drivers, DMA, IOMMU (§25-26) | Done in QEMU | VT-d and SMMUv3 (`drivers/iommu/`); no physical machine (roadmap C4/C5 open) |
| VirtIO, NVMe (§27) | Done | blk, net, console, rng, gpu; modern and legacy transports; NVMe per-CPU queues or one vector |
| Storage pool (§29) | Partial | `kernel-services/storage/pool.c`: mirror groups of up to 4 copies; no striping, RAID-Z, multiple pools or rebalancing |
| CoW FS, checksums, snapshots (§30-32) | Done | cosmofs (~27K lines): snapshots, clones, rollback, fsck/scrub repair, resilver, compression, encryption; no send/receive, no dedup |
| Networking and mbufs (§33-34, §36) | Done | IPv4/IPv6, TCP/UDP, firewall, NAT, per-CPU receive |
| Zero-copy I/O (§35) | **Not started** | Only shared `M_EXT` clusters inside the kernel; no sendfile/splice/user-mapped buffers (backlog NET-19) |
| VFS (§37) | Done | Mount namespaces, page cache, hard links, long names (1.2 A2) |
| Linux compatibility (§38-40) | Mostly done | 178 x86-64 / 147 AArch64 table entries, 175 / 144 real handlers; static, PIE and musl-dynamic run; glibc untested |
| Hypervisor (§41-43) | Partial | AMD SVM (QEMU TCG only); Intel VMX 1,172 lines **never executed**; AArch64 EL2 boots Linux guests; one vCPU per guest |
| Security model (§44) | Partial | Handle rights + privilege bits (`cred_has`, 58 sites) + uid checks (`vfs_permission`); verified boot, signed modules, user ASLR; no KASLR, Secure Boot or TPM; root filesystem not verified |
| Userland (§45) | Partial | BusyBox is the real userland; native coreutils is 14 tools; tcc on the image (A1) |
| libc (§46) | Partial | Native libc 6K lines, static only, no `math.h`; Linux programs use musl |
| Packages and ports (§47-48) | Partial | Signed `pkg`, network repository; ports not yet built on the target (A3) |
| Reproducible builds (§49) | Done | `make reproducible` in CI and in the release workflow |
| Testing (§57-61) | Strong | Self-tests, ASan/UBSan host tests, TSan models, litmus tests, fuzzers, fault injection, crash replay, probe/`--old` convention |

### FreeBSD and Linux lineage

| | Present | Absent |
|---|---|---|
| FreeBSD | mbufs and clusters; ports and signed pkg; boot environments (`cosmo-upgrade`); a ZFS-like CoW filesystem with snapshots, clones, mirrors and scrub; chroot plus process domains and mount/UTS namespaces (a partial jail) | kqueue, DTrace, Capsicum, RAID-Z, send/receive |
| Linux | The Linux ABI; epoll (ET, callbacks); futex; eventfd, timerfd, signalfd; memfd; SysV shm; AF_UNIX | cgroups, eBPF, inotify, `/sys`, PID and network namespaces, `unshare`: no containers |

### Micro, hybrid or monolithic

Lines in tracked `.c/.h/.S/.nova` files:

| Tree | Lines | Ring |
|---|---|---|
| `kernel/` | 106,452 | 0 |
| `kernel-services/` | 66,211 | 0 |
| `drivers/` | 15,328 | 0 |
| `compat/` | 7,426 | 0 |
| `userland/` + `libc/` + `pkg/` | 36,131 | 3 |

About 195K lines run in ring 0, of which about 52K are self-tests. Drivers
are separate signed modules but run in ring 0; VFS, filesystems, the
network stack and the hypervisor are linked into the kernel. The only
user-space servers are the VM device backends (`vblk`, `vnet` behind
`vmctl`), `syslogd` and `svc`. This satisfies the master prompt's
"hybrid, not a pure microkernel", but the "small trusted core" is not
small, and nothing outside ring 0 is isolated from it. If the
"micro" half of the original idea still matters, it is the largest
unstarted direction.

### The fifteen invariants (§62)

No CI check enforces the layering invariants; these are grep spot checks.

| # | Invariant | Verdict |
|---|---|---|
| 1 | Generic code is architecture-free | Small leak: `kernel/core/percpu.c:28-35` (`rdtsc` / `mrs` in `boot_canary`); `virtualization/vcpu.c:550`, `vmm.c:74` branch on `ARCH_AARCH64` |
| 2 | No kernel pointers to user space | Holds (`uaccess.S`, SMEP/SMAP, `uaccesstest.c`) |
| 3 | Scheduler independent of fs/net | Holds |
| 4 | VFS independent of any filesystem | Small leak: `kernel-services/vfs/fsctl.c:31` includes `kernel/cosmofs.h` |
| 5-9 | Net/NIC, block/driver, Linux/native ABI, pkg/kernel, VirtIO/hypervisor separations | Hold |
| 10 | Assembly isolated | Same `percpu.c` leak as #1 |
| 11 | Ownership and lifetime | kobject refcounts, quiescence, census tests |
| 12 | No hidden global locks | 89 `g_*` locks under lockdep; "each documented" not verified |
| 13 | Security not by convention | Verified boot and module signing; but `/etc`, which later boots trust, is writable by a root session (guide limitations) |
| 14 | Untrusted input validated | 15 fuzz targets (ELF, GPT, cosmofs, net, virtq, USB, Linux, pkg, …) |
| 15 | Optimisations keep correctness | Probe/`--old` convention, alternated benchmarks |

### Roadmap status

- 1.0 M1-M6: all ticked. 1.1 P1-P6, S1-S7: all ticked.
- 1.2 "Always", B1-B5, N1: done. A1: done. A2-A6: open. C1-C6: open
  (deferred).
- **Ordering question.** `roadmap-1.2.md:55` says the A milestones run
  "in order (A1 first; each needs the one before)". A2's box is unticked
  (its PRs #473/#474 merged without the acceptance test), yet A5 has used
  three of its five PRs (LX-14, #476 nice, #477 fair share) on the
  scheduler pieces it absorbs, with no OpenSSH or git work yet. That is
  within A5's written scope (`:154` absorbs LX-13 and LX-14), but it
  leaves A5 with two PRs for its headline acceptance test.

---

## 3. Documentation that no longer matches the code

Severity: **H** misleads about behaviour, format or ABI; **M** stale name,
count or missing entry; **L** cosmetic.

**Status.** The pull request that adds this report fixes every **H** row
below, plus the M rows in `docs/README.md`'s Layout table, SMP-1 and the
backlog's own rule for where follow-ups go. Two H rows are fixed by
recording rather than rewriting: `vfs/design.md`'s cosmofs summary is
replaced by a pointer to `cosmofs/design.md`, and the missing
`ENETUNREACH`/`ENOBUFS` names (a code change) are backlog NET-24. The
remaining M and L rows, the two stale code comments and a drift check
for CI are backlog DOC-3.

### 3.1 Top-level and governance documents

| Sev | Where | Says | Code / fact |
|---|---|---|---|
| H | `README.md:67`, `:129-141` | Status is 1.0; links the `v1.0.0` release; "units proposed next are in the 1.1 roadmap" | Latest tag v1.0.14; 1.1 complete; work is on roadmap 1.2 |
| H | `AGENTS.md:9` | 1.2 roadmap is "(direction B, storage depth)" | B is complete; direction A was chosen 2026-10-27 |
| H | `docs/README.md:29` | Scheduler is a "round-robin policy" | Weighted fair share at the default level (`kernel/scheduler/fair.c`), nice values |
| H | `docs/README.md:38` | `SYS_COUNT` 106 calls | 113 (`kernel/include/uapi/cosmo/syscall.h:182`) |
| H | `docs/README.md:50` | cosmofs "format version 4" | `CFS_VERSION 15`, `CFS_FORMAT_VERSION 11` (`cosmofs_format.h:18,24`) |
| M | `docs/README.md:49` | "twelve filesystem system calls" (11-22) | Also 81, 89-91, 100, 102, 107-112 |
| M | `docs/README.md:51` | Network "system calls 23-31" | Also 92, 97-99 (getsockopt, sendmsg/recvmsg, socketpair); AF_UNIX |
| M | `docs/README.md:57` | Virtualization: "two x86-64 implementations", "system calls 43-49" | AArch64 EL2 backend; ten calls incl. 79-80 |
| M | `docs/README.md:47-48` | NVMe "own MSI-X vector" per queue; VirtIO = blk/rng/console, "modern transport" | One-vector mode for Parallels; net and gpu drivers; legacy transport |
| M | `docs/README.md:25,54,30,58` | AArch64 "GICv2"; IPC = pipes + futex; timer = TSC/LAPIC/PIT; Linux row lacks epoll etc. | `gicv3.c` exists; `fifo.c`, `unix.c`, `shm.c`; AArch64 generic timer, `rtcdev.c`; epoll, fd objects, memfd, shm, AF_UNIX |
| M | `docs/README.md` Layout | — | No rows for `roadmap-1.2.md`, `backlog.md`, `releases/`, procfs |
| M | `docs/backlog.md:279` (SMP-1) | "Fixed-priority round-robin only: no fair … policy" | Fair share is on main since f87a808; #477 struck LX-13 but left SMP-1 unchanged |
| L | `docs/guide/limitations.md:3-6` | "What CosmoOS 1.0 does not do"; points to `plan.md` for remaining work | AGENTS.md: `plan.md` is historical, `backlog.md` is current |

### 3.2 Kernel core (memory, scheduler, process, syscall, smp, timer, quiesce)

| Sev | Where | Says | Code |
|---|---|---|---|
| H | `kernel/scheduler/architecture.md:51-55` | "fixed priority with round-robin … Fairness … are later policies" | Contradicts the same directory's `design.md:173-217` and invariant S35 |
| H | `kernel/scheduler/api.md:244-249` | Policy table is `{enqueue, dequeue, pick_next, tick, slice_new}` | Also `pick_migratable`, `charge`, `moved` (`sched.h:35-53`); fair-level preemption by `fair_should_preempt` |
| H | `kernel/process/invariants.md:39-44` (P5) | Two pages eager; at most `INITIAL_STRINGS_MAX` (300) strings; fails `-EINVAL` | `INITIAL_STACK_PAGES` 2 minimum, `INITIAL_STACK_PAGES_MAX` 40, `-E2BIG` (`process.c:224-276`); `INITIAL_STRINGS_MAX` does not exist |
| M | `quiesce/design.md:237,264`, `quiesce/invariants.md:75`, `timer/api.md:147` | `pcb_free_locked`; TCP timer callbacks take only `g_work_lock` | No `pcb_free_locked`; `tcp.c` has `pcb_put`, `pcb_retire_locked`, `pcb_kill_locked`, `pcb_end_locked`. This is the lock-order argument for `timer_cancel_sync`: re-check it, not only rename |
| M | `scheduler/api.md` | — | `thread_set_nice` (`thread.h:199`) and `kernel/include/kernel/fair.h` not documented anywhere |
| M | `process/architecture.md` | Files are `process.c`, `spawn.c`, `elf.c` | Also `nice.c/.nova`, `cputime.c/.nova`, `itimer.c`, `cpu_account.c`, `cred.c`, `signal.c`, `native_signal.c` |
| M | renamed identifiers | `SCHED_PRIO_IDLE`, `sched_enqueue`, `idle_thread_main`, `arch_percpu_setup`, `x86_cpu_identify/enable_features`, `x86_shootdown_handle`, `g_shootdown_acks`, `ap_started`, `console_panic_mode`, `timer_tick_hook`, `X86_VECTOR_TIMER`, `timer_tick_isr`, `timer_run_expired`, `buddy_free/alloc`, `pmm_free_range_core`, `phys_limit`, `user_thread_start`, `arch_user_regs_from_frame`, `signal_setup_frame`, `LX_TID_BASE` | Now `SCHED_PRIO_LOWEST`, `sched_enqueue_new`, `idle_main`, `arch_percpu_install`, `x86_cpu_init/enable`, `arch_mmu_shootdown_ipi_handler`, `g_shootdown_acked`, `g_ap_started`, `console_in_panic_mode`, `timer_set_tick_hook`, `timer_vector`, `tick_isr`, `run_expired`, `buddy_*_block`, `buddy_free_range`, `bootinfo_phys_limit`, `process_thread_start`, `arch_user_regs_from_syscall/_trap`, `signal_deliver`/`signal_frame`, literal `0x10000u` |

### 3.3 Kernel platform (arch, interrupt, diagnostics, security, module, device, tty, boot)

| Sev | Where | Says | Code |
|---|---|---|---|
| H | `kernel/module/api.md:12`, `module/design.md:28` | `COSMO_MODULE_ABI_VERSION` is 1 | 11 (`module.h:23`); the same `api.md:72` says 11 |
| H | `boot/design.md:5` | Boot protocol "version 9" | `COSMOBOOT_VERSION 10` (`cosmoboot.h:75`) |
| H | `boot/design.md:588-622` | No framebuffer or command line; x86 loader only; AArch64 loader and "protocol version 4" planned | All shipped (v6, v7, AArch64 loader) |
| H | `kernel/tty/architecture.md:74-82` | Raw mode, termios, job control, `^C` signals, PTYs, `/dev/tty` are non-responsibilities; "`^C` is dropped" | All implemented: `tty.c:203` (SIGINT), `:304` `tty_set_pgrp`, `:525-535` termios, `kernel/tty/pty.c` |
| H | `kernel/diagnostics/architecture.md:41-42,131` | "No log ring buffer yet"; dmesg, timestamps, lock diagnostics planned | `kernel/core/log.c:15-18` has the ring; lockdep exists. `log.c:4-6`'s own comment is also stale |
| M | `kernel/diagnostics/architecture.md:125` | "Five self-tests" | About 525 entries in `kernel/core/selftest.c` |
| M | `kernel/arch/design.md:177` | `ist[0]` = `g_df_stack` | `IST_COUNT` 4 per-CPU IST stacks (#DF, NMI, #MC) (`gdt.c:56-70`) |
| M | `kernel/device/design.md:493,566-568` | Locks `g_device_lock`, `g_pci_lock`; virtio-net, NVMe, AHCI, USB, hot-plug are future | `g_device_mutex`, no PCI registry lock (`g_hp_lock` only); all those drivers exist |
| M | `kernel/security/invariants.md:334` | `g_key` writers: `reseed_locked`, `ratchet_locked`, jitter | No `ratchet_locked` (inline in `random_get_bytes`); `random_init` also writes it |
| M | renamed identifiers | `x86_vector_alloc/free`, `pagecache_limit_pages`, `pagecache_reclaim_if_needed`, `arch_hv_vcpu_irq_taken`, `modelf_parse`, `test_modreloc`, `boot/uefi/cpu.c`, semihosting `SYS_EXIT` | `arch_vector_alloc/free`, `pagecache_limit()`, `pagecache_reclaim()`, `arch_hv_vcpu_irq_delivered/_waiting`, `modelf_validate`, only `test_reloc_aarch64`, `boot/uefi/arch/<arch>/cpu.c`, `SYS_EXIT_EXTENDED` |
| L | `kernel/interrupt/*.md` | — | GICv3 never mentioned (only under `kernel/arch/aarch64/`) |

### 3.4 Kernel services (VFS, cosmofs, network, virtualization)

| Sev | Where | Says | Code |
|---|---|---|---|
| H | `vfs/api.md:735`, `vfs/design.md:516` | `struct cosmo_stat` has `pad` / omits the field | `uint32_t dev` (mount id) is ABI (`syscall.h:677`) |
| H | `vfs/design.md:392-506` (cosmofs summary) | Superblock version 1; max 264 extents; `cfs_dirent` with fixed `name[48]`; `struct spool` with one device; files `cosmofs_txn.c`, `cosmofs_alloc.c`; commit only at sync/unmount | `CFS_MAX_EXTENTS` 4096; names up to 255 with continuation entries (v15); inode has `parent`, `csum_*`, `compress_algo`; spool has members, epoch, `state_seq`; those files don't exist; contradicts invariant V17. Recommend replacing the section with a pointer to `cosmofs/design.md`, which is current |
| H | `network/api.md:843-844` | `COSMO_ENETUNREACH` (101), `COSMO_ENOBUFS` (105) exported to user space | In neither `uapi/cosmo/syscall.h` nor `libc/include/errno.h`, though the kernel returns both |
| M | `vfs/api.md:714-716`; `virtualization/api.md:115` | `SYS_COUNT` "is 101 now"; "`SYS_COUNT` 50" | 113 |
| M | `virtualization/architecture.md:30-46`, `api.md:115-127` | Seven calls; `vmx.c` "(next backend)"; no AArch64 backend; table lacks 79-80 | Ten calls (`vm_raise_spi`, `vm_lower_spi`); `vmx.c`, `vmx_ept.c`, `vmx_run.S` exist; `kernel/arch/aarch64/hv*.c`; `vuart.c` unlisted |
| M | `virtualization/api.md:274` and `svm.c:775` | `x86/hvops.h` | Does not exist |
| M | `vfs/api.md:652` | Snapshots "(format version 3)"; "no new system call" | `/dev/fsctl` `SNAP_CREATE/ROLLBACK/CLONE` (`fsctl.h:47-52`), format 12 |
| M | `cosmofs/architecture.md:14-21`; `vfs/architecture.md:17-26` | File maps | Missing `cosmofs_{snap,clone,rollback,member,resilver,scrub,check,crypt,reach}.c`; `blkctl.c`, `fsctl.c`, `memdev.c`, `mountns.c` |
| M | `network/design.md:1869` | "Nothing is per CPU … (`g_rxq`)" | `g_rxq` gone; per-CPU `netrx/N` workers |
| L | renamed identifiers | `arp_lock`/`udp_lock`, `mbuf_cache`/`mcluster_cache`, `tcp_attach_socket`, `vfs_walk`, `pool_state_seq`, `DL_COPY`, `tap_dev_activate`, `mss_index`; `cosmofs_format.h:2-7` says "(version 2)" | `g_lock`, `g_mbuf_cache`/`g_cluster_cache`, `state_seq`; the others don't exist |

### 3.5 Drivers and the Linux personality

| Sev | Where | Says | Code |
|---|---|---|---|
| H | `compat/linux/api.md:156-158` | 129 entries, 123 translated, 6 `-ENOSYS` | 178 (x86-64) / 147 (AArch64); 2 `-ENOSYS` (`rseq`, `clone3`), 1 returns 0 (`set_robust_list`) |
| H | `compat/linux/architecture.md:93-94` | "87 calls; 13 more … refused" | As above |
| H | `compat/linux/api.md` | — | About 50 implemented calls have no row: epoll_*, eventfd*, signalfd*, timerfd_*, memfd_create, ftruncate, shm*, statx, chown/chmod family, utimensat, chroot, mount, umount2, reboot, settimeofday, clock_settime, prlimit64, rlimits, statfs, set*uid/gid, groups |
| H | `compat/linux/design.md:247,220,192,166`; `api.md:297-298` | `epoll_*`, `sysinfo`, `sched_getaffinity`, non-`AT_FDCWD` dirfd, `mremap` return `-ENOSYS` | All implemented (`lx_mremap` at `syscalls.c:4678`; `at_base()` at `:312`) |
| H | `drivers/virtio/invariants.md:3-7` (V1), `architecture.md:47-48`, `api.md:26-28` | Legacy devices refused; `VERSION_1` always negotiated | Legacy transport (`virtio.c:109-115`, `virtio_pci.c:64-72`); `design.md:137` documents it |
| M | `compat/linux/design.md:44-50` | `struct linux_state` has `clear_child_tid`, `sigmask`, `act[64]`, `altstack` | Holds `brk_start/brk`, `sigtramp`, `unknown_syscalls`, `wx_logged`, `shm_attaches`, `shm_lock` (`syscalls.c:74-81`) |
| M | `compat/linux/architecture.md:75,127` | File mappings refused with `ENODEV`; native has no futex | File and shared mappings supported (`syscalls.c:2168-2207`); `SYS_futex_*` 83, 84, 94 |
| M | `compat/linux/api.md:230`, `design.md:304` | `linux_thread_exit`, `signal_setup_frame` | `thread_clear_tid`, `linux_signal_frame` |
| M | `drivers/virtio/architecture.md:14-24` | Four device drivers | Also `virtio_gpu.ko` (`build/module.mk`) |
| M | `drivers/nvme/invariants.md:43-48` (M4), `architecture.md` | Every queue's vector routed to its CPU | One-vector mode (`nvme.c:112, 909-915`) is an exception the invariant doesn't state |
| L | `drivers/nvme/api.md:52,58`; `README.md:44` | Banner and error text; VirtIO list | Banner has interrupt-mode field; message is "neither MSI-X nor MSI"; GPU missing |

### 3.6 Userland, libc, build, verification, user guide

| Sev | Where | Says | Code |
|---|---|---|---|
| H | `guide/limitations.md:52`, `guide/shell.md:127-128` | "no hard links (`ln` without `-s`)" | `SYS_link` 107, `lx_link`, cosmofs and ramfs `.link`, BusyBox `CONFIG_LN` (commits 0ecb550c, 3beab977 on main) |
| M | `guide/shell.md:80-92` | "The full list" of applets and `/sbin` | Missing `halt hwclock mount nice ntpd reboot renice umount`; `/sbin` missing `cosmo-upgrade cosmo-verify lsblk blkid mkfs.cosmofs cosmo-snap fsck.cosmofs cosmo-pool cosmo-key` |
| M | `userland/api.md:105-122` | `/sbin` table | 11 of 25 programs missing (`hostname svc fsctl cosmo-verify netcfg ip syslogd cosmo-snap fsck.cosmofs cosmo-pool cosmo-key`) |
| M | `libc/api.md` | — | 38 of 221 declared functions absent, no `termios.h` section (chmod/chown/chroot/umask/fsync/lstat/symlink/readlink/link, uid/gid set, hostname, tc*, `spawnve_*`) |
| M | `development.md:60-90`, `build/api.md` | Target tables | Missing `test-smp2`, `test-chaos`, `test-harness-retry` (AGENTS.md matrix), `test-install`, `test-upgrade`, `test-selfhost`, `test-modcycle`, `test-fsck`, `test-mirror`, `test-snapshot`, `nova`, `nova-check`, `check-docs`, `litmus`, `release-set`, `analysis-gate`. No phantom targets |
| M | `build/testing.md:189-192` | ELF loader has no fuzz harness "once `tools/` exists" | `tests/fuzz/fuzz_elf.c`, `fuzz_modelf.c` |
| L | `development.md:81`, `verification/architecture.md:14-19` | Six fuzz families | 15 targets |
| L | `development.md:30-32` | Setup script installs "the same set CI installs" | `ci/Dockerfile` also installs `bzip2 xz-utils openssh-client` |
| L | paths | `libc/crt0.S`, `libc/src/crt0.S`, `tests/host/shim_fs.c`, `tests/hv/*.S` "seven" | `libc/src/arch/<arch>/crt0.S`, `tests/fuzz/shim_fs.c`, 7 only on x86-64 (~30 on AArch64) |
| L | `ports/README.md:8-9`; `guide/limitations.md:53-55` | Port list; fixed `/proc` files | Omits `demo-c`, `demo-d`, `libdemo`, the image-only ports; `/proc/net`, `/proc/self` |
| L | guide | — | Never mentions `/usr/bin/cc` (tcc, A1) |

### Checked and consistent

These match the code: CI job names cited in docs, the Nova file list and
pin (0.1.1), macOS build docs, `pkg` subcommands, lockdep limits, every
`SYS_*` in `syscall/api.md`, io syscall numbers, the verified-boot
manifest, `cosmofs/{design,api,invariants}.md`, and the snapshots, mirrors,
encrypted-root, checking and networking guide pages.

---

## 4. Half-finished items

### 4.1 In the code

| Item | Evidence | Judgement |
|---|---|---|
| Intel VMX backend never executed | `kernel/arch/x86_64/vmx.c:12-17` (1,172 lines, "inert until … Intel hardware"); only `tests/host/test_vmx.c` | Risk: large untested code (VIRT-1, roadmap C1) |
| x86 hypervisor exits | MOV-DR and RDPMC exits enabled, not handled; WBINVD a no-op (VIRT-2) | Real gap (from backlog; handlers not read) |
| Fair share leaves residue | Nice is per process, not per thread; balancer counts threads, not weights (backlog LX-13 "Left") | Real gap |
| Dead code | `syscall_count()`, `syscall_unknown_count()` (`kernel/syscall/syscall.c:150,155`) have no callers; `p->syscalls++` (`:88`) is a plain shared increment (OBS-1) | Delete; small correctness risk |
| Misleading comment | `kernel-services/vfs/ramfs.c:772`: sticky bit "recorded, not yet enforced" | `sticky_denies()` enforces it (`vfs.c:2730`); fix the comment |
| termios `VTIME` | `uapi/cosmo/syscall.h:506`: "accepted and not implemented" | Real gap, documented |
| Common Linux calls absent | `waitid`, `sendfile`, `splice`, `copy_file_range`, `fallocate`, `truncate`, `flock`, `preadv/pwritev`, `inotify_*`, POSIX timers, `rt_sigtimedwait`, `close_range`, `faccessat2`, `openat2`, `pidfd_open`, `*xattr`, `mlock`, `capget/capset`, `unshare` go to `lx_unknown` | Real gap; will bite A4-A6 (curl, OpenSSH, git, Python) |
| `setsockopt` | Returns `-ENOPROTOOPT` for everything (LX-8) | Real gap |
| No libm | No `math.h` in `libc/include/`; `Todo.md` plan "not started (2026-10-06)" | Real gap, outside the roadmap; Python (A6) will need it |
| Six stale report probes | accept-order, epollet, zero-window, cond-phase, net-rx-dup, cosmofs-metadata-csum: anchors gone, `apply` fails | Dead tooling; re-anchor or mark retired |
| Open correctness items | LX-24 (`yes \| head` never exits), LOCK-9 (PI donation-cycle warning on release boots), LOCK-10 (lockdep class table only grows), CFS-4 (rekey leaves key block in open txn), CFS-32 residue (→ CFS-39), NET-23, TEST-18/21 flakes | Tracked in backlog |

### 4.2 In the working tree and repository

| Item | State | Judgement |
|---|---|---|
| `../CosmoOS-irqp` worktree (`irqpoll-investigate`) | 84 uncommitted lines in `irqpoll.c`, `irqpolltest.c`, `selftest.c`, `irqpoll.h` | Decide: land, backlog, or discard |
| `../CosmoOS-a2x` worktree (`a2-experiment`) | Untracked `tools/a2x/` | Experiment; keep or delete |
| `always-tests-v1`, `always-tests-v2` | 2 commits each not on main (by `git cherry`) | Check if superseded, then delete |
| `origin/repo`, `origin/usr8-pkg11-build7` | Unmerged remote branches (2026-10-26 Pages index re-sign; 2026-10-24) | Likely obsolete; confirm and delete |
| Eight worktrees under `../CosmoOS-*` | Six on merged branches, clean | Remove (`git worktree remove`) |
| `a5-fair` branch | Merged as PR #477 (f87a808) | Delete the branch |
| `Todo.md` (untracked) | libm plan, plus ThorVG blockers (C++ runtime, setjmp/longjmp, pthreads shim, several libc functions) | Move into the backlog or delete |
| `prompts/prompt-oct-5.md`, `prompts/promt-oct-8.md` (untracked, one misspelled) | Old task prompts for finished work; they say to use `docs/plan.md` as the ledger | Risk: AGENTS.md tells agents to read `prompts/` as governing rules. Move out of `prompts/` |

---

## 5. Recommendations

Items 1 and 2 are done by the pull request that adds this report; 3 is
backlog DOC-3.

1. **One documentation-only PR for the summary layer** (exempt from the
   validation matrix under AGENTS.md): `README.md` Status, `AGENTS.md`
   scope line, `docs/README.md` Layout table, SMP-1 in the backlog, the
   hard-links lines in the guide. These are what a newcomer reads first.
2. **A second docs PR per area for the H rows** in §3.2-3.5: module ABI,
   boot protocol, tty and diagnostics non-responsibilities, the cosmofs
   summary in `vfs/design.md` (replace with a link), `cosmo_stat.dev`,
   the Linux counts and `-ENOSYS` lists, virtio V1, process P5. Re-derive
   the `pcb_free_locked` lock-order argument rather than renaming.
3. **Make the drift checkable.** `scripts/check-docs.py` only checks that
   five files exist. The auditors' token scripts (backticked identifiers
   grepped against the tree) found most of this in minutes; a CI version
   with an allow-list for spec and Linux names would stop it from
   returning. Count lines (`SYS_COUNT`, table sizes, versions) can be
   generated or checked against the headers.
4. **Clean up the half-finished items outside main** (§4.2) and move
   `Todo.md` and the stray prompts out of the governing directory.
5. **For the vision:** the biggest gaps are not documentation. They are
   VMX on hardware (C1), zero-copy I/O (NET-19), the capability model
   versus ambient privilege, pools beyond mirrors, and moving anything
   out of ring 0. Per AGENTS.md these wait for the owner to choose them
   as roadmap units.
