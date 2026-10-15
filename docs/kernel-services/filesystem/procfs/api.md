# procfs: API

Mounted at `/proc` during boot (`kernel/core/main.c`, after the boot
archive is unpacked). `mount("none", "/proc", "procfs", 0)` mounts
another instance; a block device is `EINVAL`, since nothing here comes
off one.

## Paths

| Path | Type | Contents |
| --- | --- | --- |
| `/proc` | dir | `.`, `..`, `self`, `net`, the system files, and one directory per visible pid |
| `/proc/self` | symbolic link | to the reading process's directory: `readlink` answers its pid, relative |
| `/proc/<pid>` | dir, owned by the process's effective user and group | `.`, `..` (which a walk can take: `/proc`), `status`, `limits`, `fd`, `stat`, `cmdline` |
| `/proc/<pid>/fd` | dir, 0500 | one symbolic link per open handle, named by its number (roadmap M4) |
| `/proc/<pid>/fd/<n>` | symbolic link | what the handle is: a terminal by its path (`/dev/console`, `/dev/tty`, `/dev/ptmx`, `/dev/pts/N`), a directory by the path it was opened by, else `file:[ino]` or `<kind>:[address]` |
| `/proc/<pid>/status` | file, 0444 | `key: value` lines, below |
| `/proc/<pid>/limits` | file, 0444 | one `name: value` line per `COSMO_RLIMIT_*` |
| `/proc/<pid>/stat` | file, 0444 | Linux's record (proc(5)) through field 44: pid, `(name)`, state `R`, `S` or `Z`, ppid, pgid, sid, utime (all cpu time; stime 0), num_threads, starttime, processor; times in 1/100 s; page faults, vsize and rss 0 |
| `/proc/<pid>/cmdline` | file, 0444 | argv, each string NUL-terminated, the first 256 bytes |
| `/proc/meminfo` | file, 0444 | `MemTotal`, `MemFree`, `MemAvailable` (= MemFree), `Buffers` (0), `Cached` (the page cache), `SwapTotal`, `SwapFree` (0), `SReclaimable` (0), in kB |
| `/proc/stat` | file, 0444 | `cpu` and `cpuN` lines (user = busy, idle; the other columns 0) in 1/100 s, `ctxt`, `btime`, `processes` (the last pid), `procs_running`, `procs_blocked` (0) |
| `/proc/loadavg` | file, 0444 | the 1, 5 and 15 minute averages, `running/threads`, the last pid |
| `/proc/uptime` | file, 0444 | seconds since boot and the CPUs' idle seconds summed, to 1/100 s |
| `/proc/mounts` | file, 0444 | one line per mount the reader's namespace sees, root first: source (`/dev/<disk>` or the filesystem's name), path (space, tab, newline and backslash as `\ooo`), type, `rw` or `ro`, `0 0` |
| `/proc/net` | dir, 0555 | `.`, `..` (`/proc`), and the files below (shell completeness S1) |
| `/proc/net/dev` | file, 0444 | Linux's two header lines, then one line per interface: received bytes, packets, errors, drops, then 0 for fifo, frame, compressed and multicast; the same for transmit (colls, carrier, compressed 0) |
| `/proc/net/route` | file, 0444 | Linux's header and a line per IPv4 route (the default first), each padded to 127 characters: interface, destination, gateway and mask as native-endian hex, flags (`0001` up, `0003` up through a gateway), the rest 0 |
| `/proc/net/arp` | file, 0444 | `IP address`, `HW type` (`0x1`), `Flags` (`0x2` complete, `0x0` resolving), `HW address`, `Mask` (`*`), `Device` |
| `/proc/net/tcp`, `tcp6` | file, 0444 | Linux's header and a line per pcb of that family, padded to 149: slot, local and remote address:port in hex, Linux's state number, the send and receive queues (a listener's backlog and queued children), uid 0, inode 0 |
| `/proc/net/udp`, `udp6` | file, 0444 | the same for each bound UDP pcb, padded to 127: state `01` connected, else `07`; the receive queue in datagrams; drops last |
| `/proc/net/if_inet6` | file, 0444 | one line per interface's IPv6 address (`lo`'s `::1`, scope `10`; each other's link-local, scope `20`): the address as 32 hex digits, ifindex, prefix length, scope and flags (`80`) in hex, the name |

`status` holds `name`, `pid`, `ppid`, `state` (`running`, `exiting`,
`exited`), `uid`, `gid`, `privs` (roadmap M5: the privileges by name,
`mount sys_admin net proc cred device` or a subset, `none` when there are
none), `domain`, `threads`, `syscalls`, `cpu_ns`. No file renders a
kernel address (an open handle that is not a file or a terminal is named
by its kind alone).
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
| `ENOENT` | a name not in the table above; a pid that does not exist or that the caller may not see |
| `ESRCH` | opening a file of a process that has already exited |
| `EINVAL` | mounting with a block device |
| `ENOMEM` | no memory for a vnode or a listing |

Nothing here is writable: the files are `0444` and the directories have
no `create`, `mkdir`, `unlink` or `rename`, so a write is `ENOTSUP` from
the VFS.

## What is deliberately absent

`/proc/cpuinfo` and anything else system-wide not on P4's list: those
are `sysctl` names, which is a curated list, and a second spelling of a
fact is how a namespace rots (invariant P2). A process's environment and
memory map: the kernel does not keep the first, and the second would
need a lock held across a read. `/sys` is not created.
