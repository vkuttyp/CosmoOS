/*
 * test_pagecache.c - The page cache (kernel-services/vfs/pagecache.c) on the host: roadmap 2.0 SA7. A digest
 * recorded from the C over random sessions on files of three mounts -- one whose cache is its store, with a page
 * budget, and two that write back -- with reads, writes past the end and across pages, syncs whose writepage and
 * writepages fail or report nonsense, dirty checks that refuse, truncations, drops, whole-page gets and puts,
 * faults through mappings that execute, are text and hold the frames, reclaim under a changing limit with
 * references that cannot be had and locks that are busy, the dirty bound's throttle with a write-back thread that
 * runs while a writer sleeps, and the frame allocator running dry. Every call into the kernel with its arguments,
 * every result, every byte read back and the statistics after each step are in the digest; every frame, entry and
 * reference is given back.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/mutex.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>

/* The C library's headers are not included: their pid_t is not the kernel's. */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void free(void *p);
int printf(const char *fmt, ...);

/* The digest of the C (pagecache.c, 2026-11-07), which the CeeSafe port gives. */
#define PAGECACHE_DIGEST 0x1c24149eb69a9ee7ull
#define ROUNDS 30000u
#define SEED 0x9A6EC0DE20261107ull
#define NV 6u              /* files */
#define MAXPAGES 40u       /* a file's pages at most, the store's size: more than PC_HASH, so buckets chain */
#define ARENA_PAGES 48u    /* fewer frames than the files can want */
#define NMAPS 3u
#define MAXREFS 6u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void note(uint32_t which, uint64_t value)
{
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

static bool g_faults = true;

static bool chance(unsigned n) { return g_faults && pick(n) == 0; }

/* ---- the files ---- */

static struct mount g_mnt[3];      /* 0: the cache is the store, with a budget; 1 and 2 write back */
static struct vnode g_vn[NV];
static uint8_t g_store[NV][MAXPAGES][PAGE_SIZE];

static unsigned vid(const struct vnode *vn) { return (unsigned)(vn - g_vn); }

static int fake_readpage(struct vnode *vn, uint64_t index, void *buf)
{
    note(10, vid(vn));
    note(11, index);
    if (chance(14)) {
        note(12, 0);
        return -EIO;
    }
    if (index < MAXPAGES)
        memcpy(buf, g_store[vid(vn)][index], PAGE_SIZE);
    else
        memset(buf, 0x5A, PAGE_SIZE);
    return 0;
}

static void store(struct vnode *vn, uint64_t index, const void *buf)
{
    note(13, index);
    mix(buf, PAGE_SIZE);
    if (index < MAXPAGES)
        memcpy(g_store[vid(vn)][index], buf, PAGE_SIZE);
}

static int fake_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    note(14, vid(vn));
    note(15, index);
    if (chance(10)) {
        note(16, 0);
        return -EIO;
    }
    store(vn, index, buf);
    return 0;
}

static int fake_writepages(struct vnode *vn, uint64_t index, void *const *pages, unsigned n, unsigned *done)
{
    note(17, vid(vn));
    note(18, index);
    note(19, n);
    EXPECT(n >= 2 && n <= PAGECACHE_WRITE_RUN);
    if (chance(12)) {
        note(20, 0);
        return -ENOSPC;
    }
    if (chance(15)) {   /* nonsense: nothing, or more than it was given */
        unsigned bad = pick(2);
        *done = bad == 0 ? 0 : n + 1;
        note(21, *done);
        return 0;
    }
    unsigned k = 1 + pick(n);
    for (unsigned i = 0; i < k; i++)
        store(vn, index + i, pages[i]);
    *done = k;
    return 0;
}

static int fake_dirty_check(struct vnode *vn, uint64_t index, unsigned dirty)
{
    note(22, vid(vn));
    note(23, index);
    note(24, dirty);
    unsigned r = pick(24);
    int rc = r == 0 ? -ENOSPC : r == 1 ? -EFBIG : r == 2 ? -EIO : 0;
    if (dirty > 4 && r < 6)
        rc = pick(2) == 0 ? -ENOSPC : -EFBIG;   /* room the file's own dirty pages hold */
    note(25, (uint64_t)(int64_t)rc);
    return rc;
}

/* A filesystem with writepages and a dirty check; one with writepage alone; one with neither (its pages are
 * written by nobody: write-back marks them clean). */
