/*
 * vfstest_floor.c - The C under the VFS's CeeSafe self-tests (roadmap
 * 2.0 SA7): the values they write as their own constants, asserted, and
 * the stat record's size, which they keep on their stacks in u64s; the
 * test devices' operation tables and the completions a blocked read
 * waits on, C data; and the bounce's fallback under fault injection,
 * which a build without it compiles out. The
 * self-tests themselves are CeeSafe (vfstest_*.cee, generated into
 * vfstest_*.c).
 */

#include <kernel/completion.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/pagecache.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/types.h>
#include <kernel/utsns.h>
#include <kernel/vfs.h>
#include <kernel/page.h>

#include <uapi/cosmo/fsctl.h>

_Static_assert(ENOENT == 2 && EBADF == 9 && EBUSY == 16 && EEXIST == 17 && EXDEV == 18 && ENODEV == 19 &&
                   ENOTDIR == 20 && EISDIR == 21 && EINVAL == 22 && ENAMETOOLONG == 36 && ENOTEMPTY == 39 &&
                   ELOOP == 40,
               "vfstest_base.cee's errno.h values");
_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_RDWR == 2 && COSMO_O_ACCMODE == 3 &&
                   COSMO_O_CREAT == 0x40 && COSMO_O_EXCL == 0x80 && COSMO_O_TRUNC == 0x200 &&
                   COSMO_O_APPEND == 0x400 && COSMO_O_DIRECTORY == 0x10000 && COSMO_O_NOFOLLOW == 0x20000 &&
                   COSMO_SEEK_SET == 0 && COSMO_SEEK_CUR == 1 && COSMO_DT_REG == 1 && COSMO_DT_DIR == 2 &&
                   COSMO_DT_LNK == 6 && COSMO_HOST_NAME_MAX == 64,
               "vfstest_base.cee's syscall.h values");
_Static_assert(VNODE_DIR == 2 && VNODE_DEAD == 2 && VFS_PATH_MAX == 1024 && VFS_MAX_SYMLINKS == 8 &&
                   HANDLE_RIGHT_OWNER == 0x1C && SCHED_PRIO_DEFAULT == 32 && PAGE_SIZE == 4096 && KMEM_ZERO == 1,
               "vfstest_base.cee's vfs.h, handle.h, thread.h, types.h and kmalloc.h values");
_Static_assert(ENOSPC == 28 && VNODE_REG == 1 && MOUNT_CACHE_IS_STORE == 2 && sizeof(struct pagecache_stats) == 11 * 8,
               "vfstest_mount.cee's errno.h, vfs.h and pagecache.h values");
_Static_assert(sizeof(struct cosmo_stat) == 7 * 8, "the self-tests' stat records: seven u64s");

_Static_assert(ENOMEM == 12 && ERANGE == 34 && EMSGSIZE == 90 && COSMO_FSCTL_VERSION == 3 && COSMO_FSCTL_LIST == 1 &&
                   IO_CHUNK == 1024 && IO_BOUNCE_MAX == 65536 && sizeof(struct cosmo_fsctl) == 11 * 8 &&
                   sizeof(struct io_bounce) == 3 * 8,
               "vfstest_dev.cee's errno.h, fsctl.h and syscall.h values");

/* --- vfstest_dev.cee's devices, completions and the bounce's fallback ---- */

int vfstest_chropen_open(struct vnode *vn, struct file *f);
void vfstest_chropen_release(struct vnode *vn, struct file *f);
int64_t vfstest_chropen_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t vfstest_chrblock_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t vfstest_chrblock_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);

const struct chrdev_ops *vfstest_floor_chropen_ops(void);
const struct chrdev_ops *vfstest_floor_chrblock_ops(void);
struct completion *vfstest_floor_completion(unsigned which);
bool vfstest_floor_bounce_fallback(const char **reason);

static const struct chrdev_ops chropen_ops = {
    .open = vfstest_chropen_open, .release = vfstest_chropen_release, .read_file = vfstest_chropen_read_file,
};

static const struct chrdev_ops chrblock_ops = {
    .read_file = vfstest_chrblock_read_file, .write_file = vfstest_chrblock_write_file,
};

const struct chrdev_ops *vfstest_floor_chropen_ops(void)
{
    return &chropen_ops;
}

const struct chrdev_ops *vfstest_floor_chrblock_ops(void)
{
    return &chrblock_ops;
}

/* The blocked read's release (0) and its reader's and writer's ends (1, 2). */
static struct completion g_completion[3];

struct completion *vfstest_floor_completion(unsigned which)
{
    return &g_completion[which];
}

/* A heap that refuses: the stack chunk, never an error. Fault injection
 * is what makes the heap refuse, and a build without it has none. */
bool vfstest_floor_bounce_fallback(const char **reason)
{
#if CONFIG_FAULTINJECT
    char stack[IO_CHUNK];
    struct io_bounce b;
    uint64_t fb0 = syscall_bounce_fallback_count();
    faultinject_set(FI_KMALLOC, 1, 1, thread_current());
    syscall_bounce_get(&b, stack, 65536);
    faultinject_clear(FI_KMALLOC);
    struct fi_stats fst;
    faultinject_stats(FI_KMALLOC, &fst);
    bool ok = fst.hits == 1 && !b.heap && b.cap == IO_CHUNK && b.buf == stack &&
              syscall_bounce_fallback_count() == fb0 + 1;
    syscall_bounce_put(&b);
    if (!ok)
        *reason = "check failed: the bounce fell back to the stack chunk once, under FI_KMALLOC";
    return ok;
#else
    (void)reason;
    kinfo("selftest: read-bounce: the fallback needs fault injection, compiled out of this build");
    return true;
#endif
}

_Static_assert(EIO == 5 && VFS_UMOUNT_FORCE == 1, "vfstest_wb.cee's errno.h and vfs.h values");

/* --- vfstest_wb.cee's fault injection, and its stand-ins without it ------ */

void vfstest_floor_inject_blk(unsigned count);
uint64_t vfstest_floor_inject_end(void);

#if CONFIG_FAULTINJECT

/* FI_BLK_COMPLETE refuses this thread's next `count` write-backs. */
void vfstest_floor_inject_blk(unsigned count)
{
    faultinject_set(FI_BLK_COMPLETE, 1, count, thread_current());
}

/* The injection's end: how many it refused. */
uint64_t vfstest_floor_inject_end(void)
{
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    return fst.hits;
}

#else

void vfstest_floor_inject_blk(unsigned count)
{
    (void)count;
}

uint64_t vfstest_floor_inject_end(void)
{
    return 0;
}

#define WB_STUB(fn, name)                                                                     \
    bool fn(const char **reason);                                                             \
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

_Static_assert(EFAULT == 14 && EFBIG == 27 && EOPNOTSUPP == 95 && PG_PAGECACHE == 128 && COSMO_SEEK_CUR == 1,
               "vfstest_wb.cee's and vfstest_fill.cee's errno.h, page.h and syscall.h values");

#if !CONFIG_SELFTEST
/* aio-pwrite-dirty needs the ring's test body, which only a self-test
 * build has (vfstest_fill.cee). */
bool selftest_aio_pwrite_dirty(const char **reason);
bool selftest_aio_pwrite_dirty(const char **reason)
{
    (void)reason;
    return true;
}
#endif
