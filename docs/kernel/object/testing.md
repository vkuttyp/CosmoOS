# Kernel Object Model and Handles: Testing

## What runs where

| Level | What | Command |
|---|---|---|
| Target self-tests (debug builds, `CONFIG_SELFTEST=1`) | `objects`, `rlimit`, `vfs-put-race`, `epoll-close`, `unix-handles`, `wb-error-close`, `process-rlimit`, `process-user`, `dirfd-rights` (table below) | `make test` |
| User-mode suite (`/boot/init --selftest`, `USERTEST:` lines) | sections `proc` (generic rights) and `net` (socket rights) in `userland/init/init.c` | `make test` |
| Linux personality (`lxtest`, `LINUXTEST:` lines) | the exec section: close-on-exec slots (L17) | `make test` |
| C11 model (herd7, RC11) | `tests/litmus/epoll/watched.litmus`: `handle_close`'s fence against epoll's add (OB5) | `make litmus` |
| Defect probe | `tools/epoll-close-probe.py`: rebuilds `kernel/object/handle.c` without the last-close epoll notice (`--old`) and shows `epoll-close` fail | `python3 tools/epoll-close-probe.py --arch x86_64 [--old]` |

`tests/host/test_handles.c` compiles `kernel/object/` on the host
(`make host-test`, below). The cosmofs and network fuzzers replace it with
a plain-count shim (`tests/fuzz/shim_fs.c`, `tests/fuzz/shim_net.c`), so
`make fuzz` exercises callers of this code, not this code.

## The object census

`kobject_census` counts kobjects initialised and not yet released or
discarded, by type (`api.md`). The self-test runner takes it before each
test and, after the test and its deferred releases, requires that no type
has more live objects than before; a release that comes late (a reaped
process's handles, a deferred free) is given two seconds. A failure names
the type: `selftest: <test> left kernel objects of type <type>: <n> live
before it, <m> after`. Fewer is logged, not failed: a test may remove
what an earlier one made (`ahci-stop-ack` removes a disk), or an earlier
test's release may arrive late.

What a test leaves on purpose is listed, with the reason, in
`census_allowed` (`kernel/core/selftest.c`):

| Test | Type | Why |
|---|---|---|
| `process-user` | `vnode` | the files the user-mode suite creates stay on the root filesystem |
| `process-user` | `mount` | a mount the suite leaves (Gaps) |
| `process-user` | `process` | one process object, not yet attributed (Gaps) |
| `syscall-fuzz` | `vnode` | the files twenty thousand random system calls create stay |

When the census was added (2026-10-29) it found two kinds of leftover,
both fixed: creation failures that freed an initialised object by hand
(`process_unbuild`, `vcpu_create`), which now `kobject_discard` it; and
tests that never dropped their creator's reference (`device`,
`device-remove-busy`, `dma`, `device-reset`, `net-dhcp-client`) or left a
device node behind (`vfs-chrdev-open`, `vfs-chr-write-during-blocked-read`).

## Host test (`tests/host/test_handles.c`)

| Test | Steps | Proves |
|---|---|---|
| `object-refcount` | init, get, get_n, tryget, puts to zero: released once; tryget from zero false; a get from zero and an underflow panic | OB1, OB2; the census |
| `object-discard` | discard of a fresh object leaves the census as before and runs no release; discard of a shared one panics | OB2's exception |
| `object-put-and-lock` | from 2 without the lock; from 1 with the lock held, the release only at `kobject_release_final` | OB3 |
| `handle-table` | install, a missing right, install-at, close-on-exec, a close returning the flush's `-EIO`, the exit's gate | OB4, OB6, OB7, OB8 |
| `handle-trace` | the recorded trace (below) | everything above, in combination |

