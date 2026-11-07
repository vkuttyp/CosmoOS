/*
 * vfstest.c - Self-tests for CRC32C, the page cache and the VFS on ramfs.
 */

#include <arch/cpu.h>
#include <kernel/aio.h>
#include <kernel/blk.h>
#include <kernel/completion.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/faultinject.h>
#include <kernel/object.h>
#include <kernel/pagecache.h>
#include <kernel/pipe.h>
#include <kernel/pmm.h>
#include <kernel/ramblk.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/page.h>
#include <kernel/utsns.h>
#include <kernel/selftest.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <uapi/cosmo/fsctl.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/thread.h>
#include <kernel/percpu.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* --- write-back errors: recorded, reported once, counted when lost ---------
 *
 * cosmofs on a RAM block device, which completes every bio in its
 * submitter's context, so FI_BLK_COMPLETE scoped to this thread refuses
 * exactly the next `budget` write-backs this thread issues. */

static int wb_setup(struct blkdev **out)
{
    /* A test that failed mid-way left its mount: take it down first, so
     * one failure does not fail every test after it at setup. */
    (void)vfs_umount2("/mnt/wb", VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(256);
    if (bd == NULL)
        return -ENOMEM;
    int rc = cosmofs_format(bd);
    if (rc == 0) {
        rc = vfs_mkdir(NULL, "/mnt/wb", 0755);
        if (rc == -EEXIST)
            rc = 0;
    }
    if (rc == 0)
        rc = vfs_mount("/mnt/wb", "cosmofs", bd, 0);
    if (rc) {
        ramblk_destroy(bd);
        return rc;
    }
    *out = bd;
    return 0;
}

static void wb_teardown(struct blkdev *bd)
{
    vfs_umount("/mnt/wb");
    vfs_rmdir(NULL, "/mnt/wb");
    ramblk_destroy(bd);
}

#if CONFIG_FAULTINJECT

/* A page of a known pattern, `k` distinguishing files. */
static void wb_fill(uint8_t *page, unsigned k)
{
    for (size_t i = 0; i < PAGE_SIZE; i++)
        page[i] = (uint8_t)(i * 3u + k);
}

static bool wb_reads_back(const char *path, const uint8_t *page)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f) != 0)
        return false;
    uint8_t *buf = kmalloc(PAGE_SIZE, 0);
    bool ok = buf != NULL && file_read(f, buf, PAGE_SIZE) == (int64_t)PAGE_SIZE && memcmp(buf, page, PAGE_SIZE) == 0;
    kfree(buf);
    file_put(f);
    return ok;
}

bool selftest_wb_error_fsync(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *data = kmalloc(3 * PAGE_SIZE, 0);
    CHECK(data != NULL);
    for (unsigned k = 0; k < 3; k++)
        wb_fill(data + k * PAGE_SIZE, k);
    struct pagecache_stats p0, p1;
    pagecache_get_stats(&p0);

    struct file *f;
    CHECK(vfs_open(NULL, "/mnt/wb/f", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, data, 3 * PAGE_SIZE) == 3 * (int64_t)PAGE_SIZE);
    CHECK(f->vn->pc.nr_dirty == 3);

    /* One refused write-back: fsync says so, and throws nothing away. */
    faultinject_set(FI_BLK_COMPLETE, 1, 1, thread_current());
    int s1 = file_sync(f);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 1);
    CHECK(s1 == -EIO);
    CHECK(f->vn->pc.nr_dirty > 0);
    pagecache_get_stats(&p1);
    CHECK(p1.wb_errors == p0.wb_errors + 1);

    /* The next attempt writes, and the file has already been told. */
    CHECK(file_sync(f) == 0);
    CHECK(f->vn->pc.nr_dirty == 0);
    CHECK(file_sync(f) == 0);
    file_put(f);

    /* On disk: a fresh mount reads it back. */
    CHECK(vfs_umount("/mnt/wb") == 0);
    CHECK(vfs_mount("/mnt/wb", "cosmofs", bd, 0) == 0);
    CHECK(wb_reads_back("/mnt/wb/f", data));
    kfree(data);
    wb_teardown(bd);
    return true;
}

bool selftest_wb_error_once(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *page = kmalloc(PAGE_SIZE, 0);
    CHECK(page != NULL);
    wb_fill(page, 9);

    struct file *a, *b, *c;
    CHECK(vfs_open(NULL, "/mnt/wb/o", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &a) == 0);
    CHECK(vfs_open(NULL, "/mnt/wb/o", COSMO_O_RDWR, 0, &b) == 0);
    CHECK(a->vn == b->vn);
    CHECK(file_write(a, page, PAGE_SIZE) == (int64_t)PAGE_SIZE);

    /* A's attempt fails; A hears its own failure once. */
    faultinject_set(FI_BLK_COMPLETE, 1, 1, thread_current());
    int sa = file_sync(a);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 1 && sa == -EIO);

    /* B's attempt writes the page, and B is told of the failure it was
     * open for -- once. A was told already. C, opened later, never. */
    CHECK(file_sync(b) == -EIO);
    CHECK(b->vn->pc.nr_dirty == 0);
    CHECK(file_sync(b) == 0);
    CHECK(file_sync(a) == 0);
    CHECK(vfs_open(NULL, "/mnt/wb/o", COSMO_O_RDONLY, 0, &c) == 0);
    CHECK(file_sync(c) == 0);
    file_put(c);
    file_put(b);
    file_put(a);
    CHECK(wb_reads_back("/mnt/wb/o", page));
    kfree(page);
    wb_teardown(bd);
    return true;
}

/* A file in a handle table, as the syscall holds it. */
static int wb_open_in_table(struct handle_table *t, const char *path, const uint8_t *page)
{
    struct file *f;
    int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f);
    if (rc)
        return rc;
    if (file_write(f, page, PAGE_SIZE) != (int64_t)PAGE_SIZE) {
        file_put(f);
        return -EIO;
    }
    int h = handle_install(t, &f->obj, HANDLE_RIGHT_OWNER);
    file_put(f);   /* the table holds the reference now */
    return h;
}

