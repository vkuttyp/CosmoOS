/*
 * vmm.h - Virtual memory: address spaces, regions, kernel VA arena,
 * page faults.
 *
 * Only the kernel address space exists in this phase. The region model is
 * built so user spaces (Phase 4) add region kinds and a per-process
 * vm_space without changing these signatures.
 *
 * All functions are non-blocking. They take vm_space.lock (irqsave) and
 * may allocate from the heap (region structs) and the PMM (frames, table
 * pages). Lock order: vm_space.lock -> kmem_cache.lock -> pmm_zone.lock.
 */

#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#include <kernel/mutex.h>
#include <arch/mmu.h>
#include <arch/trap.h>

enum vm_region_kind {
    VM_REGION_PHYS,  /* fixed physical backing: image, direct map, MMIO */
    VM_REGION_ANON,  /* frames owned by the region; demand-zero unless populated */
    VM_REGION_FILE,  /* a file's pages, from its page cache: shared, or copy-on-write (private) */
};

struct vnode;
struct vm_space;

/*
 * What the vnode knows about one mmap of it (docs/audit/next-subsystem-file-regions.md,
 * "A third kind of region"). Created by vm_user_map_file, linked on the
 * vnode's pagecache.mappings under the cache mutex, and pointed at by
 * every region cut from that mapping. It describes the range AS FIRST
 * MAPPED: a piece since unmapped or reprotected is a sub-range whose
 * PTEs are absent or different, which every walk over it tolerates.
 * `regions` counts the regions pointing here (atomic; changed under the
 * space lock by split and by every region free); the one that takes it
 * to zero unlinks the record and drops the vnode reference, never under
 * the space lock, because the cache mutex is taken to do it.
 */
struct vm_file_map {
    struct vnode *vn;        /* referenced for the record's life */
    struct vm_space *space;
    vaddr_t base;            /* as first mapped */
    size_t size;
    uint64_t off;            /* file offset of `base` (page aligned) */
    bool shared;             /* MAP_SHARED: the cache's frames, writes reach the file */
    /* This mapping is a program's text, because the loader said so
     * (VM_MAP_TEXT). Set once at creation and never changed, so the question
     * "is anyone executing this file" is answered by the presence of
     * such a mapping on the vnode's list rather than by a counter kept
     * beside it (docs/audit/next-subsystem-elf-shared-text.md). */
    bool text;
    vm_prot_t maxprot;       /* the most vm_user_protect may grant */
    unsigned regions;
    struct list_node link;   /* pagecache.mappings */
};

/* Region flags. */
#define VM_REGION_GUARD_BELOW (1u << 0)
#define VM_REGION_GUARD_ABOVE (1u << 1)
#define VM_REGION_POPULATED   (1u << 2)  /* ANON: fully populated at creation */
#define VM_REGION_USER        (1u << 3)  /* accessible from user mode (U/S) */
/*
 * A replacement owns this region and is tearing its pages down. The range
 * stays owned -- so no mmap(NULL, ...) can be handed it -- but nothing may
 * fault a page into it and nothing but the owning replacement may unlink
 * it. A user fault on such a region installs nothing and returns, so the
 * instruction retries; a kernel fault inside a user copy takes the fixup
 * and reports -EFAULT. Set and cleared by vm_user_map_anon_replace, which
 * holds vm_space::replace_lock throughout, so it is never seen by anyone
 * but that one writer and the readers below.
 */
#define VM_REGION_QUIESCED    (1u << 4)

struct vm_region {
    struct list_node link;   /* in vm_space.regions, sorted by base */
    vaddr_t base;            /* first mapped byte (guards excluded) */
    size_t size;             /* page multiple, guards excluded */
    vm_prot_t prot;
    vm_cache_t cache;
    enum vm_region_kind kind;
    unsigned flags;
    paddr_t phys;            /* PHYS: physical base */
    struct vm_file_map *fmap; /* FILE: the mapping record; NULL otherwise */
    const char *name;        /* immortal string */
};

