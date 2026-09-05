# Verification infrastructure: invariants

Rules the infrastructure keeps and rules it enforces on the tree. Each has
a **Check** and, where honest, a **Gap**.

## Rules enforced on the tree

**F1. Every parser that takes untrusted bytes has a fuzz target, and
`make fuzz` passes.** Targets: module ELF (`fuzz_modelf`), user ELF
(`fuzz_elf`), package manifest/index/tar/version (`fuzz_pkg`), Linux ABI
conversions (`fuzz_linux`), the split virtqueue against a hostile device
(`fuzz_virtq`), cosmofs images (`fuzz_cosmofs`). A new parser of external
bytes adds a target. Check: `make fuzz` in CI on every push (bounded runs)
and locally. Gap: the network input path (`ipv4_input`, `tcp_input`) has no
host target yet (it needs the mbuf pool, timers and sockets); the audit's
"IP-TCP parsers" item is met for the ABI conversions and deferred for the
stack (`testing.md`).

**F2. A parser rejects, it never trusts.** An accepted input's derived
structure lies inside the input (module sections and metadata, ELF
segments, tar members) and inside the caller's buffers (Linux
conversions); the virtqueue returns only cookies the driver added and not
yet reclaimed; a mounted image's every block reached carries a validated
header. Check: the `FUZZ_ASSERT`s in each target; the first fuzz run found
and the fix closed an unaligned in-place read of the section header table
(`modelf.c`, "not 8-byte aligned"; regression `unaligned-tables` in
`test_modelf`).

**F3. Every allocation and block-I/O failure path is a clean error.**
Under injected faults `vfs_open`/`file_write`, `ksock_create`,
`module_load`, cosmofs writes and syncs return the operation's documented
error (`-ENOMEM`, `-EIO`) or succeed; nothing else, never a panic; and the
heap's live-object count returns to its baseline once the rule is cleared
and the successes undone. Check: `fault-kmalloc` (one injected failure at
the i-th allocation of each attempt, so every allocation site on the path
fails once; a bounded budget bounds the failures exactly), `fault-blk`
(completion and submission errors under a cosmofs workload, a forced
unmount, a clean remount reading every file back). Gap: the network worker,
the reaper and the timer paths are not targeted (a rule for every thread
would hit paths that panic on allocation failure by design); the boot
parameter form exists for manual runs of exactly that.

**F4. cosmofs is crash consistent under every prefix of its write stream.**
Every prefix of the recorded writes of the `cosmofs-replay` workload,
intact and with the last write torn, mounts, contains every file the
workload had committed by the last sync whose commit write (the
superblock; trailing flushes change nothing) is intact, and walks and
reads without error; a torn commit write may show either root. Check: `cosmofs-replay` on every debug boot
(all prefixes when the log is short, every entry around each sync point
plus a stride otherwise). Gap: block-bitmap and space accounting are not
checked; "uncommitted data is absent" is not asserted (a sync that
completed on the device is legitimately visible).

**F5. The syscall boundary survives any argument from an unprivileged
process.** 20 000 seeded random calls over 47 of the 56 system calls return
a value or an errno and the kernel keeps running. Check: `syscall-fuzz` on
every debug boot. Gap: the nine excluded calls (`exit`, `read`,
`recvfrom`, `accept`, `connect`, `wait`, `kill`, `spawn`, `vcpu_run`) are
covered only by the ordinary user-mode self-test; a fuzzer with a watchdog
thread could include the blocking ones.

**F6. Every self-test reports its duration, and none approaches the hang
watchdog.** Check: `run_boot_test.py` fails a test over
`SELFTEST_BUDGET_MS` (8000 ms); the slowest today is `net-lo-tcp` at about
2.8 s.

## Rules the infrastructure keeps

**F7. Fuzz runs are reproducible.** The driver's mutation sequence is a
function of `FUZZ_SEED` and the seeds/corpus; a crashing input is saved
and replays with `driver <file>`. Check: `make fuzz FUZZ_SEED=…` twice
gives the same log.

**F8. Fuzz targets run the real code.** Each target compiles the kernel or
userland source unchanged (`modelf.c`, `elf.c` with `ELF_HOST_TEST`
leaving out the VMM half, `manifest.c`/`tar.c`/`version.c`, `convert.c`,
`virtqueue.c`, `cosmofs_core.c` and `cosmofs.c`); shims replace only
services (heap, pool, VFS bookkeeping, mutexes). Check: `tests/fuzz/fuzz.mk`
source lists; review.

**F9. Fault injection is compiled out of release builds and never fires in
interrupt context or on a thread it was not aimed at.** Check:
`CONFIG_FAULTINJECT` defaults to `CONFIG_DEBUG`; `BUILD=release` in the
verification chain; `faultinject_should_fail` checks `irq_depth` and
`only` before counting; `fault-kmalloc`'s budget assertion (`hits == 5`)
would fail if another thread consumed a failure.

**F10. There is no write path through `sysctl`.** Fault injection is set by
the kernel API and the boot parameter; `debug.faultinject` is read-only.
Making `sysctl` settable is a system-call ABI decision recorded as future
work, not taken here. Check: review of `sys_sysctl`.

**F11. The RAM device's recorded log is in completion order and complete
or marked.** `submit` records under the device lock before completing;
entries beyond `max_entries` are counted in `dropped` and the harness
requires `dropped == 0`. Check: `cosmofs-replay` (`log->dropped == 0`).

**F12. Harness state leaves nothing behind.** `cosmofs-replay` and
`fault-blk` unmount, remove their mountpoints and destroy their devices;
the vnode count and (for `fault-kmalloc`) the heap's live objects return to
their baselines. Check: the tests' own final assertions.