bool selftest_wb_error_close(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *page = kmalloc(PAGE_SIZE, 0);
    CHECK(page != NULL);
    wb_fill(page, 5);
    struct handle_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    CHECK(t != NULL);
    handle_table_init(t);
    struct pagecache_stats p0, p1;
    pagecache_get_stats(&p0);

    int h = wb_open_in_table(t, "/mnt/wb/c", page);
    CHECK(h >= 0);
    /* close: the flush's write-back is refused and close says so; the
     * handle is closed regardless; the release's own retry then writes
     * the page (the injection is spent), so nothing is lost. */
    faultinject_set(FI_BLK_COMPLETE, 1, 1, thread_current());
    int rc = handle_close(t, h);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 1);
    CHECK(rc == -EIO);
    CHECK(handle_close(t, h) == -EBADF);
    pagecache_get_stats(&p1);
    CHECK(p1.dropped_dirty == p0.dropped_dirty);
    CHECK(p1.wb_errors == p0.wb_errors + 1);
    /* A clean close reports nothing. */
    h = wb_open_in_table(t, "/mnt/wb/c2", page);
    CHECK(h >= 0);
    CHECK(handle_close(t, h) == 0);
    handle_table_destroy(t);
    kfree(t);

    CHECK(vfs_umount("/mnt/wb") == 0);
    CHECK(vfs_mount("/mnt/wb", "cosmofs", bd, 0) == 0);
    CHECK(wb_reads_back("/mnt/wb/c", page));
    CHECK(wb_reads_back("/mnt/wb/c2", page));
    kfree(page);
    wb_teardown(bd);
    return true;
}

bool selftest_wb_error_lost(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *page = kmalloc(PAGE_SIZE, 0);
    CHECK(page != NULL);
    wb_fill(page, 2);
    struct handle_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    CHECK(t != NULL);
    handle_table_init(t);
    struct pagecache_stats p0, p1;
    pagecache_get_stats(&p0);

    int h = wb_open_in_table(t, "/mnt/wb/l", page);
    CHECK(h >= 0);
    /* Three refusals: the flush's, the file release's retry, the vnode
     * release's last attempt. Then the page is gone -- counted, said. */
    faultinject_set(FI_BLK_COMPLETE, 1, 3, thread_current());
    int rc = handle_close(t, h);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 3);
    CHECK(rc == -EIO);
    pagecache_get_stats(&p1);
    CHECK(p1.dropped_dirty == p0.dropped_dirty + 1);
    CHECK(p1.wb_errors == p0.wb_errors + 3);
    handle_table_destroy(t);
    kfree(t);

    /* The mount is not poisoned: a new file writes and syncs. */
    struct file *f;
    CHECK(vfs_open(NULL, "/mnt/wb/after", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, page, PAGE_SIZE) == (int64_t)PAGE_SIZE);
    CHECK(file_sync(f) == 0);
    file_put(f);
    kinfo("selftest: wb-error-lost: one page lost after three refusals, counted; the mount still writes");
    kfree(page);
    wb_teardown(bd);
    return true;
}

#else

#define WB_STUB(fn, name)                                                                     \
    bool fn(const char **reason)                                                              \
    {                                                                                         \
        (void)reason;                                                                         \
        kinfo("selftest: " name ": fault injection is compiled out of this build");          \
        return true;                                                                          \
    }
WB_STUB(selftest_wb_error_fsync, "wb-error-fsync")
WB_STUB(selftest_wb_error_once, "wb-error-once")
WB_STUB(selftest_wb_error_close, "wb-error-close")
WB_STUB(selftest_wb_error_lost, "wb-error-lost")

#endif

/* --- benchmarks: the object path per request size ------------------------
 *
 * Prints, asserts nothing. The audit's missing "read/write bandwidth at
 * 1 KiB chunking": a 1 MiB file read and written through file_read /
 * file_write with kernel buffers of 1 KiB, 4 KiB and 64 KiB -- the
 * object side of what the syscall bounce now allows per call. The
 * syscall side (entry, range check, copy) is measured from user mode
 * (init --selftest, USERBENCH lines). */

static void bench_pass(const char *what, struct file *f, uint8_t *buf, size_t req, bool write)
{
    file_seek(f, 0, COSMO_SEEK_SET);
    uint64_t t0 = clock_now_ns();
    unsigned calls = 0;
    size_t total = 0;
    while (total < 1024u * 1024u) {
        int64_t n = write ? file_write(f, buf, req) : file_read(f, buf, req);
        if (n <= 0)
            break;
        total += (size_t)n;
        calls++;
    }
    uint64_t dt = clock_since_ns(t0);
    kinfo("selftest: %s-bench: %s %zu KiB requests: %u calls, %llu us, %llu MiB/s", write ? "write" : "read", what,
          req / 1024, calls, (unsigned long long)(dt / 1000),
          (unsigned long long)(dt ? (uint64_t)total * 1000000000ull / dt / (1024 * 1024) : 0));
}

/*
 * MEM-9, V40: dirty pages bounded, the writer asleep holding nothing.
 * The cache limit is lowered for the test to DB_LIMIT pages, a dirty
 * bound of DB_LIMIT / 2. Part one: one writer of eight times the bound,
 * with write-back on, never has more than the bound and one write's
 * pages dirty (it writes its own file back at the bound). Part two: a
 * file left dirty past the bound with write-back off, which the writer of
 * another file cannot clean; that writer sleeps until a thread syncs the
 * first file 300 ms later, and is woken by it -- the sleep and the wake
 * lockdep's `pagecache-dirty` class sees.
 */
