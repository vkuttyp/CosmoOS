# procfs: API

Mounted at `/proc` during boot (`kernel/core/main.c`, after the boot
archive is unpacked). `mount("none", "/proc", "procfs", 0)` mounts
another instance; a block device is `EINVAL`, since nothing here comes
off one.

## Paths

| Path | Type | Contents |
| --- | --- | --- |
| `/proc` | dir | `.`, `..`, `self`, and one directory per visible pid |
| `/proc/self` | symbolic link | to the reading process's directory: `readlink` answers its pid, relative |
| `/proc/<pid>` | dir | `.`, `..` (which a walk can take: `/proc`), `status`, `limits`, `fd` |
| `/proc/<pid>/fd` | dir, 0500 | one symbolic link per open handle, named by its number (roadmap M4) |
| `/proc/<pid>/fd/<n>` | symbolic link | what the handle is: a terminal by its path (`/dev/console`, `/dev/tty`, `/dev/ptmx`, `/dev/pts/N`), a directory by the path it was opened by, else `file:[ino]` or `<kind>:[address]` |
| `/proc/<pid>/status` | file, 0444 | `key: value` lines, below |
| `/proc/<pid>/limits` | file, 0444 | one `name: value` line per `COSMO_RLIMIT_*` |

`status` holds `name`, `pid`, `ppid`, `state` (`running`, `exiting`,
`exited`), `uid`, `gid`, `domain`, `threads`, `syscalls`, `cpu_ns`.
`limits` names `as`, `mem`, `nofile`, `nproc` and `vmem`, with
`unlimited` for `COSMO_RLIM_INFINITY`.

`fd` and its links answer only the process's own user and the
privileged (`ENOENT` to anyone else, as for a pid the caller may not
see): which files a process holds is its own business. musl's `ttyname`
reads them, so `tty` names a pty slave.

A pid the caller may not see does not exist: `lookup` and `readdir` both
apply `process_visible_to_current`, the same rule `procinfo` uses, and a
pid that fails it is `ENOENT` rather than `EACCES`.

## Errors

| Error | When |
| --- | --- |
| `ENOENT` | a name that is not `self`, a decimal pid, `status` or `limits`; a pid that does not exist or that the caller may not see |
| `ESRCH` | opening a file of a process that has already exited |
| `EINVAL` | mounting with a block device |
| `ENOMEM` | no memory for a vnode or a listing |

Nothing here is writable: the files are `0444` and the directories have
no `create`, `mkdir`, `unlink` or `rename`, so a write is `ENOTSUP` from
the VFS.

## What is deliberately absent

`/proc/meminfo`, `/proc/cpuinfo` and anything else system-wide: those
are `sysctl` names, which is a curated list, and a second spelling of a
fact is how a namespace rots (invariant P2). A process's argument
vector, its open handles and its memory map: the kernel does not keep
the first, the handle table has no names for the second, and the third
would need a lock held across a read. `/sys` is not created.
