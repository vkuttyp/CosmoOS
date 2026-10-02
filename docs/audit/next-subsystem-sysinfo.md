# NEXT SUBSYSTEM — the Linux personality stubs sysinfo

> Constitution §68 report. This PR adds the report and the probe
> (`tools/sysinfo-probe.py`); the `lx_sysinfo` handler, the `struct lx_sysinfo`
> and the `lxtest` check described under "Design" and the edits in "Affected
> files" are planned work that lands in the implementation PR that follows,
> gated on CI. As committed here, `sysinfo` returns `-ENOSYS`.

## Problem

`sysinfo(2)` (x86-64 99, AArch64 179) is listed in `linux_table` but wired to
the `lx_nosys` stub (`compat/linux/syscalls.c`), so it returns `-ENOSYS`.
`sysinfo` is the small, old interface a program uses to read the machine's
uptime and a coarse memory picture (total and free RAM, swap, a process count)
in one call — `top`, `free`, language runtimes sizing their heaps, and health
checks all reach for it. The call is present in the table only as a placeholder;
a program that makes it gets `-ENOSYS` rather than the numbers.

The kernel already keeps everything `sysinfo` needs. The buddy allocator
exports a managed/free page count (`pmm_get_stats`, `struct pmm_stats`'s
`total_pages`/`free_pages`, `kernel/include/kernel/pmm.h`); the monotonic clock
is time since boot (`clock_now_ns`, `kernel/include/kernel/timer.h`); and the
process table has a live count (`process_count`, `kernel/include/kernel/
process.h`). `sysinfo` is a marshalling of those into the Linux struct — no new
accounting.

## Probe

`tools/sysinfo-probe.py` adds one check to `tests/linux/lxtest.c` (which runs
in the standard boot); `LX_sysinfo` is already defined, so no syscall-number
header changes:

```
LXSYSINFO: sysinfo stubbed -> -ENOSYS; no uptime/memory snapshot
```

The check asserts `sysinfo(&si)` returned `-ENOSYS` at the report commit,
before the implementation PR. The marker prints only when the syscall really
returned `-ENOSYS`, so grepping it cannot show a false result after a failed
check.

## Why it matters

- **The coarse machine snapshot programs expect.** `sysinfo` is a one-call
  read of uptime and memory that a lot of portable software uses; a hard
  `-ENOSYS` makes those programs fail or mis-size themselves.
- **It is pure marshalling of stats that exist.** The page counts, the boot
  clock, and the process count are all already maintained; this unit only
  copies them into `struct sysinfo`, so it is small and low-risk.

## The implementation before this unit

| piece | where | what it gives |
|---|---|---|
| page accounting | `pmm_get_stats(&st)` → `st.total_pages`, `st.free_pages` (`pmm.h`) | managed and free page counts for the buddy allocator |
| uptime | `clock_now_ns()` (`timer.h`) | monotonic nanoseconds since boot |
| process count | `process_count()` (`process.h`) | the number of live processes |
| the stub | `[LX_sysinfo] = lx_nosys` (`syscalls.c`) | returns `-ENOSYS` today |

## Design

### 1. The struct

`compat/linux/linux_abi.h` gains `struct lx_sysinfo` in the kernel's LP64
layout (identical on x86-64 and AArch64, so no arch packing):

```c
struct lx_sysinfo {
    int64_t  uptime;       /* seconds since boot */
    uint64_t loads[3];     /* 1/5/15-min load average, fixed point */
    uint64_t totalram, freeram, sharedram, bufferram;
    uint64_t totalswap, freeswap;
    uint16_t procs;        /* current process count */
    uint16_t pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;     /* the unit totalram etc. are counted in (bytes) */
};
```

It matches glibc's `struct sysinfo` field-for-field (the compiler inserts the
same alignment padding before `totalhigh` and at the tail), so a `sizeof` on
both sides agrees; the `lxtest` asserts the size.

### 2. The handler

`lx_sysinfo(user_ptr)` fills a zeroed `struct lx_sysinfo`:

- `uptime = clock_now_ns() / 1'000'000'000`.
- `mem_unit = 1` (report bytes). `totalram = st.total_pages * PAGE_SIZE`,
  `freeram = st.free_pages * PAGE_SIZE` from `pmm_get_stats`.
- `procs = process_count()`.
- `loads`, `sharedram`, `bufferram`, `totalswap`, `freeswap`, `totalhigh`,
  `freehigh` are left zero — this kernel has no load average, no swap, and no
  separate high-memory zone, and reporting zero is how `sysinfo` says "none"
  (a program reads `freeram`/`totalram`, which are real).

Then `copy_to_user`; `-EFAULT` on a bad pointer. The `[LX_sysinfo]` table entry
moves from `lx_nosys` to `lx_sysinfo`.

## Affected files

| file | change |
|---|---|
| `compat/linux/linux_abi.h` | `struct lx_sysinfo` |
| `compat/linux/syscalls.c` | `lx_sysinfo`, `#include <kernel/pmm.h>`, `[LX_sysinfo]` → `lx_sysinfo` |
| `tests/linux/lxtest.c` | the sysinfo check |
| `README.md` | Status entry |

No syscall-number header change: `LX_sysinfo` is already defined (99 / 179).

## APIs

The Linux `sysinfo(2)` system call. No native ABI change: the stats it reports
are already exported by `pmm`, the clock, and the process table; the gap this
closes is the Linux one.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| `sysinfo` | `sysinfo(&si)` returns `0`; `uptime > 0` and plausible (a handful of seconds, not decades); `mem_unit == 1`; `totalram > 0` and `freeram <= totalram`; `procs >= 1`; `sizeof(struct lx_sysinfo)` is the expected LP64 size |

**Planned mutations** (each alone, boot confirmed):
- `lx_sysinfo` reporting `free_pages` as `totalram`: the `freeram <= totalram`
  check fails (free is a subset, so swapping them inverts the relation once any
  memory is in use).
- `mem_unit` left `0`: the `mem_unit == 1` check fails.
- `procs` left `0`: the `procs >= 1` check fails.

## Benchmarks

None.

## Risks

- **No load average.** `loads[3]` is reported zero; this kernel keeps no
  run-queue load average, and zero is the honest "unknown" a reader tolerates
  (the field is advisory). A real load average would be a separate unit.
- **No swap or high memory.** `totalswap`/`freeswap` and `totalhigh`/`freehigh`
  are zero because the system has neither; this matches what `sysinfo` reports
  on a machine without them.
- **`mem_unit = 1` (bytes).** `totalram`/`freeram` are byte counts; they fit a
  64-bit field for any memory this kernel manages, so no scaling unit is
  needed.

## Alternatives considered

- **`signalfd` or `mremap` instead.** `signalfd` needs core signal-queue work
  (pending signals are a bitmask, not a readable queue) and `mremap` needs VM
  resize/move primitives the VM layer lacks; `sysinfo` is pure marshalling of
  stats that already exist, so it is the better-bounded pick, especially after
  a large unit.
- **A fuller `/proc/meminfo`-grade memory report.** `sysinfo` is deliberately
  coarse; matching its small struct is the compat obligation. A richer memory
  interface (cached, buffers, slab) is out of scope and not what `sysinfo`
  promises.
- **Scaling `mem_unit` to page size.** Reporting bytes with `mem_unit = 1` is
  simplest and exact for this kernel's memory sizes; a non-unit `mem_unit`
  would only matter on machines far larger than this one targets.
