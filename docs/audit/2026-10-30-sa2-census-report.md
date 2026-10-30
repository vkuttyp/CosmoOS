# SA2, first PR: the object census and a recorded handle trace, 2026-10-30

Roadmap 2.0 SA2 converts the kernel object and handle layer (`kernel/object/object.c`, `handle.c`) to Nova. Its
acceptance asks for a fault-injection sweep with no leak in the object census, and a differential host test against
the C on a recorded trace of 10,000 operations before the C is deleted. Neither existed: there was a network census
but no object census, and no host test compiled `kernel/object/` (`docs/kernel/object/testing.md`). This PR adds
both, in C, so the conversion has something to be held to. The owner chose, on 2026-10-30, to convert the layer with
Nova-owned types, and to land the census first.

## Scope and baseline

Branch `sa2-object` from main at `22df0a3c` (#490).

## Audit (main at `22df0a3c`, read, not copied)

| Item | Where | What it means |
|---|---|---|
| `object.c` (152 lines): counts, release, put_and_lock, I/O dispatch; `handle.c` (266 lines): a 64-slot table under one spinlock, rights, flags, the exit gate | `kernel/object/` | what SA2 converts |
| `struct kobject` is embedded in every object; `struct handle_table` in `struct process` | headers | the C layout stays |
| Outside the layer, C writes only `handle_table.limit` (the NOFILE limit) and `kobject.watchers` (epoll, under its lock) | `process.c`, `memtest.c`, `epoll.c` | the surface a Nova-owned table must keep |
| Neither file allocates | `kernel/object/` | the fault sweep fails the creators' allocations, not this layer's |
| "Every type has a release; the release frees the object's memory and nothing else does" | `object.h` | the rule the census turned out to check |

## What changed

- **The census** (`object.c`, `object.h`): `kobject_census()` and, by type, `kobject_census_by_type` and
  `kobject_census_name` (128 slots; a type's name is copied, so a module's type can be named after the module is
  unloaded). Raised by `kobject_init`, lowered after the release and by the new `kobject_discard`.
- **`kobject_discard`**: for an object never published (count 1) that its creator frees by hand. It balances the
  census and the owner module's live-object count, which was left raised before (harmless for the kernel's own
  types, a leak for a module's).
- **The self-test runner** (`selftest.c`): no type may end a test with more live objects than it started with
  (two seconds for late releases), except the leftovers in `census_allowed`, each with its reason.
- **What the census found**, fixed here:
  - `process_unbuild` (every failed process creation: an `-ENOEXEC` image, an `RLIMIT_NPROC` refusal) and
    `vcpu_create`'s two failure paths freed an initialised object by hand. They now `kobject_discard` it.
    `process-nproc` alone had left twelve.
  - Tests that never dropped their creator's reference: `device`, `device-remove-busy`, `device-reset` (static
    devices), `dma` (a stack device, now discarded) and `net-dhcp-client` (the interface, as the other network
    tests drop it).
  - Tests that left a character-device node in `/dev`: `vfs-chrdev-open` (and its reference),
    `vfs-chr-write-during-blocked-read`.
- **Allowed, with reasons:** `process-user` (vnodes of the files the user-mode suite leaves, one mount, one process
  object) and `syscall-fuzz` (vnodes of the files its random calls create). The mount and the process object are not
  yet attributed (testing.md, Gaps).
- **`tests/host/test_handles.c`** (`make host-test`): the layer compiled on the host with three shims (module owner,
  epoll notice), direct checks of OB1–OB8 including OB1's panics, and the recorded trace: 10,000 seeded operations
  over three tables and eight objects; each result and the counts it leaves is a line, and the lines' FNV-1a digest
  is recorded (`0xfd5772457a0ad4a1`). In the recorded run each operation succeeds and fails in each of its ways (for
  example closes: 325 ok, 47 `-EIO` from a flush, 695 `-EBADF`; transfers: 66 ok, 120 `-EPERM`, 343 `-EBADF`), with
  240 releases and 65 epoll notices. The trace ends with every object released once and the census at its start.
- Docs: `docs/kernel/object/api.md` (`kobject_discard`, the census), `invariants.md` (OB2's exception),
  `testing.md` (the census, the host test, the trace, gaps), a history entry.

## Tests (run 2026-10-30, macOS host, QEMU)

| Test | Result |
|---|---|
| `gmake ARCH=aarch64 test` (debug: lockdep on) | PASS in 202.6 s; 523 self-tests; the census clean |
| `gmake ARCH=x86_64 test` | PASS in 204.7 s; 523 self-tests; `ahci-stop-ack`'s removal logged |
| `gmake ARCH=aarch64 analyze`, `gmake ARCH=x86_64 analyze` | clean (7 baselined, 0 unexpected, 0 stale) |
| `gmake host-test` | PASS; `handle-trace` digest `0xfd5772457a0ad4a1` |
| `gmake check-docs` | PASS |

An AArch64 boot run while the host was loaded (load average 8) passed every self-test but went over two time budgets
(`cosmofs-replay` 48.4 s of 40, `module-cycle` 8.5 s of 8); alone it took 19.4 s and 4.8 s. During the work: a census that failed every test changing the count found 12
failing tests on AArch64 and 2 more on x86-64 (`hv-guest-spin`: `vcpu_create`; `ahci-stop-ack`: a removal, now
logged rather than failed); after the fixes, 523 self-tests pass on both.

## Not covered

- The fault-injection sweep over object creation: SA2's later PR, with the Nova layer.
- Leaks in paths no self-test reaches: the census sees only what runs.
