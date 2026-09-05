# Verification infrastructure: design

## 1. Fuzzing (`tests/fuzz/`)

### Targets

Each target is one file defining `int LLVMFuzzerTestOneInput(const uint8_t
*data, size_t size)` (libFuzzer's contract) and `size_t fuzz_seed(unsigned
i, uint8_t *buf, size_t cap)`, which writes the i-th programmatic seed and
returns its length, or 0 past the last. Seeds are built by code, not
committed as binaries: a valid module image (the builder shared with
`test_modelf`), a valid static user executable, a valid manifest, index and
tar, a formatted cosmofs image, a few Linux ABI structures. A target must
never crash, hang or read out of bounds on any input; a parser rejecting
the input is the expected outcome and is not checked, except where the
target adds assertions (below).

| Target | Code under test | Extra assertions |
|---|---|---|
| `fuzz_modelf` | `modelf_validate`, `modelf_check_info` (`kernel/module/modelf.c`, `MODELF_HOST_TEST`) | a layout returned with 0 describes sections inside the image |
| `fuzz_elf` | `elf_validate` (`kernel/process/elf.c`, `ELF_HOST_TEST` leaves out `elf_load_into`) | every segment reported lies inside the image and the user window |
| `fuzz_pkg` | `manifest_parse`, `index_parse`, `version_parse`, `depend_parse`, `path_allowed`, `hex_decode`, `tar_open`/`tar_next` (`pkg/`) | a tar member's data lies inside the buffer |
| `fuzz_linux` | `lx_sockaddr_to_netaddr`, `lx_sockaddr_from_netaddr`, `lx_dirents_from_native`, `lx_open_flags`, `lx_prot` (`compat/linux/convert.c`) | the output length never exceeds the capacity given |
| `fuzz_virtq` | the split virtqueue (`drivers/virtio/virtqueue.c`) against a device model driven by the input | `virtq_pop` returns only cookies the driver added and not yet reclaimed; `num_free` never exceeds the size |
| `fuzz_cosmofs` | cosmofs mount and tree walk (`cosmofs_core.c`, `cosmofs.c`) over a memory pool holding the input as the image | every block a header validates has a plausible kind; the walk terminates |

`fuzz_virtq` interprets the input as a program: one byte selects an
operation (add a chain of n buffers, the device completes a used element
with an id and length from the input, the driver pops, the device rewrites
a descriptor field, the device advances `used->idx` by k), the following
bytes are its operands. The device side writes the shared ring exactly as
`test_virtq` does; the driver-private records must decide what happens.

`fuzz_cosmofs` runs the real filesystem code on the host: `cosmofs_core.c`
and `cosmofs.c` compile unchanged against `tests/fuzz/shim_fs.c`, which
supplies the storage pool over a memory image (`pool_open/read/write/flush`),
the VFS services the glue calls (`vnode_alloc`, `vnode_hash_insert`,
`vnode_lookup_cached`, `vnode_get/put`, `vnode_sync`, `vfs_now_ns`,
`pagecache_*`, `cred_current`), a no-op mutex and a plain-count kobject;
the heap is the kernel's own slab and kmalloc over the harness's page
arena. The target mounts the image, walks every directory through the
`vnode_ops` (lookup, readdir), reads the first pages of every regular
file, and unmounts. Inputs are one image of `CFS_MIN_BLOCKS` (64) blocks;
the seed is `cosmofs_format` on the memory pool.

### Driver

`tests/fuzz/driver.c` is the portable engine used where libFuzzer is not
available (Apple clang ships no fuzzer runtime) and for the bounded run in
CI: it loads every programmatic seed and every file in an optional corpus
directory, runs the target on each, then runs `FUZZ_RUNS` iterations
(default 20 000) of mutations drawn with a seeded xorshift generator (bit
flips, byte sets, interesting values, chunk insert, delete, duplicate,
truncate, extend, splice of two inputs), on inputs bounded by
`FUZZ_MAX_LEN`. Any crash is an ASan or UBSan report (non-zero exit);
inputs that crash are written to `out/<arch>/fuzz/crash-<target>-<n>` for
replay with `driver <target> <file>`. `FUZZ_ENGINE=libfuzzer` builds the
same target sources with `-fsanitize=fuzzer` instead of the driver, for
long runs on Linux. The same seed and run count give the same sequence, so
a CI failure reproduces locally.

## 2. Fault injection (`kernel/core/faultinject.c`, debug builds)

```c
enum fi_kind { FI_KMALLOC, FI_BLK_SUBMIT, FI_BLK_COMPLETE, FI_KIND_COUNT };
struct fi_rule { unsigned every; unsigned budget; struct thread *only; uint64_t seen, hits; };
```

A rule fails every `every`-th eligible event (1 = every one), at most
`budget` times (0 = unlimited), for the thread `only` or for every thread
when NULL. Eligible events: `kmalloc` and `kmem_cache_alloc` (the large-page
path included, so `kzalloc`/`krealloc` are covered) return NULL;
`blk_submit` returns `-EIO` before the driver sees the bio;
`bio_complete` turns a successful completion into `-EIO`. The check is a
few loads on the hot path in debug builds and compiles out in release.

Configuration: the kernel API (`faultinject_set`, `faultinject_clear`,
`faultinject_stats`) for self-tests, and the boot parameter
`opt/cosmo/faultinject` (fw_cfg), `kind:every[:budget]` entries separated
by commas, applied before the self-tests run, for manual experiments.
`sysctl("debug.faultinject")` reports each kind's rule and counters. There
is no write path through `sysctl`: the audit's phrase "behind a debug
sysctl" is honoured for observation; making `sysctl` writable is a
system-call ABI decision left to a later milestone and recorded in
`invariants.md`.

Targeting the calling thread is what makes injection usable: a global
allocation failure rate would hit the network worker, the reaper and the
timer paths at once, several of which panic on an allocation failure by
design (boot-time enumeration, thread creation for essential workers).
The tests inject into their own thread only and check two properties: the
operation under test fails with a clean error (`-ENOMEM`, `-EIO`) or
succeeds, never anything else, and after the rule is cleared and the
successes undone the live-object count of the heap is what it was.

## 3. The RAM block device and the write recorder (`kernel/block/ramblk.c`, debug builds)

`ramblk_create(nblocks)` registers a `struct blkdev` named `ram<letter>`
whose storage is `nblocks` kmalloc'd 4 KiB blocks (each DMA-mappable, so
`blk_submit`'s check passes); `submit` copies and completes synchronously
(as the `blk-lifetime` fake does); `release` frees the storage. Three
debug operations exist for the harnesses:

- `ramblk_record_start/stop`: while recording, every completed write and
  flush is appended to a log (`sector, nsectors, data copy` or a flush
  marker) in completion order.
- `ramblk_snapshot/restore`: copy the whole device to a kmalloc'd image
  and back.
- `ramblk_replay(bd, log, count, torn)`: apply the first `count` entries
  of a log to the device; with `torn`, the last write is applied only up
  to half its sectors (a torn sector write of the kind a power cut leaves).

## 4. Crash consistency (`kernel-services/filesystem/cosmofs/cosmofscrash.c`, self-test `cosmofs-crash`)

The property cosmofs promises (`docs/kernel-services/filesystem/cosmofs/design.md`):
a committed generation is never partially visible; after any interruption
the device mounts to the last committed root or the one before it, and
everything committed then is intact. The harness checks it the way a power
cut would:

1. Create a RAM device, format it, snapshot the image (`I₀`).
2. Mount, start recording, run a workload with `S` sync points: create
   files and directories with recognisable contents, rewrite, rename,
   unlink, and after each step `vfs_sync`. After each sync record the
   expected set of paths and contents (`E₁ … E_S`) and the write-log length
   at that moment (`L₁ … L_S`). A sync's last entries are flushes, which
   change nothing on the device: its *commit point* `C_s` is the length
   up to and including the last data-bearing write (the superblock).
3. Unmount (a final commit), stop recording: the log `W[0..n)`.
4. For every prefix length `k` in the sample (every entry when `n ≤ 256`,
   else every entry around each sync point plus a stride elsewhere) and
   for both `torn = false` and `torn = true`: restore `I₀`, replay
   `W[0..k)`, mount. The mount must succeed. Let `s` be the largest sync
   point with `C_s ≤ k` (with `torn`, `C_s ≤ k − 1`: the incomplete write
   may be the commit write): every path in `E_s` must exist with its exact
   contents (durability of what was committed). With `torn`, if `W[k−1]`
   is itself a commit write the device may show either root, depending on
   which half carried the superblock, so the state after that sync is
   accepted too. Every directory must walk
   without error and every regular file must read fully (integrity of what
   is visible; on-disk headers are checked by the mount and read paths
   through kind and checksum). Unmount.
5. Report the number of prefixes checked and the write count.

What it does not check: block-bitmap consistency and space accounting
(the gap in `testing.md`); the property that uncommitted data is *absent*
(cosmofs may legitimately expose a newer committed root after a sync
that completed on the device but whose completion the workload had not
yet observed).

## 5. The syscall fuzzer (`userland/init/init.c`, `init --syscall-fuzz N SEED`)

An unprivileged process (it drops to uid/gid 1000 first, closes handle 0,
and maps one scratch page) makes `N` system calls chosen by a seeded
xorshift generator and reports `USERTEST: syscall-fuzz ok: N calls, E
errors` when it survives; the kernel test `syscall-fuzz` runs it with a
fixed seed and requires exit status 0. Argument values come from pools
designed to reach both the checks and the paths behind them: handles
(negative, 3..31, huge; never 0..2, which are the process's console),
pointers (NULL, the scratch page, its last bytes so a copy straddles the
end, an unmapped low address, a kernel-half address, an unaligned address,
a huge value), lengths (0, 1, small, a page, the page plus one, huge,
"negative"), strings (valid paths, a nonexistent path, a very long path, a
path with `..`, an unterminated buffer), flags (random 32-bit and the
documented bits). Calls that could block the fuzzer forever or damage it
rather than the kernel are excluded and named in the source: `exit`,
`read`, `recvfrom`, `accept`, `connect`, `wait`, `kill`, `spawn`,
`vcpu_run`; `sleep_ns` is capped at 1 ms; `munmap` addresses are confined
to the scratch page or to invalid ranges; `mmap` never sets `MAP_FIXED`;
`dup` never targets 0..2. Every other call, including `mount`, `umount`,
`klog`, `setgroups` and the VMM calls, is made and must answer with
`-EPERM`, another errno, or a value. Privilege is dropped so the fuzzer
cannot unmount the root or reconfigure the system; what it can create it
creates under `/tmp`.

## 6. Per-test timing (`kernel/core/selftest.c`, `tests/boot/run_boot_test.py`)

Each self-test line carries its duration: `SELFTEST: name ... ok (12 ms)`.
After the run, `SELFTEST: timing total=N ms slowest=name (M ms)`. The boot
harness parses the durations, prints the five slowest tests and the total
in its own report, and fails a test that exceeded `SELFTEST_BUDGET_MS`
(default 8000, the hang watchdog's period) so a test that only just
finishes is noticed before it becomes a timeout.

## Ownership and lifetime

Host targets own everything they allocate per input and free it before
returning (leak detection is on in `make fuzz`; the cosmofs shim frees the
pool and every vnode at unmount). Fault-injection rules are static; the
`only` thread pointer is cleared by `faultinject_clear` and a test clears
its rule before returning, on every path. The RAM device is a kobject:
`ramblk_destroy` unregisters and puts the creator's reference; the
recorder's log and snapshots are kmalloc'd and freed by the caller.

## Concurrency

Fault injection reads its rules without a lock (a rule is written by one
thread, read by many; the counters are relaxed atomics; a stale read only
moves a failure by one event). The RAM device's submit runs under its own
spinlock; the recorder appends under the same lock (a write completes
before `blk_submit` returns, so the log is in completion order). The
crash harness runs on one thread. The syscall fuzzer is one process and
the kernel is expected to serialise it like any other.

## Memory

RAM device images: `nblocks × 4 KiB`, twice while a snapshot exists, plus
the log (one copy per recorded write). The harness uses 512 blocks
(2 MiB) so a full run stays under 8 MiB. Fuzz targets: the input plus the
parser's own allocations; the cosmofs target's arena is 8 MiB.

## Error handling

A fuzz target never returns an error: it either survives or the sanitizer
aborts. Fault injection never injects on a path that the kernel would
panic on by design (boot-time paths run before `faultinject_init`);
tests target their own thread. The crash harness reports the first prefix
that fails with the prefix length, torn flag and the check that failed.
The syscall fuzzer counts errnos and never treats one as a failure; a
kernel panic fails the boot test.

## Performance

Debug boot: the crash harness adds about 1 s (a few hundred mounts of a
2 MiB image), the syscall fuzzer about 0.5 s for 20 000 calls; fault
injection adds two loads per allocation. `make fuzz` with the default run
count takes about a minute per target on the host; CI runs it with a
smaller count.

## Security

The fuzzers exercise exactly the parsers an attacker reaches (module
images, executables, packages, guest-controlled ABI structures, device-
controlled rings, disk images) and the syscall boundary from an
unprivileged process. Findings are fixed as CRITICAL-class bugs with a
regression test, as the constitution requires.

## Future extensibility

- A target-side network fuzzer feeding `ipv4_input` from a self-test.
- Coverage-guided runs in CI (libFuzzer with `-use_value_profile`) and
  corpus persistence as CI artefacts.
- A settable `sysctl` (or a dedicated debug system call) so fault injection
  can be driven from userland tests.
- Block-bitmap and space-accounting checks in the crash harness once
  cosmofs grows an `fsck`.