struct vm_space {
    struct arch_mmu_context mmu;
    struct list_node regions;
    spinlock_t lock;
    /*
     * Serialises vm_user_map_anon_replace against itself. The teardown in
     * the middle of a replacement cannot run under `lock` (it takes it
     * per chunk), so two replacements of overlapping ranges could
     * otherwise interleave and one would meet records the other removed.
     * A mutex, not a spinlock: the operation sleeps. User spaces only --
     * the kernel space never replaces.
     */
    struct mutex replace_lock;
    vaddr_t arena_lo;        /* kernel VA arena for dynamic allocations */
    vaddr_t arena_hi;
    vaddr_t near_lo;         /* arena inside the top 2 GiB, above the image (modules) */
    vaddr_t near_hi;
    bool user;               /* a process address space (lower half) */
    uint64_t anon_pages;     /* frames populated for this space's ANON regions, and COW copies */
    uint64_t file_pages;     /* page-cache frames installed in this space's FILE regions */
    /*
     * Shared file-mapping records pointing into this space (atomic): the
     * futex classifies a word only in a space that has one, so a process
     * that never maps a file MAP_SHARED pays an integer test per futex
     * call and never walks its regions (docs/audit/next-subsystem-shared-futex.md).
     */
    uint64_t shared_maps;
    /*
     * User spaces: the CPUs that may hold translations of this space.
     * A CPU joins on switch-in and leaves only when something flushes
     * what it holds -- a tag-generation rollover, or this space's
     * destruction. It is *not* cleared on switch-out: with address-space
     * tags a CPU keeps a space's translations after leaving it, which is
     * the whole point of the tags (M35, kernel/asid.h).
     */
    cpumask_t tlb_cpus;
    uint64_t mapped_pages;   /* user: pages covered by regions (COSMO_RLIMIT_AS) */
    uint64_t limit_mapped_pages;   /* user: vm_user_map_anon refuses beyond this (-ENOMEM) */
    uint64_t limit_anon_pages;     /* user: a demand-zero fault at or beyond this is "no memory" */
};

extern struct vm_space kernel_space;

/* --- user address spaces (Phase 4) --- */

/* The user window: canonical lower half minus the first 4 MiB (null
 * page and legacy space) and the last page. */
#define VM_USER_LO 0x0000000000400000ULL
#define VM_USER_HI 0x00007FFFFFFFF000ULL

/* The process layer tells the fault handler which user space the
 * current thread runs in and how to terminate it on a fatal fault. */
struct arch_trap_frame;
struct vm_user_hooks {
    struct vm_space *(*current_space)(void);              /* NULL for kernel threads */
    /* A user fault no region services (`sig` SIGSEGV), or a FILE fault
     * whose page the file cannot supply -- past the end, or a read that
     * failed (`sig` SIGBUS). Returns only when a signal handler frame was
     * set up on `frame` (the trap then returns into the handler). */
    void (*fatal)(uint64_t addr, unsigned fault_flags, struct arch_trap_frame *frame, int sig);
};
void vm_set_user_hooks(const struct vm_user_hooks *hooks);

/* Fresh user space whose kernel half mirrors the kernel tables.
 * Returns 0 or -ENOMEM. */
int vm_space_create_user(struct vm_space **out);

/* Tear down every region, free frames, free lower-half tables, free the
 * struct. Must not be the space active on the calling CPU. */
void vm_space_destroy(struct vm_space *space);

/* Map an anonymous user region at exactly [base, base+size). `prot`
 * must not be W+X; VM_PROT_NONE reserves the range (every access faults).
 * flags: VM_REGION_POPULATED for eager zeroed frames, VM_REGION_GUARD_BELOW
 * for a guard page below. The new region merges with an adjacent one of
 * the same kind, prot, flags and name. Returns 0, -EEXIST if it overlaps,
 * -EINVAL, -ENOMEM. */
int vm_user_map_anon(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot, unsigned flags,
                     const char *name);

/* Unmap every page of [base, base+size), splitting regions at the ends.
 * VM_UNMAP_STRICT: every page must be mapped, else -EINVAL and nothing
 * changes (the native munmap). Without it unmapped pages are skipped
 * (Linux munmap, brk, MAP_FIXED replacement). -EINVAL for a range outside
 * the user window; -ENOMEM if a split needs a region struct there is no
 * memory for (nothing changes). */
#define VM_UNMAP_STRICT (1u << 0)
int vm_user_unmap(struct vm_space *space, uint64_t base, size_t size, unsigned flags);

