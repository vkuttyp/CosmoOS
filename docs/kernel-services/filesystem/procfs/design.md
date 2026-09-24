# procfs: design

Constitution section 56 asks for pseudo-filesystems for observability,
with clear ownership, and says not to turn them into an uncontrolled
dumping ground for kernel internals. The ownership question has to be
answered before any file is added, because every pseudo-filesystem that
became a dumping ground did so one reasonable-looking file at a time.

## What `/proc` is for, and what it is not for

**`/proc` holds facts about processes. Everything else stays where it
already lives.** That is the whole rule, and it is enforceable because
the name says it: a file belongs here only if it describes a process.

There is already a curated place for system-wide values -- `sysctl`,
whose names are a list in one array (`kernel.*`, `hw.*`, `vm.*`, `hv.*`,
`net.*`) and whose contents somebody had to add deliberately. `dmesg`
reads the log ring. `/dev` holds device nodes that drivers create. None
of those move here, and `/proc/meminfo` and `/proc/cpuinfo` are not
added, because `sysctl vm.pages_free` and `sysctl hw.ncpu` already
answer those and a second spelling of the same fact is how a namespace
starts to rot.

What `/proc` adds that no existing interface does is **addressability**:
a process's facts at a path, so that `cat`, the shell and any program
that reads files can get at them without a system call of their own.
`procinfo` returns a fixed struct to a caller that knows the ABI; a file
is readable by anything.

## Layout

```text
  /proc/<pid>/status     one `key: value` line per fact
  /proc/<pid>/limits     one line per resource limit
  /proc/self             a link to the reader's own directory
```

Three files rather than thirty. Each new file is a promise to keep
producing that text, so the bar for adding one is that something needs
it and nothing else offers it.

`status` carries what `procinfo` already returns -- pid, ppid, name,
state, uid, gid, threads, syscalls, cpu time -- and `domain`, which
`procinfo` uses to decide visibility but does not report. `limits` is
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

Also absent, each because the kernel does not keep the fact: a process's
argument vector (`argv` is copied into the new address space and not
retained), its open handles as a directory (the handle table has no
names for its entries), its memory map, and anything that would require
holding a lock across a read of user-controlled length.
