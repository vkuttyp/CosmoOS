# Memory Subsystem: Testing

Three layers: boot-time self-tests inside the kernel under QEMU, native
host unit tests of the pure algorithms under AddressSanitizer and
UndefinedBehaviorSanitizer, and the crash test that proves the fault
report. All three run in CI (`.github/workflows/ci.yml`).

## Kernel self-tests (`make test`)

Implemented in `kernel/memory/memtest.c`, registered in
`kernel/core/selftest.c`, compiled when `CONFIG_SELFTEST=1` (debug
builds by default; `SELFTEST=1` forces it). Each test snapshots
`pmm_get_stats().free_pages` at the start and requires the same value at
the end, so a leak or a miscount in the code under test fails the test
that caused it.

### `SELFTEST: pmm` (`selftest_pmm`)

| Step | Proves |
|---|---|
| order-0 alloc: refcount 1, order 0, no `PG_BUDDY`/`PG_RESERVED`, inside the direct map, writable | basic allocation and descriptor state |
| order-3 alloc with `PMM_FLAGS_ZERO`: pfn aligned to 8, `page->order == 3`, sampled bytes zero, 8 pages gone | splitting, alignment, zeroing, exact accounting |
| `PMM_FLAGS_ZONE_DMA` result below 16 MiB with `zone == PMM_ZONE_DMA`; `PMM_FLAGS_ZONE_DMA32` below 4 GiB | zone selection |
| `pmm_page_get` to 2, `put` to 1 keeps the frame, `put` to 0 frees it | reference counting |
| 64 single pages allocated then freed in a shuffled order (`i * 37 mod 64`); count returns to baseline | coalescing and the M5 counting rule (this is the check that exposed the double-count bug) |
| `phys_to_page(page_to_phys(p)) == p`, `virt_to_page(page_to_virt(p)) == p`, `virt_to_phys` round trip | conversions |
| `pmm_alloc_pages(PMM_MAX_ORDER, 0) == NULL` | invalid order rejected without panic |
| largest order allocation, if available, is naturally aligned | top-order path |
| `deferred_pages == 0`, `free_pages <= total_pages` | deferred release completed in `vmm_init` |

### `SELFTEST: vmm` (`selftest_vmm`)

The test first allocates and frees one guarded page in the arena as a
warm-up. The first mapping in a fresh arena window allocates intermediate
page-table pages that `arch_mmu_unmap` does not reclaim (invariant M19),
so the free-page baseline is taken after that warm-up; without it every
subsequent "back to baseline" check would be off by the table pages. The
arena-placement section repeats the warm-up and retakes the baseline for
the same reason: what the earlier boot mapped decides which arena region
the section's two allocations land in, and the IOMMU's register windows
moved that (a warm-up is the fix, not a wider tolerance).

| Step | Proves |
|---|---|
| `vm_query` on `selftest_vmm` itself: RX, WB, 4 KiB; on a `static const int`: R; on a `static int`: RW | the kernel-owned tables carry ELF-derived W^X permissions |
| direct map of a fresh frame: RW, translates back to the same physical, leaf is 4 KiB/2 MiB/1 GiB | HHDM correctness and large-page use |
| `vm_kernel_alloc(3 pages, GUARD | POPULATE, RW)`: inside the arena, three pages RW, page below and above unmapped, contents zero, writes stick, `vm_find_region` finds it and not the guard | populated allocations and guard pages |
| `vm_kernel_free`: unmapped afterwards, free count back to baseline | frames and record released |
| lazy `vm_kernel_alloc(4 pages, GUARD)`: no frames consumed, middle page unmapped; a read populates a zero page through the real `#PF` path; a write to the same page needs no second fault; other pages stay unmapped; `faults_handled` incremented by one; free returns the single frame | demand-zero population end to end through `isr.S`, `x86_trap_dispatch`, `interrupt_dispatch`, `vm_fault_handler` |
| `vm_map_phys` of a RAM frame with `VM_CACHE_UC`: query reports UC, RW, the right physical; a write through the window is visible through the direct map; `vm_unmap_phys` unmaps | MMIO-style windows and cache attributes |
| two guarded allocations are page aligned, distinct, and at least two pages apart | arena placement |
| size 0, unaligned size, and `VM_PROT_NONE` return 0 | argument validation |

### `SELFTEST: user-vmm` (`selftest_user_vmm`, milestone 5)

