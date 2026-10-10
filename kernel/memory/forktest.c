/*
 * forktest.c - vm_space_fork, the copy-on-write duplication of a user
 * address space (roadmap M3; docs/kernel/memory/design.md, "fork";
 * docs/kernel/memory/testing.md, "vm-fork").
 *
 * Spaces made here are never run by a process: the pages are read and
 * written through the direct map, and a write fault is resolved with
 * vm_test_write_fault, which is the fault handler's own copy-on-write
 * decision (cow_write_locked). The one place a real fault is taken is
 * the TLB check: this CPU switches to the parent, caches a translation,
 * and after the fork a kernel write through it must fault.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/selftest.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

#include <arch/cpu.h>
#include <arch/mmu.h>
#include <arch/user.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#if CONFIG_SELFTEST

#define FT_ANON   0x0000310000000000ULL   /* four populated anonymous pages */
#define FT_SHARED 0x0000310000100000ULL   /* a MAP_SHARED file mapping */
#define FT_PRIV   0x0000310000200000ULL   /* a private file mapping */
#define FT_PAGES  4u

struct pte {
    bool present;
    paddr_t pa;
    vm_prot_t prot;
};

static struct pte pte_of(struct vm_space *sp, vaddr_t va)
{
    struct pte t = { 0 };
    t.present = arch_mmu_query(&sp->mmu, va, &t.pa, &t.prot, NULL, NULL);
    return t;
}

static uint32_t refs(paddr_t pa)
{
    return __atomic_load_n(&phys_to_page(pa)->refcount, __ATOMIC_ACQUIRE);
}

static uint8_t byte_at(struct vm_space *sp, vaddr_t va)
{
    struct pte t = pte_of(sp, va);
    return t.present ? *(uint8_t *)phys_to_virt(t.pa) : 0;
}

/* A one-byte kernel write to a user address of the space this CPU has
 * active, inside the guard bracket, as the kernel writes user memory.
 * Returns the bytes not written: 0, or 1 when it faulted. */
static size_t user_write_byte(vaddr_t va, uint8_t v)
{
    arch_user_access_begin();
    size_t left = arch_copy_user_raw((void *)(uintptr_t)va, &v, 1);
    arch_user_access_end();
    return left;
}

/* How many mapping records the vnode's page cache lists. */
static unsigned vnode_mappings(struct vnode *vn)
{
    unsigned n = 0;
    struct list_node *it;
    pagecache_lock(vn);
    for (it = vn->pc.mappings.next; it != &vn->pc.mappings; it = it->next)
        n++;
    pagecache_unlock(vn);
    return n;
}

/* Everything up to and including the fork; the caller owns both spaces. */
static bool fork_setup(struct vm_space **src_out, struct vm_space **dst_out, struct vnode *vn, const char **reason)
{
    struct vm_space *src = NULL;
    CHECK(vm_space_create_user(&src) == 0);
    *src_out = src;
    CHECK(vm_user_map_anon(src, FT_ANON, FT_PAGES * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "ft-anon") == 0);
    for (unsigned i = 0; i < FT_PAGES; i++) {
        struct pte t = pte_of(src, FT_ANON + i * PAGE_SIZE);
        CHECK(t.present && (t.prot & VM_PROT_WRITE) && refs(t.pa) == 1);
        memset(phys_to_virt(t.pa), 0x40 + (int)i, PAGE_SIZE);
    }
    CHECK(vm_user_map_file(src, FT_SHARED, PAGE_SIZE, VM_PROT_RW, VM_PROT_RW, VM_MAP_SHARED, vn, 0, "ft-shared") == 0);
    CHECK(vm_user_map_file(src, FT_PRIV, PAGE_SIZE, VM_PROT_READ, VM_PROT_RW, 0, vn, 0, "ft-priv") == 0);
    CHECK(vm_space_fork(src, dst_out) == 0);
    return true;
}

