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
subsequent "back to baseline" check would be off by the table pages.

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

- **Out-of-memory injection.** No test drives the buddy to exhaustion in
  the kernel or makes a slab grow fail; the NULL paths in `kmalloc`,
  `vm_kernel_alloc` rollback, and the fault handler's OOM panic are
  exercised only by the host `exhaustion` test at the buddy level.
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