#define DB_LIMIT 256u
#define DB_WRITES 128u   /* of 64 KiB: 2048 pages */

struct db_syncer {
    struct file *f;
    int rc;
};

static void db_syncer_thread(void *arg)
{
    struct db_syncer *s = arg;
    thread_sleep_ms(300);
    s->rc = file_sync(s->f);
    thread_exit(0);
}

static struct mount *db_mount(void)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, "/mnt/db", &vn))
        return NULL;
    struct mount *m = vn->mnt;
    vnode_put(vn);
    return m;
}

static void db_noise(uint8_t *p, size_t len, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (uint8_t)x;
    }
}

bool selftest_pagecache_dirty_bound(const char **reason)
{
    struct blkdev *bd = ramblk_create(4096);
    uint8_t *buf = kmalloc(65536, 0);
    uint8_t *back = kmalloc(65536, 0);
    struct file *a = NULL, *b = NULL;
    uint64_t saved = pagecache_limit();
    bool mounted = false;
#define DB_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            goto out;                                                          \
        }                                                                      \
    } while (0)
    bool ok = false;
    DB_CHECK(bd != NULL && buf != NULL && back != NULL);
    DB_CHECK(cosmofs_format(bd) == 0);
    int mrc = vfs_mkdir(NULL, "/mnt/db", 0755);
    DB_CHECK(mrc == 0 || mrc == -EEXIST);
    DB_CHECK(vfs_mount("/mnt/db", "cosmofs", bd, 0) == 0);
    mounted = true;
    const uint64_t bound = DB_LIMIT / 2;
    struct pagecache_stats s0, s1, s2;
    pagecache_get_stats(&s0);
    pagecache_set_limit(DB_LIMIT);

    /* One writer, write-back on. */
    DB_CHECK(vfs_open(NULL, "/mnt/db/one", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &a) == 0);
    uint64_t peak = 0;
    for (unsigned i = 0; i < DB_WRITES; i++) {
        db_noise(buf, 65536, i);
        DB_CHECK(file_write(a, buf, 65536) == 65536);
        uint64_t d = pagecache_dirty_pages();
        if (d > peak)
            peak = d;
    }
    DB_CHECK(file_sync(a) == 0);
    pagecache_get_stats(&s1);
    kinfo("selftest: pagecache-dirty-bound: %u pages written, at most %llu dirty against a bound of %llu",
          DB_WRITES * 16, (unsigned long long)peak, (unsigned long long)bound);
    DB_CHECK(peak <= bound + 16 + 8);   /* the bound, one write's pages, and a few of the system's own */
    for (unsigned i = 0; i < DB_WRITES; i += 31) {
        db_noise(buf, 65536, i);
        DB_CHECK(file_pread(a, back, 65536, (uint64_t)i * 65536) == 65536);
        DB_CHECK(memcmp(back, buf, 65536) == 0);
    }
    file_put(a);
    a = NULL;

    /* Another file's dirty pages, which this writer cannot clean. */
    cosmofs_test_set_writeback(db_mount(), false);
    DB_CHECK(vfs_open(NULL, "/mnt/db/held", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &a) == 0);
    for (unsigned i = 0; i < (bound + 32) / 16; i++) {
        db_noise(buf, 65536, 1000 + i);
        DB_CHECK(file_pwrite_held(a, buf, 65536, (uint64_t)i * 65536) == 65536);   /* past the bound, unthrottled */
    }
    DB_CHECK(pagecache_dirty_pages() >= bound);
    DB_CHECK(vfs_open(NULL, "/mnt/db/waits", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &b) == 0);
    struct db_syncer sy = { a, -1 };
    struct thread *t = thread_create(db_syncer_thread, &sy, "db-syncer", SCHED_PRIO_DEFAULT);
    DB_CHECK(t != NULL);
    uint64_t t0 = clock_now_ns();
    int64_t wn = file_write(b, buf, 4096);
    uint64_t waited_ms = clock_since_ns(t0) / 1000000ull;
    thread_join(t);
    pagecache_get_stats(&s2);
    kinfo("selftest: pagecache-dirty-bound: a writer behind another file's dirty pages waited %llu ms, "
          "%llu sleep(s), woken by their sync", (unsigned long long)waited_ms,
          (unsigned long long)(s2.throttled - s1.throttled));
    DB_CHECK(wn == 4096 && sy.rc == 0);
    DB_CHECK(s2.throttled > s1.throttled);
    DB_CHECK(waited_ms >= 200 && waited_ms < 4000);   /* woken by the sync, not the 5 s give-up */
    ok = true;
out:
    pagecache_set_limit(saved);
    if (a)
        file_put(a);
    if (b)
        file_put(b);
    if (mounted) {
        cosmofs_test_set_writeback(db_mount(), true);
        if (vfs_umount("/mnt/db") != 0)
            (void)vfs_umount2("/mnt/db", VFS_UMOUNT_FORCE);
    }
    (void)vfs_rmdir(NULL, "/mnt/db");
    if (bd)
        ramblk_destroy(bd);
    kfree(buf);
    kfree(back);
    return ok;
#undef DB_CHECK
}

/* The benches' CHECK: frees the bench buffer before returning on a
 * failure, so a failing check leaks nothing (the analyzer's finding). */