/* MAP_FIXED with POSIX semantics: take [base, base+size) whatever is
 * there. The range is owned by a region at every instant: the new
 * region goes in under the same lock that clears the old ones, marked
 * VM_REGION_QUIESCED, and stays claimed across the teardown. So no
 * concurrent mmap(NULL, ...) can be handed any part of the range --
 * including a HOLE it spanned, which an earlier version left
 * unclaimed -- and no fault can populate a page the teardown would
 * then free.
 *
 * Every fallible step runs before the first mutation, and the page
 * accounting is applied up front, so the finishing swap cannot fail.
 * VM_REGION_POPULATED is refused with -EINVAL for exactly that reason:
 * populating allocates, allocation can fail, and nothing fallible may
 * run after the point of no return. -ENOMEM if the region or a split
 * spare cannot be allocated or COSMO_RLIMIT_AS would be exceeded, in
 * which case nothing has changed. Anonymous user memory only. */
int vm_user_map_anon_replace(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot,
                             unsigned flags, const char *name);

/*
 * Map a file into [base, base+size): the pages of `vn` from offset `off`
 * (page aligned), demand-paged from its page cache. VM_MAP_SHARED: the
 * cache's own frames, so a write through the mapping is a write to the
 * file and is seen by read() and by every other mapping; without it a
 * private, copy-on-write mapping whose written pages are its own and
 * never reach the file. VM_MAP_REPLACE: MAP_FIXED semantics, the range
 * taken whatever is there, owned throughout (M40), instead of -EEXIST on
 * an overlap. `maxprot` bounds what vm_user_protect may later grant
 * (-EACCES past it): R|X for a shared mapping of a file opened
 * read-only, RWX otherwise. `prot` must be within `maxprot` and not W+X.
 * The caller has checked the file's rights and type (a regular file);
 * this takes its own reference to the vnode for the mapping's life.
 * Returns 0, -EEXIST, -EINVAL, -ENOMEM (also COSMO_RLIMIT_AS).
 */
#define VM_MAP_SHARED  (1u << 0)
#define VM_MAP_REPLACE (1u << 1)
/*
 * This mapping is a program's text, made by the loader: the file is
 * "busy" while it exists and a write to it is -ETXTBSY
 * (docs/audit/next-subsystem-elf-shared-text.md).
 *
 * Only `elf_load_into` passes it, and that is the point. "Shared and
 * executable" is not the same question: a program may map a file
 * executable and write to it on purpose -- the page cache syncs the
 * instruction cache for exactly that case -- and refusing those writes
 * broke that behaviour and its test. What must not change underneath a
 * process is the program it is *running*.
 */
#define VM_MAP_TEXT    (1u << 2)
int vm_user_map_file(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot, vm_prot_t maxprot,
                     unsigned flags, struct vnode *vn, uint64_t off, const char *name);

/*
 * msync: write back the dirty pages of every file mapped in
 * [base, base+size). -ENOMEM if a page of the range is unmapped (checked
 * before anything is written); an anonymous range in it is skipped.
 * Two passes, because pagecache_sync sleeps under the cache mutex and
 * the lock order is vnode -> pagecache -> vm_space: the wholly-mapped
 * check under the space lock, then a cursor walk that finds the FILE
 * region containing the cursor (or the first after it), takes a vnode
 * reference under the space lock, releases it, and syncs under the vnode
 * lock as file_sync does. Returns the first write-back error. MS_ASYNC
 * and MS_INVALIDATE are the callers' business: the dirty pages are
 * already the cache's to write, and the mapping IS the cache.
 */
int vm_user_msync(struct vm_space *space, uint64_t base, size_t size);

/*
 * The page cache's two ways of reaching every mapping of a file, called
 * with the cache mutex held (docs/kernel-services/vfs/design.md, "Page
 * cache"): unmap the pages of the record whose file index is at or past
 * `keep` (truncate: before the cache frees them), and lower to read-only
 * the present PTEs of `n` pages from file index `index` (write-back:
 * the next write must fault to dirty the page again; shared records
 * only, a private record never maps a cache frame writable).
 */
