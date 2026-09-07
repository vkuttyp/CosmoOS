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

**P2. `/proc` holds facts about processes and nothing else.** System
wide values stay in `sysctl`, whose names are a curated list; the log
stays in `dmesg`; device nodes stay in `/dev`. A second spelling of a
fact that already has a home is how a namespace becomes the dumping
ground section 56 forbids.

Check: the filesystem synthesises exactly `<pid>/status`, `<pid>/limits`
and `self`, and the self-test requires an unknown name under `/proc` and
under `/proc/<pid>` to be `ENOENT`, so a file appears only when someone
adds it deliberately.

**P3. A file's text is made when it is read, and says so when the
process is gone.** No vnode holds a rendering between reads, so the
memory cost follows what is being read rather than how many processes
exist. A read of a process that has exited is `ESRCH`: the path
resolved and the process did not, which is worth saying rather than
returning nothing and looking like an idle process.

Check: the self-test opens a child's `status`, waits for the child to
exit and be reaped, and requires the pending read to fail with `ESRCH`
rather than return stale text.
