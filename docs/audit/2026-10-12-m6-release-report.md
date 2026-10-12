# Roadmap M6: release — 2026-10-12

Milestone M6 of the [1.0 roadmap](../roadmap-1.0.md): the tagged release
images for x86-64 and AArch64 rebuild bit-for-bit from the tag, boot, and
pass the M1-M5 acceptance tests in CI; the docs and user guide exist.
Base: main `c2f6e132` (M1-M5 done, CI green). Decisions 1-9 of the task
are the owner's and were not reopened; this report records how each was
built.

## Plan as worked

| PR | Content |
|---|---|
| 1 | Decision 1 (the native signal frame and the WXN test's cache maintenance behind arch functions), decision 2 (no stack-frame exemptions, the stack high-water check), decision 3 (the package repository on the installed system). |
| 2 | Decision 7 (the CI toolchain image on GHCR, pinned by digest) and decision 8's `make release` with the full reproducibility check. |
| 3 | Decision 4 (the five-file docs for ACPI, objects, syscalls and cosmofs) and the docs CI check. |
| 4 | Decisions 5 and 6 (the user guide with its command job, README, the plan's preface, the inventory). |
| 5 | The release workflow, the release key, the `v1.0.0-rc1` dry run, M6 ticked, then `v1.0.0`. |

## PR 1 (as built): architecture cleanups, stack frames, packages

### Decision 1: architecture code behind arch interfaces

| Was | Now |
|---|---|
| `kernel/process/native_signal.c` had an `#if defined(ARCH_X86_64)` / `#elif defined(ARCH_AARCH64)` / `#else` block with two copies of `native_signal_frame` and of `sigreturn_frame_addr` | One generic `native_signal_frame`: fill the frame, `arch_sigframe_place(r, size)`, `copy_to_user`, `arch_sigframe_enter(r, frame, handler, restorer, sig, info)`; `sys_sigreturn` calls `arch_sigreturn_frame(r)`. The three are declared in `kernel/include/arch/user.h` and defined in `kernel/arch/x86_64/user.c` and `kernel/arch/aarch64/user.c`. The file has no architecture conditional. |
| `kernel/core/main.c` (`CRASH_TEST == 2`) issued `dc cvau; dsb ish; ic ivau; dsb ish; isb` inline | `arch_mmu_sync_icache_kernel(phys_to_virt(pa), PAGE_SIZE)`, the existing arch function for bytes written through the direct map that a mapping executes; on AArch64 it cleans by that alias and invalidates the whole instruction cache. `main.c` has no `asm`. |

Behaviour is unchanged: the bytes written to the user stack and the
registers set are the same, in the same order (frame, then the
restorer slot or frame record, then the registers). Proof: the existing
tests -- the native signal sections of the user-mode suite, `lxsig`
(both personalities share the delivery core), and `make test-wxn` on
AArch64 -- pass on both architectures (validation below).

The Linux personality's frames (`compat/linux/signal.c`) keep their
per-architecture code: their layouts are Linux's two ABIs, not one
frame with per-architecture placement, and the roadmap names only
`native_signal.c` and `main.c`.

### Decision 2: stack frames

The baseline (`scripts/frame-baseline.txt`) listed **32** functions, not
39 (the file had 39 lines, seven of them the header comment). All 32 are
rewritten below 2048 bytes; **none is exempt**. Largest frame in the tree
afterwards, over x86-64 and AArch64, debug and release (`clang
-fstack-usage` builds of the kernel and modules): 1904 bytes
(`selftest_net_firewall`, AArch64 release), which was already under the
limit and not touched.

| Function(s) | Was (bytes) | How |
|---|---|---|
| `sys_open`, `do_open`, `lx_openat` | 3136, 3160, 3184 | the three 1 KiB path buffers in one `kmalloc(3 * VFS_PATH_MAX)` |
| `sys_rename`, `sys_symlink`, `lx_rename`, `lx_renameat`, `symlink_common`, `lx_symlinkat` | 2072-2120 | the two path buffers in one `kmalloc(2 * VFS_PATH_MAX)` (as `sys_readlink` already did) |
| `process_chdir` (`chdir_inner` inlined) | 2128 | base and new path in one `kmalloc` |
| `process_spawn` | 3648 | the cwd base and name in one `kmalloc`, only when a cwd is given |
| `ramfs_populate_boot` | 2160 | `ensure_parents` cuts the path in place instead of copying it |
| `cosmofs_check` | 2336 | the 1.6 KiB report `kzalloc`ed |
| `nvme_probe` | 4336 | the 4 KiB namespace list `kmalloc`ed |
| `nvme_submit` | 2784 | the PRP entries are written straight into the slot's PRP list under the queue lock (`prp_walk`), so the 1 KiB copy on the stack is gone; the segment mapping, and the count that refuses an oversized request, stay before the lock |
| `linux_signal_frame`, `lx_rt_sigreturn` (AArch64) | 5344, 5456 | the 4.7 KiB frame is written to user memory in pieces (zeroed, then each part at its offset) and read back in pieces; `probe_user` reads the whole `ucontext` first so a frame that faulted before still faults |
| 15 self-tests and helpers (`selftest_cosmofs_crash`, `selftest_net_mbuf`, `selftest_el2_virtq_device`, `selftest_net_nonblock`, `selftest_net_icmp_limit`, `udp_roundtrip`, `selftest_net_hoststate`, `selftest_net_hostinput`, `selftest_cosmofs_orphan_idempotent`, `selftest_linux_elf`, `selftest_nvme`/`nvme_run_workers`, `selftest_net_output`, `selftest_quiesce_stress`, `selftest_smp_parallel`) | 2152-8592 | their large buffers, statistics snapshots and per-CPU arrays became `static` (the runner runs one test at a time; every thread using them is joined before return) |

No rewrite changes an ABI or a result. The heap allocations add an
`-ENOMEM` return to calls that already allocate on the same path (the
open, rename and spawn paths allocate vnodes, files and processes).

Found on the way, fixed because it is in the release image:
`process_spawn` returned directly when the `cwd` lookup failed or named
a non-directory, after creating the child's mount and uts namespaces,
which leaked them. Both paths now go through `out_cwd`.

`scripts/frame-baseline.txt` now holds only its header: the M3 history,
and the form an exemption must take (file, function, frame, deepest
measured stack use, reason). `scripts/check-frame-baseline.py` refuses a
line without those fields.

**The high-water check.** Debug builds paint every thread stack
`thread_prepare` allocates (`STACK_PAINT`), measure each thread in
`thread_put` before its stack is freed and every live thread at the end
of the boot (`thread_report_stack_use`, after the self-tests, the
user-mode suite and the shell session), and log the deepest five thread
names. A thread over 75 % (12288 bytes) is a `WARN` line the boot
harness forbids; the summary line with `0 over 75%` is required in every
debug boot (`tests/boot/run_boot_test.py`, `STACK_SUMMARY_MARKER`).
Invariant **S32** (`docs/kernel/scheduler/invariants.md`); design in
`docs/kernel/scheduler/design.md`, "Stack use".

| Debug boot (`make test`, 4 CPUs) | x86-64 | AArch64 |
|---|---|---|
| deepest stack | 8128 of 16384 bytes (49 %), `fsctl` | 8672 (52 %), `fsctl` |
| next | `init` 8128, `mount` 6944, `rm` 6768, `cosmo-sh` 6536 | `init` 8672, `mount` 7392, `svc` 7232, `rm` 7120 |
| stacks measured | 4659 | 4716 |
| over 75 % | 0 | 0 |

The check is not vacuous: with the threshold lowered to 25 % in a
throwaway worktree, the x86-64 debug boot failed on the forbidden
warning (`usbs-racer` 4616 bytes, `blk-bench` 5160) and on the missing
`0 over 75%` summary.

### Found writing the ACPI docs (PR 3), fixed here

- `parse_madt` (`drivers/acpi/acpi.c`) accepted a GICC entry when flag
  bit 1 was set, the LAPIC entry's "online capable"; in a GICC entry bit
  1 is the performance interrupt's trigger mode and "online capable" is
  bit 3 (ACPI 6.5, 5.2.12.14). A disabled GICC entry with an
  edge-triggered performance interrupt would have been counted as a
  CPU. Now `MADT_GICC_ONLINE_CAPABLE`. QEMU marks every GICC entry
  enabled, so no test here can reach the difference (no hardware matrix
  in 1.0); recorded in the inventory.
- `parse_madt` read the MADT's fixed fields without checking the table
  holds them; a shorter MADT is now ignored with a warning.
- `setup_ecam` (`drivers/pci/pci.c`) computed the MCFG entry count as
  `length - sizeof(header)` without checking the length first, which
  wraps for a short table; such a table is now ignored.

### Decision 3: packages on an installed system

`cosmo-install` creates `/var/lib/pkg`, copies the image's repository
(`/boot/repo`, the boot archive's `repo/`) to `/var/lib/pkg/repo`, and
writes the installed `/etc/pkg/repos.conf` as that one line; an image
without `/boot/repo` is an install failure. The keys that verify the
repository travel with `/etc` (`/etc/pkg/keys`). `test-install`'s install
stage checks the installed `repos.conf` and the repository's `INDEX`;
its persist stage (the second boot of the installed disk) runs `pkg
update && pkg install hello && hello && pkg list` and requires exit 0,
`hello, world (hello 1.1)` and `hello 1.1 prints a greeting`. Inventory
line struck. Docs: `docs/userland/design.md` ("The installer"),
`docs/userland/testing.md`, `docs/pkg/design.md`, `api.md`,
`invariants.md`.

### Validation (PR 1)

The matrix of `AGENTS.md`, local (macOS host, QEMU at default priority),
both architectures: see the PR description for each step's result.

## PR 2 (as built): the CI image and the reproducible release set

### Decision 7: the CI container

| Was | Now |
|---|---|
| every job pulled `debian:trixie` (`debian:sid` for litmus) by tag from ECR Public's mirror and ran `apt-get install` | `ci/Dockerfile` with two targets, `toolchain` (trixie) and `litmus` (sid + herdtools7), both bases pinned by digest (`debian:trixie@sha256:913f6706...`, `debian:sid@sha256:c53e0503...`, resolved from ECR Public on 2026-10-12) |
| | job `image` runs `scripts/ci-image.sh ghcr.io/vkuttyp/cosmoos-ci`: the tag is the target and the Dockerfile's SHA-256 prefix; it is built and pushed only when that tag is not in the registry, and the job outputs `ghcr.io/vkuttyp/cosmoos-ci@sha256:...` for each target |
| | every other job `needs: image` and runs in `${{ needs.image.outputs.toolchain }}` (litmus: `litmus`), pulled from this repository's GitHub Container Registry with the job's token; workflow permission `packages: read`, `packages: write` for `image` alone |

The rate-limit failures this ends happened before any code ran: Docker
Hub (run 37991912859) and ECR Public ("Data limit exceeded", M4). The
packages inside an image are fixed by its digest, not by the Dockerfile,
so each image records them in `/usr/share/cosmoos-ci/packages.txt`, and
the release notes copy the compiler and QEMU versions from there.

### Decision 8, first half: `make release`

`make release` runs `release-set` with `BUILD=release`, and
`scripts/mkrelease.py` copies the image, loader, kernel, archive,
command line and manifest into `out/release/<arch>/` (refusing a
directory that holds anything but an earlier set) as
`cosmoos-<arch>.img`, `cosmoos-<arch>-BOOTX64.EFI` (`BOOTAA64.EFI`),
`cosmoos-<arch>-kernel.elf`, `-boot.tar`, `-cmdline`, `-manifest`, with
`SHA256SUMS` in `sha256sum -c` form. Signed with what `SIGNING` selects;
the release workflow (PR 5) passes the release key.

`make reproducible` now also builds `image` and `release-set` in both
trees and compares `cmdline`, `manifest`, `cosmoos.img` and the set's
`SHA256SUMS`; every file is required in both trees (release builds make
the test modules and the BusyBox testsuite archive too). The FAT image
was already deterministic: `mkimage.sh` gives `mformat` a fixed serial,
and two runs on the same inputs a minute apart, with the kernel's mtime
changed between them, gave identical bytes (mtools 4.0.49). CI's new
`release-set` job runs `make reproducible BUILD=release` and uploads one
`make release` per architecture.
## PR 3 (as built): the five-file docs and their check

| Directory | Added | Corrected |
|---|---|---|
| `docs/drivers/acpi/` | `design.md`, `api.md`, `invariants.md` (AC1-AC8), `testing.md` | `architecture.md`: the read-only mapping fallback, GICR/ITS entries, which overflows are silent, the full panic list |
| `docs/kernel/object/` | `design.md`, `invariants.md` (OB1-OB12, with an index of the object rules other subsystems own), `testing.md` | `architecture.md` (`process_lookup` uses `kobject_tryget`; the `*_of_err` accessors), `api.md` (console write is `tty_write`; `read_undo`, `handles`, `watchers`, the NOFILE `limit`) |
| `docs/kernel/syscall/` | `design.md`, `invariants.md` (SC1-SC14), `testing.md` | `api.md`, `architecture.md`: `SYS_COUNT` 105 (said 101/97), thread calls 82-87, the 21-word x86-64 frame and IRETQ exit, the AArch64 `SVC` path, exception-table user access, the dispatcher's filter/signal/restart work, `PRIV_MOUNT` |
| `docs/kernel-services/filesystem/cosmofs/` | `api.md`, `invariants.md` (CF1-CF15, with an index of the V/F/S rules), `testing.md` (71 registered tests) | `architecture.md`: `CFS_VERSION` 11, seventeen corruptions |
| `docs/drivers/usb/`, `e1000e/`, `ahci/` | `architecture.md` each (they had the other four; `docs/README.md` listed the last two as `drivers/network/` and `drivers/storage/`, which do not exist) | |

The docs were written from the code by reading it; every claim names a
file, function or constant. Code findings that are not release defects
are inventory lines (section 3: ACPI, cosmofs, syscalls and handles).
The two that were defects in the release image (the GICC flag bit and
the unchecked MADT/MCFG lengths) are fixed in PR 1.

**The check.** `scripts/check-docs.py` (`make check-docs`, CI job
`docs`) reads the Layout table of `docs/README.md` and fails when a
listed subsystem directory is missing, or lacks or has an empty one of
the five files; `history/`, `testing/` and `audit/` are records. It
checks 37 directories. With `docs/kernel/object/invariants.md` moved
away it fails naming the file.

## PR 4 (as built): the user guide, README, plan

### Decision 5: the guide

`docs/guide/`: `README.md` (the index, and how the guide is tested),
`installing.md` (QEMU and firmware on Linux and macOS, checking the
images with `SHA256SUMS`, booting the live image with a blank disk on
both architectures, `cosmo-install`, the disk layout, verified boot and
the `UNVERIFIED BOOT` banner), `shell.md` (the installed system, BusyBox
`ash`, the applet list, job control, what is missing), `networking.md`
(DHCP, `netcfg show`, `/etc/resolv.conf`, `wget` by address and by name,
why HTTP only), `ssh.md` (a key, dropbear, root by key only, a command
and a pty session), `packages.md` (`pkg` against `/var/lib/pkg/repo`),
`privileges.md` (the six privileges, `priv`, an ssh login's `net cred`),
`limitations.md` (from the inventory, the roadmap's out-of-scope list and
the M1-M5 reports).

**The command job.** `tests/guide/run_guide.py` runs every ```` ```sh ````
and ```` ```console ```` block of the guide, in the index's order, in one
directory that starts with the release set: each block is preceded by a
`<!-- test: ... -->` line saying how it runs (a host command in one
persistent bash, an interactive session -- QEMU or ssh -- waited on for
its prompt, or a console block typed at the current session's prompt
with its output lines required in order, `...` matching what varies); a
command block without one is an error. Blocks name the host (`linux`,
`macos`) and architecture they apply to when the steps differ. CI: job
`guide` (Linux, both architectures, in the toolchain image, after `make
release`) on every run; job `guide-macos` (macOS 14 runner, Homebrew's
QEMU, the `release-set` job's images) on pushes to main; the release
workflow runs both against the signed images. `ci/Dockerfile` gains
`sudo`, because the Linux steps use it as written.

Writing the guide against the real system found what the first draft
had wrong, each now as the system prints it: `uname` says `Linux` (BusyBox
runs under the Linux personality); the blank disk is `vda` on x86-64 and
`vdb` on AArch64 (the live disk is SATA `ahci0p0` on `q35`, the first
virtio disk on `virt`); the release kernel logs every process start and
exit on the console; `priv` prints the set without a prefix; `mount`'s
syntax is `mount SOURCE TARGET FSTYPE`; an ssh session's prompt is `~ # `.
The runner itself needed host commands to read `/dev/null` (ssh read the
runner's next lines) and sessions to nest (ssh while QEMU runs).

Local results (macOS 27 host, Homebrew QEMU 11.1.2, developer-signed
release builds of this branch): `--host macos --arch aarch64` PASS (12
host blocks, 7 sessions, 20 console blocks); `--host macos --arch x86_64`
PASS (11, 7, 20). The Linux blocks run in CI.

### Decision 6: README, plan, inventory

`README.md`: Status says what 1.0 is and links the guide, the roadmap,
the history and the reports; "What exists" opens with the M1-M5 system
(installer and cosmofs root, fork/vfork/execve, BusyBox `ash`, PTYs and
job control, DHCP and DNS, dropbear, verified boot, ASLR, privileges,
randomness); a "Not in 1.0" section points at the guide's limitations and
`docs/plan.md`. `docs/plan.md` is retitled "After 1.0" with a preface
saying so. Inventory lines closed by M1-M6 are struck (user-space ASLR,
Linux `execve`, ash job control, the installed repository, the native
shell's replacement on installed systems). `docs/audit/README.md` lists
the M3-M6 reports; `docs/README.md` lists `guide/`.

## PR 5 (as built): the release workflow, the dry run, the tag

`.github/workflows/release.yml`, on a tag `v*` (docs/development.md,
"Releases"): `image`; `build` (each architecture twice, copies `a` and
`b`, separate jobs and so separate fresh containers, with the release
key: `scripts/release-key.sh` writes the secret `COSMOOS_RELEASE_KEY` to
a 0600 file and refuses one whose public half is not
`tools/keys/release-2026.pub`); `reproducible`
(`scripts/compare-release-sets.sh`, file by file); `acceptance` (each
architecture × `test-entropy`, `test-install`, `test-busybox`,
`test-net-login`, `test-security` and the guide, each on a build whose
`SHA256SUMS` must equal the published set's); `guide-macos`; `publish`
(`scripts/release-notes.py`, `gh release create`, a pre-release for a
tag with a hyphen). `ci/Dockerfile` gains `gh`.

**The release key.** Generated by the owner; public half
`tools/keys/release-2026.pub`, key id `36ae7bb4947896ab`, public key
`d2c778bcb38775790b6b6e843631bd2e76d6a29ec52296ece6e2d67c1bef41f8`. Dev
builds trust it beside their per-machine key (`TRUSTED_RELEASE_PUBS`);
the kernel's ring is regenerated when it changes (checked: the debug
build after the key landed rebuilt `keyring_builtin.c` with
`release-2026`).

### The dry run: `v1.0.0-rc1`

Tagged on this PR's branch head before review (run 38278497559), every
job green the first time:

| Job | x86-64 | AArch64 |
|---|---|---|
| build a / build b (release key) | 213 s / 211 s | 208 s / 214 s |
| two builds compared | identical, 7 files; image `bbfd4105...80186b4` | identical, 7 files; image `dec2fc7a...23a4236` |
| test-entropy | pass 251 s | pass 268 s |
| test-install (with `pkg install hello`) | pass 190 s | pass 268 s |
| test-busybox | pass 201 s | pass 291 s |
| test-net-login | pass 231 s | pass 253 s |
| test-security | pass 289 s | pass 317 s |
| guide, Linux | PASS, 11 host blocks, 7 sessions, 20 console blocks | PASS, 12/7/20 |
| guide, macOS | PASS 77 s | PASS 90 s |
| GitHub release | pre-release `v1.0.0-rc1`, 13 files | |

The notes it wrote carry the release key and its id, the toolchain image
`ghcr.io/vkuttyp/cosmoos-ci@sha256:f6aee147...be385` and litmus image
`@sha256:c8b16f9c...5a5a9`, clang/lld/llvm 19, QEMU 10.0.13, OVMF and
AAVMF 2025.02, mtools 4.0.48, the four pinned third-party digests, and
the limitations page.

### After review: `v1.0.0-rc2`, `-rc3`, `-rc4`

Review of #372-#374 changed the guide (a wait for dropbear before the
first ssh, `(no output)`), the runner and `mkrelease.py`/`ci-image.sh`,
so the dry run was repeated on the final head:

- **rc2** was cancelled and its tag deleted before it finished: CI's
  Linux AArch64 guide job on #374 had just shown a review change wrong.
  Restricting exit status 33 to x86-64 QEMU sessions broke AArch64,
  whose semihosting exit reports a clean power-off as 33 too
  (`kernel/arch/aarch64/shutdown.c`); fixed, and every QEMU session
  accepts 0 or 33 again (ssh must end 0).
- **rc3** (run 38280733354): 19 of 20 jobs green; `guide (macos,
  aarch64)` failed on the firmware-handover stall, its sixth sighting
  and the first on AArch64 or macOS (`docs/testing/flakes.md`): edk2
  never handed over to the loader. The guide runner was the one QEMU
  launcher without the harnesses' single retry; its QEMU sessions now use
  `tests/boot/firmware_stall.py` too (checked with a fake QEMU that
  stalls once: retried after 30 s, the stalled attempt's log kept, the
  second attempt passes). Its tag stays as the record of the sighting.
- **rc4** (run 38282688120, the head with every fix), every job green:

| Job | x86-64 | AArch64 |
|---|---|---|
| build a / build b (release key) | 199 s / 198 s | 177 s / 219 s |
| two builds compared | identical, 7 files; image sha256 `6b745997...818f09d9` | identical, 7 files; image sha256 `0dfa1222...4290badee` |
| test-entropy | pass 208 s | pass 242 s |
| test-install | pass 181 s | pass 254 s |
| test-busybox | pass 190 s | pass 244 s |
| test-net-login | pass 237 s | pass 259 s |
| test-security | pass 276 s | pass 300 s |
| guide, Linux | PASS: 12 host blocks, 7 sessions, 20 console blocks | PASS: 13/7/20 |
| guide, macOS | PASS 72 s | PASS 93 s |
| GitHub release | pre-release `v1.0.0-rc4`, 13 files | |

M6 is ticked in `docs/roadmap-1.0.md` by this PR; `v1.0.0` is tagged on
its merge commit once main's CI is green on it, and the README and the
plan's preface say 1.0 from that commit on.
