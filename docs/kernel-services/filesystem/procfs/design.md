# procfs: design

Constitution section 56 asks for pseudo-filesystems for observability,
with clear ownership, and says not to turn them into an uncontrolled
dumping ground for kernel internals. The ownership question has to be
answered before any file is added, because every pseudo-filesystem that
became a dumping ground did so one reasonable-looking file at a time.

## What `/proc` is for, and what it is not for

**`/proc` holds facts about processes, and a fixed list of Linux's own
system files. Everything else stays where it already lives.** A file
belongs here if it describes a process, or if it is on that list.

There is already a curated place for system-wide values -- `sysctl`,
whose names are a list in one array (`kernel.*`, `hw.*`, `vm.*`, `hv.*`,
`net.*`) and whose contents somebody had to add deliberately. `dmesg`
reads the log ring. `/dev` holds device nodes that drivers create. None
of those move here.

The list (revised in v1.0.2, the everyday commands): `meminfo`, `stat`,
`loadavg`, `uptime` and `mounts`. This reverses the first design, which
left `/proc/meminfo` out because `sysctl vm.pages_free` already answered
it. That holds for programs written for this system; it does not hold
for the Linux programs it runs, which find these facts by these paths in
these formats and nowhere else -- BusyBox's `free` opens
`/proc/meminfo`, `top` opens `stat`, `meminfo` and `loadavg`, and `df`
reads `/proc/mounts` through `getmntent`. A file earns a place on the
list by being one a Linux program here opens; it is written in Linux's
format and holds nothing Linux's file does not (P4). The list is in one
array (`g_system_files`), as sysctl's names are, so adding to it is a
deliberate act.

Shell completeness S3 revises the bar once more, for two files no program
here opens: `version` and `cpuinfo`. They are what a person at the shell
reads by name -- `cat /proc/version` to learn what is running, `cat
/proc/cpuinfo` to learn on what -- and the shell is the user interface of
this system. So a file also earns a place by being one the shell's users
read by that name, which the roadmap must name (S3 names these two); it is
held to the same format rule. `version` says what `uname` says (the
release, `KERNEL_LINUX_RELEASE`, and the version), with the compiler and
without a builder nobody recorded. `cpuinfo` is a block per online CPU in
Linux's field names for the architecture: x86-64's vendor, family, model,
name and stepping from CPUID; AArch64's implementer, architecture,
variant, part and revision from MIDR_EL1. Every CPU is described by the
boot CPU's identification (`arch_cpu_info`).

Shell completeness S1 adds a directory of them, `/proc/net`: `dev`,
`route`, `arp`, `tcp`, `tcp6`, `udp`, `udp6` and `if_inet6`, which
BusyBox's `ifconfig`, `route`, `netstat` and `arp` open. The same bar
applies -- each is opened by a program here, in Linux's format -- and
the list is again one array, the network stack's `procnet_names`. procfs
owns the paths and the snapshot at the open; the stack owns the text
(`kernel/net/procnet.h`; the network design's "Inspecting the stack"),
because the formats are about its tables, not about processes.

What `/proc` adds that no existing interface does is **addressability**:
a process's facts at a path, so that `cat`, the shell and any program
that reads files can get at them without a system call of their own.
`procinfo` returns a fixed struct to a caller that knows the ABI; a file
is readable by anything.

## Layout

```text
  /proc/<pid>/status     one `key: value` line per fact
  /proc/<pid>/limits     one line per resource limit
  /proc/<pid>/fd/        a link per open handle (roadmap M4)
  /proc/<pid>/stat       Linux's one-line record (top, ps)
  /proc/<pid>/cmdline    argv, NUL-separated
  /proc/<pid>/environ    envp as exec gave it (shell completeness S3)
  /proc/<pid>/cwd, root, exe   links: where it stands and what it runs (S3)
  /proc/self             a link to the reader's own directory
  /proc/meminfo, stat, loadavg, uptime, mounts, version, cpuinfo
                         the system files (P4)
  /proc/net/dev, route, arp, tcp, tcp6, udp, udp6, if_inet6
                         the network stack's tables (P4, shell completeness S1)
```

A file's text is at most a page, except a `/proc/net` file's: a line
per socket, up to just under 64 KiB (`PROCNET_TEXT_MAX`), served a page at
a time from the text rendered at the open. The stack renders it into a
buffer that starts at a page and doubles as the text needs, and procfs
keeps the text at its own length, so a short file holds a short buffer for
as long as it is open.

Each new file is a promise to keep producing that text, so the bar for
adding one is that something needs it and nothing else offers it.

The Linux-format files say what this kernel measures and no more, in the
places Linux's readers look. A field the kernel has no figure for is 0
where readers expect the field and absent where they tolerate its
absence: no user/system split is kept, so all busy time is user time;
page faults are not counted; a CPU's idle time is its idle thread's run
time (`sched_cpu_idle_ns`) and the rest is busy; the load averages are
Linux's (`sched_loadavg`: runnable threads every five seconds, the same
constants); MemAvailable is MemFree, because the page cache gives pages
back only past its own limit and not when memory runs short. A
process's memory sizes (`vsize` and `rss` in `<pid>/stat`, `VmSize` and
`VmRSS` in `status`) are its space's own counts -- the pages its regions
cover, and the anonymous and page-cache frames resident in them -- read
under the process's lock, which every change of the process's space
holds (the process layer's P38): exit tears the space down (P34) and
exec replaces it, and a reader holding the lock sees the old space whole
or the new one, never one being freed. A reference on the space would
have been the other way, and the wrong one: the reader could then be
the last user, and the teardown would run after the exit had published
`EXITED`. Once the space is gone the sizes are 0 and `status` leaves its
memory lines out, as Linux's does for a zombie. There is no nice value,
so `priority` is 20 and `nice` 0. The controlling terminal (`tty_nr`) is
Linux's number for the same terminal (the console 5,1, `/dev/pts/N`
136,N), because this kernel numbers no devices. A process's state
is `Z` once exited, `T` while job control has it stopped, `R` with a
thread running or ready, else `S`; its CPU time counts each thread's
current running stretch too (`sched_thread_run_ns`), which the scheduler
charges only at a switch-out; its group and session are read under the
process table's lock, as every other reader reads them. The first load
average fold is one whole period after the first tick, and the figures
are rounded to two places as Linux rounds them.

