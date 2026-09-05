# Verification infrastructure: testing

How the infrastructure is itself tested, what each piece found, and the
numbers from the tree at the end of the milestone (x86-64 and AArch64,
QEMU TCG on an Apple M-series host, 2026-09-05).

## What runs where

| Level | What | Command |
|---|---|---|
| Host fuzz | `fuzz_modelf`, `fuzz_elf`, `fuzz_pkg`, `fuzz_linux`, `fuzz_virtq`, `fuzz_cosmofs`: seeds plus `FUZZ_RUNS` mutations each, ASan + UBSan | `make fuzz` (CI: `FUZZ_RUNS=50000` on the x86-64 job) |
| Host unit | `test_modelf` `unaligned-tables` (the fuzz finding's regression) | `make host-test` |
| Target, debug | `fault-kmalloc`, `fault-blk`, `cosmofs-replay`, `syscall-fuzz`; every test's duration and the timing summary | `make test`, `QEMU_SMP=1 make test`, `ARCH=aarch64 make test` |
| Target, release | the four tests report "compiled out" and pass; no fault-injection hook, no `debug.faultinject` | `make BUILD=release test` |
| Boot harness | `boot-test: N self-tests, T ms total; slowest: …`; a test over `SELFTEST_BUDGET_MS` fails the run | `make test` |

## Fuzz targets

Each target runs its seeds, then the mutations; a run prints
`fuzz: S seed(s), C corpus input(s), N mutation(s): no failure`. At
20 000 mutations per target a full `make fuzz` takes about 10 s on the
development host (the cosmofs target dominates: every input is a mount
plus a walk).

| Target | Seeds | Code under test | Property beyond "no sanitizer report" |
|---|---|---|---|
| `fuzz_modelf` | a full synthetic module image, a bare `cosmo_module_info` | `kernel/module/modelf.c` | every accepted section and table lies inside the image |
| `fuzz_elf` | a minimal `ET_EXEC` | `kernel/process/elf.c` (`ELF_HOST_TEST`) | every accepted segment lies inside the image and the user window |
| `fuzz_pkg` | a manifest, an index, a tar, a version | `pkg/manifest.c`, `version.c`, `tar.c` | every tar member lies inside the buffer |
| `fuzz_linux` | sockaddrs, a dirent batch, flag words | `compat/linux/convert.c` | converted records never exceed the caller's capacity |
| `fuzz_virtq` | a well-behaved add/complete/pop program | `drivers/virtio/virtqueue.c` | only in-flight cookies are ever popped; `num_free ≤ size` |
| `fuzz_cosmofs` | `cosmofs_format` of a 64-block device | `cosmofs_core.c`, `cosmofs.c`, `crc32c.c`, `slab.c`, `kmalloc.c` | mount either fails or every reachable directory and file walks and reads; unmount leaves no vnode |

**Finding.** The first run of `fuzz_modelf` (iteration 2 of the
mutations) produced a UBSan misaligned-load report: `e_shoff` was
bounds-checked but not alignment-checked and the section header table
is read in place. `modelf.c` now rejects an unaligned `e_shoff` and an
unaligned `sh_offset` of any structured section (symbol table,
relocations, `.cosmo.module`, `.ksymtab`); `test_modelf` gained
`unaligned-tables`. On the target the same image would have been an
unaligned read that x86 tolerates and AArch64 may not, from a signed
module: a reject, not a trust boundary breach, but a real defect.

**Reproducing a crash.** The driver saves the input to
`out/<arch>-<build>/fuzz/crash-<n>`; `out/…/fuzz/fuzz_modelf
out/…/fuzz/crash-<n>` replays it once. `make fuzz FUZZ_SEED=7` reruns a
different mutation sequence; the same seed and count give the same
sequence (invariant F7).

## Fault injection

`fault-kmalloc` (x86-64 debug boot):

```
selftest: fault-kmalloc: module load 0 ok / 6 -ENOMEM under one injected failure each
selftest: fault-kmalloc: 36 files created / 4 -ENOMEM, 7 sockets / 5 -ENOMEM, heap 1082 objects before and after
```

The file loop injects one failure at the i-th allocation of each attempt
(i = 1 … 40): the first four attempts fail at the first four allocation
sites of the create-and-write path, the rest run clean. The socket loop
fails every second allocation with a budget of five and requires exactly
five failures and seven successes. The module loop fails the 2nd … 7th
allocation of `module_load`, all of which are on the path, so every
attempt returns `-ENOMEM`; a clean load happens when i exceeds the
path's allocations, which six attempts do not reach. The heap's
live-object count is the same before and after.

**Finding.** The first run of the file loop faulted the kernel at
address 0x68: `ramfs_new` allocates the vnode and then its private data,
and when the second allocation failed it dropped a vnode whose `ops` was
still NULL; `vnode_release` called `ops->writepage`. `vnode_release` now
guards both calls on `ops` (`kernel-services/vfs/vfs.c`); the file loop
is the regression test (attempt 2 hits exactly that allocation).

`fault-blk`:

```
selftest: fault-blk: 6 writes ok / 4 with -EIO under injected completion errors; remount clean, generation 7
```

Eight completion errors over ten create-write-sync rounds, three
submission errors over one more, a forced unmount, a clean remount that
reads back every file it shows.

## Crash consistency

```
cosmofs-replay: sync 0 committed at log entry 12 (returned at 13)
cosmofs-replay: sync 1 committed at log entry 26 (returned at 27)
…
selftest: cosmofs-replay: 75 writes recorded over 5 sync points; 139 prefix images mounted and checked
```

Seventy-five entries (writes and flushes) over five syncs and the final
unmount; every prefix is sampled (the log is under 256 entries) and every
multi-sector last write also in its torn variant: 139 mounts in 1.3 to
1.6 s. No filesystem defect was found: every prefix mounted, and every
file committed at the last superblock write in the prefix read back.

The harness itself needed two corrections during bring-up, recorded
because they are the kind of oracle error a reader should look for: the
expected paths were stored with the mount prefix and prefixed again
(every check hit a nonexistent path), and a sync point was first taken to
commit at the length the log had when `vfs_sync` returned, which includes
the trailing flush. The flush changes nothing on the device, so the
prefix ending right after the superblock write already shows the new
root and the oracle reported a phantom failure (a 9000-byte file where
the previous root's 100-byte one was expected). The commit point is now
the last data-bearing write, and a torn commit write accepts either root
(`design.md`).

## Syscall fuzzer

```
USERTEST: syscall-fuzz ok: 20000 calls, 16779 errors, 3221 successes, 47/56 system calls exercised, seed 20260905
```

1.8 to 1.9 s per boot on either architecture. The kernel survived on the
first run; the nine excluded calls are the blocking and process-ending
ones (`api.md`). The error count is the point: sixteen thousand rejected
argument combinations, each a clean errno.

## Timing

```
boot-test: 95 self-tests, 11567 ms total; slowest: net-lo-tcp 2775 ms, syscall-fuzz 1843 ms, cosmofs-replay 1625 ms, net-lo-tcp-loss 1075 ms, process-user 797 ms
SELFTEST: timing total=11315 ms slowest=net-lo-tcp (2753 ms)
```

Nothing is within a factor of two of the 8 s budget. The four new tests
add about 3.5 s to a debug boot; the boot test as a whole is 18 to 22 s
under TCG.

## Results

| Run | Result |
|---|---|
| `make fuzz` (6 targets × 20 000) | PASS, no failure |
| `make host-test` | PASS |
| `make test` x86-64 | `SELFTEST: PASS (95 tests)` |
| `QEMU_SMP=1 make test` x86-64 | `SELFTEST: PASS (95 tests)` |
| `make ARCH=aarch64 test` | `SELFTEST: PASS (95 tests)` |
| release, crash, no-enforce, analyze, reproducible (both architectures) | see the PR's verification log |

## Gaps

- The network input path (`ipv4_input`, `tcp_input`) has no host fuzz
  target; it needs the mbuf pool, the timer wheel and a socket to
  deliver into. The plan is a `shim_net.c` like `shim_fs.c`.
- Fault injection targets the caller's thread only; the network worker,
  the reaper and timer callbacks are exercised only through the boot
  parameter by hand.
- `cosmofs-replay` checks durability and integrity, not space accounting
  and not the absence of uncommitted data.
- The syscall fuzzer runs one process with a fixed seed. A second seed
  per boot, or a longer run under `make fuzz`-style opt-in, would widen
  it; the excluded blocking calls need a watchdog thread.
- `FUZZ_ENGINE=libfuzzer` is built and run by hand on Linux; CI uses the
  portable driver, whose mutations are simpler than libFuzzer's
  coverage-guided ones.
