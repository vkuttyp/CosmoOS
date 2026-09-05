/*
 * fuzz_cosmofs.c - cosmofs images under fuzzing (docs/verification/design.md).
 *
 * The input is a disk image of CFS_MIN_BLOCKS blocks (zero-padded or
 * truncated). The real filesystem code mounts it over the host pool shim,
 * walks every directory through the vnode operations, reads the first
 * pages of every regular file, and unmounts; every vnode must be gone
 * afterwards. Any input must be handled without reading outside the image
 * or the filesystem's own allocations; rejection (-EIO on mount, errors on
 * lookups) is the expected outcome for most mutations.
 */

#include "fuzz.h"
#include "shim_fs.h"

#include "../host/harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "cosmofs_format.h"

#include <stdlib.h>
#include <string.h>

#define IMAGE_BLOCKS 64u   /* CFS_MIN_BLOCKS */

static struct blkdev g_bd;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    host_arena_init(16u << 20);
    kmalloc_init();
    memset(&g_bd, 0, sizeof(g_bd));
    strcpy(g_bd.name, "fuzz");
    g_bd.sector_size = 512;
    g_bd.capacity = IMAGE_BLOCKS * (4096 / 512);
    g_bd.max_sectors = 8;
    return 0;
}

size_t fuzz_max_len(void)
{
    return IMAGE_BLOCKS * 4096u;
}

struct entry {
    char name[64];
    size_t len;
    enum vnode_type type;
};

struct listing {
    struct entry e[64];
    unsigned n;
};

static int collect(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct listing *l = arg;
    (void)ino;
    if (l->n == 64)
        return 1;
    if (len >= sizeof(l->e[0].name))
        len = sizeof(l->e[0].name) - 1;
    memcpy(l->e[l->n].name, name, len);
    l->e[l->n].name[len] = '\0';
    l->e[l->n].len = len;
    l->e[l->n].type = type;
    l->n++;
    return 0;
}

static void walk(struct vnode *dir, unsigned depth, unsigned *budget)
{
    if (depth > 6 || *budget == 0 || dir->ops == NULL || dir->ops->readdir == NULL)
        return;
    struct listing *l = kzalloc(sizeof(*l));
    if (l == NULL)
        return;
    uint64_t pos = 0;
    mutex_lock(&dir->lock);
    int rc = dir->ops->readdir(dir, &pos, collect, l);
    mutex_unlock(&dir->lock);
    if (rc == 0) {
        for (unsigned i = 0; i < l->n && *budget > 0; i++) {
            (*budget)--;
            struct vnode *child = NULL;
            mutex_lock(&dir->lock);
            rc = dir->ops->lookup ? dir->ops->lookup(dir, l->e[i].name, l->e[i].len, &child) : -ENOTSUP;
            mutex_unlock(&dir->lock);
            if (rc || child == NULL)
                continue;
            if (child->type == VNODE_DIR && child != dir)
                walk(child, depth + 1, budget);
            else if (child->type == VNODE_REG && child->ops && child->ops->readpage) {
                uint8_t *page = kmalloc(4096, 0);
                if (page) {
                    uint64_t pages = (child->size + 4095) / 4096;
                    for (uint64_t p = 0; p < pages && p < 4; p++) {
                        mutex_lock(&child->lock);
                        child->ops->readpage(child, p, page);
                        mutex_unlock(&child->lock);
                    }
                    kfree(page);
                }
            }
            vnode_put(child);
        }
    }
    kfree(l);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    shim_image_set(data, size, IMAGE_BLOCKS);

    struct mount *mnt = kzalloc(sizeof(*mnt));
    FUZZ_ASSERT(mnt != NULL);
    for (unsigned i = 0; i < VNODE_HASH; i++)
        list_init(&mnt->vnodes[i]);
    mnt->fs = &cosmofs_fs_type;
    mnt->bdev = &g_bd;

    int rc = cosmofs_fs_type.mount(&cosmofs_fs_type, &g_bd, 0, mnt);
    if (rc == 0) {
        FUZZ_ASSERT(mnt->root != NULL && mnt->root->type == VNODE_DIR);
        unsigned budget = 256;
        walk(mnt->root, 0, &budget);
        struct cosmofs_stats st;
        cosmofs_stats(mnt, &st);
        FUZZ_ASSERT(st.total_blocks <= IMAGE_BLOCKS);
        cosmofs_fs_type.unmount(mnt);
        struct vnode *root = mnt->root;
        mnt->root = NULL;
        vnode_put(root);
        FUZZ_ASSERT(mnt->nr_vnodes == 0);   /* every vnode the walk touched is gone */
    } else {
        FUZZ_ASSERT(rc == -EIO || rc == -EINVAL || rc == -ENOMEM);
        FUZZ_ASSERT(mnt->fs_priv == NULL && mnt->root == NULL);
    }
    kfree(mnt);
    return 0;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    if (i != 0 || cap < IMAGE_BLOCKS * 4096u)
        return 0;
    static const uint8_t zero[1] = { 0 };
    shim_image_set(zero, 0, IMAGE_BLOCKS);
    if (cosmofs_format(&g_bd) != 0)
        return 0;
    uint64_t n;
    const uint8_t *img = shim_image_get(&n);
    memcpy(buf, img, IMAGE_BLOCKS * 4096u);
    return IMAGE_BLOCKS * 4096u;
}
