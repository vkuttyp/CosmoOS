/*
 * procfs_floor.c - The C under procfs.cee (roadmap 2.0 SA7): the vnode
 * operation tables and the file system type, C data that names the
 * operations by address; what a macro or a build configuration
 * decides -- /proc/version's strings and compiler, and the debug build's
 * count of live texts; a thread from its link in its process's list
 * (container_of); the name of an object's kind; a process's dumpable
 * flag, read atomically; /proc/net's file names, a C array; and the
 * values procfs.cee writes as its own constants, asserted. /proc itself
 * is CeeSafe (procfs.cee, generated into procfs.c).
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/net/procnet.h>
#include <kernel/pagecache.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/version.h>
#include <kernel/vfs.h>

_Static_assert(ENOENT == 2 && ESRCH == 3 && ENOMEM == 12 && EACCES == 13 && EINVAL == 22,
               "procfs.cee's errno.h values");
_Static_assert(VNODE_REG == 1 && VNODE_DIR == 2 && VNODE_LNK == 6 && VNODE_PINNED == 1 && PAGE_SIZE == 4096 &&
                   VFS_PATH_MAX == 1024 && offsetof(struct vnode, type) == 56 && sizeof(((struct vnode *)0)->type) == 4,
               "procfs.cee's vfs.h values");
_Static_assert(HANDLE_TABLE_SIZE == 64 && CONFIG_MAX_CPUS == 64 && PROCESS_EXITED == 2 && THREAD_READY == 0 &&
                   THREAD_RUNNING == 1 && PRIV_PROC == 8 && COSMO_RLIM_INFINITY == ~0ull && COSMO_RLIMIT_COUNT == 6 &&
                   COSMO_PRIV_COUNT == 6 && PROCNET_COUNT == 8 && PROCESS_NAME_MAX == 32 &&
                   PROCESS_CMDLINE_MAX == 256 && sizeof(pid_t) == 4,
               "procfs.cee's process.h, thread.h, cred.h, handle.h and uapi values");
_Static_assert(sizeof(((struct process *)0)->cwd_path_locked) == 1024 && sizeof(((struct process *)0)->state) == 4 &&
                   sizeof(((struct thread *)0)->state) == 4 && sizeof(unsigned long) == 8,
               "procfs.cee's process and thread field sizes");
_Static_assert(sizeof(struct pmm_stats) <= 16 * 8 && offsetof(struct pmm_stats, total_pages) == 0 &&
                   offsetof(struct pmm_stats, free_pages) == 8 && sizeof(struct pagecache_stats) <= 16 * 8 &&
                   offsetof(struct pagecache_stats, pages) == 24 && sizeof(struct cputime_pair) == 16 &&
                   offsetof(struct cputime_pair, user_ns) == 0 && offsetof(struct cputime_pair, system_ns) == 8,
               "procfs.cee's slots for pmm_stats, pagecache_stats and cputime_pair");

/* The operations, CeeSafe (procfs.cee). */
int procfs_readpage(struct vnode *vn, uint64_t index, void *buf);
void procfs_evict(struct vnode *vn);
int procfs_environ_open(struct vnode *vn, struct file *f);
int procfs_link_readlink(struct vnode *vn, char *buf, size_t len);
int procfs_fd_readlink(struct vnode *vn, char *buf, size_t len);
int procfs_self_readlink(struct vnode *vn, char *buf, size_t len);
int procfs_root_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out);
int procfs_pid_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out);
int procfs_fd_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out);
int procfs_net_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out);
int procfs_root_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg);
int procfs_pid_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg);
int procfs_fd_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg);
int procfs_net_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg);
int procfs_mount(void *fs, void *bdev, uint32_t flags, struct mount *mnt);
int procfs_unmount(struct mount *mnt);

/* What procfs.cee calls here. */
const struct vnode_ops *procfs_floor_ops(unsigned which);
const char *procfs_floor_version(unsigned which);
void procfs_floor_count(int delta);
struct thread *procfs_floor_thread(struct list_node *link);
const char *procfs_floor_kind_name(struct kobject *obj);
bool procfs_floor_dumpable(struct process *p);
const char *procfs_floor_net_name(unsigned i);
void procfs_floor_loadavg(uint64_t *out);

/* The tables, in procfs.cee's OPS_* order. */
static const struct vnode_ops g_ops[] = {
    { .lookup = procfs_root_lookup, .readdir = procfs_root_readdir },                                /* /proc */
    { .lookup = procfs_pid_lookup, .readdir = procfs_pid_readdir },                                  /* /proc/<pid> */
    { .lookup = procfs_fd_lookup, .readdir = procfs_fd_readdir },                                    /* /proc/<pid>/fd */
    { .readlink = procfs_fd_readlink },                                                       /* an fd's link */
    { .readpage = procfs_readpage, .evict = procfs_evict },                                   /* a rendered file */
    { .readpage = procfs_readpage, .evict = procfs_evict, .open = procfs_environ_open },      /* environ */
    { .readlink = procfs_link_readlink },                                                     /* cwd, exe, root */
    { .lookup = procfs_net_lookup, .readdir = procfs_net_readdir },                                  /* /proc/net */
    { .readlink = procfs_self_readlink },                                                     /* self */
};

const struct vnode_ops *procfs_floor_ops(unsigned which)
{
    return which < sizeof(g_ops) / sizeof(g_ops[0]) ? &g_ops[which] : NULL;
}

/* /proc/version's strings: the release, the compiler, uname's name, version and build. */
#ifndef PROCFS_COMPILER
#define PROCFS_COMPILER "clang " __clang_version__   /* the host test, which gcc may build, names its own */
#endif
const char *procfs_floor_version(unsigned which)
{
    static const char compiler[] = PROCFS_COMPILER;
    static const char *const parts[] = { KERNEL_LINUX_RELEASE, compiler, KERNEL_NAME, KERNEL_VERSION, COSMO_BUILD_ID };
    return which < 5 ? parts[which] : "";
}

#if CONFIG_DEBUG
/* Rendered texts alive now: the proc-exit-read self-test's leak check. */
static unsigned g_proc_texts;
unsigned procfs_texts_live(void)
{
    return __atomic_load_n(&g_proc_texts, __ATOMIC_RELAXED);
}
void procfs_floor_count(int delta)
{
    __atomic_add_fetch(&g_proc_texts, (unsigned)delta, __ATOMIC_RELAXED);
}
#else
void procfs_floor_count(int delta)
{
    (void)delta;
}
#endif

struct thread *procfs_floor_thread(struct list_node *link)
{
    return container_of(link, struct thread, proc_link);
}

const char *procfs_floor_kind_name(struct kobject *obj)
{
    return obj->type && obj->type->name ? obj->type->name : "object";
}

bool procfs_floor_dumpable(struct process *p)
{
    return __atomic_load_n(&p->dumpable, __ATOMIC_RELAXED) != 0;
}

const char *procfs_floor_net_name(unsigned i)
{
    return procnet_names[i];
}

/* The 1, 5 and 15 minute averages with 11 fractional bits, as words. */
void procfs_floor_loadavg(uint64_t *out)
{
    unsigned long l[3];
    sched_loadavg(l, 11);
    for (unsigned i = 0; i < 3; i++)
        out[i] = l[i];
}

static int mount_fs(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    return procfs_mount(fs, bdev, flags, mnt);
}

struct fs_type procfs_fs_type = {
    .magic = 0x9fa0u,   /* Linux's PROC_SUPER_MAGIC */
    .name = "procfs",
    .mount = mount_fs,
    .unmount = procfs_unmount,
    .sync = NULL,
};
