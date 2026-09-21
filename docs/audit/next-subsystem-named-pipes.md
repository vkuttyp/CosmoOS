# NEXT SUBSYSTEM — named pipes: a pipe with a name, and files that can be waited on

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report. It closes
the other half of the sentence the unix-sockets unit
(`docs/audit/next-subsystem-unix-sockets.md`) left open in the
deferred-work inventory (§1.3): "no named pipes and ~~no unix
sockets~~". That unit said a FIFO would be short -- `mknod` and a node
type are half of it, the pipe's ring the other half -- and this report
holds it to that, with one addition the FIFO forces and the tree has
wanted anyway: a `struct file` that can say whether it would block.

## What is established (before this unit)

This section describes the tree as the report found it, which is the
state the design below starts from.

**The pipe is a ring with two end objects.** `kernel/ipc/pipe.c`:
`struct pipe { lock, buf[PIPE_SIZE], head, tail, used, readers, writers,
rd_wq, wr_wq, rd, wr }`, the two `struct pipe_end` kobjects embedded,
`readers` and `writers` counting the live end objects, the pipe freed
with the last. `pipe_read` returns 0 when the ring is empty and
`writers == 0`; `pipe_write` returns `-EPIPE` when `readers == 0`;
writes of at most `PIPE_BUF` land whole; waits are killable; the lock is
never held while blocking or touching user memory. Each end carries a
`nonblock` bit shared by every handle to it, and the end types implement
`ready` and `poll_wq`, so a pipe end is pollable and submittable. The
ring's logic is written against the end objects: `pipe_read` takes a
`struct pipe_end`, and the counts are the ends' lifetimes.

**A node for a FIFO does not exist, but the operation that would make
one does.** The unix-sockets unit added `VNODE_SOCK` and the optional
`mknod` vnode operation (`vfs_mknod`, ramfs implements it, cosmofs has
no on-disk type and refuses); `COSMO_DT_FIFO` (4) has been a dirent
type all along, and a pipe end's `fstat` already reports it.
`vfs_mknod` and `ramfs_mknod` accept `VNODE_SOCK` only.

**A `struct file` cannot say whether it would block.** The file kobject
type (`kernel-services/vfs/vfs.c`, `file_type`) has `read`, `write` and
`flush` and no `ready`, `poll_wq` or `set_nonblock`: `SYS_ioready` on a
file answers "always readable and writable", `SYS_setnonblock` on one
answers `-EOPNOTSUPP`, and `poll` and the async ring treat a file as a
thing that never waits -- true of a regular file, and false of every
character device the tree has (`/dev/tty`, `/dev/net/tap`, `/dev/vmm`),
whose per-open `read_file` can block. The native `open` has no
`O_NONBLOCK` flag at all (`COSMO_O_*` has `RDONLY..NOFOLLOW`, bit
`0x0800` free); the Linux door's `open` drops `O_NONBLOCK`, and its
`fcntl(F_SETFL)` reaches only objects with a `set_nonblock`, so a Linux
program cannot make a device file non-blocking either. A device with
per-open state gets it through `chrdev_ops.open` into `file->priv`.

**The two doors and the tools.** `SYS_COUNT` is 100; the Linux tables
have `mknodat` numbers (259 on x86-64, 33 on AArch64) with no entry.
`userland/coreutils` has `mkdir` and its siblings and no `mkfifo`; the
shell test script (`SHTEST`) runs from `/etc/rc` in self-test builds.

## The problem

### The other half of the sentence

A shell pipeline connects two processes it started; a named pipe
connects two that found each other by a path, which is how a producer
and a consumer written separately meet on every Unix
(`mkfifo /tmp/log; logger > /tmp/log & tail -f /tmp/log`). The README
has said since the service manager that the kernel has neither of the
two Unix control channels; one is built, and the sentence should be
retired rather than half-retired.

### A file that blocks and cannot be asked

The FIFO is the case that forces the question -- a reader of an empty
FIFO waits, and a program that multiplexes it needs `poll` to say so --
but the question is older than the FIFO: a `struct file` over a
character device already blocks in `read_file`, and `poll` over it lies.
Making a FIFO a file (which is what it is: it has a path, a mode, an
owner, an `open` with rules) means giving files the three operations
every other blocking object has, and every device file gets them in the
same act.

