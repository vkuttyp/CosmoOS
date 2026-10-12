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
| 5 | The release workflow, the `v1.0.0-rc1` dry run, then `v1.0.0`. |

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