static bool fork_body(struct vm_space *src, struct vm_space *dst, struct vnode *vn, unsigned maps_before,
                      const char **reason)
{
    struct vm_stats st0, st1;
    vm_get_stats(&st0);

    /* Shared, read-only in both, one reference per mapping. */
    for (unsigned i = 0; i < FT_PAGES; i++) {
        vaddr_t va = FT_ANON + i * PAGE_SIZE;
        struct pte a = pte_of(src, va), b = pte_of(dst, va);
        CHECK(a.present && b.present && a.pa == b.pa);
        CHECK(!(a.prot & VM_PROT_WRITE) && !(b.prot & VM_PROT_WRITE));
        CHECK(refs(a.pa) == 2);
    }
    CHECK(dst->anon_pages == src->anon_pages && dst->anon_pages >= FT_PAGES);
    CHECK(dst->mapped_pages == src->mapped_pages && dst->file_pages == src->file_pages);

    /* File mappings: a record of the child's own beside each parent's. */
    CHECK(vnode_mappings(vn) == maps_before + 4);   /* the parent's two, and the child's own two */
    const struct vm_region *ps = vm_find_region(src, FT_SHARED), *cs = vm_find_region(dst, FT_SHARED);
    const struct vm_region *pp = vm_find_region(src, FT_PRIV), *cp = vm_find_region(dst, FT_PRIV);
    CHECK(ps && cs && pp && cp && cs->kind == VM_REGION_FILE && cp->kind == VM_REGION_FILE);
    CHECK(cs->fmap != ps->fmap && cs->fmap->space == dst && cs->fmap->tag == ps->fmap->tag && cs->fmap->shared);
    CHECK(cp->fmap != pp->fmap && !cp->fmap->shared && cp->fmap->regions == 1);
    CHECK(dst->shared_maps == 1);

    /* Page 0: the child writes first and copies; the parent, now the last
     * user, takes its frame back writable without copying. */
    paddr_t orig = pte_of(src, FT_ANON).pa;
    CHECK(vm_test_write_fault(dst, FT_ANON) == 0);
    struct pte b0 = pte_of(dst, FT_ANON);
    CHECK(b0.present && b0.pa != orig && (b0.prot & VM_PROT_WRITE) && refs(b0.pa) == 1);
    CHECK(*(uint8_t *)phys_to_virt(b0.pa) == 0x40);   /* the copy has the contents */
    CHECK(refs(orig) == 1 && !(pte_of(src, FT_ANON).prot & VM_PROT_WRITE));
    CHECK(vm_test_write_fault(src, FT_ANON) == 0);
    struct pte a0 = pte_of(src, FT_ANON);
    CHECK(a0.pa == orig && (a0.prot & VM_PROT_WRITE) && refs(orig) == 1);
    /* Isolation both ways. */
    *(uint8_t *)phys_to_virt(b0.pa) = 0x99;
    *(uint8_t *)phys_to_virt(a0.pa) = 0x77;
    CHECK(byte_at(src, FT_ANON) == 0x77 && byte_at(dst, FT_ANON) == 0x99);

    /* Page 1: the other order -- the parent copies, the child takes back. */
    paddr_t orig1 = pte_of(dst, FT_ANON + PAGE_SIZE).pa;
    CHECK(vm_test_write_fault(src, FT_ANON + PAGE_SIZE) == 0);
    CHECK(pte_of(src, FT_ANON + PAGE_SIZE).pa != orig1 && refs(orig1) == 1);
    CHECK(vm_test_write_fault(dst, FT_ANON + PAGE_SIZE) == 0);
    CHECK(pte_of(dst, FT_ANON + PAGE_SIZE).pa == orig1 && (pte_of(dst, FT_ANON + PAGE_SIZE).prot & VM_PROT_WRITE));

    /* A write that finds the PTE writable already: nothing changes. */
    CHECK(vm_test_write_fault(dst, FT_ANON + PAGE_SIZE) == 0 && pte_of(dst, FT_ANON + PAGE_SIZE).pa == orig1);

    vm_get_stats(&st1);
    CHECK(st1.cow_copies >= st0.cow_copies + 2 && st1.cow_reuses >= st0.cow_reuses + 2);

    /* mprotect granting write keeps a shared frame read-only (page 2),
     * and its write still copies. */
    CHECK(vm_user_protect(src, FT_ANON + 2 * PAGE_SIZE, PAGE_SIZE, VM_PROT_READ) == 0);
    CHECK(vm_user_protect(src, FT_ANON + 2 * PAGE_SIZE, PAGE_SIZE, VM_PROT_RW) == 0);
    struct pte a2 = pte_of(src, FT_ANON + 2 * PAGE_SIZE);
    CHECK(!(a2.prot & VM_PROT_WRITE) && refs(a2.pa) == 2);
    CHECK(vm_test_write_fault(src, FT_ANON + 2 * PAGE_SIZE) == 0);
    CHECK(pte_of(src, FT_ANON + 2 * PAGE_SIZE).pa != a2.pa && refs(a2.pa) == 1);
    return true;
}

/*
 * The parent's lowered PTE must not survive in another CPU's TLB. The
 * local CPU proves nothing here: arch_mmu_protect invalidates its own
 * entry, so only other CPUs depend on the shootdown -- a check on this
 * CPU alone passed with the shootdown removed, on both architectures.
 *
 * So a helper thread on another CPU switches to the parent, writes page
 * 3 (a writable translation is now cached there), and keeps the space
 * active with preemption off and interrupts on -- the shootdown's IPI
 * must be able to land, and no switch may flush for it. After the fork it
 * writes again: a kernel thread has no process, so a write that faults
 * is unserviced and takes its fixup, and a stale writable translation
 * would have let the byte through.
 */
struct tlb_probe {
    struct vm_space *sp;
    vaddr_t va;
    uint32_t stage;          /* 1: translation cached; 2: forked; 3: done */
    size_t before, after;
};

