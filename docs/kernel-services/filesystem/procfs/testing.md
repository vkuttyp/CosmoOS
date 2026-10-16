# procfs: testing

The user-mode self-test (`userland/init/init.c`, `proc_fs_selftest`)
covers what the files say and what the namespace refuses:

- `/proc/self/status` names the caller's own pid, name and state, and
  `/proc/self/limits` names a limit — `self` resolves to the caller.
- The same facts appear under the caller's own pid, so `self` is not a
  special case with its own answer.
- `/proc/self/cmdline` is NUL-terminated and names init, and
  `/proc/self/stat` starts with its pid and `(init)` and has a non-zero,
  page-multiple vsize and a non-zero rss no larger (shell completeness
  S3); `status` starts `Name:\tinit`, says `State:\tR (running)`, root's
  four ids, and `VmSize` and `VmRSS`.
- `/proc/self/cwd` is an absolute path, `/proc/self/root` is `/`,
  `/proc/self/environ` reads, and the directory lists `environ` and `exe`
  (init comes from the boot archive, so its `exe` names nothing).
- The system files (P4) have Linux's shape: `meminfo` starts with
  `MemTotal:` and has `MemAvailable:`; `stat` has the `cpu` total, a
  `cpu0` line and `btime`; `loadavg` is three two-decimal averages,
  `running/all` with at least this thread running, and a last pid no
  smaller than init's; `uptime` is a number; `mounts` has `/` and
  `/proc procfs`; `/proc` lists `meminfo` and `mounts`; `version` starts
  `Linux version 6.0.0-cosmo ` and `cpuinfo` with `processor\t: 0`.
- `/proc/kallsyms`, `/proc/self/maps`, `/proc/0`, `/proc/01` and
  `/proc/99999` are all `ENOENT`. `01` matters: a name that is not a
  pid must miss rather than be rounded into one.
- BusyBox's scripted test (`tests/busybox/ash.sh`) runs the programs
  that read these files: `free`, `df /`, `uptime`, `top -b -n 1`; and,
  since shell completeness S3, `ps` (a background `sleep` with its pid,
  `S` and non-zero VSZ and RSS; `ruser` from `status`'s `Uid:`; init as
  the console shell's parent, `S`; the script's shell, `S`), `top`'s VSZ
  for the same `sleep`, its `status` (`VmSize`, `VmRSS`, `State:\tS
  (sleeping)`), `free` against `sysctl` (the same total; the free figure
  within 2 MiB of one read just before -- 8 to 764 kB apart in the runs
  that added it), `kill -l`, a process's own `environ`, `exe`, `cwd` and
  `root`, `version` against `uname -r` and `cpuinfo`'s blocks against
  `hw.ncpu`. `test-net-login`'s stage h runs `ps` on the installed system
  and finds init, dropbear and the console shell by pids known without
  `/proc`.
- The kernel's `proc-exit-read` self-test (P38 in the process layer)
  reads a process's `stat` and `status` across its exit: at the exiting
  hold (the space torn down, `EXITED` not yet published) `stat` says vsize
  0 and rss 0 and `status` has no memory lines; then 64 processes are
  killed while a thread on another CPU reads both files from before the
  kill until neither opens, every text whole and its sizes both zero or
  both not. No exec record and no rendered text is left over (counted by
  `exec_record_live` and `procfs_texts_live`, debug builds). The kill
  waits for the reader's first read, so a round cannot read nothing; a
  first version killed first and read 0 times in 64 rounds while passing.
- `lxtest`'s inspection section (P5): a target that drops from root to
  uid 1000 must find its dumpable flag cleared and be refused to an
  unprivileged uid-1000 reader; one that sets the flag again is readable.
  Each target reports its outcome on a pipe, so a failure is a
  `LINUXTEST: FAIL` line rather than a hang. With the clearing removed
  from `process_setresuid` (a mutation), the first target reports `3`.
  `ash.sh`'s `environ-privproc` reads another uid's `environ` with
  `sys_admin` dropped; with the file's mode back at 0400 (a mutation) it
  reads nothing.
- A child that has exited and been reaped answers a read with `ESRCH`
  rather than stale text.

Visibility (P1) is checked where the process is not privileged. The
unprivileged self-test requires that a root-owned process is neither
listed in `/proc` nor openable at `/proc/<pid>` or
`/proc/<pid>/status`, and that its own pid *is* listed — otherwise the
test would pass on a `/proc` that showed nothing at all. The domain test
runs `cat /proc/1/status` inside a process domain and requires it to
fail: pid 1 certainly exists, and a domain must not be able to read it.

Confirmed against the bug: with the filter dropped from the lookup, the
unprivileged process opens the root-owned directory and the user-mode
test fails.

Not covered: concurrent readdir against process creation and exit (the
listing is a snapshot taken under the table lock, so it can name a pid
that exits before it is opened — which reads as `ESRCH`, the same
answer as any other race with an exit); more than 256 visible processes
in one listing.