## Design

### A FIFO is the pipe's ring behind a node

**The ring becomes shareable.** `pipe.c` is split at the line it
already has: the ring (`struct pipe` without the embedded ends --
`lock`, `buf`, `head`, `tail`, `used`, `readers`, `writers`, `rd_wq`,
`wr_wq`) and its four operations, `pipe_ring_read(p, buf, len,
nonblock)`, `pipe_ring_write(p, buf, len, nonblock)`,
`pipe_ring_ready_rd(p)`, `pipe_ring_ready_wr(p)`, take the ring and a
non-blocking flag, and the anonymous pipe's ends become one client of
them: `pipe_read(end)` is `pipe_ring_read(end->pipe, buf, len,
end->nonblock)`, the end's release decrements `readers` or `writers`
under the ring's lock as before. `readers` and `writers` stay what they
are -- the number of live readers and writers -- but the ring no longer
assumes who counts them. Nothing about the pipe's behaviour changes; the
`ipc-pipe` self-test is the proof, unchanged.

**The node.** `VNODE_FIFO` (`COSMO_DT_FIFO`), made by `vfs_mknod`
through the filesystem's `mknod` (ramfs implements it beside
`VNODE_SOCK`; cosmofs and procfs have none and refuse), owned by the
caller, mode as given masked to `07777`. Its ramfs node carries a
`struct fifo` (`kernel/ipc/fifo.c`): a spinlock, a pointer to a ring
(NULL while nobody has it open), and an openers' wait queue. The ring is
made at the first open and freed by the last release -- a FIFO's data
does not outlive its openers, which is POSIX and Linux -- and its
`readers` and `writers` count the FIFO's **opens** of each side, not
end objects: the FIFO's `open` hook increments, its `release` hook
decrements, both under the ring's lock, and the ring's own rules then
give end-of-file when the last writer has closed and the ring is empty
(data written before the close is read first, as on Linux), and
`-EPIPE` when no reader remains.

**Open's rules are POSIX's.** `open(O_RDONLY)` blocks until a writer has
the FIFO open; `open(O_WRONLY)` blocks until a reader has; each wakes the
other side's openers; `O_NONBLOCK` makes a read-only open return at once
and a write-only open fail with `-ENXIO` when no reader is there;
`O_RDWR` counts as both and never blocks (Linux's behaviour, and the
one program idiom that keeps a FIFO from ever reporting end-of-file). A
blocked open is killable. The open hook is where the FIFO differs from
every device: it can wait, and it waits on the fifo's own queue, not
under any lock. **An open that fails undoes itself.** The VFS runs the
release hook only for an open that succeeded (`dev_open`), so the open
hook is the only place that can take back what a failed open did: an
opener killed while waiting (`-EINTR`), or refused (`-ENXIO`), or out of
memory, decrements the count it added before returning, and if it was
the first open and is now the last, frees the ring it made -- under the
same lock the counts live under, and waking the other side's openers
whose condition it changed. So a failed open leaves the counts, the ring
and `fifo_count` as it found them, and the peer an open woke is not
left waiting on a count that will never be released.

**Per-open state is the file.** `open` sets `file->priv` to a small
`struct fifo_open { side, nonblock }`; `read_file` and `write_file` call
the ring with the open's `nonblock`; `release` runs once per open. A
FIFO's non-blocking mode is therefore **per open**, as POSIX has it and
as the anonymous pipe's shared-per-end bit does not (that bit stays as
it is; it is the pipe's documented rule).

### Files learn readiness

Three optional operations join `struct vnode_ops` -- `ready(vn, f)`,
`poll_wq(vn, f, events)` and `set_nonblock(vn, f, on)` -- and the file
kobject type gains the three matching entries, each delegating to the
vnode's when it has one and behaving as today when it has not (`ready`:
always readable and writable; `poll_wq`: NULL, readiness never changes;
`set_nonblock`: `-EOPNOTSUPP`). The FIFO implements all three
(`pipe_ring_ready_*`, the ring's two queues, the open's bit).
`chrdev_ops` gains the same three, so a device that can block can say so
-- this unit wires none of the existing devices (each is its own
measurement) and records that in the risks. `SYS_ioready`,
`SYS_setnonblock`, `poll`, `select` and the async ring need no change:
they ask the type.

