# Verification infrastructure: API

Each entry follows constitution section 52. Everything here is test
infrastructure: kernel-internal, debug builds only unless noted.

## Fuzz targets (`tests/fuzz/fuzz.h`)

### `int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)`
The target: run the code under test on one input. Never returns an error;
a violation is a sanitizer report or `FUZZ_ASSERT` abort. libFuzzer's
contract, so the same file links with `-fsanitize=fuzzer`.

### `size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)`
Write the i-th programmatic seed; return its length, 0 past the last, 0
when it does not fit in `cap`. Seeds are valid inputs built by code (a
module image, an executable, a manifest, a tar, a formatted image).

### `int LLVMFuzzerInitialize(int *argc, char ***argv)`, `size_t fuzz_max_len(void)`
Optional (the driver provides weak defaults): per-process setup, and the
largest input the target wants when larger than the driver's 64 KiB.

### `FUZZ_ASSERT(cond)`
A target's own property check; prints file, line and iteration and aborts.

### The driver (`tests/fuzz/driver.c`)
`driver [-runs N] [-seed S] [-max_len L] [-out DIR] [-verbose] [corpus_dir | file …]`:
seeds and corpus once, then `N` seeded mutations; a regular-file argument
is replayed once (a crash reproducer); a crashing input is saved to
`DIR/crash-<iteration>`; the code under test's `klog` lines are dropped unless
`-verbose` is given (a rejected input's diagnostics are the expected
outcome). `make fuzz` runs every target with `FUZZ_RUNS`
(default 20 000) and `FUZZ_SEED`; `FUZZ_ENGINE=libfuzzer` builds the targets
against libFuzzer instead. `FUZZ_ASAN_OPTIONS` defaults to leak detection
on where the sanitizer supports it.

## Fault injection (`kernel/include/kernel/faultinject.h`, `kernel/core/faultinject.c`)

### `enum fi_kind { FI_KMALLOC, FI_BLK_SUBMIT, FI_BLK_COMPLETE, FI_DEMAND_PAGE, FI_DEMAND_COPY }`
Where a fault can be injected: `kmalloc`/`kmem_cache_alloc` return NULL
(the small-object path is hooked in the cache, the large-page path in
`kmalloc`); `blk_submit` returns `-EIO` before the driver; `bio_complete`
turns a success into `-EIO`; the frame allocation of a demand-zero fault
in a user space fails, for a user-mode fault (`demand-page`: the process
dies) or a kernel-mode fault inside a user copy (`demand-copy`: the
system call returns `-EFAULT`), `docs/kernel/memory/design.md` §6.1.

### `void faultinject_set(enum fi_kind kind, unsigned every, unsigned budget, struct thread *only)`
- Purpose: arm a rule: fail every `every`-th eligible event (1 = all), at
  most `budget` times (0 = unlimited), for thread `only` (NULL = every
  thread). Resets the counters. Disarms first so a reader never sees a
  half-written rule.
- Concurrency: written by one thread; readers take no lock.

### `void faultinject_clear(enum fi_kind kind)`, `void faultinject_clear_all(void)`
Disarm. A test clears its rules on every exit path (`CHECK` does).

### `bool faultinject_should_fail(enum fi_kind kind)`
The hook at each injection point. One acquire load when nothing is armed.
Never fails in interrupt context (`irq_depth != 0`). Counts `seen` and
`hits`; a rule whose budget reaches zero disarms itself.

### `void faultinject_stats(enum fi_kind kind, struct fi_stats *out)`
`every`, `budget` (remaining), `only_thread`, `seen`, `hits`.

### `int faultinject_configure(const char *spec)`, `void faultinject_init(void)`
`spec` is `kind:every[:budget]` entries separated by commas, kinds
`kmalloc`, `blk-submit`, `blk-complete`, `demand-page`, `demand-copy`; `-EINVAL` on a malformed entry
(unknown kind, missing or zero `every`, a field that does not fit an
unsigned 32-bit integer, trailing characters), in which case nothing is
armed: the specification is parsed completely before the first rule is
set.
`faultinject_init` applies the fw_cfg boot parameter `opt/cosmo/faultinject`
(rules for every thread), called by `kernel_main` before the self-tests.
Example: `-fw_cfg opt/cosmo/faultinject,string=kmalloc:1000`.

