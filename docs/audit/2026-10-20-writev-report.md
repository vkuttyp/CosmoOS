# writev is one write — 2026-10-20

Main turned red on the merge of #416 (`08f97a91`, run 39315782811):
`pkg-net test (aarch64)` failed `the stored index's serial after the
refusals: no line matching '^serial: 11$'`. It was the third sighting of
one failure (`docs/testing/flakes.md`, "A console line split by a kernel
log line"); the first two, on #415's heads, had been recorded with their
mechanism and an inventory line, as out of P4's scope. A defect that
turns main red is fixed (AGENTS.md), so it was. Base: main `08f97a91`.
Branch `writev-one-write`.

## The defect

The serial log has the right line with a kernel line spliced into it:
`serial: 11[ INFO] process: pid 60 'cat' exited with status 0 (23
syscalls)`, then the line's end alone. The command was `cat .../INDEX |
head -1`. `head` flushes its line-buffered stdout through musl's
`writev` of the buffered text and the new data, and the Linux
personality's `writev` (`rw_vec`, `compat/linux/syscalls.c`) made one
`write` per iovec, so the console took two writes and `cat`'s exit,
logged from another CPU, fell between them. Linux makes a `writev` one
write: to a terminal, a pipe (atomic up to `PIPE_BUF`) and a socket,
where a datagram socket sends the vector as one datagram.

## The fix

`lx_writev` takes the handle once, copies the vector in once (eight
entries on the stack, as Linux's `UIO_FASTIOV`, more from the heap),
sums that copy (`-EINVAL` past `INT64_MAX`, as Linux refuses past
`SSIZE_MAX`), takes the syscall
layer's bounce for that total (the 1 KiB stack chunk, or up to 64 KiB
from the heap), gathers the iovecs into it and hands it to the object's
`write` in one call. A vector longer than the bounce goes in
bounce-sized calls, as one long `write` does (`syscall_obj_write`). A
short write or an error stops it; bytes gathered before a fault are
still written and counted, as Linux writes what it copied. `readv` keeps
its per-iovec reads (`lx_readv`); a read's pieces cannot be interleaved
by another writer.

## Test and probe

lxtest, after its file checks, writes the same two iovecs (`"second "`,
`"line\n"`) to an `AF_UNIX` `SOCK_DGRAM` socketpair and requires one
`read` to return 12 bytes, `second line\n`. On the old code the first
datagram is `second ` alone. The check is deterministic: a datagram's
boundary does not depend on timing, unlike the console splice that
found the defect.

`tools/writev-probe.py --old` reverse-applies the fix commit's change to
`compat/linux/syscalls.c` (the lxtest check stays) in a throwaway
worktree and runs the debug boot, where lxtest runs:

| arch | mode | result |
|---|---|---|
| x86_64 | old | `LINUXTEST: FAIL dn == 12 && memeq(buf, "second line\n", 12) (7)`, the only failing check (`LINUXTEST: FAIL (1 checks)`) |
| aarch64 | old | the same line, the only failing check |
| x86_64 | fixed | `boot-test: PASS in 174.5s`, `LINUXTEST: PASS` |
| aarch64 | fixed | `boot-test: PASS in 170.1s`, `LINUXTEST: PASS` |

These are the runs after the review's change, on `1787fecb`; with it,
`make analyze`, the release build (the 2048-byte frame guard) and the
debug boot also passed on both architectures. The matrix below ran on
the first version.

## Validation

The device-lifecycle matrix on `01c7084d` (#417's first version), macOS host, both
architectures, every step exit 0 (wall seconds, the first debug step
including that architecture's build):

| step | x86_64 | aarch64 |
|---|---|---|
| `make host-test` | pass, 66 s | pass, 66 s |
| `make fuzz` | pass, 22 s | pass, 22 s |
| `make analyze` | pass, 211 s | pass, 245 s |
| debug `make test`, `QEMU_SMP=1` | pass, 143 s | pass, 404 s |
| debug `make test`, `QEMU_SMP=2` | pass, 188 s | pass, 179 s |
| debug `make test`, `QEMU_SMP=4` | pass, 163 s | pass, 174 s |
| `make test-smp2` | pass, 167 s | pass, 185 s |
| `make test-chaos` | pass, 187 s | pass, 199 s |
| `make test-harness-retry` | pass, 188 s | pass, 220 s |
| `make BUILD=release all test` | pass, 42 s | pass, 56 s |
| release `make test-pkg-net` | pass, 70 s | pass, 78 s |
| release `make test-busybox` | pass, 109 s | pass, 131 s |

The network harness passed in every debug boot (each boot's PASS
requires it). Release `test-pkg-net` includes the `serial: 11` check that
failed on main.

## Review

#417's first version read the user's iovec array twice, once to size
the bounce and once to gather, and Codex and Qodo both found what that
allows: another thread changing the array between the reads makes the
write differ from the vector that was checked, and an entry grown after
the `INT64_MAX` check is not bounded by it. The vector is now copied in
once and both passes read the copy, as Linux imports one snapshot.
Qodo also found that the probe ran `make test` without naming the build,
so a `BUILD=release` in the environment would boot without lxtest and a
fixed run would pass with the check never run: it now passes
`BUILD=debug`, and a fixed run also requires `LINUXTEST: PASS`. The fix
and the review are one commit, so `--old` reverses one.

## Records

- `docs/compat/linux/api.md`, `design.md`: `writev` is one write of the
  gathered vector; `testing.md`: the datagram check.
- `docs/testing/flakes.md`: the third sighting and the fix.
- Inventory section 19: the `writev` line struck as fixed.
