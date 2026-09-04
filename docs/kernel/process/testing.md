# Processes and User Mode: Testing

## Kernel self-tests (`kernel/process/proctest.c`)

Run from thread 0 after the SMP tests; each leaves the process count
where it found it.

### `objects`

| Step | Proves |
|---|---|
| `kobject_init` → refcount 1; get → 2; put → 1, release not called; put → release called once | reference counting and single release |
| `handle_install` returns 0, refcount +1, `handle_table_count` 1 | install takes a reference |
| lookup with READ returns the object at refcount 3; put restores | lookup takes a reference |
| lookup with WRITE on a READ-only slot → NULL; empty slot, -1, 64 → NULL | rights and bounds (P17) |
| `handle_install_at(3)` ok, again `-EBUSY`, slot 99 `-EBADF`; close 3 ok, again `-EBADF` | explicit slots and double close |
| install until `-EMFILE` after 63 more; count 64 | table is full at 64 |
| `handle_table_destroy` → count 0, refcount back to 1; final put releases | destroy drops every reference |

### `elf`

Crafted 120-byte images built by `make_elf` (one `PT_LOAD`, memsz 4096):

| Image | Expected |
|---|---|
| RX segment at `0x400000`, entry `0x400010` | 0; one segment `0x400000`+4096, entry recorded |
| RWX segment | `-ENOEXEC`, "PT_LOAD is writable and executable (W^X)" |
| RW segment, entry inside it | `-ENOEXEC`, "entry point is not inside an executable segment" |
| RX segment at `0x1000` | `-ENOEXEC` (below the user window) |
| `p_filesz` 100000 | `-ENOEXEC` (file bytes outside the file) |
| first byte `X` | `-ENOEXEC` (bad magic) |
| `e_type` = `ET_DYN` | `-ENOEXEC` |
| size 10 | `-ENOEXEC` (shorter than the header) |

### `process-reject`

128 zero bytes passed to `process_create_from_elf` → `-ENOEXEC` and no
process object; the log shows `rejected: bad ELF magic`.

### `process-user`

Runs the boot module as `init --selftest` and requires exit status 0
within 5 s, then waits for the process count to return to its baseline
(the object is released by the reaper). Skipped with a log line when
the loader found no module. The user program's checks
(`userland/init/init.c`, `selftest()`), all of which must pass for
status 0:

- **write**: 19 bytes to handle 1 → 19; zero length → 0; handle 7
  (unopened), handle 0 (stdin, no WRITE right), handle -1 → `-EBADF`;
  buffer at `0xffffffff80000000` (kernel), `0x10` (below the window),
  `0x00007FFFFFFFF000` (top of the window), `0x0000600000000000`
  (unmapped) → `-EFAULT`; length `(size_t)-1` → `-EFAULT`.
- **read**: handle 0 → 0 (EOF); handle 1 (no READ right) → `-EBADF`;
  kernel-pointer buffer → `-EFAULT`.
- **pid/yield/clock/sleep**: `getpid` > 0; `yield` → 0; a 5 ms sleep
  advances the clock by at least 5 ms and less than 200 ms; a sleep
  over one hour → `-EINVAL`.
- **mmap/munmap**: 3 pages anonymous RW → address > 0, first and last
  words read as 0 (demand-zero), written and read back; `write` of
  length 0 from it → 0; `munmap` → 0; second `munmap` → `-EINVAL`;
  length 0 and 4097 → `-EINVAL`; RWX → `-EINVAL`; non-anonymous →
  `-EINVAL`; `MAP_FIXED` at `0x10` → `-EINVAL`; `MAP_FIXED` at
  `0x0000200000000000` → that address, written; `MAP_FIXED` on the
  same page → `-EEXIST`; `munmap` of it → 0; `munmap(0x10)` → `-EINVAL`.
- **log/close/unknown**: `log` of 20 bytes → 0; kernel pointer →
  `-EFAULT`; length 4096 → `-EINVAL`; `close(7)` → `-EBADF`; `close(2)`
  → 0 then `write(2)` → `-EBADF`; numbers `SYS_COUNT`, 999999, and -1
  → `-ENOSYS`.
- **stack**: a 64 KiB local array is written at both ends through
  lazily populated stack pages.

The kernel side counts 43 system calls for this run.

### `process-fault`

Runs `init --crash`, which prints a line and writes to address 0;
requires exit status `COSMO_EXIT_FAULT` (139). The log shows
`fault: user write at 0x0000000000000000 (not present); terminating`.

## Harness markers (`tests/boot/run_boot_test.py`)

Always required: `init: hello from user mode, pid N`, `[ INFO] init
exited with status 0`, `[ INFO] boot complete`. Required whenever any
`SELFTEST:` line appears (debug builds): `USERTEST: PASS`. Release
builds disable self-tests and run only the real `init`, so the user
self-test marker is not demanded there.

## Measured results (2026-09-04, QEMU TCG, Apple Silicon host)

| Configuration | Result |
|---|---|
| debug, `-smp 4` | `SELFTEST: PASS (32 tests)`, `USERTEST: PASS`, init exits 0, ~3.0–3.6 s |
| debug, `-smp 1` | PASS |
| release, `-smp 4` | PASS (init exits 0) |
| three repeated debug boots | 3/3 |
| `make test-crash` | PASS (kernel-side fault report unchanged) |
| `make host-test` | 14/14 |
| `make analyze` | clean |
| `make reproducible` | byte-identical |

The user ELF (`out/x86_64-debug/userland/init.elf`, 57 KiB) has three
`PT_LOAD` segments (r-x, r--, rw-) and a non-executable
`PT_GNU_STACK`.

## Gaps and planned tests

- `elf_validate` compiles on the host (`ELF_HOST_TEST`) but no host
  test drives it yet; a `tests/host/test_elf.c` with the crafted cases
  and a fuzz loop over random mutations of the init image is planned.
- No fuzzing of the syscall surface from user space; a user-side
  fuzzer for argument combinations is planned once `read` has input.
- No concurrency tests for the handle table (single thread per
  process today).
- SMAP (`stac`/`clac`) is untested on `qemu64`; a run with
  `-cpu max` is planned in CI once TCG's SMAP emulation is confirmed.
- Timing bounds in the user test (5 ms sleep, 200 ms ceiling) are
  loose for TCG.
