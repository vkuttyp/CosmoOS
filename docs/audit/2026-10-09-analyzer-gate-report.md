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

The archived AArch64 runs contain 19 reports on October 3 and 28 on
October 8. Ten reports appeared and one epoll null-dereference report
disappeared: the increase of nine is a net change. Investigation starts
with those ten reports. The old target discards diagnostics, touches
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

Results will be recorded as the checks run; completion requires the full
AGENTS.md matrix on both architectures and branch and merge CI.

## Findings outside the reported diagnostics: scope decision required

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

These are source-established error-path defects; deterministic injected
runtime reproductions have not been run. Neither was fixed. A decision
is requested on whether to include their proofs and separate repairs in
PR A or retain them as deferred work. No PR has been opened or merged,
the architecture baselines are not yet checked in, and the full validation
matrix has not run. PR B has not started.

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
