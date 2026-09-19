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
| `fuzz_elf` | `elf_validate` (`kernel/process/elf.c`, `ELF_HOST_TEST` leaves out `elf_load_into`) | every segment reported lies inside the image and the user window (relative to 0 and inside the window's span for `ET_DYN`, milestone 10) |
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
enum fi_kind {
    FI_KMALLOC,
    FI_BLK_SUBMIT,
    FI_BLK_COMPLETE,
    FI_DEMAND_PAGE,   /* the frame of a user-mode demand-zero fault */
    FI_DEMAND_COPY,   /* the frame of a kernel-mode demand-zero fault in a user copy */
    FI_USB_CSW,       /* usb_storage: the CSW read queued without the doorbell */
    FI_AHCI_CI,       /* ahci: a slot filled, its PxCI bit never set */
    FI_KIND_COUNT,
};
/* Users of FI_BLK_COMPLETE beyond fault-blk: the VFS write-back tests
 * wb-error-fsync/-once/-close/-lost (docs/kernel-services/vfs/testing.md),
 * scoped to the test thread on the RAM block device, whose completion
 * runs in the submitter's context. */
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

**One line is not always one test.** `process-user` runs the entire
user-mode suite -- every filesystem, network, process, floating-point,
trap, privilege and service check `init` makes, plus a process spawn for
each tool it drives -- behind a single `SELFTEST` line. It therefore
grows whenever userland gains a test, while being measured against a
number meant for one test approaching the watchdog. On CI it stood at
7129 ms of 8000 before the unit that noticed
(`docs/audit/next-subsystem-fsctl.md`), which is a budget that fails the
next addition to userland whatever that addition is.

`cosmofs-replay` is the same shape and was added to the list for the
same reason: it mounts and structurally checks **every prefix** of a
recorded write stream, 410 complete filesystem images behind one line,
so it grows whenever a cosmofs transaction writes another block. Its CI
spread over four runs of code whose local timing is identical to `main`'s
(4801 ms against 4803) was 4703 to 8309 ms against a budget of 8000
(`docs/audit/next-subsystem-unmount-leak.md`) -- a shared runner deciding
the result. Making the suite fit by checking fewer images would trade the
coverage for the budget, which is the wrong way round: the budget is
there to catch a hang, not to cap a suite.

Such a test gets a budget sized for what it is, in
`composite_budget_ms` in the harness, beside the default rather than
instead of it: `process-user` and `cosmofs-replay` have 20 s. It keeps a budget, because a
suite that hangs must still be caught. The list is deliberately short
and each entry is an admission that the line reports too little.

**`process-user` now reports its sections** (the better answer this
section used to name and defer;
`docs/audit/next-subsystem-usertest-sections.md`). `init --selftest`
drives a **table** of sections and times each call, printing one line
per section in the machine channel beside `USERTEST: PASS`:

```text
USERTEST: section svc 1386 ms
USERTEST: sections 10, total 3610 ms
```

and the harness turns them into the summary it already prints for
tests:

```text
boot-test: user-mode suite 3610 ms in 10 sections; slowest: svc 1386 ms,
proc 912 ms, fpu 664 ms, fsctl 377 ms, fs 128 ms (the process-user line
is 3711 ms; 101 ms is spawn and teardown)
```

Three things about that design are deliberate.

**The table, not ten calls.** The driver is the only thing that calls a
section, so a section cannot be timed late, forgotten, or added
without a line: adding one is adding a row. Bracketing ten plain calls
by hand would have been a convention, and this suite already had one of
those — each section printing a `usertest: … ok` line last — which two
of the nine had quietly stopped honouring. The lowercase lines stay
prose and stay unchanged.

**No per-section budget.** Only the composite 20 s remains, and it is
the only **duration** the harness will fail a run over -- the
self-consistency refusals below are failures too, they are just not
about how long anything took. A budget on a section would ration
sections that got more thorough and need widening whenever userland
grows, which is the trap this unit exists to remove; the numbers are
for attribution, and under TCG they are attribution *between sections
of one run*, not a baseline across runs or machines.

**What the harness refuses** is a suite that stopped part-way (section
lines with no total), a declared count that does not match the lines
printed, a section it expects that produced no line, and a section
name it does not know -- the last so that a row added to the table in
`init.c` and not to `USERTEST_SECTIONS` in the harness is a failure
rather than a silent extra. A
zero-millisecond section is a reading, not an absence — `trap_selftest`
compiles to an empty function on aarch64 and duly reports `0 ms`, which
is the difference between this and the prose markers it replaces.

**The first measurement, and it is not what the check counts suggest.**
The *order* is the finding and it is stable — the same on x86-64 and
aarch64, on the default and protection-capable boots, and with the
table's rows reversed. `svc` is the slowest, about two fifths of the
suite, from **34** checks and nine sleeps waiting on service state;
`proc` with 235 checks is next; `fpu` with **4** is third, because it
spawns two partner processes. Then `fsctl`, `fs`, `trap`, `priv`,
`net`, `proc-fs`, `syscalls`. Time in this suite is spawning and
waiting, not checking, and no reading of the source would have said
so.

The individual numbers are one run each and move a few per cent
between runs — 1386 ms for `svc` on x86-64 and 1489 ms on aarch64 in
the runs quoted above. They are for attribution between sections, not
a baseline: see the budget paragraph.

**And the first thing they attributed was CI's slowdown, which is not
uniform.** The reason `process-user` has twice outgrown a budget is
that a shared runner costs some sections far more than others. Same
code, this developer's machine against CI:

| section | x86-64 local → CI | aarch64 local → CI |
| --- | --- | --- |
| `svc` | 1386 → 1890 ms (1.36×) | 1489 → 2823 ms (**1.90×**) |
| `proc` | 912 → 1170 ms (1.28×) | 939 → 1814 ms (**1.93×**) |
| `fsctl` | 377 → 503 ms (1.33×) | 416 → 627 ms (1.51×) |
| `fs` | 128 → 159 ms (1.24×) | 145 → 160 ms (1.10×) |
| **`fpu`** | 664 → 671 ms (**1.01×**) | 665 → 685 ms (**1.03×**) |
| whole suite | 3620 → 4727 ms | 3889 → 6360 ms |

`fpu` is **flat** — a few per cent, on both architectures, between a
laptop and a loaded shared runner — while `svc` and `proc` nearly
double on aarch64. So the budget was not being outgrown evenly, and a
section's sensitivity to host load is not predictable from its size:
`fpu` is 4 checks and `svc` is 34, and they behave oppositely.

**Why they differ is not established, and this section deliberately
does not guess.** A first draft of this paragraph said `fpu` was flat
because it spends its time in a fixed clock hold while `svc` waits on
something other than a clock. Both halves are wrong about the code:
`fpu_hold` runs 300 rounds *alternating* `cosmo_yield()` with a
200 µs `usleep`, and `svc_selftest` polls service state with **fixed**
`cosmo_sleep_ns` sleeps of 5 and 10 ms. A causal story pointed at the
wrong timing mechanism would send the next investigation the wrong
way, which is worse than the honest gap. The numbers are the finding;
the explanation is open.

Before this unit none of it was available, and the budget was widened
twice without it.

A failing self-test is also named against the **load-sensitive list**
in `docs/testing/flakes.md` (the table under its "The list" heading): the
failure report gains a `note:` line saying the test is on the list and
that a re-run distinguishes a flake from a regression. The run fails
regardless; the note is a label, not a retry, and a run in which a
self-test failed while the list file is missing or parses to nothing says
that instead, so the list cannot go silently empty. The rule for putting
a test on the list is in that file.

## 7. The guard boot (`make test-guard`, `tests/boot/run_boot_test.py`)

A test on one CPU model proves what that model enforces. The default
models enforce neither SMAP/SMEP/UMIP nor PAN, so the kernel's guard on
its own access to user memory was a no-op in every CI boot until the
hardening unit. `test-guard` boots the same image on a model that has
the guard (`QEMU_GUARD=1`, `QEMU_CPU` set by the target) and the
harness requires the kernel's `hardening:` line whole, the
`uaccess-guard` self-test's `guard live` sentence and, on x86-64,
`usertest: umip: enforced`, forbidding `hardening: absent`; the default
boot stays the control, where the same tests say what they could not
assert (`docs/kernel/arch/testing.md`, "The guard boot"). The second
model also found what only a second model can: `cortex-a76`'s 40-bit
physical range exposed a stage-2 start level the architecture forbids
below 43 bits (`docs/kernel-services/virtualization/design.md`, "Stage
2"). `make test-wxn` is the same idea for one bit: a build that does the
forbidden thing on purpose, and a harness kind (`--expect-panic wxn`)
that requires that panic and no other.

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
