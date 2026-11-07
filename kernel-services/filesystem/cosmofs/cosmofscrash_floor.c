/*
 * cosmofscrash_floor.c - The C under cosmofscrash.cee (roadmap 2.0 SA7):
 * a directory's entries listed through its vnode's readdir operation --
 * a function its operations table names, which CeeSafe cannot call
 * through a field, with a callback whose type takes an enum -- and the
 * self-test's stand-in in a build without CONFIG_DEBUG. The crash
 * harness is CeeSafe (cosmofscrash.cee, generated into cosmofscrash.c);
 * the layout it writes as constants is asserted here.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/ramblk.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_CREAT == 0x40 && COSMO_O_TRUNC == 0x200 &&
                   VFS_UMOUNT_FORCE == 1 && EEXIST == 17 && ENOENT == 2 && VNODE_DIR == 2 &&
                   sizeof(struct ramblk_write) == 24 && offsetof(struct ramblk_write, nsectors) == 8,
               "cosmofscrash.cee's syscall.h, vfs.h, errno.h and ramblk.h values");

#define CRASH_LIST_MAX 64u
#define CRASH_NAME_MAX 32u

int cfs_crash_floor_list(struct vnode *vn, char *names, uint8_t *isdir, unsigned *n);

struct crash_list {
    char *names;
    uint8_t *isdir;
    unsigned n;
};

static int collect(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct crash_list *l = arg;
    (void)ino;
    if (l->n == CRASH_LIST_MAX || len >= CRASH_NAME_MAX)
        return 1;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    memcpy(l->names + l->n * CRASH_NAME_MAX, name, len);
    l->names[l->n * CRASH_NAME_MAX + len] = '\0';
    l->isdir[l->n] = type == VNODE_DIR;
    l->n++;
    return 0;
}

/* Up to 64 entries of `vn`, names of under 32 bytes, "." and ".." left
 * out, into `names` (64 * 32 bytes) and `isdir`; readdir's result. */
int cfs_crash_floor_list(struct vnode *vn, char *names, uint8_t *isdir, unsigned *n)
{
    struct crash_list l = { .names = names, .isdir = isdir, .n = 0 };
    uint64_t pos = 0;
    mutex_lock(&vn->lock);
    int rc = vn->ops->readdir ? vn->ops->readdir(vn, &pos, collect, &l) : -ENOTSUP;
    mutex_unlock(&vn->lock);
    *n = l.n;
    return rc;
}

#if !CONFIG_DEBUG
bool selftest_cosmofs_replay(const char **reason);

bool selftest_cosmofs_replay(const char **reason)
{
    (void)reason;
    kinfo("selftest: cosmofs-replay: debug builds only");
    return true;
}
#endif