On a user space created for the test and never activated: four
populated RW pages are one region with four frames; unmapping the middle
two leaves two regions and two frames, the strict unmap refuses the gap
and the whole range (nothing changes), the lenient one skips it;
refilling the gap with equal attributes merges back to one region; a
different name does not merge; `mprotect` of the middle two to
`PROT_NONE` splits into three regions with all four frames still
attached and `arch_mmu_query` reporting no permissions, the shootdown
collects no acknowledgements (no other CPU runs the space), mapping
over the `PROT_NONE` pages is `-EEXIST`; back to RW merges to one;
`protect` across a gap is `-ENOMEM`, W+X `-EINVAL`; a `PROT_NONE`
reservation has no frames and can be split by `protect`; a lenient unmap
across two regions and a gap leaves exactly the expected pieces;
`vm_space_destroy` returns the frames.

### `SELFTEST: uaccess` (`selftest_uaccess`, `kernel/syscall/uaccesstest.c`)

From a kernel thread (no process, so every user address is unmapped):
the exception table has at least one entry (one on x86-64, four on
AArch64), each naming kernel text; `user_range_ok` edges; four real
kernel-mode faults at user addresses through `copy_from_user`,
`copy_to_user` and `strncpy_from_user` each return `-EFAULT` with the
kernel buffer untouched, and `vm_stats.fixups` rises by exactly four; a
kernel pointer is refused by the range check without a fault.

### `SELFTEST: kmalloc` (`selftest_kmalloc`)

| Step | Proves |
|---|---|
| `kzalloc` of 1, 15, 16, 17, 100, 255, 256, 1000, 4096, 8192, 8193, 65536, 1 MiB: non-NULL, 16-byte aligned, `kmalloc_size >= size`, first and last byte zero; each filled with a distinct byte and re-verified | every size class and the page path, no overlap between live allocations |
| after freeing all: `live_objects` and `large_pages` back to their starting values | both free paths |
| `krealloc` 40 → 3000 → 20 preserves the first 20 bytes | copy semantics in both directions |
| 4096 × 64-byte objects allocated, verified, half freed and re-allocated, all freed | slab growth, full/partial/empty transitions, retention and release |
| `kmem_cache_create("selftest-obj", 200, 64)`: two objects 64-byte aligned, distinct; freed; cache destroyed | dedicated caches with non-default alignment |
| `kmalloc(0)`, `kmalloc(KMALLOC_MAX_SIZE + 1)` return NULL; `kfree(NULL)` is a no-op | argument validation |
| free pages within 64 of the baseline | bounded empty-slab retention (M29) |

The self-test summary line is `SELFTEST: PASS (8 tests)`; the harness
`tests/boot/run_boot_test.py` requires it.

## Host unit tests (`make host-test`)

`tests/host/host.mk` compiles the algorithms natively with the host
`clang` and `-fsanitize=address,undefined -fno-sanitize-recover=undefined`
into `out/<arch>-<build>/host/test_buddy` and `test_slab`, then runs both.
The kernel sources compiled unchanged are `kernel/memory/buddy.c`,
`kernel/memory/slab.c`, and `kernel/memory/kmalloc.c`.

### Shims

- `tests/host/shim/arch/irq.h` and `shim/arch/cpu.h` replace the
  architecture headers: interrupt state is a no-op that reports enabled,
  `arch_cpu_id()` is 0, `arch_cpu_halt_forever()` aborts. The shim
  directory precedes `kernel/include` on the include path.
- `tests/host/harness.c` provides `panic`, `panic_frame`,
  `backtrace_print`, `klog`, `kprintf`, `ksnprintf`, and a page-frame
  arena: an `mmap`ed region with `pmm_hhdm_base` pointing at it, a
  `calloc`ed page array, and `pmm_alloc_pages`/`pmm_free_pages`
  implemented over one `pmm_zone` managed by the real `buddy.c`.
- `tests/host/shim_spinlock.c` replaces `kernel/core/spinlock.c`. It is
  the same one-word lock plus a stack of held locks. `EXPECT_PANIC(stmt)`
  works by having `panic()` call `harness_release_all_locks()` and then
  `longjmp` back into the test; without the release, a panic raised
  inside a critical section (every double-free check runs under the cache
  lock) would leave the lock held and the next acquisition would itself
  panic. The kernel's real spinlock is not used on the host only because
  it cannot know which locks a longjmp skipped; on the target `panic`
  never returns, so the question does not arise.