**`O_NONBLOCK` at open.** The native `COSMO_O_NONBLOCK` (`0x0800`, Linux's
own value) is accepted by `open` and stored in `file->flags`; the FIFO's
open reads it for its rules and seeds the open's bit from it. A device
file may consult it too; a regular file ignores it, as everywhere. The
Linux door passes `O_NONBLOCK` through instead of dropping it.

### The doors and the tools

Native **`SYS_mknod(path, mode, type)`** (100; `SYS_COUNT` 101):
`type` is `COSMO_DT_FIFO` (a `COSMO_DT_SOCK` name without a socket is a
dead name and is refused, `-EINVAL`; `bind` is how a socket node is
made). Linux **`mknodat(dirfd, path, mode, dev)`** (259 / 33): `S_IFIFO`
makes a FIFO, `S_IFSOCK` is refused the same way, anything else
`-EPERM` as Linux gives an unprivileged caller; `dirfd` follows
`check_dirfd`'s rule. libc: `mkfifo(path, mode)`, `O_NONBLOCK`, and
`fcntl(F_SETFL, O_NONBLOCK)` on a FIFO handle. A **`mkfifo`** coreutil
(the shape of `mkdir`), so the shell can make one, and a line in the
shell test script that uses it.

### Lifetime, in one paragraph

The ring belongs to the FIFO node while any open holds it and is freed
by the last release, and an open that fails is not an open -- it takes
back its count and, if it made the ring, the ring; an unlinked FIFO
with opens keeps its ring until they close (the node lives as long as its files do, as any unlinked
node); the node's `evict` frees nothing (there is nothing to free once
the opens are gone) and asserts the ring is NULL. Invariant **I9**: *a
FIFO's ring exists exactly while an open of it does, its reader and
writer counts equal the live opens of each side, and the pipe's
end-of-file and `-EPIPE` rules follow from those counts alone.*

## Affected files

| file | change |
| --- | --- |
| `kernel/ipc/pipe.c`, `kernel/include/kernel/pipe.h` | the ring split from the ends: `pipe_ring_alloc/free`, `pipe_ring_read/write/ready_rd/ready_wr`; the ends over them; behaviour unchanged |
| `kernel/ipc/fifo.c`, `kernel/include/kernel/fifo.h` (new) | `struct fifo`, the node's ops (`open`, `release`, `read_file`, `write_file`, `ready`, `poll_wq`, `set_nonblock`), POSIX's open rules |
| `kernel/ipc/fifotest.c` (new) | `ipc-fifo` |
| `kernel/include/kernel/vfs.h`, `kernel-services/vfs/vfs.c`, `kernel-services/vfs/ramfs.c` | `VNODE_FIFO`; `vnode_ops.ready/poll_wq/set_nonblock`; `file_type` delegating; `vfs_mknod` and `ramfs_mknod` accepting `VNODE_FIFO` with `ramfs_fifo_ops`; `chrdev_ops` gaining the three |
| `kernel/include/uapi/cosmo/syscall.h` | `COSMO_O_NONBLOCK 0x0800`; `SYS_mknod` 100; `SYS_COUNT` 101 |
| `kernel/syscall/native.c` | `sys_mknod`; `open` keeping `O_NONBLOCK` in the file's flags |
| `compat/linux/syscalls.c`, `compat/linux/nr_*.h` | `lx_mknodat`; `O_NONBLOCK` kept at `open` |
| `libc/include/sys/stat.h`, `libc/include/fcntl.h`, `libc/src/` | `mkfifo`, `O_NONBLOCK` |
| `userland/coreutils/mkfifo.c`, `userland/userland.mk`, `userland/etc/rc` (the shell test) | the tool and one use of it |
| `userland/init/init.c` | a `fifo` section of `init --selftest` |
| `tests/boot/run_boot_test.py`, `tests/linux/lxtest.c` | `"fifo"` in `USERTEST_SECTIONS`; the Linux rows |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | `ipc-fifo` registered |
| `docs/kernel/ipc/{design,api,invariants,testing,architecture}.md`, `docs/kernel-services/vfs/{design,api}.md`, `docs/kernel/object/api.md` (files' readiness), `docs/compat/linux/{api,testing}.md`, `docs/kernel/syscall/api.md`, `docs/libc/api.md`, `docs/userland/{design,testing}.md`, README Status, the inventory (§1.3's row struck whole), `kernel/ipc/README.md` | as built |

