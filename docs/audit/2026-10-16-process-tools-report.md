# Process tools on /proc: S3 — 2026-10-16

The 1.1 roadmap's S3, the third unit of the shell-completeness track, which
widens P2 (process memory in procfs) and so does it too. Base: main
`985e89b1` (S2 and the crash-test fix merged, main green). Goal: BusyBox's
`ps`, `top`, `free`, `uptime` and `kill -l` working from `/proc` as they do
on Linux, and the `/proc` files they and the shell's users read.

## Before

| What | Before |
|---|---|
| `ps` | the native `/sbin/ps`: no options, no memory; BusyBox's `CONFIG_PS` off |
| `kill -l` | ash's builtin only; no `/bin/kill` |
| `/proc/<pid>/stat` | vsize and rss 0 (the space could be torn down under a reader, P34, and nothing held it); tty_nr 0, tpgid -1 |
| `/proc/<pid>/status` | lower-case `key: value` lines; no ids beyond the effective, no sizes |
| `environ`, `cwd`, `root`, `exe` | absent (`ENOENT`, checked by procfs P2's self-test) |
| `/proc/version`, `/proc/cpuinfo` | absent: P4 admitted only files a program here opens |
| `meminfo`, `stat`, `loadavg`, `uptime` | present since v1.0.2 |

## The two rules settled first

**Who may look inside a process** (procfs P5). Linux guards `environ`,
`cwd`, `root` and `exe` with its ptrace read check, and there is no ptrace
here, so the rule is that check's: the process itself; a holder of
`PRIV_PROC`; or a reader whose effective uid and gid are all three of the
target's ids while the target is dumpable. A process that cleared
`PR_SET_DUMPABLE` keeps these from its own user without `PRIV_PROC`, which
is what the roadmap asks. P1 has already hidden another user's processes
from an unprivileged reader (`ENOENT`); a visible process refused is
`EACCES` -- at the open for `environ`, which stays listed and `stat`able
as Linux's does, and at `readlink` for the links.

What is read is not the process's memory. Linux reads `environ` from the
target's stack; the reason this kernel left environments out was that such
a read cannot be made safe here: the space can be torn down under the
reader (P34). So exec keeps an **exec record** (`struct exec_record`,
`process.h`): envp joined and cut at 4095 bytes, and the executable's
traversed name from the exec's own lookup (`vfs_lookup_named`, P32: links
replaced, so `/bin/sh` is `/bin/busybox`). It is made in `image_build`
with everything else that can fail, swapped under `p->lock` at exec,
shared by reference with a fork child, and put at release. The difference
from Linux -- a program rewriting its own stack strings changes Linux's
file and not this one -- is recorded (inventory §13).

**P4's revision.** P4 admitted a system file only if a Linux program here
opens it; nothing opens `version` or `cpuinfo`. They are what a person at
the shell reads by name, and the shell is this system's interface, so P4
now also admits a file the shell's users read by that name, when the
roadmap names it, under the same format rule. `version` says what `uname`
says (the release, now `KERNEL_LINUX_RELEASE` for both); `cpuinfo` is a
block per online CPU, the boot CPU's identification in Linux's names
(`arch_cpu_info`: CPUID on x86-64, MIDR_EL1 on AArch64).

## The sizes, and P38

`process_space_usage` reads the pages a process's regions cover
(`mapped_pages`) and the frames resident in them (`anon_pages +
file_pages`) under `p->lock` and the space's lock. That is safe because of
a new invariant, P38: on a published process, `p->space` changes only
under `p->lock`, and the old space is put after the lock is dropped. Two
places changed it without the lock: exec's swap (an atomic store, then the
put) and the exit's teardown (the put, then `p->space = NULL`, with no
lock at all). Both now store under the lock and put after. The reader
takes no reference on the space, deliberately: a reference would let a
reader be the last user and run the teardown after the exit had published
`EXITED`, undoing P34. Once the space is gone, `stat` says 0 and 0 and
`status` leaves the memory lines out, as Linux does for a zombie.

## Changes

| Area | Change | Files |
|---|---|---|
| process | the exec record (made in `image_build`, installed at spawn, swapped at exec, shared at fork, put at release); `process_space_usage`; `process_cwd_path`; exec's space swap and the exit's teardown under `p->lock` (P38); `read_executable` names the file it found | `kernel/include/kernel/process.h`, `kernel/process/process.c`, `kernel/process/spawn.c` |
| procfs | `stat`'s vsize, rss, tty_nr, tpgid; `status` in Linux's format (`Name`, `Umask`, `State`, `Tgid`, `Pid`, `PPid`, `Uid`, `Gid`, `VmSize`, `VmRSS`, `Threads`, then `Privs`, `Domain`, `Syscalls`, `Cpu_ns`); `environ` (checked and rendered at the open); the `cwd`, `root` and `exe` links; `version` and `cpuinfo`; a debug count of rendered texts | `kernel-services/filesystem/procfs/procfs.c` |
| arch | `arch_cpu_info` | `kernel/include/arch/cpu.h`, `kernel/arch/{x86_64,aarch64}/cpu.c` |
| version | `KERNEL_LINUX_RELEASE` (`6.0.0-cosmo`), used by `uname` and `/proc/version` | `kernel/include/kernel/version.h`, `compat/linux/syscalls.c` |
| BusyBox | `ps` (`FEATURE_PS_TIME`, `FEATURE_PS_ADDITIONAL_COLUMNS`) and `kill`; `/bin` precedes `/sbin`, so they shadow the native ones | `ports/busybox/{applets,busybox.config,mkconfig.py}` |
| userland | `priv` reads `Privs:`; init's self-test reads the new formats and files | `userland/system/priv.c`, `userland/init/init.c` |
| tests | `ash.sh`'s S3 section; `test-net-login` stage h; `shelltest`'s `ps` line in BusyBox's format; the `proc-exit-read` self-test; `lxtest`'s inspection section | `tests/busybox/ash.sh`, `tests/boot/{net_login_test,shelltest}.py`, `kernel/process/proctest.c`, `tests/linux/lxtest.c` |

