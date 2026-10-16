# procfs: invariants

**P1. `/proc` shows a process exactly what `procinfo` would.** The
listing and the lookup apply the same rules as the system call: outside
domain 0 only that domain's processes exist, and an unprivileged viewer
sees only processes of its own real uid. A pid that fails either test is
`ENOENT` rather than `EACCES`, because "not permitted" confirms the pid
is in use, which is the one thing a domain is meant not to tell.

The listing matters as much as the files. A pseudo-filesystem that names
what it will not let you read is an information leak with extra steps --
the names alone say which pids exist.

Check: the user-mode self-test lists `/proc` as an unprivileged process
and requires it to contain that process and not a root-owned one, and
requires opening the root-owned pid's directory to fail with `ENOENT`;
and the domain test requires a process in a domain to see only its own
domain in `/proc`. Confirmed against the bug: with the filter dropped
from the lookup, the unprivileged process opens the root-owned
directory and the assertion fails.

**P2. `/proc` holds facts about processes and the system files of P4,
nothing else.** System wide values stay in `sysctl`, whose names are a
curated list; the log stays in `dmesg`; device nodes stay in `/dev`. A
second spelling of a fact that already has a home is how a namespace
becomes the dumping ground section 56 forbids.

Check: the filesystem synthesises exactly the names in the API's table,
and the self-test requires an unknown name under `/proc` (`kallsyms`) and
under `/proc/<pid>` (`maps`) to be `ENOENT`, so a file appears only
when someone adds it deliberately. (They were `cpuinfo` and `environ`
until shell completeness S3 added both.)

**P3. A file's length and its text come from the same rendering.** Each
open renders once and every read of that handle returns that text. The
alternative -- measuring at open and rendering at read -- lets a process
whose text grew between the two be truncated to the older length, and
one whose text shrank return trailing zeroes. A file that reports a
length must return that length.

Vnodes are not hashed, so opening again takes a fresh snapshot, and the
memory is one buffer per open file rather than one per process.
Opening a file of a process that has already gone is `ESRCH`: the path
resolved and the process did not, which is worth saying rather than
returning nothing and looking like an idle process.

Check: the self-test reads `/proc/self/status` and requires the length
it gets to match what the file reported; and it opens the `status` of a
child that has exited and been reaped, requiring `ESRCH` rather than
stale text.

**P4. A system file is one Linux programs read by that path, or one the
shell's users read by that name, in Linux's format.** `meminfo`, `stat`,
`loadavg`, `uptime` and `mounts` are on a fixed list (`g_system_files`)
because a Linux program here opens each (`free`, `top`, `df`,
`uptime`); `version` and `cpuinfo` are on it because the roadmap names
them as what a person at the shell reads (shell completeness S3, which
revised this invariant: nothing here opens either). Each says only what this kernel
measures, in the fields and units Linux's file uses; a field with no
figure is 0 or absent (design.md, "Layout"), never an estimate made to
look like a measurement. `<pid>/stat`, `<pid>/status` (Linux's
`Key:\tvalue` lines, then this kernel's own), `<pid>/cmdline` and
`<pid>/environ` hold the same promise for a process. The `/proc/net` files are a second fixed list
(the network stack's `procnet_names`, shell completeness S1) under the
same rule, rendered by the stack from its listings (the network
invariants' N32).

Check: the user-mode self-test reads each and requires Linux's shape --
`MemTotal:` first, `cpu  ` then `cpu0 ` and `btime` in `stat`, three
two-decimal averages, `running/all` and the last pid in `loadavg`, `/`
and `/proc procfs` in `mounts`, its own pid and `(init)` in
`<pid>/stat` with a non-zero vsize and rss, `Name:\tinit` and its ids in
`status`, a NUL-terminated `cmdline` naming it, `Linux version
6.0.0-cosmo` first in `version` and `processor\t: 0` in `cpuinfo` -- and
BusyBox's scripted test runs the programs that read them (`free`, `df`,
`uptime`, `top -b`, `ps`, with `free` agreeing with `sysctl`), and reads
`version` (uname's release) and `cpuinfo` (a block per CPU); `test-net-login` runs the ones that read `/proc/net`
(`ifconfig`, `route -n`, `netstat -tuln`, `arp -a`) and requires
eth0's counters, both routes, dropbear's listener and the gateway.

**P5. Looking inside a process is Linux's ptrace read check.** A
process's `environ`, and its `cwd`, `root` and `exe` links, are read by
the process itself, by a holder of `PRIV_PROC`, or by a reader whose
effective uid and gid are all three of the target's ids while the target
is dumpable (`PR_SET_DUMPABLE`); anyone else who can see the process (P1)
is refused with `EACCES` -- at the open for `environ`, which stays listed
and `stat`able, and at `readlink` for the links. (Shell completeness S3.)
What is read is the kernel's own record (the exec record: envp and the
executable's traversed name), never the process's memory, which can be
torn down under a reader.

Check: `lxtest`'s inspection section forks two processes of uid 1000, one
non-dumpable and one dumpable, and a reader of uid 1000 without
privileges: the non-dumpable one's `environ` is `EACCES` and its `cwd`
link `EACCES` while its `status` opens, the dumpable one's `environ` and
`cwd` read, and root's `environ` is `ENOENT` (P1); then the root parent,
with `PRIV_PROC`, opens the non-dumpable one's `environ`. BusyBox's
scripted test reads a process's own `environ` (a variable `env` set),
`exe` (`/bin/busybox`), `cwd` and `root`, and requires `priv -u 1000 cat
/proc/1/environ` to fail. Confirmed against the bug: with the dumpable
test removed from the rule, the reader's `EACCES` check fails (its exit
status 3) and `LINUXTEST: FAIL`.