`status` is in Linux's `Key:\tvalue` format since shell completeness S3,
because BusyBox's `ps` reads it (`Uid:` and `Gid:`, for the real ids): `Name`,
`Umask`, `State`, `Tgid`, `Pid`, `PPid`, `Uid` and `Gid` (real,
effective, saved, filesystem), `VmSize`, `VmRSS`, `Threads`, then what
this kernel adds -- `Privs`, `Domain` (which `procinfo` uses to decide
visibility but does not report), `Syscalls` and `Cpu_ns`. `limits` is
the `COSMO_RLIMIT_*` set by name, which is otherwise only reachable a
value at a time through `getrlimit` and only for oneself.

## Visibility is the syscall's, exactly

**A process sees in `/proc` precisely what `procinfo` would show it, and
the directory listing obeys the same rule as the files.** A pseudo
filesystem that lists what it will not let you read is an information
leak with extra steps: the names alone say which pids exist, which is
the thing a domain is meant to hide.

So `readdir` of `/proc` and `lookup` of `/proc/<pid>` both apply the
rules of `docs/kernel/security/design.md`: outside domain 0 only that
domain's processes exist at all, and an unprivileged viewer sees only
processes of its own real uid. A pid that fails either test is `ENOENT`
-- not `EACCES`, which would confirm it exists.

`/proc/self` is a symbolic link to the reading process's directory,
its target (the pid, relative) rendered at each `readlink` -- which is
how a process reads its own facts without knowing its pid. It was a
directory resolved to the caller at lookup, from before this VFS had
symbolic links. As a directory, a walk through it was named `self`, a
name that means a different directory to every process that walks it:
a child inheriting a working directory entered through `/proc/self`
held its parent's vnode under a name that, walked by the child, reached
its own (the cwd-name unit, P32). As a link the walk names it by pid.
A process directory answers `..` with `/proc`, which its listing always
named; until the same unit the lookup refused it, so `chdir("..")` from
one was `ENOENT`.

## Looking inside a process

A process's environment, and where its working directory, root and
executable are, say more than whether it exists. Linux guards them with
its ptrace read check; there is no ptrace here, and the rule is that
check's (P5): the process itself; a holder of `PRIV_PROC`; or a reader
whose effective uid and gid are all three of the target's ids, while the
target is dumpable. A process that cleared `PR_SET_DUMPABLE` -- OpenSSH's
`sftp-server` does -- keeps these from its own user. P1 has already
decided whether the process is visible at all, so another user's process
is `ENOENT` to an unprivileged reader, and a refusal of a visible one is
`EACCES`. `environ` is checked when it is opened (the vnode's `open`), so
it is listed and `stat`'ed like Linux's (0400, the process's own) and only
reading it is refused; a link's `readlink` is.

The environment is the one exec gave the process, cut at 4095 bytes
(`PROCESS_ENVIRON_MAX`), and the executable is the name the exec's walk
gave the file (P32: links replaced by where they led, so `/bin/sh` is
`/bin/busybox`). Both are kept by the kernel in the process's **exec
record** (the process design), rather than read back from the process's
memory as Linux reads them: that read is what could not be done safely,
because the space can be torn down under the reader. The difference is
small -- a program that rewrites its own stack strings changes Linux's
file and not this one -- and is recorded. An image the kernel started
from the boot archive has no file, so its `exe` is absent (`ENOENT`, as
for Linux's kernel threads). `cwd` is the process's own working
directory path and `root` is `/`, for a reader that stands where the
process does -- the same root and mounts. For any other reader both are
`ENOENT`: the process's paths do not name the reader's files, and this
kernel cannot say where the process's root lies in the reader's tree
(inventory).

## Each open is a snapshot

A file is rendered once, when it is opened, and that text is what every
read of that handle returns.

The alternative -- measure the length at open and render again at read
time -- lets the two disagree. A process whose syscall count gains a
digit between them renders longer than the size the reader is clamped
to, and one whose text shrinks leaves trailing zeroes in the difference.
**A file that reports a length must return that length**, so the length
and the text have to come from the same rendering.

Vnodes are deliberately not hashed, so opening again takes a fresh one
and a fresh snapshot; a reader that wants current facts opens again,
which is what a reader of `/proc` does anyway. A handle held open keeps
what it took, which is a truthful record of that moment rather than a
mixture of two.

The cost is bounded by what is open rather than by how many processes
exist: one buffer per open file, freed when the vnode is evicted.

Opening a file of a process that has already gone gives `ESRCH`: the
path resolved, the process did not, and saying so is more useful than a
zero-length read that looks like an idle process.

## Not done here

`/sys` is not added. Section 56 names it as possible, not required, and
there is nothing to put in it that `sysctl` does not already hold with a
curated name list; a second empty namespace is not observability.

Also absent, each because the kernel does not keep the fact or cannot
read it safely: a process's memory map, and anything that would require
holding a lock across a read of user-controlled length. The argument vector is kept, cut at 256 bytes
(`PROCESS_CMDLINE_MAX`), at spawn and exec, and inherited by a fork.