## Tests

The acceptance, in CI on both architectures:

- **`test-busybox`** (job `busybox`): `ps -o pid,stat,vsz,rss` lists a
  background `sleep` by its pid, `S`, with non-zero VSZ and RSS, and
  `ps -o pid,ruser` its user (from `status`'s `Uid:`); `ps` lists init
  (the console shell's parent) and the script's shell, both `S`; `top
  -bn1`'s line for the `sleep` has a non-zero VSZ; its `status` has
  non-zero `VmSize` and `VmRSS`; `free`'s total is `sysctl`'s exactly and
  its free figure within 2 MiB of `sysctl vm.pages_free` read just before;
  `uptime`; `/bin/kill -l`; a process's own `environ`, `exe`, `cwd` and
  `root`; `priv -u 1000 -- cat /proc/1/environ` refused; `version` against
  `uname -r` and `cpuinfo` against `hw.ncpu`. 152 checks in all.
- **`test-net-login` stage h** (job `net-login`): on the installed system,
  `ps -o pid,ppid,stat,comm` lists init, `dropbear` and the console shell
  with the pids and states known without `/proc` (`$$`, `$PPID`, and the
  supervisor `svc status` names as dropbear's parent), and `ps` itself
  running. Results: `init 1 S, dropbear 11 S (supervisor 10), the shell
  19 S, ps R` on both architectures.

In every debug boot:

- **`proc-exit-read`** (kernel self-test, P38): at the exiting hold (the
  space torn down, `EXITED` not published), `stat` reads vsize 0 and rss 0
  and `status` has no memory lines; then 64 processes are killed while a
  thread on another CPU reads both files from before the kill until
  neither opens -- every text whole, its sizes both zero or both not -- and
  no exec record or rendered text is left over (counted by
  `exec_record_live` and `procfs_texts_live`, debug builds only). x86-64
  SMP 2: 2,662 reads, 1,275 begun after a kill, 2.3 s; AArch64 SMP 2: 2,387
  and 1,139, 2.2 s. The first version killed before the reader started and
  read nothing in 64 rounds while passing; the kill now waits for the
  reader's first read, and a round that read nothing fails.
- **`lxtest`'s inspection section** (P5): two uid-1000 processes, one
  non-dumpable, and an unprivileged uid-1000 reader: the non-dumpable
  one's `environ` and `cwd` are `EACCES` while its `status` opens, the
  dumpable one's read, root's `environ` is `ENOENT`; the root parent
  (`PRIV_PROC`) opens the non-dumpable one's `environ`. With the dumpable
  test removed from the rule (a mutation, x86-64), the reader exits 3 (its
  `EACCES` check) and `LINUXTEST: FAIL`.
- init's procfs self-test: the formats above, `version`, `cpuinfo`, the
  links and `environ`; P2's unknown names are now `kallsyms` and `maps`.

New features are proven by their acceptance test; no probe (AGENTS.md).

## Validation

- **The validation matrix**, on `c15afcbe`, every step PASS; the boot
  times are the harness's own lines:

  | Step | x86-64 | AArch64 |
  |---|---|---|
  | `host-test`, `fuzz`, `analyze` | pass | pass |
  | debug `test`, `QEMU_SMP=1` / `2` / `4` | 146.3 / 144.6 / 154.3 s | 148.9 / 153.8 / 159.1 s |
  | `test-smp2` | 145.0 s | 150.5 s |
  | `test-chaos` | 155.1 s | 163.4 s |
  | `test-harness-retry` | 147.6 s | 161.8 s |
  | `BUILD=release test` | 17.8 s | 20.7 s |
  | `BUILD=release test-busybox` | 47.1 s (152 checks) | 53.6 s |
  | `BUILD=release test-net-login` | 65.6 s | 73.9 s |
  | `BUILD=release test-install` | 26.0 s | 40.9 s |

  The network harness passed in every debug boot (the boot test requires
  it). `proc-exit-read` in the SMP 4 boots: 832 reads (362 after a kill)
  on x86-64, 949 (424) on AArch64. `free` against `sysctl`: 764 kB apart
  on x86-64, 76 kB on AArch64 (8 to 80 kB in the earlier runs).
- **The first run** of the matrix, on `fa3265ae`, passed every step but
  `analyze`, on both architectures: `process_execve: Potential leak of
  memory pointed to by 'exe.name'`. It was real: `read_executable`
  allocated the file's name once the lookup succeeded, and its later
  refusals (not a regular file, no execute permission, a failed open or
  read) returned without freeing it. `c15afcbe` frees it on every failure,
  and the matrix was run again in full.
- **Branch CI**: pending at the time of writing; the PR records the run.

## Recorded instead of done

Inventory §13: devices have no numbers (`tty_nr` is Linux's for the same
terminal; `ps` prints `5,1`); no nice value or priority calls; no
user/system time split; `environ` is exec's copy, not live memory; `cwd`
and `root` are `ENOENT` across roots or mount namespaces; `cpuinfo` is the
boot CPU's identification for every CPU, without feature flags;
`busybox.config` differs from its README's recipe in three options S2
left. Closed: the v1.0.2 entry for VSZ and RSS read as 0.