static const struct vnode_ops g_ops_full = {
    .readpage = fake_readpage, .writepage = fake_writepage, .writepages = fake_writepages,
    .dirty_check = fake_dirty_check,
};
static const struct vnode_ops g_ops_page = { .readpage = fake_readpage, .writepage = fake_writepage };
static const struct vnode_ops g_ops_none = { 0 };

/* ---- the mutexes: held or not, tried and found busy now and then ---- */

#define NMUTEX 64u
static const struct mutex *g_mutex[NMUTEX];
static bool g_held[NMUTEX];

static unsigned mutex_slot(const struct mutex *m)
{
    for (unsigned i = 0; i < NMUTEX; i++)
        if (g_mutex[i] == m)
            return i;
    for (unsigned i = 0; i < NMUTEX; i++)
        if (g_mutex[i] == NULL) {
            g_mutex[i] = m;
            return i;
        }
    EXPECT(false);
    return 0;
}

void mutex_init(struct mutex *m, const char *name)
{
    note(30, strlen(name));
    g_held[mutex_slot(m)] = false;
}

void mutex_lock(struct mutex *m)
{
    unsigned i = mutex_slot(m);
    EXPECT(!g_held[i]);
    g_held[i] = true;
}

bool mutex_trylock(struct mutex *m)
{
    unsigned i = mutex_slot(m);
    if (g_held[i] || chance(6)) {
        note(31, 0);
        return false;
    }
    g_held[i] = true;
    return true;
}

void mutex_unlock(struct mutex *m)
{
    unsigned i = mutex_slot(m);
    EXPECT(g_held[i]);
    g_held[i] = false;
}

bool lockdep_is_held(const void *lock, unsigned kind)
{
    EXPECT(kind == LOCKDEP_KIND_MUTEX);
    return g_held[mutex_slot(lock)];
}

void lockdep_completion_init(uint16_t *slot, const char *name, uintptr_t ip)
{
    (void)ip;
    note(32, strlen(name));
    *slot = 7;
}

void lockdep_completion_wait(const void *c, uint16_t *slot, const char *name, uintptr_t ip)
{
    (void)c;
    (void)ip;
    note(33, *slot);
    note(34, strlen(name));
}

void lockdep_completion_signal(const void *c, uint16_t *slot, const char *name, uintptr_t ip)
{
    (void)c;
    (void)ip;
    note(35, *slot);
    note(36, strlen(name));
}

/* ---- the allocator ---- */

static long g_live;

void *kzalloc(size_t size)
{
    note(40, size);
    if (chance(25)) {
        note(41, 0);
        return NULL;
    }
    void *p = calloc(1, size);
    EXPECT(p != NULL);
    g_live++;
    return p;
}

void kfree(void *p)
{
    if (p == NULL)
        return;
    g_live--;
    free(p);
}

bool faultinject_should_fail(enum fi_kind kind)
{
    EXPECT(kind == FI_FILE_READPAGE);
    bool fail = chance(40);
    note(42, fail);
    return fail;
}

/* ---- references to the files ---- */

bool kobject_tryget(struct kobject *obj)
{
    (void)obj;
    if (chance(8)) {
        note(43, 0);
        return false;
    }
    return true;
}

void vnode_put(struct vnode *vn) { note(44, vid(vn)); }

/* ---- the mappings: a record on the cache's list, the frames it references ---- */

struct fmap {
    struct vm_file_map m;   /* first: the cache hands this back */
    bool exec;
    unsigned n;
    struct page *ref[MAXREFS];
    uint64_t idx[MAXREFS];
};

static struct fmap *g_maps[NV][NMAPS];

void pmm_page_get(struct page *page)
{
    EXPECT(page->refcount >= 1);
    page->refcount++;
}

static void unref(struct page *page)
{
    EXPECT(page->refcount >= 2);   /* the cache's own reference stays */
    page->refcount--;
}

bool vm_file_map_exec_at(struct vm_file_map *m, uint64_t index)
{
    struct fmap *f = (struct fmap *)m;
    note(50, index);
    return f->exec && index % 2 == 0;
}

void vm_file_map_writeprotect(struct vm_file_map *m, uint64_t index, unsigned n)
{
    (void)m;
    note(51, index);
    note(52, n);
}