### `tests/host/test_buddy.c` (7 tests, no memory behind the page array)

| Test | Proves |
|---|---|
| `free_range_maximal_blocks` | `buddy_free_range(3, 4093)` yields maximal aligned blocks, exact totals, two order-0 remnants |
| `alloc_free_roundtrip` | one alloc/free returns the zone to a single set of max-order blocks |
| `alignment_and_orders` | every order 0..10 allocates aligned and frees consistently |
| `exhaustion` | 64-page zone drains to zero, further allocations return NULL, refill merges to one order-6 block |
| `no_merge_across_zone_end` | a 48-page zone keeps its order-4 tail from merging past the zone end (M7) |
| `random_stress` | 20000 random operations against a model: no overlap, alignment, exact `nr_pages_free` after every step, `buddy_zone_check` every 500 steps, full merge at the end |
| `misuse_panics` | freeing with a live refcount, an unaligned head, a double free, and calling without the lock all panic |

### `tests/host/test_slab.c` (7 tests, 32 MiB arena, ASan-checked memory)

| Test | Proves |
|---|---|
| `cache_basic` | create/alloc/free/destroy, zeroing, `slab_of` resolution, exact frame return |
| `cache_growth_and_shrink` | 3000 objects with 64-byte alignment across many slabs; freeing every other object keeps slabs; freeing the rest leaves at most two retained slabs; destroy returns every frame |
| `cache_misuse` | freeing to the wrong cache, an interior pointer, a double free, and destroying with live objects panic; a 40000-byte object is refused |
| `kmalloc_classes` | 22 sizes across all classes and the page path; every allocation is filled to its full `kmalloc_size` (ASan proves no neighbour or header is touched); stats return to zero |
| `krealloc` | growth preserves data; `KMEM_ZERO` zeroes beyond the old usable size (8192 for a 5000-byte request); shrink; size 0 frees; bad sizes return NULL |
| `kfree_misuse` | double free via `kfree`, an interior pointer into a large allocation, and a stack address all panic |
| `random_stress` | 30000 random kmalloc/kfree operations with content tags verified before every free |

`kmalloc_init` runs once per process (the kernel asserts single
initialisation), so `test_slab` shares one arena and a warm-up
create/destroy absorbs the cache-of-caches' retained empty slab.

## Crash test (`make test-crash`)

Builds with `CRASH_TEST=1`, which makes `kernel_main` write to
`0xFFFF900000000000` after the self-tests: a canonical kernel-half
address in no region. The fault now reaches `vm_fault_handler`, which
finds no region and panics. `tests/boot/run_boot_test.py --expect-panic`
requires, among others:

```
KERNEL PANIC: page fault: kernel write at 0xffff900000000000 (not present): no region
trap 14 (#PF page fault) error=0x2
CR2=ffff900000000000 (not-present write kernel)
stack trace:
```

and the failure exit code (QEMU status 35). This proves the fault report
path, not the demand-zero path; `selftest_vmm` covers that.

## Address-space tags (`asid-*`, `kernel/memory/memtest.c`)

Six tests, each failing for its own stated reason when its property is
removed. They run on AArch64 and **skip with a logged reason on x86-64**,
where `arch_mmu_asid_bits()` is 0 and the untagged path is what runs.