#define BCHECK(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            kfree(buf);                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool selftest_read_bench(const char **reason)
{
    static const size_t reqs[] = { 1024, 4096, 65536 };
    uint8_t *buf = kmalloc(65536, 0);
    CHECK(buf != NULL);
    memset(buf, 0x5a, 65536);

    /* ramfs: the copy alone. */
    struct file *f;
    BCHECK(vfs_open(NULL, "/tmp/bench.bin", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    for (unsigned i = 0; i < 16; i++)
        BCHECK(file_write(f, buf, 65536) == 65536);
    for (unsigned i = 0; i < 3; i++)
        bench_pass("ramfs", f, buf, reqs[i], false);
    file_put(f);
    BCHECK(vfs_unlink(NULL, "/tmp/bench.bin") == 0);

    /* cosmofs on ramblk: cold (after a remount, through the device) and
     * warm (the page cache). */
    struct blkdev *bd;
    BCHECK(wb_setup(&bd) == 0);
    BCHECK(vfs_open(NULL, "/mnt/wb/bench", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    for (unsigned i = 0; i < 16; i++)
        BCHECK(file_write(f, buf, 65536) == 65536);
    BCHECK(file_sync(f) == 0);
    file_put(f);
    for (unsigned i = 0; i < 3; i++) {
        BCHECK(vfs_umount("/mnt/wb") == 0);
        BCHECK(vfs_mount("/mnt/wb", "cosmofs", bd, 0) == 0);
        BCHECK(vfs_open(NULL, "/mnt/wb/bench", COSMO_O_RDONLY, 0, &f) == 0);
        bench_pass("cosmofs cold", f, buf, reqs[i], false);
        bench_pass("cosmofs warm", f, buf, reqs[i], false);
        file_put(f);
    }
    wb_teardown(bd);
    kfree(buf);
    return true;
}

bool selftest_write_bench(const char **reason)
{
    static const size_t reqs[] = { 1024, 4096, 65536 };
    uint8_t *buf = kmalloc(65536, 0);
    CHECK(buf != NULL);
    memset(buf, 0xa5, 65536);
    for (unsigned i = 0; i < 3; i++) {
        struct file *f;
        BCHECK(vfs_open(NULL, "/tmp/wbench.bin", COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
        bench_pass("ramfs", f, buf, reqs[i], true);
        file_put(f);
        BCHECK(vfs_unlink(NULL, "/tmp/wbench.bin") == 0);
    }
    kfree(buf);
    return true;
}

/*
 * A frame a mapping references is not reclaimed (the file-regions unit):
 * pagecache_fault_page takes the mapping's reference under the cache
 * mutex, and reclaim, deciding under that mutex, leaves a frame whose
 * count is not one alone. Put the reference and it goes. cosmofs on a
 * RAM device, because ramfs pages are never reclaimed at all.
 */
bool selftest_pagecache_pinned(const char **reason)
{
    static uint8_t buf[PAGE_SIZE];
    struct file *f;
    struct pagecache_stats s0, s1;
    struct blkdev *bd = ramblk_create(1024);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, "/mnt/pin", 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount("/mnt/pin", "cosmofs", bd, 0) == 0);
    CHECK(vfs_open(NULL, "/mnt/pin/f", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    struct vnode *vn = f->vn;
    const unsigned NPAGES = 256;
    for (unsigned i = 0; i < NPAGES; i++) {
        memset(buf, (int)(i & 0xff), sizeof(buf));
        CHECK(file_write(f, buf, PAGE_SIZE) == PAGE_SIZE);
    }
    CHECK(file_sync(f) == 0);   /* every page clean: every page a candidate */

    /* Page 3 as a mapping would hold it. */
    struct page *held = NULL;
    pagecache_lock(vn);
    CHECK(pagecache_fault_page(vn, 3, false, &held) == 0);
    pagecache_unlock(vn);
    CHECK(held != NULL && (held->flags & PG_PAGECACHE) && held->refcount == 2);
    /* Past the end: the bound, not a zero page. */
    struct page *none = NULL;
    pagecache_lock(vn);
    CHECK(pagecache_fault_page(vn, NPAGES, false, &none) == -EFBIG);
    pagecache_unlock(vn);
    CHECK(none == NULL);

    /* Force reclaim: a limit below what is cached, then a read of every
     * page. Page 3 survives it, referenced. */
    pagecache_get_stats(&s0);
    uint64_t saved = pagecache_limit();
    pagecache_set_limit(s0.pages > NPAGES / 2 ? s0.pages - NPAGES / 2 : 1);
    for (unsigned i = 0; i < NPAGES; i++)
        CHECK(file_pread(f, buf, PAGE_SIZE, (uint64_t)i * PAGE_SIZE) == PAGE_SIZE);
    pagecache_get_stats(&s1);
    CHECK(s1.reclaimed > s0.reclaimed);
    CHECK(s1.pinned_skips > s0.pinned_skips);
    CHECK(held->refcount == 2 && (held->flags & PG_PAGECACHE));   /* still the cache's, still ours */
    CHECK(((uint8_t *)page_to_virt(held))[0] == 3);

    /* Put the reference: now it can go, and a sweep takes it. */
    pmm_page_put(held);
    pagecache_get_stats(&s0);
    pagecache_set_limit(1);
    for (unsigned i = 0; i < NPAGES; i++)
        CHECK(file_pread(f, buf, PAGE_SIZE, (uint64_t)i * PAGE_SIZE) == PAGE_SIZE);
    pagecache_get_stats(&s1);
    pagecache_set_limit(saved);
    CHECK(s1.reclaimed > s0.reclaimed);

    file_put(f);
    CHECK(vfs_unlink(NULL, "/mnt/pin/f") == 0);
    CHECK(vfs_umount("/mnt/pin") == 0);
    CHECK(vfs_rmdir(NULL, "/mnt/pin") == 0);
    ramblk_destroy(bd);
    kinfo("selftest: pagecache-pinned: a referenced frame survives reclaim and goes when the reference does");
    return true;
}


/*
 * renameat's two starts (docs/audit/next-subsystem-dirfd.md): a rename
 * whose two paths resolve from two different directories, each given as
 * a relative name. The result is checked by absolute path, because a
 * rename that used the first start for both names would also succeed --
 * into the wrong directory.
 */
bool selftest_vfs_rename2(const char **reason)
{
    struct cosmo_stat st;
    struct file *f;
    CHECK(vfs_mkdir(NULL, "/tmp/rn2a", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/rn2b", 0755) == 0);
    CHECK(vfs_open(NULL, "/tmp/rn2a/f", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    file_put(f);
    struct vnode *a, *b;
    CHECK(vfs_lookup(NULL, "/tmp/rn2a", &a) == 0);
    CHECK(vfs_lookup(NULL, "/tmp/rn2b", &b) == 0);
    int rc = vfs_rename2(a, "f", b, "g");
    int rc_back = rc == 0 ? vfs_stat(NULL, "/tmp/rn2b/g", &st) : rc;
    int rc_gone = vfs_stat(NULL, "/tmp/rn2a/f", &st);
    int rc_wrong = vfs_stat(NULL, "/tmp/rn2a/g", &st);
    vnode_put(a);
    vnode_put(b);
    (void)vfs_unlink(NULL, "/tmp/rn2b/g");
    (void)vfs_unlink(NULL, "/tmp/rn2a/g");
    (void)vfs_unlink(NULL, "/tmp/rn2a/f");
    CHECK(vfs_rmdir(NULL, "/tmp/rn2a") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/rn2b") == 0);
    CHECK(rc == 0);
    CHECK(rc_back == 0);          /* it is where the second start named it */
    CHECK(rc_gone == -ENOENT);
    CHECK(rc_wrong == -ENOENT);   /* and not beside the first */
    kinfo("selftest: vfs-rename2: a rename between two directories reached from two starts lands in the second");
    return true;
}

/*
 * The traversed name (P32, docs/audit/next-subsystem-cwd-name.md): a
 * table of walks, each name checked for its text and then walked again
 * from the root to the same vnode. The tree has an absolute link, a
 * relative one, a chain, a relative link into a mount, and one inside
 * the mount that leaves it through `..` -- the shapes where a lexical
 * name and the walk part company.
 */
bool selftest_vfs_lookup_named(const char **reason)
{
    CHECK(vfs_mkdir(NULL, "/tmp/ln", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/ln/a", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/ln/a/b", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/ln/m", 0755) == 0);
    CHECK(vfs_mount("/tmp/ln/m", "ramfs", NULL, 0) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/ln/m/in", 0755) == 0);
    CHECK(vfs_symlink(NULL, "/tmp/ln/abs", "/tmp/ln/a/b") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/ln/rel", "a/b") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/ln/chain", "abs") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/ln/a/up", "../m/in") == 0);   /* relative, into the mount */
    CHECK(vfs_symlink(NULL, "/tmp/ln/m/out", "../a") == 0);     /* relative, out of it through its root */
    CHECK(vfs_symlink(NULL, "/tmp/ln/a/b/toroot", "/tmp") == 0);   /* absolute, to a short name */
    struct vnode *ln;
    CHECK(vfs_lookup(NULL, "/tmp/ln", &ln) == 0);

    static const struct {
        bool from_ln;   /* relative to /tmp/ln, or from the root */
        const char *path;
        const char *want;
    } cases[] = {
        { false, "/tmp/ln/a/b", "/tmp/ln/a/b" },
        { true, "a/./b", "/tmp/ln/a/b" },
        { true, "a/b/..", "/tmp/ln/a" },
        { false, "/..", "/" },
        { true, "../../..", "/" },
        { true, "abs", "/tmp/ln/a/b" },       /* an absolute link, last */
        { true, "abs/..", "/tmp/ln/a" },      /* ... mid-path: the target's parent, not /tmp/ln */
        { true, "rel", "/tmp/ln/a/b" },       /* a relative link, last: its name must not stay in */
        { true, "rel/..", "/tmp/ln/a" },      /* ... mid-path */
        { true, "chain", "/tmp/ln/a/b" },     /* two links */
        { true, "a/up", "/tmp/ln/m/in" },     /* a relative link into a mount */
        { true, "m/in/..", "/tmp/ln/m" },
        { true, "m/..", "/tmp/ln" },          /* `..` from a mount root leaves through the mountpoint */
        { true, "m/out", "/tmp/ln/a" },       /* a link inside the mount, out through its root */
        { true, "a/b/", "/tmp/ln/a/b" },
    };
    char name[VFS_PATH_MAX];
    unsigned bad = ~0u, mismatch = ~0u;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct vnode *vn = NULL, *again = NULL;
        int rc = vfs_lookup_named(cases[i].from_ln ? ln : NULL, "/tmp/ln", cases[i].path, &vn, name, sizeof(name));
        if (rc != 0 || strcmp(name, cases[i].want) != 0) {
            kinfo("selftest: vfs-lookup-named: case %u (%s): rc %d, name '%s', want '%s'", i, cases[i].path, rc,
                  rc == 0 ? name : "", cases[i].want);
            if (bad == ~0u)
                bad = i;
        } else if (vfs_lookup(NULL, name, &again) != 0 || again != vn) {
            if (mismatch == ~0u)
                mismatch = i;   /* the name does not reach what the walk reached */
        }
        if (again)
            vnode_put(again);
        if (vn)
            vnode_put(vn);
    }
    /* A relative walk from a start with no name; a name that does not fit. */
    struct vnode *vn = NULL;
    int rc_noname = vfs_lookup_named(ln, NULL, "a", &vn, name, sizeof(name));
    if (vn)
        vnode_put(vn);
    vn = NULL;
    int rc_long = vfs_lookup_named(NULL, NULL, "/tmp/ln/a/b", &vn, name, 8);
    if (vn)
        vnode_put(vn);
    /* An overflow before an absolute link is no part of the name after
     * it: "/tmp/ln/a/b" does not fit ten bytes, the link restarts at `/`,
     * and "/tmp" does (found in review: the flag outlived the reset). */
    vn = NULL;
    char small[10];
    int rc_reset = vfs_lookup_named(ln, "/tmp/ln", "a/b/toroot", &vn, small, sizeof(small));
    bool reset_named = rc_reset == 0 && strcmp(small, "/tmp") == 0;
    if (vn)
        vnode_put(vn);
    vnode_put(ln);

    (void)vfs_unlink(NULL, "/tmp/ln/m/out");
    (void)vfs_rmdir(NULL, "/tmp/ln/m/in");
    CHECK(vfs_umount("/tmp/ln/m") == 0);
    (void)vfs_unlink(NULL, "/tmp/ln/a/up");
    (void)vfs_unlink(NULL, "/tmp/ln/a/b/toroot");
    (void)vfs_unlink(NULL, "/tmp/ln/chain");
    (void)vfs_unlink(NULL, "/tmp/ln/rel");
    (void)vfs_unlink(NULL, "/tmp/ln/abs");
    CHECK(vfs_rmdir(NULL, "/tmp/ln/m") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/ln/a/b") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/ln/a") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/ln") == 0);

    CHECK(bad == ~0u);        /* every name is the path the walk took */
    CHECK(mismatch == ~0u);   /* and walked again, it reaches the same vnode */
    CHECK(rc_noname == -ENOENT);
    CHECK(rc_long == -ENAMETOOLONG);
    CHECK(reset_named);
    kinfo("selftest: vfs-lookup-named: %u walks named by the path they took, through links and a mount",
          (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return true;
}

/*
 * One unmount at a time (docs/audit/next-subsystem-mount-rel.md): the
 * guard in vfs_umount_at that refuses a second unmount while the first
 * is in its drain. It was unreachable until unmount honoured a start: by
 * absolute path a second unmount cannot even resolve the target
 * (follow_mount refuses a mount that is unmounting), and only a path
 * from inside the mount -- "." from its root -- reaches it without
 * crossing the mountpoint.
 *
 * The order is made, not waited for: a held maintenance pass keeps the
 * first unmount in its drain, and the second starts only once
 * `unmounting` is seen set. The second runs in a thread with a bound,
 * because without the guard it would block in the same drain.
 */
struct umount_once {
    struct vnode *start;
    const char *path;
    int rc;
    unsigned done;
};

static void umount_once_main(void *arg)
{
    struct umount_once *u = arg;
    u->rc = vfs_umount_at(u->start, u->path, 0);
    /* A start is the thread's own reference, dropped only when its walk
     * is over: a thread that starts late must not walk from a vnode the
     * test has already let go of (found in review). */
    if (u->start)
        vnode_put(u->start);
    __atomic_store_n(&u->done, 1u, __ATOMIC_RELEASE);
}

static bool umount_once_wait(unsigned *flag, uint64_t ms)
{
    uint64_t deadline = clock_now_ns() + ms * 1000000ULL;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() > deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

bool selftest_vfs_umount_once(const char **reason)
{
    unsigned mounts0 = vfs_mount_count();
    CHECK(vfs_mkdir(NULL, "/tmp/um", 0755) == 0);
    CHECK(vfs_mount("/tmp/um", "ramfs", NULL, 0) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/um/d", 0755) == 0);
    struct vnode *root;
    CHECK(vfs_lookup(NULL, "/tmp/um", &root) == 0);
    struct mount *mnt = root->mnt, *held = NULL;
    CHECK(mnt->root == root);
    CHECK(vfs_mount_acquire(mnt->id, &held) == 0);   /* the first unmount will wait for this */

    /* The first unmount, by absolute path: it sets `unmounting` and drains. */
    struct umount_once first = { .start = NULL, .path = "/tmp/um" };
    struct thread *ta = thread_create(umount_once_main, &first, "umount-first", SCHED_PRIO_DEFAULT);
    CHECK(ta != NULL);
    uint64_t deadline = clock_now_ns() + 2000000000ULL;
    bool seen = false;
    while (!seen && clock_now_ns() < deadline) {
        mutex_lock(&mnt->mountpoint->lock);
        seen = mnt->unmounting;
        mutex_unlock(&mnt->mountpoint->lock);
        if (!seen)
            thread_sleep_ms(1);
    }

    /* The second, from inside: "d/.." from the mount's root, a path that
     * names that root only from there -- resolved from anywhere else it
     * is -ENOENT (there is no `d`), not the -EBUSY the root filesystem
     * gives "." (found by a mutation that ignored the start and passed). */
    struct umount_once second = { .start = root, .path = "d/.." };
    struct thread *tb = NULL;
    if (seen) {
        vnode_get(root);   /* the thread's own, put when its unmount returns */
        tb = thread_create(umount_once_main, &second, "umount-second", SCHED_PRIO_DEFAULT);
        if (tb == NULL)
            vnode_put(root);
    }
    bool second_answered = tb != NULL && umount_once_wait(&second.done, 2000);
    bool first_waiting = !__atomic_load_n(&first.done, __ATOMIC_ACQUIRE);   /* still in its drain */

    /* Let everything go on every path: the root reference first, so the
     * first unmount's reference scan finds none, then the pass. A second
     * unmount that has not answered still holds its own reference, so
     * the first cannot free the root under it: it finds that reference
     * and refuses, and the checks below say which step failed. */
    vnode_put(root);
    vfs_mount_release(held);
    thread_join(ta);
    if (tb)
        thread_join(tb);
    (void)vfs_rmdir(NULL, "/tmp/um/d");   /* only if the first failed: its mount is gone otherwise */
    (void)vfs_umount("/tmp/um");          /* likewise */
    (void)vfs_rmdir(NULL, "/tmp/um");

    CHECK(seen);                        /* the first unmount reached its drain */
    CHECK(second_answered);             /* the second did not wait in it */
    CHECK(first_waiting);               /* ... and answered while the pass was still held */
    CHECK(second.rc == -EBUSY);         /* the guard's answer */
    CHECK(first.rc == 0);               /* the first then finished */
    CHECK(vfs_mount_count() == mounts0);
    kinfo("selftest: vfs-umount-once: a second unmount from inside the mount was refused while the first drained");
    return true;
}

/*
 * kobject_io_type.read_filled (roadmap 1.1 P3, #413's review): a large
 * read of a regular file is one read across its pieces -- the open file's
 * position lock is held while each piece is handed over, so no seek or
 * second reader on the same open file lands between two pieces -- and the
 * position moves by what was accepted, so a fault after a piece returns
 * that piece and the next read starts after it. /dev/zero fills too
 * (without a position); /dev/null does not, and is read one call at a
 * time.
 */
struct filled_ctx {
    struct file *f;
    uint8_t *dst;
    size_t done;
    unsigned pieces, fail_at;
    bool unlocked;   /* a piece was handed over without the position lock */
};

static int filled_put(void *ctx, const void *piece, size_t n)
{
    struct filled_ctx *c = ctx;
    if (c->f->vn->type == VNODE_REG && !mutex_is_locked(&c->f->lock))
        c->unlocked = true;
    if (++c->pieces == c->fail_at)
        return -EFAULT;
    memcpy(c->dst + c->done, piece, n);
    c->done += n;
    return 0;
}

bool selftest_file_read_filled(const char **reason)
{
    enum { SZ = 5 * PAGE_SIZE + 100, CAP = 2 * PAGE_SIZE };
    static uint8_t src[SZ], dst[SZ], bounce[CAP];
    for (size_t i = 0; i < SZ; i++)
        src[i] = (uint8_t)(i * 7 + i / PAGE_SIZE);
    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/filled-test", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0600, &f) == 0);
    CHECK(file_write(f, src, SZ) == SZ);
    const struct kobject_io_type *io = kobject_io_of(&f->obj);
    CHECK(io != NULL && io->read_filled != NULL);

    /* From offset 100 to the end, in pieces of two pages: 8192, 8192, then
     * 4096 at the end of file; contiguous, each under the lock. */
    CHECK(file_seek(f, 100, COSMO_SEEK_SET) == 100);
    struct filled_ctx c = { .f = f, .dst = dst };
    CHECK(io->read_filled(&f->obj, bounce, CAP, SZ, filled_put, &c) == SZ - 100);
    CHECK(c.pieces == 3 && !c.unlocked && c.done == SZ - 100);
    CHECK(memcmp(dst, src + 100, SZ - 100) == 0);
    CHECK(file_seek(f, 0, COSMO_SEEK_CUR) == SZ);
    CHECK(!mutex_is_locked(&f->lock));

    /* The second piece faults: the first piece's bytes, the position after them. */
    CHECK(file_seek(f, 0, COSMO_SEEK_SET) == 0);
    c = (struct filled_ctx){ .f = f, .dst = dst, .fail_at = 2 };
    CHECK(io->read_filled(&f->obj, bounce, CAP, SZ, filled_put, &c) == CAP);
    CHECK(file_seek(f, 0, COSMO_SEEK_CUR) == CAP);
    /* The first piece faults: -EFAULT, the position where it was. */
    c = (struct filled_ctx){ .f = f, .dst = dst, .fail_at = 1 };
    CHECK(io->read_filled(&f->obj, bounce, CAP, SZ, filled_put, &c) == -EFAULT);
    CHECK(file_seek(f, 0, COSMO_SEEK_CUR) == CAP);
    file_put(f);
    CHECK(vfs_unlink(NULL, "/tmp/filled-test") == 0);

    /* /dev/zero: three whole pieces of zeros; /dev/null: one call at a time. */
    CHECK(vfs_open(NULL, "/dev/zero", COSMO_O_RDONLY, 0, &f) == 0);
    memset(dst, 0xff, sizeof(dst));
    c = (struct filled_ctx){ .f = f, .dst = dst };
    int64_t z = io->read_filled(&f->obj, bounce, CAP, 3 * CAP, filled_put, &c);
    bool zero = true;
    for (size_t i = 0; i < 3 * CAP; i++)
        zero = zero && dst[i] == 0;
    file_put(f);
    CHECK(z == 3 * CAP && c.pieces == 3 && zero);
    CHECK(vfs_open(NULL, "/dev/null", COSMO_O_RDONLY, 0, &f) == 0);
    c = (struct filled_ctx){ .f = f, .dst = dst };
    z = io->read_filled(&f->obj, bounce, CAP, 3 * CAP, filled_put, &c);
    file_put(f);
    CHECK(z == -EOPNOTSUPP && c.pieces == 0);
    return true;
}

/*
 * MEM-9, V40, the failing test: a writer alone with its dirty pages.
 * Write-back is off, so nothing but the writer can clean a page, and the
 * cache limit is lowered to DW_LIMIT pages; the bound V40 states is half
 * of it. One writer of eight times the bound may never have more than
 * the bound and one write's pages dirty on its mount. Without a bound
 * the mount's count is every page written (1024 here); test-fill's
 * writer stage on 75e1e15f, a 256 MiB guest, was that count meeting
 * memory (`sh: cat: Out of memory`, 219 MiB of the 512 written), which a
 * fast enough write-back thread can hide.
 */
#define DW_LIMIT 256u
#define DW_WRITES 64u   /* of 64 KiB: 1024 pages */

static void dw_noise(uint8_t *p, size_t len, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (uint8_t)x;
    }
}

bool selftest_pagecache_dirty_writer(const char **reason)
{
    struct blkdev *bd = ramblk_create(4096);
    uint8_t *buf = kmalloc(65536, 0);
    uint8_t *back = kmalloc(65536, 0);
    struct file *f = NULL;
    struct mount *m = NULL;
    uint64_t saved = pagecache_limit();
    bool ok = false;
#define DW_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            goto out;                                                          \
        }                                                                      \
    } while (0)
    DW_CHECK(bd != NULL && buf != NULL && back != NULL);
    DW_CHECK(cosmofs_format(bd) == 0);
    int mrc = vfs_mkdir(NULL, "/mnt/dw", 0755);
    DW_CHECK(mrc == 0 || mrc == -EEXIST);
    DW_CHECK(vfs_mount("/mnt/dw", "cosmofs", bd, 0) == 0);
    struct vnode *root;
    DW_CHECK(vfs_lookup(NULL, "/mnt/dw", &root) == 0);
    m = root->mnt;
    vnode_put(root);
    cosmofs_test_set_writeback(m, false);
    pagecache_set_limit(DW_LIMIT);
    const uint64_t bound = DW_LIMIT / 2;

    DW_CHECK(vfs_open(NULL, "/mnt/dw/w", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    uint64_t peak = 0;
    for (unsigned i = 0; i < DW_WRITES; i++) {
        dw_noise(buf, 65536, i);
        DW_CHECK(file_write(f, buf, 65536) == 65536);
        uint64_t d = __atomic_load_n(&m->cache_dirty, __ATOMIC_RELAXED);
        if (d > peak)
            peak = d;
    }
    kinfo("selftest: pagecache-dirty-writer: %u pages written with write-back off, at most %llu dirty on the "
          "mount against a bound of %llu",
          DW_WRITES * 16, (unsigned long long)peak, (unsigned long long)bound);
    DW_CHECK(peak <= bound + 16);   /* the bound and one write's pages */
    DW_CHECK(file_sync(f) == 0);
    for (unsigned i = 0; i < DW_WRITES; i += 21) {
        dw_noise(buf, 65536, i);
        DW_CHECK(file_pread(f, back, 65536, (uint64_t)i * 65536) == 65536);
        DW_CHECK(memcmp(back, buf, 65536) == 0);
    }
    ok = true;
out:
    pagecache_set_limit(saved);
    if (f)
        file_put(f);
    if (m) {
        cosmofs_test_set_writeback(m, true);
        if (vfs_umount("/mnt/dw") != 0)
            (void)vfs_umount2("/mnt/dw", VFS_UMOUNT_FORCE);
    }
    (void)vfs_rmdir(NULL, "/mnt/dw");
    if (bd)
        ramblk_destroy(bd);
    kfree(buf);
    kfree(back);
    return ok;
#undef DW_CHECK
}

/*
 * MEM-9, V40, the review's case: one COSMO_AIO_PWRITE of twice the
 * dirty bound (the cache limit lowered to DW_LIMIT, so 128 pages, and
 * write-back off, as above), through the ring's own body
 * (aio_test_pwrite). The request has no size cap, and its loop holds the
 * open file's lock, under which the throttle may not sleep; a writer
 * throttled once before the loop leaves every page of the request dirty
 * (256 here). Bounded, the mount never holds more than the bound and one
 * piece's page.
 */
#define AW_BYTES (1u << 20)   /* 256 pages: twice the bound */

bool selftest_aio_pwrite_dirty(const char **reason)
{
#if !CONFIG_SELFTEST
    (void)reason;
    return true;
#else
    struct blkdev *bd = ramblk_create(4096);
    uint8_t *buf = kmalloc(AW_BYTES, 0);
    uint8_t *back = kmalloc(65536, 0);
    struct file *f = NULL;
    struct mount *m = NULL;
    uint64_t saved = pagecache_limit();
    bool ok = false;
#define AW_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            goto out;                                                          \
        }                                                                      \
    } while (0)
    AW_CHECK(bd != NULL && buf != NULL && back != NULL);
    AW_CHECK(cosmofs_format(bd) == 0);
    int mrc = vfs_mkdir(NULL, "/mnt/aw", 0755);
    AW_CHECK(mrc == 0 || mrc == -EEXIST);
    AW_CHECK(vfs_mount("/mnt/aw", "cosmofs", bd, 0) == 0);
    struct vnode *root;
    AW_CHECK(vfs_lookup(NULL, "/mnt/aw", &root) == 0);
    m = root->mnt;
    vnode_put(root);
    cosmofs_test_set_writeback(m, false);
    pagecache_set_limit(DW_LIMIT);
    const uint64_t bound = DW_LIMIT / 2;

    AW_CHECK(vfs_open(NULL, "/mnt/aw/w", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    for (unsigned i = 0; i < AW_BYTES / 65536; i++)
        dw_noise(buf + (size_t)i * 65536, 65536, i);
    AW_CHECK(aio_test_pwrite(f, buf, AW_BYTES, 0) == (int64_t)AW_BYTES);
    /* Write-back is off: what the request left dirty is still dirty. */
    uint64_t left = __atomic_load_n(&m->cache_dirty, __ATOMIC_RELAXED);
    kinfo("selftest: aio-pwrite-dirty: one PWRITE of %u pages with write-back off left %llu dirty on the mount "
          "against a bound of %llu",
          AW_BYTES / 4096, (unsigned long long)left, (unsigned long long)bound);
    AW_CHECK(left <= bound + 16);   /* the bound, as pagecache-dirty-writer's */
    AW_CHECK(file_sync(f) == 0);
    for (unsigned i = 0; i < AW_BYTES / 65536; i += 5) {
        AW_CHECK(file_pread(f, back, 65536, (uint64_t)i * 65536) == 65536);
        AW_CHECK(memcmp(back, buf + (size_t)i * 65536, 65536) == 0);
    }
    ok = true;
out:
    pagecache_set_limit(saved);
    if (f)
        file_put(f);
    if (m) {
        cosmofs_test_set_writeback(m, true);
        if (vfs_umount("/mnt/aw") != 0)
            (void)vfs_umount2("/mnt/aw", VFS_UMOUNT_FORCE);
    }
    (void)vfs_rmdir(NULL, "/mnt/aw");
    if (bd)
        ramblk_destroy(bd);
    kfree(buf);
    kfree(back);
    return ok;
#undef AW_CHECK
#endif
}