void vm_file_map_truncate(struct vm_file_map *m, uint64_t keep)
{
    struct fmap *f = (struct fmap *)m;
    note(53, keep);
    unsigned w = 0;
    for (unsigned i = 0; i < f->n; i++) {
        if (f->idx[i] >= keep) {
            unref(f->ref[i]);
        } else {
            f->ref[w] = f->ref[i];
            f->idx[w] = f->idx[i];
            w++;
        }
    }
    f->n = w;
}

void arch_mmu_sync_icache_kernel(vaddr_t va, size_t len)
{
    note(54, (va - pmm_hhdm_base) % PAGE_SIZE);
    note(55, len);
}

static void map_remove(unsigned v, unsigned k)
{
    struct fmap *f = g_maps[v][k];
    if (f == NULL)
        return;
    for (unsigned i = 0; i < f->n; i++)
        unref(f->ref[i]);
    list_remove(&f->m.link);
    free(f);
    g_maps[v][k] = NULL;
}

/* ---- the sleeps: wait_event_timeout's primitives (the C) and wait_floor_until_timeout (the port) ---- */

static bool g_expired;

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    (void)wq;
    note(60, strlen(name));
}

unsigned waitqueue_wake_all(struct waitqueue *wq)
{
    (void)wq;
    note(61, 0);
    return 0;
}

void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e) { (void)wq; (void)e; }
void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e) { (void)wq; (void)e; }
void wait_timeout_init(struct wait_timeout *wt, struct waitqueue *wq) { (void)wt; (void)wq; g_expired = false; }
void wait_timeout_fired(struct timer *t, void *arg) { (void)t; (void)arg; }
bool wait_timeout_expired(const struct wait_timeout *wt) { (void)wt; return g_expired; }
void timer_setup(struct timer *t, timer_fn fn, void *arg) { (void)t; (void)fn; (void)arg; }
void timer_start(struct timer *t, uint64_t ns) { (void)t; note(62, ns); }
bool timer_cancel_sync(struct timer *t) { (void)t; return false; }

/* The writer sleeps: a write-back thread cleans another file meanwhile, or the time runs out. */
void sched_block_current(void)
{
    unsigned v = pick(NV);
    note(63, v);
    if (pick(2) == 0) {
        int rc = pagecache_sync(&g_vn[v]);
        note(64, (uint64_t)(int64_t)rc);
    }
    g_expired = pick(3) == 0;
}

bool wait_floor_until_timeout(struct waitqueue *wq, bool (*cond)(void *), void *arg, uint64_t ns)
{
    bool ok = cond(arg);
    if (!ok) {
        wait_timeout_init(NULL, wq);
        timer_start(NULL, ns);
        for (;;) {
            ok = cond(arg);
            if (ok || wait_timeout_expired(NULL))
                break;
            sched_block_current();
        }
    }
    return ok;
}

/* ---- a session ---- */

static uint32_t g_seen[NV];

static void check_stats(void)
{
    struct pagecache_stats st;
    pagecache_get_stats(&st);
    mix(&st, sizeof(st));
    note(70, pagecache_dirty_pages());
    note(71, pagecache_dirty_over());
}

static uint64_t some_offset(void)
{
    unsigned kind = pick(8);
    uint64_t page = pick(MAXPAGES);
    uint64_t in = pick(PAGE_SIZE);
    if (kind == 0)
        return page * PAGE_SIZE;
    if (kind == 1)
        return page * PAGE_SIZE + PAGE_SIZE - 1 - pick(8);
    return page * PAGE_SIZE + in;
}

static size_t some_length(uint64_t off)
{
    unsigned kind = pick(6);
    uint64_t room = (uint64_t)MAXPAGES * PAGE_SIZE - (off < (uint64_t)MAXPAGES * PAGE_SIZE ? off : 0);
    size_t n = kind == 0 ? 0 : kind == 1 ? 1 + pick(16) : kind == 2 ? PAGE_SIZE : 1 + pick(3 * PAGE_SIZE);
    return n > room ? (size_t)room : n;
}

static uint8_t g_buf[4 * PAGE_SIZE];

static void op_read(struct vnode *vn)
{
    uint64_t off = pick(10) == 0 ? vn->size + pick(3) : some_offset();
    size_t len = some_length(off);
    note(80, off);
    note(81, len);
    memset(g_buf, 0xEE, len);
    int64_t rc = pagecache_read(vn, off, g_buf, len);
    note(82, (uint64_t)rc);
    if (rc > 0)
        mix(g_buf, (size_t)rc);
}