static void tlb_probe_thread(void *arg)
{
    struct tlb_probe *t = arg;
    preempt_disable();
    arch_irq_state_t s = arch_irq_save();
    struct vm_space *restore = this_cpu()->cur_space ? this_cpu()->cur_space : &kernel_space;
    vm_space_switch(restore, t->sp);
    arch_irq_restore(s);
    t->before = user_write_byte(t->va, 0x11);
    __atomic_store_n(&t->stage, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&t->stage, __ATOMIC_ACQUIRE) != 2u)
        arch_cpu_relax();
    t->after = user_write_byte(t->va, 0x22);
    s = arch_irq_save();
    vm_space_switch(t->sp, restore);
    arch_irq_restore(s);
    preempt_enable();
    __atomic_store_n(&t->stage, 3u, __ATOMIC_RELEASE);
    thread_exit(0);
}

static bool fork_tlb(const char **reason, bool *skipped)
{
    *skipped = false;
    cpumask_t others = cpu_online_mask() & ~CPUMASK_OF(raw_cpu_id());
    if (others == 0) {
        *skipped = true;   /* one CPU: there is no other TLB to check */
        return true;
    }
    struct vm_space *src = NULL, *dst = NULL;
    const vaddr_t va = FT_ANON + 3 * PAGE_SIZE;
    CHECK(vm_space_create_user(&src) == 0);
    CHECK(vm_user_map_anon(src, FT_ANON, FT_PAGES * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "ft-tlb") == 0);
    static struct tlb_probe t;
    t = (struct tlb_probe){ .sp = src, .va = va };
    struct thread *th = thread_create_on(tlb_probe_thread, &t, "fork-tlb", SCHED_PRIO_DEFAULT, others);
    CHECK(th != NULL);
    while (__atomic_load_n(&t.stage, __ATOMIC_ACQUIRE) != 1u)
        sched_yield();
    int frc = vm_space_fork(src, &dst);
    __atomic_store_n(&t.stage, 2u, __ATOMIC_RELEASE);
    thread_join(th);
    uint8_t seen = byte_at(src, va);
    if (dst)
        vm_space_destroy(dst);
    vm_space_destroy(src);
    CHECK(frc == 0);
    CHECK(t.before == 0);                  /* writable before: the translation was cached there */
    CHECK(t.after == 1 && seen == 0x11);   /* read-only after, on that CPU too */
    return true;
}

bool selftest_vm_fork(const char **reason)
{
    struct file *f = NULL;
    CHECK(vfs_open(NULL, "/tmp/vm-fork-test", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0600, &f) == 0);
    static const char text[] = "the file under a forked mapping";
    bool ok = file_write(f, text, sizeof(text)) == (int64_t)sizeof(text);
    struct vnode *vn = f->vn;
    vnode_get(vn);
    file_put(f);
    unsigned maps_before = vnode_mappings(vn);

    struct vm_space *src = NULL, *dst = NULL;
    if (ok)
        ok = fork_setup(&src, &dst, vn, reason);
    else
        *reason = "cannot write the test file";
    if (ok)
        ok = fork_body(src, dst, vn, maps_before, reason);

    /* Teardown order: the parent first. The child keeps every frame it
     * still shares, now at one reference, and its contents. */
    if (ok) {
        paddr_t pa3 = pte_of(dst, FT_ANON + 3 * PAGE_SIZE).pa;
        vm_space_destroy(src);
        src = NULL;
        if (refs(pa3) != 1 || byte_at(dst, FT_ANON + 3 * PAGE_SIZE) != 0x43 || byte_at(dst, FT_ANON) != 0x99 ||
            vnode_mappings(vn) != maps_before + 2) {
            *reason = "the child lost a frame or a mapping record when the parent went";
            ok = false;
        }
    }
    if (src)
        vm_space_destroy(src);
    if (dst)
        vm_space_destroy(dst);
    if (ok && vnode_mappings(vn) != maps_before) {
        *reason = "a mapping record outlived both spaces";
        ok = false;
    }
    vnode_put(vn);
    (void)vfs_unlink(NULL, "/tmp/vm-fork-test");
    if (!ok)
        return false;

    /* Limits: a parent already over its anonymous limit cannot fork. */
    struct vm_space *p2 = NULL, *c2 = NULL;
    CHECK(vm_space_create_user(&p2) == 0);
    CHECK(vm_user_map_anon(p2, FT_ANON, FT_PAGES * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "ft-limit") == 0);
    vm_space_set_limits(p2, UINT64_MAX, FT_PAGES - 1);
    int lrc = vm_space_fork(p2, &c2);
    vm_space_destroy(p2);
    CHECK(lrc == -ENOMEM && c2 == NULL);

    bool tlb_skipped;
    if (!fork_tlb(reason, &tlb_skipped))
        return false;
    kinfo("selftest: vm-fork: pages shared read-only at two references, a copy on either side's write and "
          "the last user's frame taken back, mprotect kept a shared frame read-only, child file records "
          "of its own, limits refuse, parent-first teardown; %s",
          tlb_skipped ? "one CPU: the other-CPU TLB check needs two, not run"
                      : "another CPU's writable translation gone after the fork");
    return true;
}

#else
bool selftest_vm_fork(const char **reason)
{
    (void)reason;
    return true;
}
#endif
