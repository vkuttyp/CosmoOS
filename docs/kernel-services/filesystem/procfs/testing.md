# procfs: testing

The user-mode self-test (`userland/init/init.c`, `proc_fs_selftest`)
covers what the files say and what the namespace refuses:

- `/proc/self/status` names the caller's own pid, name and state, and
  `/proc/self/limits` names a limit — `self` resolves to the caller.
- The same facts appear under the caller's own pid, so `self` is not a
  special case with its own answer.
- `/proc/meminfo`, `/proc/self/cmdline`, `/proc/0`, `/proc/01` and
  `/proc/99999` are all `ENOENT`. `01` matters: a name that is not a
  pid must miss rather than be rounded into one.
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