static void op_write(struct vnode *vn)
{
    uint64_t off = some_offset();
    size_t len = some_length(off);
    if (pick(40) == 0) {
        off = UINT64_MAX - pick(PAGE_SIZE);   /* past the end of the offsets */
        len = PAGE_SIZE;
    }
    uint8_t fill = (uint8_t)pick(256);
    for (size_t i = 0; i < len; i++)
        g_buf[i] = (uint8_t)(fill + i * 7);
    note(83, off);
    note(84, len);
    mutex_lock(&vn->lock);
    int64_t rc = pagecache_write(vn, off, g_buf, len);
    mutex_unlock(&vn->lock);
    note(85, (uint64_t)rc);
    note(86, vn->size);
}

static void op_fault(unsigned v)
{
    struct vnode *vn = &g_vn[v];
    unsigned k = pick(NMAPS);
    if (g_maps[v][k] == NULL) {
        struct fmap *f = calloc(1, sizeof(*f));
        EXPECT(f != NULL);
        f->m.vn = vn;
        bool text = pick(4) == 0;
        bool exec = pick(3) == 0;
        f->m.text = text;
        f->exec = exec;
        g_maps[v][k] = f;
        pagecache_lock(vn);
        list_push_back(&vn->pc.mappings, &f->m.link);
        pagecache_unlock(vn);
        note(90, k);
        return;
    }
    struct fmap *f = g_maps[v][k];
    uint64_t index = pick(MAXPAGES + 2);
    bool dirty = pick(3) == 0;
    note(91, index);
    note(92, dirty);
    pagecache_lock(vn);
    struct page *page = NULL;
    int rc = pagecache_fault_page(vn, index, dirty, &page);
    note(93, (uint64_t)(int64_t)rc);
    if (rc == 0) {
        EXPECT(page != NULL && (page->flags & PG_PAGECACHE));
        mix(page_to_virt(page), PAGE_SIZE);
        if (f->n < MAXREFS) {
            f->ref[f->n] = page;
            f->idx[f->n] = index;
            f->n++;
        } else {
            unref(page);
        }
    }
    pagecache_unlock(vn);
}

static void op_unmap(unsigned v)
{
    unsigned k = pick(NMAPS);
    struct fmap *f = g_maps[v][k];
    if (f == NULL)
        return;
    unsigned how = pick(3);
    note(94, how);
    pagecache_lock(&g_vn[v]);
    if (how == 0) {
        map_remove(v, k);
    } else if (f->n > 0) {
        unsigned i = pick(f->n);
        unref(f->ref[i]);
        f->ref[i] = f->ref[f->n - 1];
        f->idx[i] = f->idx[f->n - 1];
        f->n--;
    }
    pagecache_unlock(&g_vn[v]);
}

static void op_truncate(struct vnode *vn)
{
    unsigned kind = pick(3);
    uint64_t size = kind == 0 ? 0 : kind == 1 ? vn->size / 2 : some_offset();
    if (size > vn->size)
        size = vn->size;
    note(95, size);
    mutex_lock(&vn->lock);
    pagecache_truncate(vn, size);
    vn->size = size;   /* after the trim, as the filesystems do */
    mutex_unlock(&vn->lock);
}

static void op_drop(unsigned v)
{
    struct vnode *vn = &g_vn[v];
    for (unsigned k = 0; k < NMAPS; k++)
        map_remove(v, k);
    bool lost = pick(2) == 0;
    unsigned n = pagecache_drop(vn, lost);
    note(96, lost);
    note(97, n);
    vn->size = 0;
    vn->pc.trim_bound = UINT64_MAX;
}

static void op_pages(struct vnode *vn)
{
    uint64_t index = pick(MAXPAGES);
    note(98, index);
    if (pick(2) == 0) {
        memset(g_buf, 0xEE, PAGE_SIZE);
        int rc = pagecache_get_page(vn, index, g_buf);
        note(99, (uint64_t)(int64_t)rc);
        if (rc == 0)
            mix(g_buf, PAGE_SIZE);
    } else {
        uint8_t fill = (uint8_t)pick(256);
        memset(g_buf, fill, PAGE_SIZE);
        int rc = pagecache_put_page(vn, index, g_buf);
        note(100, (uint64_t)(int64_t)rc);
    }
}