| Test | Proves | Failure when its bug is reintroduced |
|---|---|---|
| `asid-alloc` | 255 tags per generation at 8 bits, each distinct; exhaustion rolls over exactly once; a released tag is reusable and a stale one frees nothing | check fails on the count or the generation |
| `asid-isolation` | two spaces at one address, 40 switches, no flush between them | `a space read another space's byte: the switch did not carry its tag` |
| `asid-rollover` | a tag reissued across a forced rollover carries nothing forward | `a recycled tag carried the old space's translations across a rollover` |
| `asid-paranoid` | the isolation property holds with every switch flushing too | as `asid-isolation` |
| `asid-destroy-reuse` | a tag freed at destroy and handed to the next space | `a tag released at destroy carried the old space's translations to its next owner` |
| `asid-quiet` | 401 switches, half through the kernel's root, perform 0 flushes and allocate 0 tags | `the switch path still flushes the TLB` / `the kernel's root was given an address-space tag` |

Two details are deliberate. `asid-rollover` and `asid-destroy-reuse`
**assert the tag was really reissued** (`tag_a == tag_b`) before checking
the byte, so neither can pass by failing to reuse the tag. And every
measurement runs with interrupts off and is checked *after* they are
restored: `CHECK` returns, and returning with interrupts disabled hangs
the kernel until the boot test's timeout -- which is how one early
version of `asid-alloc` presented itself.

### Counted, not timed

`asid-quiet` counts full TLB invalidations performed on the switch path,
at the instruction rather than at the decision, so a switch path that
flushes without asking the allocator is still visible. In paranoid mode
it inverts: 401 switches must perform 401 flushes. Measured both ways:

```text
QEMU_ASID='':          401 switches,   0 TLB flushes performed
QEMU_ASID='paranoid':  401 switches, 401 TLB flushes performed
```

**Timing this change under TCG does not work, and the attempt is
recorded so it is not repeated.** A microbenchmark of 200 back-to-back
switches measured a *tagged* switch at 46,000-174,000 ns against a
steady ~8,100 ns for a flushing one -- the tagged path apparently 6-20x
slower, and swinging threefold between runs of one binary:

| vCPUs | tagged | flushing every switch |
|---|---|---|
| 1 | 117,975 ns | 8,184 ns |
| 2 | 46,160 ns | 8,615 ns |
| 4 | 154,195 ns | 8,120 ns |

That is emulation, not hardware: QEMU's software TLB is not ASID-tagged,
so a root write that changes the ASID takes a path an explicit TLBI
short-circuits, and adding a flush therefore makes emulation faster.
Whole-boot self-test totals are unchanged (40-41 s before and after), so
nothing real is slower. The claim this unit is entitled to is structural
-- a broadcast TLB invalidate removed from every process switch, proved
by count -- and what that is worth in nanoseconds needs hardware this
tree has never run on.

### `selftest_kmalloc` measures the whole machine, and that is fragile

`selftest_kmalloc` asserts `ks1.live_objects == ks0.live_objects` across
its own run. That is a statement about *the machine*, not about the
allocator under test: any background allocation on any CPU breaks it.
It held by luck until the six `asid-*` tests were placed before it,
whereupon it failed in roughly three runs out of four under
`QEMU_KBD=hub` -- always by two or four objects, always in the generic
`kmalloc-64` bucket, never in `vm_space` or `vm_region`, and never at a
test boundary (the count is identical entering every test). The tags
tests leak nothing; they wake other CPUs, whose deferred work lands
inside the next test's window.

Reducing the disturbance did not remove it. Shortening the measured loop
from 200 rounds to 50, measuring with interrupts enabled instead of
disabled, cutting the switches through the kernel's root from 200 to
four, and removing the destroy-path broadcast each moved the failure
rate without eliminating it. What removed it was **ordering**: the
`asid-*` tests now run *after* `kmalloc`, so nothing of theirs is in
flight while it counts.

That is a mitigation, not a repair. The assertion remains true only
while whatever precedes it happens to be quiet, and the next test placed
before it will find the same edge. Fixing it properly means measuring
what the allocator itself allocates rather than what the machine holds
-- a change to that test, and to the accounting it can reach, which
belongs to a unit of its own rather than to this one.

### The destroy-path invalidate has no test, and why

Removing `arch_mmu_invalidate_asid` from `vm_space_destroy` leaves every
test passing. It is not dead code and it is not proved: by the time it
runs, `user_range_teardown` has already invalidated every mapped page of
the space across *every* tag, so nothing observable remains for it to do.
It is kept so that destroying a space does not depend for its safety on a
decision made in `arch_mmu_invalidate` -- where the natural optimisation,
naming the tag, is exactly what would break it (see M38's gap). Recorded
rather than counted, as the FP/SIMD and terminal-mode units recorded
theirs.

## The page-poison check (every debug boot)

Not a test with a name in the list: `pmm_alloc_pages` verifies, on every
allocation of a frame that has been freed before, that nothing wrote to
it in between (M37). It runs for the whole of every debug boot, on both
architectures, and a violation is a panic at the reuse:

```text
[ERROR] pmm: pfn 309343 was written while free: 32 byte(s) at offset 4064-4096 (poison 5a); last freed from 0xffffffff8007e090
[ERROR] pmm:   +4064: 12 00 00 00 00 00 00 00  00 80 85 4b 00 00 01 00
[ERROR] pmm:   +4080: b8 08 01 80 ff ff ff ff  00 00 00 00 00 00 00 00
KERNEL PANIC: pmm: use after free of pfn 309343 (see the lines above)
```

Read it in this order. The **freer** is a return address inside the
kernel: `llvm-nm -n out/<arch>-debug/kernel/kernel.elf` and take the
last symbol at or below it (here `el2_vcpu_destroy+0x44`, the free of a
vCPU's context page). The **offset and length** say what shape the
write had: 32 bytes at the very top of a page is a stack push. The
**bytes** say what was written: here `x0` = `0x12` (`HV_EL2_CALL_TLBI`)
and `x1` = a `VTTBR` -- the arguments of the hypercall that VM
destruction issues right after freeing that page. That was the whole
diagnosis of a defect which had shown itself only as a shell jumping to
address 0, `init` faulting on a valid `mov`, and an `anon_pages`
residue, each once in tens of boots: one 32-byte write, landing on
whatever the frame had become.

Reintroducing that bug (removing the `SP_EL2` restore from
`hv_el2_switch.S`) fails every debug boot with exactly the lines above,
about ten seconds in; it is the bug-proof for virtualization V18 as well
as the demonstration of this check.

What the check cannot see: a frame written *after* it was handed to its
next owner (the new owner's data simply changes), and a stray store of
the poison byte itself. What it costs: one 4 KiB fill per freed frame
and one 4 KiB scan per allocated one, in debug builds only.

## Measured results

At the end of the Phase 2 bring-up on the Apple Silicon host under QEMU
TCG (256 MiB guest):

| Check | Result |
|---|---|
| `make test` (debug) | PASS, `SELFTEST: PASS (8 tests)`, about 2.3 s |
| `make BUILD=release test` | PASS, about 2.3 s |
| `make host-test` | 14/14 pass under ASan + UBSan |
| `make test-crash` | PASS (VMM fault report and exit 35) |
| `make analyze` | clean |
| `make reproducible` | `kernel.elf` and `BOOTX64.EFI` byte-identical |

Boot log lines from the memory subsystem in that configuration:

```
[ INFO] pmm: 256 MiB RAM span, 246 MiB free, 9 MiB reserved, 0 MiB deferred, page array 2048 KiB
[ INFO] kmalloc: 15 size classes up to 8192 bytes, page path up to 4096 KiB
[DEBUG] vmm: kernel page tables active, root 0xf4ec000, direct map covers 256 MiB
[DEBUG] vmm: freed 56 KiB of bootstrap page tables
[ INFO] vmm: 246 MiB free after takeover, arena 0xffffc00000000000-0xffffe00000000000
```

## Coverage gaps and planned tests

Not tested yet:

- **Out-of-memory injection** is now partly covered: the `fault-kmalloc`
  self-test (`kernel/core/faulttest.c`, `docs/verification/`) makes
  `kmem_cache_alloc` and the large-page path of `kmalloc` return NULL on
  a per-thread schedule, and checks the file, socket and module paths
  return `-ENOMEM` cleanly with the heap's live-object count back at its
  baseline. Real exhaustion of the buddy in the kernel, `vm_kernel_alloc`
  rollback, and the fault handler's OOM panic are still exercised only
  by the host `exhaustion` test at the buddy level.
- **Concurrency.** Everything runs on one CPU. Zone, cache, and space
  locks are taken but never contended. Phase 3 SMP must add multi-CPU
  stress and a lock-order checker.
- **Deferred release with RAM above 4 GiB.** The 256 MiB QEMU guest never
  produces `PG_DEFERRED` frames; the path is exercised only when
  `QEMU_MEM` exceeds 4 GiB (`make test QEMU_MEM=5G` runs it manually).
- **1 GiB pages.** QEMU's `qemu64` CPU lacks `pdpe1gb`, so the 1 GiB leaf
  path in `mmu.c` is compiled but not executed in CI.
- **`arch_mmu_protect`.** Not called by any current code path or test.
- **Fuzzing.** No fuzz driver for `arch_mmu_map`/`unmap` sequences or the
  region allocator.
- **Power loss** does not apply; nothing here is persistent.

Planned: a `QEMU_MEM=6G` CI job once the runners allow it, an OOM
injection hook (`PMM_FLAGS_FAIL_INJECT` under `CONFIG_DEBUG`), a host
test for `mmu.c` against a fake direct map, and SMP stress with Phase 3.
