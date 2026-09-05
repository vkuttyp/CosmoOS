# Verification infrastructure: architecture

Milestone 4 of the post-roadmap plan (`docs/audit/2026-09-post-roadmap-audit.md`
§19; closes finding #33: "zero fuzzing, fault injection, crash-consistency
or concurrency tests"). The audit's security argument for every parser was
"bounds-checked by inspection"; this subsystem makes it an argument by
machine.

## Where it sits

```
  host (make fuzz, make host-test)                target (make test, debug builds)
  ────────────────────────────────                ─────────────────────────────────
  tests/fuzz/driver.c   portable corpus+mutation   kernel/core/faultinject.c   kmalloc / block-I/O faults,
                        driver, or libFuzzer                                   per thread, read by sysctl
  tests/fuzz/fuzz_*.c   one LLVMFuzzerTestOneInput kernel/block/ramblk.c       RAM block device with a
                        per parser: module ELF,                                write recorder and snapshots
                        user ELF, pkg manifest/    cosmofscrash.c              record → replay prefixes →
                        index/tar, Linux ABI                                   mount → check (crash consistency)
                        conversions, virtqueue     userland/init --syscall-fuzz  random system calls from an
                        (hostile device), cosmofs                              unprivileged process
                        image (mount + walk)       selftest.c                  per-test timing and summary
  tests/host/shim_fs.c  pool / VFS / mutex shims   run_boot_test.py            timing table, slowest tests
                        for cosmofs on the host
```

Everything on the target side is debug-build only (`CONFIG_DEBUG`); the
release kernel carries none of it.

## Purpose

- **Fuzzing** turns "every parser is bounds-checked" into a property that
  is exercised on every CI run: each parser that takes untrusted bytes
  (module images, user executables, packages, Linux ABI structures, the
  device-writable virtqueue rings, cosmofs on-disk images) runs under
  ASan and UBSan against seeds, a corpus and bounded mutations, and under
  libFuzzer where the toolchain has it.
- **Fault injection** exercises the error paths that positive tests never
  reach: an allocation that fails, a block request that errors. The
  failures are targeted at the calling thread so the rest of the kernel
  keeps running, and the tests check that the operation fails cleanly and
  leaks nothing.
- **Crash consistency** checks cosmofs's copy-on-write promise the way a
  power cut would: record the write stream of a workload, replay every
  prefix (and torn last writes) onto the pre-workload image, and require
  each result to mount and to contain every file the workload had
  committed by then.
- **The syscall fuzzer** throws random system-call numbers and arguments
  at the kernel from an unprivileged process; the kernel must answer every
  one with a value or an errno and keep running.
- **Timing** makes every self-test's cost visible so a regression in a
  test's duration is noticed like a regression in its verdict.

## Responsibilities

- Build and run the fuzz targets on any host with `make fuzz`, with a
  fixed seed so a run is reproducible, and with libFuzzer when
  `FUZZ_ENGINE=libfuzzer` and the compiler provides it (Linux CI).
- Provide programmatic seeds for every target (a valid module image, a
  valid user executable, a valid manifest and tar, a formatted cosmofs
  image), so no binary corpus is committed.
- Inject faults into `kmalloc`/`kmem_cache_alloc` and into block
  submission and completion, per kind, per thread, with a period and a
  budget; report the configuration and the hit counts through `sysctl`.
- Provide a RAM block device that records its write stream, snapshots
  its contents and replays a recorded prefix onto a snapshot.
- Run the crash-consistency harness as a self-test on every debug boot.
- Run the syscall fuzzer as a self-test on every debug boot, bounded and
  seeded.
- Print per-test timings and a summary; have the boot harness surface the
  slowest tests.

## Non-responsibilities

- Coverage measurement, benchmarks and statistics transport (later
  milestones).
- Fuzzing the network input path on the host: `ipv4_input`/`tcp_input`
  need the mbuf pool, timers and sockets; the milestone fuzzes the ABI
  conversions and defers the stack itself to a target-side harness (a gap
  `testing.md` records).
- A write path for `sysctl`: fault injection is set by a boot parameter
  (`opt/cosmo/faultinject`) or the kernel API; `sysctl` reads it. Adding a
  settable sysctl is a system-call ABI decision this milestone does not
  take.
- Block-level (bitmap) consistency checking of cosmofs images; the harness
  checks what a user sees (every committed file is present and readable,
  every directory walks) and what the on-disk headers guarantee (kind and
  checksum of every block reached).

## Interfaces at a glance

| Interface | Where | Notes |
|---|---|---|
| `make fuzz [FUZZ_RUNS=N] [FUZZ_ENGINE=libfuzzer]` | `tests/fuzz/fuzz.mk` | builds and runs every target |
| `int LLVMFuzzerTestOneInput(const uint8_t *, size_t)` | each `tests/fuzz/fuzz_*.c` | the target |
| `size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)` | each target | programmatic seeds, `0` past the last |
| `faultinject_set(kind, every, count, only_this_thread)` | `kernel/faultinject.h` | debug builds |
| `sysctl("debug.faultinject")` | `kernel/syscall/native.c` | configuration and hits |
| `ramblk_create/destroy/record_start/record_stop/snapshot/replay` | `kernel/ramblk.h` | debug builds |
| `init --syscall-fuzz N SEED` | `userland/init/init.c` | the guest fuzzer |
| `SELFTEST: name ... ok (N ms)`, `SELFTEST: timing …` | `kernel/core/selftest.c` | parsed by `run_boot_test.py` |

Documents: `design.md`, `api.md`, `invariants.md`, `testing.md`.