### `int faultinject_sysctl(char *out, size_t n)`, sysctl `debug.faultinject`
One line per kind: `kmalloc every=0 budget=0 thread=all seen=N hits=M`.
The sysctl exists in debug builds; release builds answer `-ENOENT`. There
is no write path through `sysctl` (`invariants.md` F5).

## The RAM block device (`kernel/include/kernel/ramblk.h`, `kernel/block/ramblk.c`)

### `struct blkdev *ramblk_create(uint64_t nblocks)`, `void ramblk_destroy(struct blkdev *bd)`
A registered block device (`ram<letter>`, 512-byte sectors, `nblocks`
4 KiB blocks of kmalloc'd storage, synchronous completion). The caller
holds the creator's reference; `ramblk_destroy` unregisters and drops it
(the storage goes with the last holder, `docs/kernel/quiesce/`).

### `void ramblk_record_start(struct blkdev *bd, unsigned max_entries)`, `struct ramblk_log *ramblk_record_stop(struct blkdev *bd)`, `unsigned ramblk_record_count(struct blkdev *bd)`, `void ramblk_log_free(struct ramblk_log *)`
Record every completed write (a copy of its data) and flush (an entry with
`nsectors == 0`) in completion order, up to `max_entries` (the rest are
counted in `dropped`). `record_count` reads the current length, which a
harness stores at each sync point.

### `uint8_t *ramblk_snapshot(struct blkdev *bd)`, `void ramblk_restore(struct blkdev *bd, const uint8_t *image)`
The whole device as one kmalloc'd image, and back.

### `void ramblk_set_deferred(struct blkdev *bd, unsigned limit)`
Deferred mode: completions run on a worker thread and `submit` answers
`-EAGAIN` above `limit` requests in flight (a virtqueue with every slot
taken); 0 returns to synchronous completion after completing what is
deferred. The block layer's pending queue is tested against it
(`blk-queue`).

### `void ramblk_replay(struct blkdev *bd, const struct ramblk_log *log, unsigned count, bool torn)`
Apply the first `count` entries; with `torn` the last write's second half
of sectors is left out (a write of one sector is applied whole).

## The syscall fuzzer (`userland/init/init.c`)

### `init --syscall-fuzz N SEED`
Drops to uid/gid 1000, closes handle 0, maps one scratch page, makes `N`
system calls with arguments from the pools in `design.md`, and prints
`USERTEST: syscall-fuzz ok: N calls, E errors, S successes, C/56 system
calls exercised, seed SEED`; exit status 0. The kernel self-test
`syscall-fuzz` runs it with `20000 20260905` and requires status 0. The
excluded calls and the per-call constraints are listed in the source and
in `design.md`.

## Self-test timing (`kernel/core/selftest.c`, `tests/boot/run_boot_test.py`)

Every result line ends with the duration: `SELFTEST: name ... ok (N ms)`;
the run ends with `SELFTEST: timing total=N ms slowest=name (M ms)`. The
harness prints the five slowest tests and the total, and fails a test whose
duration exceeds `SELFTEST_BUDGET_MS` (environment, default 8000: the hang
watchdog's period).

## New self-tests

| Test | File | Runs |
|---|---|---|
| `fault-kmalloc` | `kernel/core/faulttest.c` | debug builds |
| `fault-blk` | `kernel/core/faulttest.c` | debug builds |
| `cosmofs-replay` | `kernel-services/filesystem/cosmofs/cosmofscrash.c` | debug builds |
| `syscall-fuzz` | `kernel/process/proctest.c` + `init --syscall-fuzz` | debug builds |

The pre-existing `cosmofs-crash` (a software discard on unmount plus one
torn-superblock byte flip) stays; `cosmofs-replay` is the write-stream
harness.