## New APIs

```c
/* kernel/include/kernel/pipe.h */
struct pipe *pipe_ring_alloc(void);
void pipe_ring_free(struct pipe *p);              /* readers == writers == 0 */
int64_t pipe_ring_read(struct pipe *p, void *buf, size_t len, bool nonblock);
int64_t pipe_ring_write(struct pipe *p, const void *buf, size_t len, bool nonblock);
unsigned pipe_ring_ready_rd(struct pipe *p);      /* READABLE with bytes; READABLE|HANGUP with no writer */
unsigned pipe_ring_ready_wr(struct pipe *p);      /* WRITABLE with PIPE_BUF free; WRITABLE|ERROR with no reader */

/* kernel/include/kernel/fifo.h */
extern const struct vnode_ops fifo_node_ops_template;   /* what ramfs_fifo_ops is built from */
int  fifo_open(struct vnode *vn, struct file *f);       /* POSIX's rules; blocks; -ENXIO */
void fifo_release(struct vnode *vn, struct file *f);
int64_t fifo_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t fifo_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);
unsigned fifo_ready(struct vnode *vn, struct file *f);
struct waitqueue *fifo_poll_wq(struct vnode *vn, struct file *f, unsigned events);
int  fifo_set_nonblock(struct vnode *vn, struct file *f, int on);
unsigned fifo_count(void);                              /* live rings, for the leak test */

/* struct vnode_ops, optional */
unsigned (*ready)(struct vnode *vn, struct file *f);
struct waitqueue *(*poll_wq)(struct vnode *vn, struct file *f, unsigned events);
int (*set_nonblock)(struct vnode *vn, struct file *f, int on);

/* uapi */
#define COSMO_O_NONBLOCK 0x0800
#define SYS_mknod 100   /* (const char *path, uint32_t mode, uint32_t type) -> 0; type COSMO_DT_FIFO */
```

## Migration plan

Nothing existing changes meaning. The anonymous pipe keeps its objects,
its shared-per-end non-blocking bit and its tests; the ring split is
internal. A file that had no `ready` keeps answering "always ready"; a
device gains the three operations only when it implements them.
`SYS_COUNT` moves to 101 and the syscall filter's table follows by
number. The Linux `open` now honours `O_NONBLOCK`, which only a FIFO
(and a device that implements `set_nonblock`) can act on; a regular
file ignores it, as on Linux.

## Tests

| test | what it proves |
| --- | --- |
| `ipc-pipe` (unchanged) | the ring split changed nothing for the anonymous pipe |
| `ipc-fifo` (kernel) | a FIFO node made by `vfs_mknod` is `DT_FIFO`; `open(O_RDONLY)` waits for a writer and `open(O_WRONLY)` for a reader (each from a second thread, both orders); `O_NONBLOCK` read-only returns at once, write-only `-ENXIO` without a reader; `O_RDWR` never blocks; bytes both ways; the last writer's close gives end-of-file after the bytes it wrote; the last reader's close gives `-EPIPE`; two readers and two writers, the counts following each close; readiness through the file (`READABLE` with bytes, `READABLE\|HANGUP` with no writer, `WRITABLE` with room); `set_nonblock` per open (one open non-blocking, another not, on the same FIFO); `unlink` while open (reads and writes continue, a new open `-ENOENT`); the ring freed by the last release (`fifo_count`); a blocked opener killed returns `-EINTR` **and leaves nothing behind**: the side's count and `fifo_count` back at their values before the open, and a later opener of the other side still waiting for a real peer |
| `init --selftest`, section `fifo` (native) | `mkfifo` a path; a child writes lines the parent reads across the blocking open; `O_NONBLOCK` rules from user mode; `ioready` on a FIFO handle; `mknod` of a `DT_SOCK` name `EINVAL`; `mkfifo` on `/proc` refused; `stat` reports `S_ISFIFO` |
| `lxtest` rows | `mknodat(AT_FDCWD, path, S_IFIFO\|0644)`; `open(O_RDONLY\|O_NONBLOCK)` 0, `open(O_WRONLY\|O_NONBLOCK)` `-6`; a reader and a writer, bytes across, `fstat` `S_IFIFO`; `fcntl(F_SETFL, O_NONBLOCK)` then `read` `-11`; `mknodat(S_IFCHR)` `-1` (`EPERM`) |
| the shell test (`SHTEST`) | `mkfifo /tmp/sh-fifo; echo via-fifo > /tmp/sh-fifo & cat /tmp/sh-fifo` prints the line |