static void op_errors(unsigned v)
{
    struct vnode *vn = &g_vn[v];
    int err = 0;
    uint32_t now = 0;
    bool newer = pagecache_error_since(&vn->pc, g_seen[v], &err, &now);
    note(101, newer);
    note(102, (uint64_t)(int64_t)err);
    note(103, now);
    if (pick(2) == 0)
        g_seen[v] = now;
    note(104, pagecache_wb_seq(&vn->pc));
}

static void session(void)
{
    static const uint64_t limits[] = { 0, 8, 20, 40, 64 };
    for (unsigned r = 0; r < ROUNDS; r++) {
        unsigned v = pick(NV);
        struct vnode *vn = &g_vn[v];
        unsigned op = pick(22);
        note(200, op);
        note(201, v);
        if (r == ROUNDS / 6)
            pagecache_dirty_init();   /* the throttle is a no-op until then */
        switch (op) {
        case 0: case 1: case 2: case 3:
            op_read(vn);
            break;
        case 4: case 5: case 6: case 7: case 8:
            op_write(vn);
            break;
        case 9: case 10: {
            mutex_lock(&vn->lock);
            int rc = pagecache_sync(vn);
            mutex_unlock(&vn->lock);
            note(110, (uint64_t)(int64_t)rc);
            break;
        }
        case 11:
            op_truncate(vn);
            break;
        case 12:
            if (pick(4) == 0)
                op_drop(v);
            break;
        case 13:
            op_pages(vn);
            break;
        case 14: case 15:
            op_fault(v);
            break;
        case 16:
            op_unmap(v);
            break;
        case 17: {
            unsigned max = pick(12);
            note(111, pagecache_reclaim(max));
            break;
        }
        case 18: {
            uint64_t limit = limits[pick(5)];
            pagecache_set_limit(limit);
            note(112, pagecache_limit());
            break;
        }
        case 19:
            pagecache_throttle(vn);
            break;
        case 20: {
            pagecache_lock(vn);
            note(113, pagecache_text_busy(vn));
            pagecache_unlock(vn);
            break;
        }
        case 21:
            op_errors(v);
            break;
        }
        check_stats();
        for (unsigned i = 0; i < NMUTEX; i++)
            EXPECT(!g_held[i]);
    }
}

static void setup(void)
{
    g_mnt[0].flags = MOUNT_CACHE_IS_STORE;
    g_mnt[0].cache_limit_pages = 20;
    for (unsigned v = 0; v < NV; v++) {
        struct vnode *vn = &g_vn[v];
        vn->mnt = &g_mnt[v % 3];
        vn->type = v == 5 ? VNODE_DIR : VNODE_REG;
        vn->ops = v % 3 == 0 ? &g_ops_page : v == 4 ? &g_ops_none : &g_ops_full;
        mutex_init(&vn->lock, "vnode");
        pagecache_init(&vn->pc);
        note(120, vn->pc.trim_bound);
        for (unsigned p = 0; p < MAXPAGES; p++)
            memset(g_store[v][p], (int)(v * 31 + p), PAGE_SIZE);
    }
}

static void test_pagecache_sessions(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    host_arena_init((size_t)ARENA_PAGES * PAGE_SIZE);
    uint64_t free0 = host_arena_free_pages();
    setup();
    session();

    /* A cache with a mapping is not dropped: the frames are the mapping's. */
    g_faults = false;
    op_fault(1);
    if (g_maps[1][0] == NULL && g_maps[1][1] == NULL && g_maps[1][2] == NULL)
        op_fault(1);
    EXPECT_PANIC(pagecache_drop(&g_vn[1], true));
    g_held[mutex_slot(&g_vn[1].pc.lock)] = false;

    for (unsigned v = 0; v < NV; v++)
        op_drop(v);
    check_stats();
    EXPECT(pagecache_dirty_pages() == 0);
    EXPECT(host_arena_free_pages() == free0);
    EXPECT(g_live == 0);
    for (unsigned m = 0; m < 3; m++)
        EXPECT(g_mnt[m].cache_pages == 0 && g_mnt[m].cache_dirty == 0);
    printf("  pagecache digest 0x%016llx\n", (unsigned long long)g_digest);
    EXPECT(g_digest == PAGECACHE_DIGEST);
    host_arena_destroy();
}

int main(void)
{
    static const struct host_test tests[] = {
        { "pagecache_sessions", test_pagecache_sessions },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