void vm_file_map_truncate(struct vm_file_map *m, uint64_t keep);
void vm_file_map_writeprotect(struct vm_file_map *m, uint64_t index, unsigned n);
/* Whether the record's region covering file page `index` is executable:
 * a write() into such a page must synchronise the instruction stream
 * (M41), which the cache does by the frame's kernel alias. */
bool vm_file_map_exec_at(struct vm_file_map *m, uint64_t index);

/*
 * The futex's identity for a user word (docs/audit/next-subsystem-shared-futex.md):
 * a word in a shared file mapping is keyed by (vnode, file offset) with
 * a vnode reference taken into `out->held`; anything else by (space,
 * uaddr) with no reference. `private` (the Linux flag) skips the lookup,
 * and so does a space with no shared mapping at all. The lookup runs
 * under the space lock. Returns 0; the caller releases the reference
 * with vnode_put when the key is done with.
 */
struct futex_key;
int vm_user_futex_key(struct vm_space *space, uint64_t uaddr, bool private, struct futex_key *out);

/* Change the protection of every page of [base, base+size), splitting
 * regions at the ends and merging equal neighbours afterwards. -EINVAL
 * for W+X or a bad range; -EACCES if a FILE region in the range has a
 * maxprot that does not cover `prot` (nothing changes); -ENOMEM if a
 * page of the range is unmapped (nothing changes) or a split cannot be
 * allocated; -EBUSY if a
 * MAP_FIXED replacement has claimed part of the range -- splitting a
 * VM_REGION_QUIESCED region would copy the claim into pieces the
 * owner does not know about (invariant M40). The -EBUSY arrived with
 * that unit and this contract did not: a stale contract in a header
 * outranks one in a report, and review caught it. */
int vm_user_protect(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot);

/* Make the bytes of [base, base+size) visible to instruction fetch, for
 * the pages that are PRESENT. A demand-zero page never touched has no
 * leaf translation and nothing written to it: nothing to sync, and on
 * AArch64 cache maintenance by a VA with no translation is a
 * translation fault at EL1 with no fixup -- an unprivileged
 * mmap(RW); mprotect(RX) would panic the kernel. Walks the range page
 * by page under space->lock, so a leaf cannot be torn down between the
 * query and the maintenance. Call from the owning process's context;
 * the range must be inside the user window. */
void vm_user_sync_icache(struct vm_space *space, uint64_t base, size_t size);

/* Number of regions in a user space (tests). */
unsigned vm_user_region_count(struct vm_space *space);

/* Sum of every user region's size, in pages (tests): mapped_pages
 * should always equal it, and a replacement that miscounts is the way
 * to make it not. */
uint64_t vm_user_mapped_pages_sum(struct vm_space *space);

/* Whether any region of [base, base+size) is still claimed by a
 * replacement (tests): a claim outlasting its replacement would hang a
 * faulting thread rather than fail it. */
bool vm_user_range_quiesced(struct vm_space *space, uint64_t base, size_t size);

/* The process layer's resource limits, in pages (docs/kernel/security/design.md
 * §2): mapped pages bound vm_user_map_anon, populated pages bound the
 * demand-zero fault and populated maps. Lowering below the current use is
 * allowed: nothing already mapped changes, growth is what is refused. */
void vm_space_set_limits(struct vm_space *space, uint64_t mapped_pages, uint64_t anon_pages);

/* The calling CPU switches its translation root from `prev` to `next`
 * (either may be the kernel space): maintains tlb_cpus around the
 * arch activation. Called by arch_thread_switch_prepare with interrupts
 * off. */
void vm_space_switch(struct vm_space *prev, struct vm_space *next);

/* Lowest free range of `size` bytes at or above `from` inside
 * [USER_LO, USER_HI) with a guard gap; 0 if none. */
uint64_t vm_user_find_free(struct vm_space *space, uint64_t from, size_t size);

/* True if every page of [addr, addr+len) is inside one or more regions
 * of `space` that all carry `prot`. */
bool vm_user_range_mapped(struct vm_space *space, uint64_t addr, size_t len, vm_prot_t prot);

/* Take over paging from the loader. Requires pmm_init and kmalloc_init.
 * After return: kernel tables active, all RAM in the direct map, boot
 * tables freed, deferred frames released, fault handler installed. */
void vmm_init(void);

