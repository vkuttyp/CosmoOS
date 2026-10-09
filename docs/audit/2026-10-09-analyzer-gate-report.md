# Static analyzer gate

## Scope and baseline

Work started 2026-10-09 from main `55b1825ebc28c7f5cdb948477019808dc8ed5517`
(PR #336), on branch `analyzer-diagnostic-gate`. PR A inventories target
analyzer diagnostics, repairs proven defects and removable reports, and
gates remaining diagnostics against reviewed architecture baselines.
Device-lifecycle Unit 2 is a separate PR after this PR's merge CI passes.

Fresh baseline commands use GNU make, debug defaults and distinct OUT trees:
`gmake -j4 ARCH=<arch> OUT=out/analyze-gate-baseline-<arch> analyze`.
Logs: `out/analyze-gate-baseline-{x86_64,aarch64}.log`.

The recorded AArch64 counts are 19 on October 3 and 28 on October 8. The
October 3 compiler identity and structured reports were not retained. To
audit the claimed increase against source, the October 3 snapshot
`fc17ca50255d0186b52b8a42a55fa27b41e22b97` was analyzed on 2026-10-09
with the installed Apple Clang 21.0.3; it emitted 28 diagnostics, including
the ten items in the reported increase. That run also emitted the older epoll
null-dereference report, while the October 8 report instead names an
uninitialized output in `hook_item`. Thus the stated net increase of nine
cannot be reproduced from source with today's analyzer; the missing
October 3 compiler identity/report set prevents attributing that count
change to source or analyzer behavior. Each of the ten items was still
investigated below. The old target discards diagnostics, touches
`.analyzed` stamps and unconditionally prints `static analysis: clean`.
Consequently, unchanged files do not report again on an incremental run.

## Gate design

Every `analyze` invocation analyzes every enumerated target translation
unit and writes a Clang plist report under OUT. The gate validates all
expected reports and compares each diagnostic with the selected architecture's
checked-in baseline. Keys include translation unit, checker, diagnostic
file, function and message; line and column are display information only.
Duplicate keys are counted so an additional identical report cannot hide
behind an existing entry. Every baseline entry requires a specific reason.
Missing or malformed reports and unreviewed diagnostics fail the target.
The success message states both the reviewed count and zero unexpected
diagnostics. No analyzer checker is disabled.

Full analysis avoids stale diagnostics after header, configuration or
compiler changes. CI runs the same `analyze` target. The probe introduces
a diagnostic in a throwaway worktree and compares the original target's
false success with the new gate's rejection on each architecture.

## Investigation and validation

Fresh unchanged-main results: x86-64 27 diagnostics; AArch64 28.
Both targets returned 0 and printed `static analysis: clean`. Structured
captures reproduce all 27/28 when header reports are retained with
`plist-multi-file`. The initial plain-plist capture omitted the two
header diagnostics; it is not the inventory used below.

`sched_balance_affinity_pinned`: proven uninitialized cleanup read. If
the first `thread_create_on` returns NULL, the creation loop ends before
assigning `t[1]`; the cleanup loop still reads both slots. The analyzer
reports `core.UndefinedBinaryOperatorResult` on that exact path for both
architectures. `analyzer-fix-probe.py --old --case smp-affinity` reproduces
one diagnostic on each architecture (logs `out/analyzer-smp-old-<arch>.log`).
Initialize the ownership array before creation so cleanup reads only NULL
or a created thread. This is a static path proof; it is not a claim of a
runtime allocation-failure boot.

`selftest_fsctl_result_per_open`: two real leaks when one of the two
buffer allocations fails, plus early assertion exits while buffers and
file references are owned. Both old-source architecture probes report
exactly two `unix.Malloc` diagnostics. Route every check through common
cleanup with ownership initialized before the first acquisition. Both
fixed target analyses report zero diagnostics for `vfstest.c`.

The local validation matrix is complete on both architectures; branch and
merge CI results will be appended after the PR review and merge.

## Analyzer findings fixed and remaining

Fresh full target runs with Clang 21.0.3 on 2026-10-09 now produce eight
diagnostics per architecture: 19 of the original 27 x86-64 findings and
20 of the original 28 AArch64 findings were removed. The current inventory
is the same on both architectures:

| Architecture | Checker | File | Function | Message | Baseline reason |
|---|---|---|---|---|---|
| both | `core.uninitialized.Assign` | `drivers/nvme/nvme.c` | `nvme_submit` | Assigned value is uninitialized | The block layer rejects zero-sector non-flush bios and the mapper emits at least one PRP for every valid data bio; the analyzer does not infer the segment-count contract across the mapper call. |
| both | `core.uninitialized.Branch` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_mount_no_early_wb` | Branch condition evaluates to a garbage value | `held` is allocated with `KMEM_ZERO`; slab and large allocations zero every slot, including untouched slots on partial setup failure. |
| both | `core.uninitialized.Branch` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_orphan_reserved` | Branch condition evaluates to a garbage value | `fs_` is allocated with `KMEM_ZERO`; slab and large allocations zero every slot, including untouched slots on partial setup failure. |
| both | `core.NullDereference` | `kernel-services/network/nettest.c` | `selftest_net_tcpverdict` | Access to field 'tcp' results in a dereference of a null pointer (loaded from field 's') | The test waits for successful worker completion and joins it before reading the worker-published socket; the analyzer does not model this synchronization. |
| both | `core.NullDereference` | `kernel-services/network/tcp.c` | `port_in_use_locked` | Dereference of null pointer | The circular-list sentinel is tested by member address before the loop dereferences an entry; the empty-list container pointer is never used as an object. |
| both | `unix.Malloc` | `kernel/include/kernel/list.h` (TU `kernel-services/virtualization/guestmem.c`) | `list_pop_front` | Use of memory after it is freed | `guestmem_release` unlinks and advances the sentinel before freeing the detached region; the analyzer attributes the caller's later list operation to the inlined helper. |
| both | `unix.Malloc` | `kernel/include/kernel/list.h` (TU `kernel/ipc/unix.c`) | `list_pop_front` | Use of memory after it is freed | `unix_release` unlinks and advances the sentinel before freeing the detached object; the analyzer attributes the caller's later list operation to the inlined helper. |
| both | `core.BitwiseShift` | `kernel/scheduler/sched.c` | `sched_migrate_from` | Left shift overflows the capacity of 'cpumask_t' | The function rejects `to >= cpu_count()`; the configured maximum is 64 CPUs, so the shift is at most 63. |

The checked-in baselines are `tools/analysis/x86_64.json` and
`tools/analysis/aarch64.json`; each file has reviewed compiler-specific sets.
Each key uses translation unit, checker, diagnostic file, function and
message, with an individual reason and no line number. Full `gmake -j4 ARCH=x86_64 analyze` and
`gmake -j4 ARCH=aarch64 analyze` pass with eight reviewed reports and zero
unexpected reports. `tools/analyze-gate-probe.py --old` reproduces the
old target's warning-plus-success on both architectures. Fixed mode injects
the same null dereference into `kernel/core/main.c` and runs the complete
`make analyze` target; it fails on exactly that unexpected diagnostic on
both. The four old/fixed full-target logs are under
`out/analyze-gate-probe/<arch>/<mode>/analyze.log`.

The separately committed diagnostic repairs are `88cba235` (initialize
the scheduler test's thread slots), `c2d889e5` (close VFS result-test
resources), `9b13ca89` (reject corruption tests without an inode target),
`9e232598` and `30b6125c` (remove two dead stores), `ccbfd16b` (initialize
NVMe thread slots), `457546bc`, `d91bce66`, `0ff606ad` and `2c336e01`
(close four cosmofs test cleanup paths), `e03e7b80` (validate guest-memory
copy buffers and page lookups), `777c728b` (initialize epoll's output
array), `dc0b63ab` (initialize UART sibling state), and `6bb691cf`
(guard a missing current process in `kill_one`). Their `analyzer-fix-probe.py`
old/fixed runs verify the named diagnostic count on both architectures,
with the UART case correctly AArch64-only. The additional runtime leak
proofs and their separate repairs are recorded above.

### Audit of the ten reported AArch64 additions

| Reported addition | Clang 21.0.3 on Oct. 3 source | Disposition on this branch |
|---|---|---|
| `sched_balance_affinity_pinned`: uninitialized thread slot | Present | Fixed in `88cba235`; old/fixed probes reproduce 1/0 on both architectures. |
| `guestmem.c`: uninitialized lookup offset arithmetic | Present | Fixed in `e03e7b80` by checking the per-page lookup while the VM lock is held; old/fixed probes reproduce 1/0. |
| `guestmem.c`: possible null destination to `memcpy` | Present | Fixed in `e03e7b80` by requiring exactly one non-null buffer for a nonempty copy; old/fixed probes reproduce 1/0. |
| `list_pop_front` use-after-free, TU `guestmem.c` | Present | Baseline false positive: the node is detached and the sentinel advanced before the region is freed. |
| `list_pop_front` use-after-free, TU `unix.c` | Present | Baseline false positive: the node is detached and the sentinel advanced before the object is freed. |
| `hook_item`: uninitialized wait-queue output | Not under this exact key; the Oct. 3 report was the epoll snapshot null-dereference below | Fixed in `777c728b`; initializing the two-slot output array gives old/fixed probes 1/0 on both architectures. |
| `selftest_el2_guest_uart_race`: sibling counter arithmetic | Present on AArch64 only | Fixed in `dc0b63ab`; old/fixed probes give AArch64 1/0 and x86-64 0/0. |
| `selftest_fsctl_result_per_open`: `ba` and `bb` leaks | Present | Fixed in `c2d889e5`; the two reports disappear after common cleanup, with old/fixed probes 2/0. |
| `nvme_submit`: possibly uninitialized first PRP | Present | Baseline: valid non-flush bios have nonzero sectors and `data_ok` requires a nonempty valid flat buffer or vector list; every mapped nonempty segment contributes a PRP. The analyzer does not infer this contract across `bio_segments`. |

The reported disappearance was `epoll.c:453`, a possible null dereference
of the zero-capacity snapshot. The October 3 version allocated no snapshot
when `ep->nr == 0`; `snapshot()` returns zero when its list is empty, so
`snap[i]` is unreachable. The subsequent epoll redesign (commits
`b2e7ec7c`, `cf8808a2`, `7a3676d2`, `6a793655`, `51d2099d`) removed the
snapshot wait path in favor of callbacks and a ready list, so that warning's
code path no longer exists. This finding is recorded as refuted by the
source invariant and removed by the redesign, not as a new suppression.

## Additional cleanup defects authorized for PR A

The user's stop rule requires recording and asking before addressing
correctness defects outside the targets. Source inspection identified:

1. `cosmofstest.c:engine_mount`: after `ramblk_create` succeeds, a failure
   in `cosmofs_format`, directory creation or `vfs_mount` returns through
   CHECK without `ramblk_destroy`. The RAM device is registered by
   `ramblk_create`, and `*bdp` is written only after all setup succeeds,
   so the caller cannot reclaim it on a false return. This leaks both the
   creator reference and device storage. It is separate from the named
   `selftest_cosmofs_*` allocation diagnostics.
2. `kernel/device/devtest.c:selftest_nvme`: `workers[c].buf` is allocated
   before `thread_create_on`. If creation returns NULL, the join/cleanup
   loop skips that CPU on `threads[c] == NULL` and never frees the buffer.
   `thread_create_on`/`thread_prepare` do not take ownership of the entry
   argument on failure. This is separate from the analyzer's reported
   uninitialized thread-pointer comparison.

The user explicitly added both defects to PR A on 2026-10-09, requiring
deterministic proofs and separate fixes. The tests inject format I/O and
directory/mount allocation failures after RAM-device creation, then check
that only the test reference remains and the registry no longer contains
the device. The NVMe test injects thread allocation failure after its worker
buffer allocation and checks that the exact buffer is released. These seams
and ownership observations are self-test/debug only.

Before either repair, the registered tests failed deterministically on both
architectures. `out/cleanup-proof-old-x86/boot-test.log` (2026-10-09,
QEMU_SMP=1) reported NVMe `allocated=1 hits=1 started=0 released=0` and
cosmofs `released=0` at all three injected stages. The same assertions failed
in `out/cleanup-proof-old-arm/boot-test.log`. Both boots ran 451 self-tests;
the only failures were these two, and `net-harness` passed with 100/100
guest-to-host rounds. The full harness therefore returned the expected
failure due to these assertions, not an unrelated boot failure.

The checked-in `tools/cleanup-path-probe.py --old` restored both old cleanup
paths in throwaway worktrees and passed its exact-failure oracle on x86-64
and AArch64. `--old` required precisely these two failing self-tests, the
cosmofs failures at all three setup stages, the NVMe `released=0` observation,
and a passing network harness. Fixed-mode probe boots on both architectures
reported NVMe `released=1`, cosmofs `released=1` at stages 1, 2 and 3,
`SELFTEST: PASS (451 tests)`, and a passing network harness. The fixes are
separate commits `abc8bc63` (RAM fixture cleanup) and `80fb43db` (NVMe worker
buffer cleanup); their commit bodies give the ownership reason in one line.

## Local validation (2026-10-09)

Every item below passed on x86-64 and AArch64. Each debug boot ran 451
self-tests and passed the network harness (100/100 guest-to-host rounds).

| Check | x86-64 | AArch64 |
|---|---|---|
| `host-test` | PASS (standalone rerun) | PASS |
| `fuzz` | PASS | PASS |
| `analyze` | PASS, 8 reviewed / 0 unexpected | PASS, 8 reviewed / 0 unexpected |
| Debug `test`, `QEMU_SMP=1` | PASS | PASS |
| Debug `test`, `QEMU_SMP=2` | PASS | PASS |
| Debug `test`, `QEMU_SMP=4` | PASS | PASS |
| `test-smp2` | PASS | PASS |
| `test-chaos` | PASS | PASS |
| `test-harness-retry` | PASS, injected first attempt failed and retry passed; one early firmware handover retry recorded in `flakes.md` | PASS, injected first attempt failed and retry passed |
| Release `BUILD=release test` | PASS | PASS |
| `test-crash` | PASS | PASS |
| `reproducible` | PASS, all 15 artifacts matched | PASS, all 15 artifacts matched |
| `check-tools` | PASS | PASS |

The initial host-test overlap failure and retry observations are recorded in
`docs/testing/flakes.md`; no assertion or budget changed. The reproducibility
script invokes `make` internally, so it was run with a temporary PATH shim
resolving `make` to GNU make 4.4.1 after the shell's BSD `make` failed the
project's GNU make version check.

## PR review follow-up

Qodo identified that `selftest_nvme_worker_cleanup` failed debug boots without
`nvme0n1`; commit `0ea9d3ff` gives it the same skip contract as the existing
NVMe tests. Qodo also identified that a stale baseline entry could remain
after its diagnostic disappeared. Commit `40f70272` makes missing baseline
occurrences fail and adds a host test for that case. Full local analysis after
this change passes with eight observed and zero stale entries per architecture.
The CI compiler/version finding was resolved using the runner-produced
analysis inventories retained as CI artifacts.

CI run `37881297811` established the exact Debian Clang identity:
`Debian clang version 19.1.7 (3+b1)`. Its x86-64 and AArch64 analysis
inventories each contain eight findings. Compared with Apple Clang 21,
Clang 19 changes the NVMe PRP message to "Assigned value is garbage or
undefined", retains the `hook_item` output warning despite both output
slots being explicitly initialized, and does not report the scheduler
shift warning. The `hook_item` Clang 19 entry is reasoned as an analyzer
false positive: `wqs` starts as `{NULL, NULL}`, and `member_wqs` writes
each slot below its returned count. Run `37881297811` failed at the
previous unversioned gate with two unexpected message keys per architecture;
the structured inventories were uploaded as
`analysis-inventory-{x86_64,aarch64}` artifacts. The updated baselines
select by exact `clang_version`, reject an unreviewed compiler, and reject
both new diagnostics and stale baseline entries.

## Complete unchanged-main diagnostic inventory

The x86-64 and AArch64 columns identify every diagnostic: 27 and 28,
respectively. Header reports include their translation unit to distinguish
the two `list_pop_front` reports. Keys exclude line numbers.

| Architecture | Checker | File | Function | Message |
|---|---|---|---|---|
| both | `core.uninitialized.Assign` | `drivers/nvme/nvme.c` | `nvme_submit` | Assigned value is uninitialized |
| both | `core.CallAndMessage` | `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `cosmofs_test_corrupt` | 2nd function call argument is an uninitialized value |
| both | `core.uninitialized.Assign` | `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `cosmofs_test_corrupt` | Assigned value is uninitialized |
| both | `core.uninitialized.Assign` | `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `cosmofs_test_corrupt` | The expression uses uninitialized memory |
| both | `deadcode.DeadStores` | `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `freelog_fill` | Value stored to 'x' during its initialization is never read |
| both | `deadcode.DeadStores` | `kernel-services/filesystem/cosmofs/cosmofs_core.c` | `load_root` | Value stored to 'rc' is never read |
| both | `core.uninitialized.Branch` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_mount_no_early_wb` | Branch condition evaluates to a garbage value |
| both | `core.uninitialized.Branch` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_orphan_reserved` | Branch condition evaluates to a garbage value |
| both | `unix.Malloc` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_badmap` | Potential leak of memory pointed to by 'page' |
| both | `unix.Malloc` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_compress` | Potential leak of memory pointed to by 'back' |
| both | `unix.Malloc` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_compress` | Potential leak of memory pointed to by 'dense' |
| both | `unix.Malloc` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_compress` | Potential leak of memory pointed to by 'sparse_data' |
| both | `unix.Malloc` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_cosmofs_mirror` | Potential leak of memory pointed to by 'sblk' |
| both | `unix.Malloc` | `kernel-services/filesystem/cosmofs/cosmofstest.c` | `selftest_fsctl_check` | Potential leak of memory pointed to by 'h' |
| both | `core.NullDereference` | `kernel-services/network/nettest.c` | `selftest_net_tcpverdict` | Access to field 'tcp' results in a dereference of a null pointer (loaded from field 's') |
| both | `core.NullDereference` | `kernel-services/network/tcp.c` | `port_in_use_locked` | Dereference of null pointer |
| both | `unix.Malloc` | `kernel-services/vfs/vfstest.c` | `selftest_fsctl_result_per_open` | Potential leak of memory pointed to by 'ba' |
| both | `unix.Malloc` | `kernel-services/vfs/vfstest.c` | `selftest_fsctl_result_per_open` | Potential leak of memory pointed to by 'bb' |
| both | `core.UndefinedBinaryOperatorResult` | `kernel-services/virtualization/guestmem.c` | `copy` | The right operand of '-' is a garbage value |
| both | `unix.Malloc` | `kernel/include/kernel/list.h (TU: kernel-services/virtualization/guestmem.c)` | `list_pop_front` | Use of memory after it is freed |
| both | `unix.cstring.NullArg` | `kernel-services/virtualization/guestmem.c` | `copy` | Null pointer passed as 1st argument to memory copy function |
| AArch64 | `core.UndefinedBinaryOperatorResult` | `kernel-services/virtualization/hvtest.c` | `selftest_el2_guest_uart_race` | The right operand of '+' is a garbage value |
| both | `core.UndefinedBinaryOperatorResult` | `kernel/device/devtest.c` | `selftest_nvme` | The left operand of '==' is a garbage value |
| both | `core.uninitialized.Assign` | `kernel/io/epoll.c` | `hook_item` | Assigned value is uninitialized |
| both | `unix.Malloc` | `kernel/include/kernel/list.h (TU: kernel/ipc/unix.c)` | `list_pop_front` | Use of memory after it is freed |
| both | `core.BitwiseShift` | `kernel/scheduler/sched.c` | `sched_migrate_from` | Left shift overflows the capacity of 'cpumask_t' |
| both | `core.UndefinedBinaryOperatorResult` | `kernel/scheduler/smptest.c` | `sched_balance_affinity_pinned` | The left operand of '!=' is a garbage value |
| both | `core.NullDereference` | `kernel/syscall/native.c` | `kill_one` | Access to field 'pid' results in a dereference of a null pointer (loaded from variable 'cur') |
