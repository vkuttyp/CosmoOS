# Roadmap M3: real process model — 2026-10-10

Milestone M3 of the [1.0 roadmap](../roadmap-1.0.md): static musl BusyBox
`ash` is the shell; a fixed list of applets passes a scripted test; a
subset of BusyBox's own testsuite passes; both architectures, in CI. Base:
main `3dcd7436` (M1 and M2 done, CI green). Decisions 1-8 of the task are
the owner's and were not reopened; this report records how each was built.

## Plan as worked

| PR | Content |
|---|---|
| 1 | Personality lifecycle and initial-stack hooks; the kernel-stack guard (`-Wframe-larger-than=2048` with a checked baseline); `vm_space_fork`, the copy-on-write duplication of a user space, with its fault and the `vm-fork` self-test. No new system call. |
| 2 | Linux `fork`, fork-like `clone`, `CLONE_VM|CLONE_VFORK`, `wait4` for children, handle-table duplication. |
| 3 | `execve` (Linux) and `SYS_exec` (native), close-on-exec, `#!`, personality switching on exec, vfork release. |
| 4 | The BusyBox port, the image layout, `make test-busybox` and its CI job; M3 ticked. |
| 5 | Added on the way: TCP sender silly-window avoidance (#349, merged before PR 3), the defect behind `net-bench`'s 1 MiB/s mode that PR 3's layout made frequent on CI ([report](2026-10-11-tcp-sws-report.md)). |

## PR 1 (as built)

| Area | Change | Files |
|---|---|---|
| Personalities | `struct personality` gains `claims_elf`, `init`, `release`, `platform`, `auxv`, `fork`, `exec`; `personality_for_elf` chooses (kernel-created: native; the CosmoOS note: native; otherwise Linux); `process.c` names no personality. The native auxiliary vector moved into the native personality. Only change: a native initial stack no longer carries the unused AT_PLATFORM string. | `kernel/include/kernel/process.h`, `kernel/syscall/personality.c`, `kernel/syscall/native.c`, `compat/linux/syscalls.c`, `kernel/process/process.c` |
| Stack guard | `-Wframe-larger-than=2048` on kernel and modules, an error. Measured on all four builds: ~135 functions over 1 KiB, 33 over 2 KiB, the largest 8.6 KiB. The 33 are wrapped in `FRAME_EXEMPT_BEGIN/END` and listed in `scripts/frame-baseline.txt`; `scripts/check-frame-baseline.py` at the kernel link fails when the two differ. | `build/toolchain.mk`, `kernel/include/kernel/compiler.h`, `scripts/`, 18 source files |
| fork primitive | `vm_space_fork`: phase A links a child mapping record per parent record (parent's tag), phase B copies regions and shares every present page under both space locks (child nested), private pages read-only in both, then shoots the parent's lowered ranges down. Limits inherited, `-ENOMEM` when the child would start over them. | `kernel/memory/vmm.c`, `kernel/include/kernel/vmm.h` |
| Copy-on-write fault | `cow_write_locked`: a write to a present read-only anonymous frame in a private region copies it while shared, takes it back writable when this mapping is the last user. `vm_user_protect` granting write keeps a shared frame read-only. | `kernel/memory/vmm.c` |
| Tests | `vm-fork` (design in `docs/kernel/memory/testing.md`) | `kernel/memory/forktest.c` |

Design: `docs/kernel/memory/design.md` §8; invariants M48-M50;
`docs/kernel/process/design.md`, "Personalities"; `docs/build/design.md`,
"Stack frames".

### The TLB check, twice vacuous before it was a check

`vm-fork`'s TLB check was mutation-tested by removing the parent's
shootdown in `vm_space_fork`:

| Version | x86-64 | AArch64 |
|---|---|---|
| this CPU caches a translation, switches away and back, writes after the fork | passed with the shootdown removed: qemu64 has no PCID, so the switch flushes | — |
| the same where address-space tags exist | — | passed with the shootdown removed: `arch_mmu_protect` invalidates the local entry itself |
| a thread on another CPU keeps the space active (preemption off, interrupts on) | **fails** with the shootdown removed (`t.after == 1`), passes with it | passes either way, correctly: `tlbi vaae1is` is broadcast to every CPU by the hardware, so the protect already reached the other CPU |

The kept version is the third. Probe convention: M3 is a new feature; the
mutation runs above are the evidence the test can fail, not a defect proof.

The guard was also built with CI's own compiler (Debian clang 19.1.7, the
`debian:trixie` container): kernel and modules of the debug, release,
crash, chaos and harness-break builds of both architectures, no frame-size
error and the exemption pragma honoured.

### PR 1 validation (local, macOS host, QEMU, TCG)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | first run: one new diagnostic, a dead store in `vm_space_fork`'s retry (`nmaps = 0` before a `goto` that recomputes it), removed; rerun clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 133.6 s | PASS 144.4 s |
| debug `test-smp2` | PASS 149.8 s | PASS 143.1 s |
| debug `test`, `QEMU_SMP=4` | PASS 151.0 s | PASS 145.9 s |
| `test-chaos` | PASS 150.4 s | PASS 156.1 s |
| `test-harness-retry` | PASS 154.8 s | PASS 153.6 s |
| `BUILD=release test` | PASS 17.0 s | PASS 20.1 s |
| `BUILD=release test-install` | PASS 22.8 s | PASS 37.0 s |
| `test-crash` | PASS 132.3 s | PASS 127.5 s |

Every debug boot runs `vm-fork`, whose cross-CPU TLB check skips on one
CPU (`QEMU_SMP=1`) and runs at 2 and 4.

## PR 2 (as built)

| Area | Change | Files |
|---|---|---|
| fork | `process_fork`: a copy of the caller (process design, "fork"; P35). Spawn and fork now share `process_alloc`, `process_publish` (NPROC, pid, group and session), `process_first_thread`, `link_to_parent` and `process_unbuild`. The child's one thread takes the caller's registers (result 0), thread pointer, mask, alternate stack and FP/SIMD registers (`arch_fpu_inherit`, new on both arches). | `kernel/process/process.c`, `kernel/include/kernel/process.h`, `kernel/arch/*/fpu.c`, `kernel/include/arch/fpu.h` |
| Handles | every parent handle at the same number and rights, through `handle_install_at` | `kernel/process/process.c` |
| Signals | dispositions copied (`signal_process_fork`); nothing pending | `kernel/process/signal.c` |
| vfork | `vm_space.users`, `vm_space_share`/`vm_space_put` (memory §8.4, M51); processes release spaces only through `vm_space_put`. `process_vfork_wait` (killable) and `process_vfork_release`, run at the child's last thread. | `kernel/memory/vmm.c`, `kernel/process/process.c` |
| Linux | `fork`, `vfork`, `clone` without `CLONE_THREAD` via `lx_fork_common` (Linux design, "fork"; L16): `CLONE_VM` only with `CLONE_VFORK`, exit signal `SIGCHLD`; `CHILD_SETTID` written in the child before its first instruction. The personality's fork hook copies the break and each SysV shm attach record (`shm_attach_dup`). `MAP_SHARED|MAP_ANONYMOUS` is an unnamed ramfs file, so it stays shared across a fork. | `compat/linux/syscalls.c`, `compat/linux/linux_abi.h`, `kernel/ipc/shm.c` |
| Tests | `lxtest`'s fork section, both arches (`docs/compat/linux/testing.md`, "fork") | `tests/linux/lxtest.c` |

`wait4` needed nothing: children were already reaped by `process_wait_child`.
The musl program for fork moves to PR 4, where musl is built from source
for both architectures; today's `hello_musl` uses the CI runner's
`musl-gcc` and exists on x86-64 only.

No module ABI bump: `struct process`, `struct thread` and `struct
vm_space` gained fields, and no module reads them.

### Each new check fails without what it checks (x86-64, one boot each)

| Mutation | Result |
|---|---|
| `arch_fpu_inherit` not called | the child's `xmm8` is the reset value: exit 2, `wait4` status 512 |
| `process_vfork_wait` returns at once | `g_vfork_stage == 2` fails (0): the caller ran before the child |
| the shm attach records not copied | `shm_nattch == 2` fails (1); the child's `shmdt` fails (exit 11) |
| `MAP_SHARED|MAP_ANONYMOUS` private as before | `shr[0] == 22` fails (21) |
| dispositions not copied | the child's disposition check fails (exit 5) |

The dispositions run first *hung* to the harness timeout: the child failed
before writing to the report pipe, and the parent, holding the pipe's write
end itself, never saw EOF. Each side now closes the ends it does not use,
and the rerun fails in 171 s with the child's status.

### PR 2 validation (local, macOS host, QEMU, TCG)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 145.6 s | PASS 144.8 s |
| debug `test-smp2` | PASS 144.7 s | PASS 159.3 s |
| debug `test`, `QEMU_SMP=4` | PASS 154.7 s | PASS 144.8 s |
| `test-chaos` | PASS 154.8 s | PASS 157.9 s |
| `test-harness-retry` | PASS 162.9 s | PASS 146.7 s |
| `BUILD=release test` | PASS 16.9 s | PASS 20.5 s |
| `BUILD=release test-install` | PASS 23.0 s | PASS 37.0 s |
| `test-crash` | PASS 148.6 s | PASS 136.4 s |

### The thread pointer a fork child starts with (found by PR 4)

BusyBox's ash on AArch64 forked children whose first thread-pointer access
faulted at small negative addresses: the child started with TPIDR_EL0 0.
`lx_fork_common` copied `thread->tls_base`, which on AArch64 is only the
value saved at the thread's last switch-out -- a program writes TPIDR_EL0
itself, without a system call, and musl does so at startup, so a program
that forks before it is first switched out gives its child 0. Fork and a
thread clone without `CLONE_SETTLS` now read `arch_get_tls_base()`, the
register on AArch64 (x86-64's FS base changes only through `arch_prctl`,
so `tls_base` stays the value there). `lxtest`'s fork test writes
TPIDR_EL0 immediately before the fork, with no system call between, and
the child checks it: with the old read restored, the AArch64 child exits
12 (`wait4` status 3072); with the fix both architectures pass (debug
boots 155.2 s AArch64, 170.4 s x86-64).

## PR 3 (as built)

| Area | Change | Files |
|---|---|---|
| exec | `process_exec_images` (process design, "exec"; P36): `image_build`, now shared with spawn, makes the new space, images, stack and frame first; `exec_single_thread` ends the other threads (`exec_thread`, reaped before the space changes); then the point of no return: the old tid word, the space switch, `vm_space_put` of the old space, vfork release, close-on-exec, `signal_exec_reset`, the personality (`exec` hook, or release and init when it changes), thread pointer and FP/SIMD reset, the syscall frame rewritten to the new entry. | `kernel/process/process.c`, `kernel/process/signal.c`, `kernel/scheduler/thread.c`, `kernel/arch/*/fpu.c` |
| Paths, scripts | `process_execve`: `#!` with one optional argument, four deep (`-ELOOP`), non-ELF non-script `-ENOEXEC` without a warning, the syscall-filter personality check as spawn; `exec_args_copy` (1024 strings, 32 KiB). The initial frame takes as many populated pages as it needs (up to 32, was a fixed 2), its scratch arrays sized by the counts. | `kernel/process/spawn.c`, `kernel/process/process.c` |
| Close-on-exec | `HANDLE_FLAG_CLOEXEC` per slot; installs with flags in one hold; `handle_get_flags`/`handle_set_flags`; fork copies the flag. Linux: `O_CLOEXEC`, `pipe2`, `dup3`, `F_GETFD`/`F_SETFD`, `F_DUPFD_CLOEXEC`, `SOCK_CLOEXEC` (socket, accept4, socketpair), `MSG_CMSG_CLOEXEC`, eventfd, signalfd, timerfd, epoll, memfd (L17). | `kernel/object/handle.c`, `compat/linux/syscalls.c` |
| Doors | Linux `execve`; native `SYS_exec` 103 and libc `cosmo_exec`; the Linux personality's `exec` hook | `compat/linux/syscalls.c`, `kernel/syscall/native.c`, `kernel/include/uapi/cosmo/syscall.h`, `libc/include/cosmo/syscall.h` |
| Tests | `lxtest`'s exec section (both arches; `docs/compat/linux/testing.md`, "execve"), usertest's native exec checks (a native image, a Linux one) | `tests/linux/lxtest.c`, `userland/init/init.c` |

`process_create_from_images` lost its frame-size exemption: with both
ELF infos in `image_build`'s heap block it fits under 2 KiB, and
`scripts/frame-baseline.txt` drops its line (32 entries).

### Each new check fails without what it checks (x86-64, one boot each)

| Mutation | Result |
|---|---|
| `handle_close_on_exec` not called | the new image finds fds 21 and 22 open: exit 5 |
| `signal_exec_reset` not called | the caught signal keeps its handler: exit 6 |
| no vfork release at exec | the exec'd image waits 3 s for the caller's byte that never comes: exit 3 |
| the exec'ing thread keeps its own tid | `gettid != getpid` in the image an exec from a second thread started: exit 2 |
| the `#!` line's argument dropped | the interpreter runs with an unknown mode: exit 99 |
| native `SYS_exec` refuses | usertest: the `-ENOENT` and `-EACCES` checks fail, and the process-user self-test with them |

The `#!` mutation first passed the check only by timing out: the
mis-parsed script ran lxtest without a mode, which ran the whole test
again inside the child. lxtest now treats any argument as a checking
mode and exits 99 for an unknown one.

### PR 3 validation (local, macOS host, QEMU, TCG)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 140.3 s | PASS 134.2 s |
| debug `test-smp2` | PASS 144.3 s | PASS 142.9 s |
| debug `test`, `QEMU_SMP=4` | PASS 144.9 s | PASS 158.9 s |
| `test-chaos` | PASS 153.9 s | PASS 147.9 s |
| `test-harness-retry` | PASS 152.7 s | PASS 152.8 s |
| `BUILD=release test` | PASS 17.3 s | PASS 20.3 s |
| `BUILD=release test-install` | PASS 23.2 s | PASS 36.8 s |
| `test-crash` | PASS 130.7 s | PASS 130.8 s |

The matrix ran on PR 3 over PR 2's head before PR 2's thread-pointer fix;
PR 3 was then rebased onto it (the fix touches `lx_fork_common`,
`lx_clone` and the arch user code, none of PR 3's files). PR 4's matrix
runs over both.

## PR 4 (as built)

| Area | Change | Files |
|---|---|---|
| Port | BusyBox 1.37.0, musl 1.2.5 and compiler-rt 19.1.7's builtins, each pinned by SHA-256 and fetched once (`scripts/fetch-pinned.py`, `.cache/ports`); musl and the builtins built with the project's clang, BusyBox with the checked-in `busybox.config` through a wrapper that links static with musl's start files and the image at 4 MiB. Built per architecture in `out/busybox-<arch>`, shared by the build variants; reproducible (`make reproducible` compares it). | `ports/busybox/` |
| Image | `/bin/busybox` and the 48 applet links (ustar symbolic-link entries: `mkbootarchive.py` `NAME@TARGET`, `bootarchive.c`, `ramfs_populate_boot`; the archive holds up to 256 entries); colliding native programs as `/bin/cosmo-<name>`; init's console shell from `/etc/console-shell`, which `cosmo-install` sets to `/bin/sh`; `/etc/rc.test` keeps testing the native shell. | `scripts/mkbootarchive.py`, `kernel/core/bootarchive.c`, `kernel-services/vfs/ramfs.c`, `userland/` |
| Kernel | `vfs_setattr` and the `setattr` vnode operation (cosmofs: `inode_sync`); Linux `chmod`, `fchmod`, `fchmodat`, `fchmodat2`, `utimensat`; a per-process umask (decision 2's inheritance list), applied by the Linux door; `/dev/null`, `/dev/zero`; exec's argument block raised to 8192 strings and 64 KiB (BusyBox xargs), the initial frame to 160 KiB. | `kernel-services/vfs/`, `compat/linux/`, `kernel/process/` |
| Acceptance | `make test-busybox` (`tests/boot/busybox_test.py`): `tests/busybox/ash.sh` (72 checks) and `tests/busybox/suite.sh` (the fifteen testsuite files); CI job `busybox`. | `tests/` , `Makefile`, `.github/workflows/ci.yml` |

### Found on the way, each blocking the acceptance test

| Defect | Seen as | Fix | Check | Without the fix |
|---|---|---|---|---|
| a fork child's thread pointer on AArch64 was the value saved at the parent's last switch-out | ash's children faulting at small negative addresses | PR 2's `arch_get_tls_base` (PR 2 report) | lxtest fork | AArch64 child exits 12 |
| an interrupted `nanosleep` wrote 0 into `rem` | BusyBox `sleep` ending at once after `^Z` and `fg` in the interactive harness | the unslept time | lxtest: 5 s sleep interrupted at 200 ms leaves 3-5 s | the `rem` check fails |
| `mkdir` of `.` was `-EINVAL` | `tar: can't create directory './'` | `-EEXIST`, as Linux | lxtest `mkdir /tmp/.` and `/tmp/./` | both checks fail |
| arm64's `O_DIRECTORY`, `O_NOFOLLOW`, `O_LARGEFILE` had x86 numbers | `ls: can't open '.': Invalid argument` on AArch64 only | arm64's numbers | lxtest opens a directory by Linux's literal number | AArch64: both checks fail (the directory open and the file's `-ENOTDIR`) |
| no umask applied | a file created 0666 by the shell (testsuite: tar into a read-only dir) | the process's umask, inherited | lxtest: 0666 with umask 027 is 0640; a fork child's is 027 | the file is 0666 (`st_mode` 33206) |

Each "without" column is a mutation run with the old code restored (one
debug boot; AArch64 for the two AArch64-only rows, x86-64 for the rest).
These are recorded as mutation runs rather than `tools/*-probe.py`
probes: the checks are lxtest lines that fail on main's behaviour, and
the milestone's PR budget went to the acceptance test.

Two BusyBox build matters, not kernel defects: lld places a static
executable at 2 MiB, below `USER_LO` (`--image-base=0x400000`), and
BusyBox's const-pointer-to-globals trick does not survive clang on
AArch64 (`-DBB_GLOBAL_CONST=`, the switch `libbb.h` names for such
toolchains; `awk`, `diff`, `ls` and `find` faulted at address 4, on Linux
in an arm64 container too).

### Acceptance (local, macOS host, QEMU, TCG; `BUILD=release`)

| | x86-64 | AArch64 |
|---|---|---|
| scripted test (`ash.sh`) | PASS, 72 checks | PASS, 72 checks |
| cut | 28 pass | 28 pass |
| sed | 93 pass, 3 excluded | 93 pass, 3 excluded |
| grep | 44 pass, 7 skipped by runtest | the same |
| tr | 10 pass | 10 pass |
| sort | 25 pass | 25 pass |
| uniq | 14 pass | 14 pass |
| head | 3 pass | 3 pass |
| tail | 3 pass, 1 excluded | the same |
| expr | 2 pass | 2 pass |
| seq | 25 pass | 25 pass |
| basename | 2 pass | 2 pass |
| dirname | 7 pass | 7 pass |
| wc | 5 pass | 5 pass |
| xargs | 11 pass, 1 excluded | the same |
| tar | 19 pass, 11 excluded, 3 skipped by runtest | the same |
| whole boot | PASS in 29.9 s | PASS in 37.7 s |

The exclusions and their reasons are in `docs/userland/testing.md`,
"make test-busybox": three sed cases fail identically under Linux with
this binary (an arm64 container), five tar cases make hard links, eight
need an applet outside the M3 list (`dd`, `md5sum`, `bunzip2`,
`uudecode`). Skipped and untested cases are runtest's own verdicts for
features the configuration leaves out.

### PR 4 validation (local, macOS host, QEMU, TCG)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 140.6 s | PASS 143.2 s |
| debug `test-smp2` | PASS 155.7 s | PASS 153.2 s |
| debug `test`, `QEMU_SMP=4` | PASS 144.6 s | PASS 154.8 s |
| `test-chaos` | PASS 152.9 s | PASS 146.7 s |
| `test-harness-retry` | PASS 152.0 s | PASS 154.3 s |
| `BUILD=release test` | PASS 17.9 s | PASS 21.1 s |
| `BUILD=release test-install` | PASS 22.4 s | PASS 36.4 s |
| `BUILD=release test-busybox` | PASS 29.0 s | PASS 40.3 s |
| `test-crash` | PASS 123.8 s | PASS 132.5 s |
| `make reproducible` (x86-64, debug) | yes: BusyBox, its testsuite and the boot archive identical across two trees | -- |