**The recorded trace** (roadmap 2.0 SA2's differential test). 10,000
operations, chosen by a xorshift generator from a fixed seed, over three
tables and eight objects (half with an I/O type whose flush can fail,
some marked as watched by epoll): installs with rights and flags,
install-at, lookups with and without `missing`, get and get-flags,
set-flags, close, close-on-exec, transfer checks, limit changes,
references taken (get, tryget) and dropped (put, put_and_lock), creators
letting go, and tables destroyed and remade. A released object is made
again. Each operation's result and the counts it leaves (references,
handles, releases, flushes, epoll notices) are one line; the lines' FNV-1a
digest is recorded in the test (`TRACE_DIGEST`). An implementation of this
layer in another language must give the same digest before the C is
deleted. `HANDLES_TRACE_DUMP=1 out/<arch>-debug/host/test_handles` prints
the lines, to find the first that differs. At the end every table is
destroyed and every reference dropped: each object released exactly once
and the census back where it started. In the recorded run every operation
both succeeds and fails in each of its ways at least once (closes: 325
ok, 47 `-EIO`, 695 `-EBADF`). The object layer in CeeSafe (`object.cee`,
SA2's second step) gives the recorded digest with the C handle table, and
so does the handle table in CeeSafe (`handle.cee`, the third step) over it
(2026-10-30).

## Target self-tests

All are registered in `kernel/core/selftest.c` and print
`SELFTEST: <name> ... ok|FAIL`.

| Test | File | Steps | Proves |
|---|---|---|---|
| `object-fault-sweep` | `kernel/core/faulttest.c` | for each of a pipe, an eventfd, an epoll instance watching a pipe end, a TCP socket, a unix socket pair, a created file and a spawned process (init `--exec-done 0`): allocation N of the creating thread fails, for N = 1, 2, … until an attempt runs with none failed; each attempt installs what it made in a table, destroys the table (and unlinks the file) and requires the object census back where it was, within two seconds; every result is a success or `-ENOMEM` | no leak on any failure path of object creation (roadmap 2.0 SA2) |
| `objects` | `kernel/process/proctest.c` | a counted object: init 1, get 2, put 1 (no release), put 0 (released once); a table: install at 0 takes a reference; `handle_lookup` with READ returns it at 3; WRITE on a READ slot, an empty slot, -1 and 64 are NULL; `handle_lookup_rights` sets `missing` only for the held-but-unrighted slot; `handle_install_at(3)` then `-EBUSY`, slot 99 `-EBADF`; close 3 then `-EBADF`; 63 more installs, the next `-EMFILE`, count 64; destroy: count 0, and lookup, get, install and install-at all fail afterwards; the creator's put releases once | OB1, OB2, OB6, OB7, OB8; P17; S9's gate |
| `rlimit` | `kernel/memory/memtest.c` | a table's `limit` defaults to 64; at `limit` 2 the third install of the console object is `-EMFILE`, a close makes room; at 64 slots 2..63 install in order and the 65th is `-EMFILE`; destroy | OB6, OB12 |
| `vfs-put-race` | `kernel-services/vfs/vfstest.c` | up to eight racers on CPUs 1..n-1 drop the same vnode at the same instant through `vnode_put` (`kobject_put_and_lock`); the vnode count returns to its baseline. Skipped with fewer than three CPUs | OB3 |
| `epoll-close` | `kernel/io/epolltest.c` | a pipe end, an eventfd and a TCP socket closed while registered: the registration leaves with the last descriptor, the refcount returns to the test's own; a `dup` keeps it; a close under a blocked wait | OB4, OB5; A9 |
| `unix-handles` | `kernel/ipc/unixtest.c` | among the unix message checks, `handle_transfer_check` on a table: SAME returns the held rights, a subset is honoured, a wider set and a handle without TRANSFER are `-EPERM`, a free slot `-EBADF` | S9 (transfer rule) |
| `wb-error-close` | `kernel-services/vfs/vfstest.c` | a file whose write-back fails: `close` returns `-EIO` from `flush` and the handle is closed | OB8 |
| `process-rlimit` | `kernel/process/proctest.c` | runs `init --probe rlimit-root` and an unprivileged probe: `pipe` is `-EMFILE` at a NOFILE limit of 4 with three handles open; NOFILE above 64 is `-EINVAL`; raising it unprivileged is `-EPERM`; with `/dev/vmm` available, VM and vCPU copies missing one type right get `-EPERM` | OB6, OB9 |
| `process-user` | `kernel/process/proctest.c` | runs the user-mode suite (below) | P17, P18, S9 |
| `dirfd-rights` | `kernel/process/proctest.c` | a directory handle narrowed to READ stays narrowed through the Linux door (`lxcwd`); skipped when `lxcwd` is not in the boot archive | S9 for directory handles (P31) |

## User-mode suite (`userland/init/init.c`)

| Section | Steps | Proves |
|---|---|---|
| `proc` | a READ-only copy of a read-write file: `write` is `EBADF`, `read` works; widening it, `dup` of it, giving it to a child, and `cosmo_setnonblock` are `EPERM`; a copy keeping DUP and TRANSFER can be duplicated and given to a child and is no wider; the pre-rights two-int spawn map still works | S9 |
| `net` | from a socket with every right, copies missing exactly one of BIND, CONNECT, ACCEPT, SHUTDOWN: the matching operation is `EPERM` | OB9 |

The suite prints `USERTEST: section <name> <ms>` per section and
`USERTEST: PASS` at the end (`docs/development.md`).

## Running

```sh
make test                      # every self-test and the user-mode suite, 4 CPUs
QEMU_SMP=1 make test           # one CPU (vfs-put-race skips)
make test-smp2                 # two CPUs (vfs-put-race skips)
make test-chaos                # the same under the chaos configuration
make ARCH=aarch64 test         # AArch64
make litmus                    # watched.litmus among the litmus tests (needs herd7)
```

On macOS use `gmake` (`AGENTS.md`). There is no switch to run a single
self-test; a boot runs the whole registered list.

## Gaps

- The host test drives OB1's panics; nothing drives OB11's (an object
  over-reporting a count).
- `process-user` leaves one process object and one mount alive (the
  census allows them); neither is attributed yet.
- No test performs I/O on a plain kobject such as a vCPU handle (OB10).
- `handle_get_flags` and `handle_set_flags` on an exiting table are
  untested (OB7).
- The process-table and unix-socket `kobject_tryget` paths of OB3 are
  checked by review only.
- No test reads `kobject.handles` directly (OB4).