/* vm_kernel_alloc flags */
#define VM_KALLOC_GUARD       (1u << 0)  /* unmapped page below and above */
#define VM_KALLOC_POPULATE    (1u << 1)  /* map zeroed frames now instead of on fault */
#define VM_KALLOC_NEAR_KERNEL (1u << 2)  /* place in the near arena (top 2 GiB, -mcmodel=kernel reach) */

/* Allocate `size` bytes (page multiple) of kernel virtual memory backed by
 * fresh zeroed frames. Returns the base or 0 on failure. */
vaddr_t vm_kernel_alloc(size_t size, unsigned flags, vm_prot_t prot);

/* Free a vm_kernel_alloc result, unmapping and releasing every frame it
 * populated. Panics if `base` is not a live allocation. */
void vm_kernel_free(vaddr_t base);

/* Change the protection of a whole populated vm_kernel_alloc region
 * (typically RW -> RX or RW -> R once its bytes are final). Rewrites the
 * leaf entries and runs a TLB shootdown, so it needs interrupts enabled
 * and may not be called with kernel_space.lock held. Returns 0, -EINVAL
 * (not a populated kernel allocation, W+X requested, or VM_PROT_NONE). */
int vm_kernel_protect(vaddr_t base, vm_prot_t prot);

/* Map physical [pa, pa+size) (page aligned) into the arena, for MMIO.
 * Returns the virtual base or 0. */
vaddr_t vm_map_phys(paddr_t pa, size_t size, vm_prot_t prot, vm_cache_t cache);
void vm_unmap_phys(vaddr_t base);

/* Look up the region containing va in the kernel space. Returns NULL if
 * none. The pointer is valid only while the caller holds no expectation
 * of it surviving a vm_kernel_free of that region. */
const struct vm_region *vm_find_region(struct vm_space *space, vaddr_t va);

/* Translate a kernel virtual address through the active tables. */
static inline bool vm_query(vaddr_t va, paddr_t *pa, vm_prot_t *prot, vm_cache_t *cache,
                            size_t *page_size)
{
    return arch_mmu_query(&kernel_space.mmu, va, pa, prot, cache, page_size);
}

/* Fault kind bits: the arch layer's decoding, re-exported under the VM
 * name so callers of the fault path never include arch/trap.h. */
#define VM_FAULT_PRESENT  ARCH_FAULT_PRESENT
#define VM_FAULT_WRITE    ARCH_FAULT_WRITE
#define VM_FAULT_EXEC     ARCH_FAULT_EXEC
#define VM_FAULT_USER     ARCH_FAULT_USER
#define VM_FAULT_RESERVED ARCH_FAULT_RESERVED

struct vm_stats {
    uint64_t regions;
    uint64_t anon_pages;       /* frames populated for ANON regions */
    uint64_t faults_handled;   /* demand-zero populations */
    uint64_t fixups;           /* kernel-mode faults resumed at an exception fixup */
    /* FILE regions (docs/audit/next-subsystem-file-regions.md). Atomic:
     * they are counted under many spaces' locks. */
    uint64_t file_faults;         /* cache frames installed */
    uint64_t file_cow_faults;     /* private copies made */
    uint64_t file_dirty_faults;   /* a shared page's PTE raised to writable */
    uint64_t file_fault_retries;  /* the re-find found the world changed, or the page already present */
    uint64_t file_sigbus;         /* faults the file could not serve */
    uint64_t futex_shared_keys;   /* futex calls whose word was classified as a shared file word */
};

void vm_get_stats(struct vm_stats *out);

/*
 * Debug seam for the two held-fault proofs
 * (docs/audit/next-subsystem-file-regions.md, "One seam, for two
 * proofs"). Armed, the next FILE fault taken in any user space blocks
 * after its first phase -- the space lock released, the vnode
 * referenced, the cache mutex not yet taken -- until another FILE fault
 * in that space installs, or any part of that space is unmapped.
 * Event-driven, not timed. State: 0 idle, 1 armed, 2 held; readable as
 * sysctl debug.file_fault_hold. CONFIG_DEBUG only.
 */
void vm_test_file_hold_arm(void);
unsigned vm_test_file_hold_state(void);
void vm_dump(struct vm_space *space);

#endif /* KERNEL_VMM_H */