**Bug-proofs, to run.** Each mutation alone on x86-64, the debug suite
booted, the file restored:

- the reader's open not waiting for a writer → `ipc-fifo`: the open
  returns before the writer exists (the second-thread case sees the
  order broken).
- `O_NONBLOCK` write-only not refused → `ipc-fifo` and `lxtest`: 0 for
  `-ENXIO`.
- the ring's `writers` not decremented at release → `ipc-fifo`: no
  end-of-file after the last writer closes (the read blocks; caught by
  the bounded wait).
- the ring kept past the last release → `ipc-fifo`: `fifo_count` does
  not return.
- the file type's `ready` not delegated → `ipc-fifo`: a FIFO with no
  bytes reports readable.
- `set_nonblock` shared across opens (stored in the fifo, not the open)
  → `ipc-fifo`: the second open turns non-blocking with the first.
- `O_RDWR` counted as one side → `ipc-fifo`: the `O_RDWR` open blocks or
  reads end-of-file.
- the anonymous pipe's end release forgetting its count (the split's
  own risk) → `ipc-pipe`: no end-of-file / no `-EPIPE`.
- a failed open not undoing its count → `ipc-fifo`: after the killed
  opener, the other side's open returns at once against a peer that is
  not there, and `fifo_count` does not return.

## Benchmarks

`USERBENCH: fifo`: a one-byte round trip to a child over two FIFOs,
against the two-pipe figure the unix-sockets unit measured (140 / 181 us
on x86-64 / AArch64). The same ring, so the same number is the
expectation; a gap is the cost of the file layer and is worth a
sentence.

## Risks

**The existing devices stay as they are.** `/dev/tty`, `/dev/net/tap`
and `/dev/vmm` get the ability to report readiness and not the code that
does; each has its own notion of "would block" and its own tests, and
wiring them is three small units (the tap's is the useful one: a
`select` over the tap and a socket). Named here so it is not forgotten,
and not done here so the FIFO unit stays the size it promised.

**A FIFO on cosmofs.** As with sockets: `mknod` on a cosmofs path is
`-EOPNOTSUPP`; `CFS_TYPE_FIFO` would be a format bump. FIFOs live under
the ramfs root, which is `/tmp` and `/run` on Unix too.

**The blocking open.** An opener waits on the fifo's queue with no lock;
a FIFO that never gets its counterpart holds a thread forever, as on
Linux, and `O_NONBLOCK` is the program's way out. The wait is killable.

## Alternatives considered

**The FIFO's open returning a pipe end object instead of a file.** No
file layer to teach, and readiness for free -- but a pipe end's
non-blocking bit is per object and would be shared by every open of a
side, `O_RDWR` has no object to be, and the ring's counts are end
objects, not opens. The file is what a FIFO is.

**Giving only the FIFO readiness, through a FIFO-specific object type.**
It would leave the file layer unable to say whether a device would
block, which is the older gap the FIFO merely exposes; three optional
operations on `vnode_ops` cost nothing where unused and fix the class.

**Keeping the ring inside the end objects and having the FIFO create
anonymous pipes.** Every open pair would need to be matched to an end
object, `O_RDWR` and multiple openers do not fit, and the counts would
count the wrong thing. Splitting the ring from the ends is the small
refactor that makes the FIFO a client rather than a copy.
