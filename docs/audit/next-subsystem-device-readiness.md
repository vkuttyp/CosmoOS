# NEXT SUBSYSTEM — devices that can be waited on: readiness for the terminal and the tap, and `select` for the Linux door

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report. It takes
up the risk the named-pipes unit
(`docs/audit/next-subsystem-named-pipes.md`, "Risks") named and declined
to build: that unit taught a `struct file` to say whether it would block
and gave `chrdev_ops` the three operations to say it with, and then wired
no device. This unit wires the two that block, closes the constitution's
"async I/O must work for devices" row, which the inventory marks
**unverified** (§2.11, Prompt #2 §23), and gives the Linux door the
`select` that musl's every `select` caller has been getting `-ENOSYS`
from (§2.6).

## What is established (before this unit)

This section describes the tree as the report found it, which is the
state the design below starts from.

**A file can answer, and no device does.** Since the named-pipes unit
`struct vnode_ops` and `struct chrdev_ops` have the optional `ready(vn,
f)`, `poll_wq(vn, f, events)` and `set_nonblock(vn, f, on)`, and the file
kobject type delegates `SYS_ioready`, `SYS_setnonblock`, `io_poll` and
the asynchronous ring to them when present; without them a file is
always readable and writable, never changes, and cannot be made
non-blocking (`-EOPNOTSUPP`). The FIFO implements all three
(`kernel/ipc/fifo.c`). ramfs's character ops forward all three
(`ramfs_chr_ready/poll_wq/set_nonblock`, `kernel-services/vfs/ramfs.c`)
to a `chrdev_ops` that has them, and none has: `console_dev_ops` and
`tty_dev_ops` (`kernel/tty/ttydev.c`) are `read`/`write` only,
`tap_chr_ops` (`kernel-services/network/tap.c`) is `open`/`release`/
`read_file`/`write_file`, `vmm_chr_ops`, `fsctl_ops` and `tap_ctl_ops`
are read-and-write. So a `poll` on an opened `/dev/tty` says "readable"
with nothing typed, an asynchronous `PREAD` on `/dev/net/tap` completes
at once with 0 bytes rather than parking, and `SYS_setnonblock` on either
is refused.

**The terminal blocks, and knows how to answer.** `tty_read`
(`kernel/tty/tty.c`) waits on `tty->readers` for a line (canonical) or
a byte (`VMIN` 1), returns 0 at once under `VMIN` 0, and answers
`-EAGAIN` instead of waiting only when the calling thread is inside an
I/O-ring entry (`io_nonblocking(false)`: the terminal has no non-blocking
bit of its own). `tty_read_ready(t)` is the readiness question answered
the same way the read answers it, and the console *object* -- the
inherited handles 0, 1, 2 (`kernel/object/console_obj.c`) -- already
reports it: `ready` = `WRITABLE | (tty_read_ready ? READABLE : 0)`,
`poll_wq` = `&tty->readers` for READABLE, no `set_nonblock`
(`init --selftest` asserts `-EOPNOTSUPP` on it: it never blocks a
writer). The *files* `/dev/console` and `/dev/tty` reach the same
terminal through `tty_read` and report nothing.

**The tap never blocks and is polled on a timer.** `/dev/net/tap`'s
`read_file` returns one frame the stack transmitted out the tap, or 0
when none waits -- "the owner polls" (`docs/audit/next-subsystem-tap.md`)
-- from a bounded queue (`struct tap::txq`, `TAP_TXQ_MAX` 64, dropping
when full) that `tap_transmit` fills in a read-side section. There is no
wait queue on it. `vmctl --net tap` reads it from its supervisor loop
between drains, sleeping `MACHINE_DRAIN_INTERVAL_NS` (2 ms) on a futex
with a timeout when nothing exits -- and "every kernel sleep is at least
one scheduler tick", so a host-to-guest frame waits up to a tick for
the poll that finds it (`userland/system/vmctl.c`, the loop over
`drain_console`/`vio_drain`/`vnet_drain`/`vnet_poll`). Reading the tap
inside the I/O ring is the same 0-at-once: the ring cannot park on an
object whose `poll_wq` is NULL.

**The other character devices do not block.** `/dev/vmm` reads one
line describing the backend, `/dev/fsctl` a result already computed,
`/dev/net/tapctl` a listing; the file type's defaults -- always ready,
readiness never changes -- are the truth for them.

**`poll` exists; `select` does not.** `io_poll` (`kernel/io/poll.c`)
waits on any set of objects through `ready`/`poll_wq`, the Linux door's
`poll` (7) and `ppoll` (271 / 73) translate onto it, and the native ABI
has no `poll` at all (the asynchronous ring is its multiplexer). Linux
`select` (23, x86-64 only) and `pselect6` (270 / 72) have no entry:
`lx_open_flags`-era tables list neither, so both fall to the unknown
handler and `-ENOSYS`. musl implements `select` with `pselect6` on both
architectures, so no musl program's `select` has ever worked here
(inventory §2.6, "found on the re-check"). `struct lx_timeval` exists;
no `fd_set` layout does.

**The tests.** `tty-pollraw` and `init --selftest`'s `probe_tty_poll_raw`
check the console object's readiness against its read; `io-poll` polls
pipes; `tap` and `tap-filter` move frames through a tap by direct calls
(`netif_transmit` then `tap_recv`); `el2-tap-host` bridges a real guest;
the `net` section of the user suite and `lxtest`'s `poll`/`ppoll` rows
poll sockets and pipes. Nothing polls a device file, and nothing
submits a device to the ring.

## The problem

### The ring's promise to devices is unverified because it is untrue

Prompt #2 §23 says asynchronous I/O "must work for files, sockets,
devices, timers, IPC, VM operations". The inventory's row for it says
the ring "drives any object with a readiness operation" and that
devices are "not shown by any test -- unverified". They cannot be
shown: a device file has no readiness operation, so a `PREAD` on the
tap does not park, it completes with nothing, and the only way to
consume the tap asynchronously is to resubmit in a loop. The
named-pipes unit built the hook and left the row as it was.

### A poll on the terminal lies, and the tap's owner sleeps in ticks

A Linux program that `poll`s an opened `/dev/tty` -- a shell waiting on
input and a socket, an editor with a timer -- is told the terminal is
readable and then blocks in the read the poll promised would not. The
tap's owner has no way to wait for a frame at all: `vmctl` samples the
tap every 2 ms floored to a tick, so the guest sees every host-to-guest
frame up to a tick late, and a userland tap daemon written against
`/dev/net/tap` has the choice between that and spinning.

### `select` is the one multiplexer musl programs use

`ppoll` works and `select` does not, and musl's `select` is `pselect6`.
A program ported from anywhere that waits with `select` -- most of the
small servers and tools a distribution userland is made of -- fails on
its first wait. The inventory names it as one of three small
Linux-door units; this report takes it because the use it has been
waiting for, a `select` over the tap and a socket, is what this unit
makes real.

## Design

### The terminal's files report what the console object reports

`/dev/console` and `/dev/tty` gain `ready`, `poll_wq` and
`set_nonblock` in `chrdev_ops`, and a per-open non-blocking bit. `ready`
is what `console_obj_ready` is -- `WRITABLE`, plus `READABLE` when
`tty_read_ready(t)` -- and `poll_wq` is `&t->readers` for READABLE (NULL
for the rest: the terminal never blocks a writer). For `/dev/tty` both
resolve the caller's controlling terminal as the read does; a caller
whose session has none is told `ERROR` by `ready` and given no queue,
which is what a read's `-ENXIO` looks like to a poller.

**The non-blocking bit is the open file's flag.** A device's per-open
mode is the `COSMO_O_NONBLOCK` bit in `file->flags`, which `open`
already stores there and which the FIFO reads at open: two helpers in
the VFS, `file_nonblocking(f)` and `file_set_nonblock(f, on)` (an atomic
read and swap of that bit, returning the previous value), are what a
device's `set_nonblock` and read paths use. The terminal's read gains
the bit as an argument -- `tty_read_nb(t, buf, len, nonblock)`, with
`tty_read` the blocking form the console object keeps -- and returns
`-EAGAIN` when the bit is set and `tty_read_ready` is false, in exactly
the place the I/O-ring case already returns it. The console object's
own contract does not change: handles 0, 1, 2 still cannot be made
non-blocking, because that object is shared by every process that
inherited it and a bit on it would be everybody's.

### The tap's file can be waited on, and blocks by default

`struct tap` gains a wait queue, `rx_wait`, woken by `tap_transmit`
after it enqueues a frame (`waitqueue_wake_all` takes no sleeping lock,
so it is safe in the read-side section the transmit runs in) and by
the tap's release. `tap_chr_ops` gains `ready` -- `WRITABLE`, plus
`READABLE` when `txq` holds a frame -- `poll_wq` (`&t->rx_wait` for
READABLE, NULL otherwise: an injected frame is never refused for want of
room, it is delivered to the stack or dropped as a NIC drops) and
`set_nonblock` (the file's flag).

**A read waits for a frame unless the open is non-blocking.** This is
the one contract change: the tap unit's read returned 0 when no frame
waited, because the owner had no way to wait. Now it has, and the
device behaves as Linux's `/dev/net/tun` does: a blocking open's read
sleeps on `rx_wait` (killable: `-EINTR` when the process is killed)
until a frame is queued; a non-blocking open's read returns 0 when
none waits, as today -- kept as 0 rather than `-EAGAIN` because the tap
unit documented it, `vmctl` relies on it, and a frame is never
zero-length so 0 is unambiguous. `vmctl` opens `/dev/net/tap` with
`O_NONBLOCK` and is otherwise unchanged; its tick-floored poll is the
follow-up this unit measures and does not take (Risks).

### `select` and `pselect6` over `io_poll`

`lx_pselect6(nfds, readfds, writefds, exceptfds, timespec, sigmask)`
(270 / 72) and, on x86-64, `lx_select(nfds, readfds, writefds, exceptfds,
timeval)` (23), both over `do_select`: the three sets, each `nfds` bits
of a 1024-bit `fd_set` (only the words `nfds` covers are read and
written), become one `io_pollfd` per set bit -- READABLE for a read
bit, WRITABLE for a write bit -- resolved as `do_poll` resolves handles
(a bit for a closed fd is `-EBADF`, as Linux answers); `io_poll` waits
with the timeout (`-EINVAL` for a negative one, NULL means forever);
the sets are rewritten with the bits that came back ready, HANGUP and
ERROR counting as readable and writable, which is how Linux's `select`
reports `POLLHUP` and `POLLERR` (the condition is reported by the read
or write that follows); the result is the number of bits set across
the three sets. **`exceptfds` is polled for nothing and always comes
back clear.** Linux's except set is `POLLPRI` -- priority data, TCP
urgent data in practice -- and no object in this tree reports a
priority event: there is no urgent-data path, and `COSMO_IO_ERROR` is
`POLLERR`, which `select` never puts in the except set. Mapping the
except bits to ERROR would put an ordinary socket error where Linux
puts out-of-band data; leaving them clear is the contract a tree with
no priority events can keep, and it is written in the door's table as
a deviation. `pselect6`'s
sixth argument is Linux's pair `{ const sigset_t *, size_t }`, applied
and restored as `ppoll` applies its mask; `select` does not update the
timeout it was given (Linux does; documented deviation), and `nfds`
above 1024 is `-EINVAL`. `lx_select` exists only where the number does.

### Lifetime, in one paragraph

Nothing new is allocated per open: the non-blocking bit lives in the
`struct file` the VFS already owns, the tap's queue lives in the tap the
open already owns and dies with it. A reader blocked in `read` holds
the file (the system call took the reference from `handle_lookup`), the
file holds the tap through its `tap_open`, and the release hook runs
only when the file's last reference drops -- so no release can run
under a blocked reader, the queue outlives every waiter by
construction, and the release has nobody to wake. A blocked reader
ends in one of two ways: a frame, or the kill that makes the wait
return `-EINTR`; closing the handle from another thread of the same
process does not end it (it drops one reference; the read holds
another), which is the same rule every blocking read in this kernel
follows. Invariant **V26** (vfs): *a device's `ready` answers the
question its `read_file` would answer with the same non-blocking bit,
and `poll_wq` is woken by every event that can change that answer.*
Invariant **N24** (network): *a tap's reader is woken by every frame
`tap_transmit` queues, and a reader blocked in the tap's read holds the
file and therefore the tap, so the tap's release never runs under one.*

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/vfs.h`, `kernel-services/vfs/vfs.c` | `file_nonblocking(f)`, `file_set_nonblock(f, on)`: the open file's `COSMO_O_NONBLOCK` bit as a device's per-open mode |
| `kernel/tty/ttydev.c`, `kernel/tty/tty.c`, `kernel/include/kernel/tty.h` | `tty_read_nb` (the bit as an argument; `tty_read` unchanged for the console object); `console_dev_ops`/`tty_dev_ops` gain `read_file`, `ready`, `poll_wq`, `set_nonblock` |
| `kernel-services/network/tap.c`, `kernel/include/kernel/tap.h` | `rx_wait`; `tap_recv_wait(t, nonblock)`; `tap_ready`, `tap_poll_wq`; `tap_chr_ops` gains the three; a blocking read waits |
| `userland/system/vmctl.c` | opens `/dev/net/tap` with `O_NONBLOCK` |
| `compat/linux/syscalls.c`, `compat/linux/nr_*.h`, `compat/linux/linux_abi.h` | `lx_select` (23, x86-64), `lx_pselect6` (270 / 72), `do_select` over `io_poll`; `LX_FD_SETSIZE`, the sigmask pair |
| `kernel/tty/ttytest.c` (`tty-devready`), `kernel-services/network/nettest.c` (`tap-ready`), `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | the two kernel tests |
| `userland/init/init.c` | a `devices` section of `init --selftest`: readiness and the ring on the tap and the terminal from user mode |
| `tests/linux/lxtest.c`, `tests/boot/run_boot_test.py` | the `select`/`pselect6` rows, a `select` over the tap and a socket; `"devices"` in `USERTEST_SECTIONS` |
| `docs/kernel/tty/{design,api,invariants,testing}.md`, `docs/kernel-services/network/{design,api,invariants,testing}.md`, `docs/kernel-services/vfs/{api,invariants}.md`, `docs/kernel/io/{design,testing}.md`, `docs/compat/linux/{api,design,testing}.md`, `docs/userland/api.md` (vmctl), README Status, the inventory (§2.6's `pselect6` and §2.11's devices row struck; the named-pipes report's risk retired) | as built |

## New APIs

```c
/* kernel/include/kernel/vfs.h */
bool file_nonblocking(const struct file *f);          /* the open's COSMO_O_NONBLOCK bit */
int  file_set_nonblock(struct file *f, int on);       /* 1/0 sets, -1 asks; returns the previous value */

/* kernel/include/kernel/tty.h */
int64_t tty_read_nb(struct tty *t, void *buf, size_t len, bool nonblock);   /* -EAGAIN when set and not ready */

/* kernel/include/kernel/tap.h */
int tap_recv_wait(struct tap *t, bool nonblock, struct mbuf **out);   /* 0 with *out the frame, or NULL: none (non-blocking) or released; -EINTR when killed waiting */
unsigned tap_ready(struct tap *t);                                    /* WRITABLE | READABLE with a frame queued */
struct waitqueue *tap_poll_wq(struct tap *t);                         /* rx_wait */

/* compat/linux: numbers */
#define LX_select   23    /* x86-64 */
#define LX_pselect6 270   /* x86-64 */  /* 72 on AArch64 */
#define LX_FD_SETSIZE 1024
```

## Migration plan

Nothing existing changes meaning except the tap's blocking read, and
its one caller (`vmctl`) opens with `O_NONBLOCK` in the same commit. The
console object keeps its contract; the terminal files gain one. `poll`
and `ppoll` on a device file start telling the truth, which a program
that polled one and then read could only be helped by. `select` and
`pselect6` answer where they answered `-ENOSYS`. No native number moves.

## Tests

| test | what it proves |
| --- | --- |
| `tty-devready` (kernel) | an opened `/dev/console` file: `ready` has no READABLE with nothing typed and READABLE after `tty_input` of a line; `poll_wq(READABLE)` is the terminal's readers queue; `io_poll` on the file with a 20 ms timeout returns 0, and returns 1 when a second thread injects a line; `set_nonblock` per open (two opens of the node, one switched, the other not: `read` `-EAGAIN` on one, the other's `ready` unchanged); the console object (handle 0's kobject) still refuses `set_nonblock`; `/dev/tty` opened from a kernel thread with no session: `ready` reports ERROR and `read` `-ENXIO` |
| `tap-ready` (kernel) | a tap created and opened as a file (the `tap` test's shape through `vfs_open` of `/dev/net/tap`): `ready` is WRITABLE alone; `poll_wq` non-NULL; a thread's blocking `read` waits (30 ms) and returns the frame once `netif_transmit` queues one; `io_poll` on the file returns 1 after a transmit; non-blocking `read` returns 0 with none queued; a process (`init --probe`, as `ipc-fifo` does it) blocked in the tap's read is killed and returns `-EINTR`, exit status 137, and the tap and its `netif` are released only after -- the reader held them (N24); `netif` count before and after |
| `init --selftest`, section `devices` (native) | `/dev/net/tap` opened `O_RDWR\|O_NONBLOCK`: `ioready` WRITABLE only; an ARP request for the gateway written; `ioready` READABLE within a bounded wait and the reply read (the tapsvc answers it); a child (`--probe`) given **the same open file** through the spawn map (each open of `/dev/net/tap` is its own tap and subnet, so a second open would see nothing of the first's frames) blocks in a `read` on it -- `O_NONBLOCK` cleared on that handle with `setnonblock`, which is per open file and so shared by both -- until the parent writes a request and the reply arrives; the parent reads nothing meanwhile (the child took the frame); **the ring**: an `AIO PREAD` on the tap parks (a `cosmo_aio_wait` with `min` 0 returns nothing) and completes with the reply after the request is written -- the constitution's devices row, shown; `setnonblock` on the tap file 0, on handle 0 still `EOPNOTSUPP`; `/dev/console` opened: `ioready` matches handle 0's |
| `lxtest` rows | `pselect6` on a pipe pair: the write end ready (1), the read end not with a zero timeout (0); after a write, readable; `nfds` 1025 `-EINVAL`; a bit for a closed fd `-EBADF`; a 20 ms timeout with nothing ready returns 0 after at least 15 ms; a sigmask admitting a pending `SIGUSR1` runs the handler and returns `-EINTR` with the old mask back; `select` (x86-64) on the same pair; **a `select` over `/dev/net/tap` and a UDP socket**: neither ready, an ARP frame written to the tap, `select` returns 1 with the tap's read bit set and the socket's clear |
| the `net` section, `el2-tap-host`, `vmctl --net tap` | unchanged: `vmctl`'s non-blocking open keeps its poll loop's contract |

**Bug-proofs, to run.** Each mutation alone on x86-64, the debug suite
booted, the file restored:

- the terminal file's `ready` not consulting `tty_read_ready` (always
  READABLE) → `tty-devready`: readable with nothing typed; the
  `devices` section: `/dev/console` disagrees with handle 0.
- the terminal file's `poll_wq` NULL → `tty-devready`: the `io_poll`
  that should wake on the injected line times out.
- the non-blocking bit shared (a static, not the file's flag) →
  `tty-devready`: the second open's read is `-EAGAIN` too.
- the tap's `ready` READABLE without a frame → `tap-ready`: WRITABLE
  alone expected; the `devices` section: READABLE before the request.
- `tap_transmit` not waking `rx_wait` → `tap-ready`: the blocked reader
  and the `io_poll` both time out; the `devices` section's `AIO PREAD`
  never completes (caught by its bounded wait).
- the tap's blocking read returning 0 when none waits (the old
  contract, ignoring the bit) → `tap-ready`: the thread's read returned
  before the transmit; the child probe returns early.
- the tap's read not killable (an unkillable wait) → `tap-ready`: the
  killed process never exits (caught by the bounded `process_wait_exit`).
- `pselect6` ignoring the write set → `lxtest`: the write end's 1 is 0.
- the sets not rewritten with the ready bits → `lxtest`: the tap's read
  bit not set on return.
- the except set mapped to ERROR → `lxtest`: a socket with a pending
  error (`SO_ERROR` set by a refused connect) reports in the except set,
  where Linux never puts it.
- `select` reading the sets past `nfds` → `lxtest`: a bit above `nfds`
  for a closed fd must not be `-EBADF`.

## Benchmarks

`USERBENCH: devices`: (1) frame-to-wake latency on the tap -- an ARP
request written, the time until the `AIO PREAD` parked on the tap
completes with the reply -- against the 2 ms poll interval (floored to a
tick) that `vmctl`'s supervisor loop imposes on every host-to-guest
frame today; the number is the latency a tap owner that waits on the
device would see instead. (2) `pselect6` against `ppoll` for one and for
64 descriptors, per call.

## Risks

**`vmctl` still polls.** Its supervisor loop services three things on
one futex-with-timeout, and making it wait on the tap as well means
either a reader thread that delivers into the virtio model under the
model's lock (the lock the loop is careful never to hold across
`cosmo_vcpu_run`) or an I/O ring beside the futex. Either is a change
to the machine's threading, which the vCPU-fairness fix (PR #112) made
deliberately simple. This unit opens the tap `O_NONBLOCK` and leaves the
loop; the benchmark's first number says what the follow-up is worth.

**The tap's read blocks now.** A program that opened `/dev/net/tap`
without `O_NONBLOCK` and relied on 0-at-once would block. The tree has
one such program and it is changed in the same commit; the tap report
is amended to say so.

**`select`'s sets are 1024 bits and the handle table is 64.** A bit at
64 or above is a closed fd and `-EBADF`, which is what Linux does with
a bit for a closed fd; a program that sets bits past its own fds gets
the error Linux would give it.

**The console object stays non-switchable.** A Linux program that
`fcntl(0, F_SETFL, O_NONBLOCK)`s its inherited stdin still gets a
silently accepted no-op (the door drops `-EOPNOTSUPP`); a program that
wants a non-blocking terminal opens `/dev/tty`. Named, not changed: the
object is shared by every process that inherited it.

## Alternatives considered

**A per-open struct for the non-blocking bit, as the FIFO has.** The
FIFO needed one for its side and ring; a device needs one bit, and the
`struct file` already carries it in `flags` from `open`. A helper pair
over that bit is one mechanism for every device instead of a small
allocation each.

**Keeping the tap's read non-blocking and adding readiness only.** It
would leave the device unlike every other device file (a blocking read
is what "readiness" is readiness *for*) and leave a Linux tap daemon
with no blocking read; the cost of changing it is one flag in one
caller.

**Building `select` on `poll` in the door as a translation of fd sets to
`pollfd`s through `do_poll`.** That is what `do_select` is, minus the
user-memory round trip: both walk the sets into `io_pollfd`s and call
`io_poll` directly.

**Taking `sysinfo` and a real `dirfd` too (§2.6's other two).** Each is
a unit of its own shape -- `sysinfo` is a struct of memory facts, a
real `dirfd` is `openat` semantics in the VFS walk -- and neither is
about waiting. They stay listed.
