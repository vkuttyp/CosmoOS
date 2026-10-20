/*
 * cosmofs_clone.c - Writable clones of a snapshot
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Clones").
 *
 * A clone starts as its origin snapshot's tree and is written through
 * the same copy-on-write paths as the live tree: an operation on a
 * clone's vnode enters the clone, which swaps its roots into the
 * in-memory superblock for the length of the operation (under fs->lock,
 * which every such operation holds), and leaves it, keeping what moved.
 * A commit publishes the roots into the clone's list entry.
 *
 * What a clone owns is exact. A block it frees is either its origin's --
 * the origin's tree reaches it, the origin keeps it -- or one the clone
 * was born with, freed outright; nothing of the live tree's history
 * applies, since the live tree never reaches a block a clone allocated.
 * The test is the origin's tree itself, walked once into a set: the
 * origin is a snapshot and does not change. An origin cannot be deleted
 * while it has a clone, so the set stays true; deleting a clone frees
 * exactly the blocks its tree reaches and the origin's does not.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include "cosmofs_internal.h"

struct cfs_clone *cfs_clone_by_id(struct cfs *fs, uint64_t id)
{
    for (unsigned i = 0; i < fs->nclones; i++)
        if (fs->clones[i].id == id)
            return &fs->clones[i];
    return NULL;
}

void cfs_clone_enter(struct cfs *fs, struct cfs_clone *c)
{
    if (fs->cur_clone != NULL)
        return;   /* one at a time: an operation stays inside one tree */
    fs->live_imap_root = fs->sb.imap_root;
    fs->live_next_ino = fs->sb.next_ino;
    fs->live_inode_count = fs->sb.inode_count;
    fs->sb.imap_root = c->imap_root;
    fs->sb.next_ino = c->next_ino;
    fs->sb.inode_count = c->inode_count;
    fs->cur_clone = c;
}

void cfs_clone_leave(struct cfs *fs)
{
    struct cfs_clone *c = fs->cur_clone;
    if (c == NULL)
        return;
    if (c->imap_root != fs->sb.imap_root || c->next_ino != fs->sb.next_ino || c->inode_count != fs->sb.inode_count)
        c->dirty = true;
    c->imap_root = fs->sb.imap_root;
    c->next_ino = fs->sb.next_ino;
    c->inode_count = fs->sb.inode_count;
    fs->sb.imap_root = fs->live_imap_root;
    fs->sb.next_ino = fs->live_next_ino;
    fs->sb.inode_count = fs->live_inode_count;
    fs->cur_clone = NULL;
}

static void reach_into(void *arg, uint64_t lin)
{
    uint8_t *bits = arg;
    bits[lin / 8] |= (uint8_t)(1u << (lin % 8));
}

/* The origin's set, built on first need. NULL when it cannot be: every
 * caller then keeps the block, which leaks at worst and frees nothing a
 * tree still reaches. */
static const uint8_t *origin_reach(struct cfs *fs, struct cfs_clone *c)
{
    if (c->origin_reach != NULL)
        return c->origin_reach;
    struct cfs_snapshot o;
    bool found = false;
    unsigned total = 0;
    if (cfs_snapshot_list(fs, NULL, 0, &total) == 0 && total > 0) {
        struct cfs_snapshot *all = kmalloc((size_t)total * sizeof(*all), 0);
        unsigned n = 0;
        if (all != NULL && cfs_snapshot_list(fs, all, total, &n) == 0)
            for (unsigned i = 0; i < n && !found; i++)
                if (all[i].id == c->origin_id) {
                    o = all[i];
                    found = true;
                }
        kfree(all);
    }
    if (!found)
        return NULL;
    uint8_t *bits = kzalloc((size_t)((fs->nblocks + 7) / 8));
    if (bits == NULL)
        return NULL;
    if (cfs_tree_walk(fs, o.imap_root, o.next_ino, reach_into, bits) != 0) {
        kfree(bits);
        return NULL;
    }
    c->origin_reach = bits;
    return bits;
}

static bool reached(const uint8_t *bits, uint64_t lin)
{
    return (bits[lin / 8] >> (lin % 8)) & 1u;
}

void cfs_clone_free(struct cfs *fs, uint64_t dva)
{
    struct cfs_clone *c = fs->cur_clone;
    uint64_t lin = cfs_dva_lin(fs, dva);
    if (c == NULL || lin == CFS_DVA_NONE)
        return;
    const uint8_t *bits = origin_reach(fs, c);
    if (bits == NULL) {
        kwarn("cosmofs: clone %llu: its origin's tree could not be walked; block %llu kept",
              (unsigned long long)c->id, (unsigned long long)dva);
        return;
    }
    if (reached(bits, lin))
        return;   /* the origin's: it keeps it */
    /* The clone's own. No snapshot's tree reaches it (snapshots are of
     * the live tree, which never does), so no snapshot is asked. */
    cfs_free_block_exempt(fs, dva);
}

int cfs_clones_load(struct cfs *fs)
{
    cfs_clones_free(fs);
    unsigned total = 0;
    int rc = cfs_snapshot_list(fs, NULL, 0, &total);
    if (rc || total == 0)
        return rc;
    struct cfs_snapshot *all = kmalloc((size_t)total * sizeof(*all), 0);
    if (all == NULL)
        return -ENOMEM;
    unsigned n = 0;
    rc = cfs_snapshot_list(fs, all, total, &n);
    unsigned k = 0;
    for (unsigned i = 0; rc == 0 && i < n; i++)
        k += (all[i].flags & CFS_SNAP_F_CLONE) ? 1u : 0u;
    if (rc == 0 && k > 0) {
        fs->clones = kzalloc((size_t)k * sizeof(*fs->clones));
        if (fs->clones == NULL)
            rc = -ENOMEM;
        for (unsigned i = 0; rc == 0 && i < n; i++) {
            if (!(all[i].flags & CFS_SNAP_F_CLONE))
                continue;
            struct cfs_clone *c = &fs->clones[fs->nclones++];
            c->id = all[i].id;
            c->origin_id = all[i].origin_id;
            c->imap_root = all[i].imap_root;
            c->next_ino = all[i].next_ino;
            c->inode_count = all[i].inode_count;
        }
        fs->clone_cap = k;
    }
    kfree(all);
    return rc;
}

void cfs_clones_free(struct cfs *fs)
{
    for (unsigned i = 0; i < fs->nclones; i++)
        kfree(fs->clones[i].origin_reach);
    kfree(fs->clones);
    fs->clones = NULL;
    fs->nclones = fs->clone_cap = 0;
    fs->cur_clone = NULL;
}

struct publish_ctx {
    struct cfs *fs;
    unsigned written;
};

static bool publish_one(struct cfs_snapshot *s, void *arg)
{
    struct publish_ctx *p = arg;
    struct cfs_clone *c = (s->flags & CFS_SNAP_F_CLONE) ? cfs_clone_by_id(p->fs, s->id) : NULL;
    if (c == NULL || !c->dirty)
        return true;
    s->imap_root = c->imap_root;
    s->next_ino = c->next_ino;
    s->inode_count = c->inode_count;
    s->generation = p->fs->gen;   /* the commit that publishes these roots */
    c->dirty = false;
    p->written++;
    return false;   /* written back by the walk; walked again for the next */
}

int cfs_clones_publish(struct cfs *fs)
{
    bool any = false;
    for (unsigned i = 0; i < fs->nclones; i++)
        any = any || fs->clones[i].dirty;
    if (!any)
        return 0;
    int rc = cfs_snapshot_list_cow(fs);
    if (rc)
        return rc;
    for (;;) {
        struct publish_ctx p = { .fs = fs, .written = 0 };
        rc = cfs_snapshot_walk_write(fs, publish_one, &p);
        if (rc || p.written == 0)
            return rc;
    }
}

bool cfs_snapshot_has_clones(struct cfs *fs, uint64_t id)
{
    for (unsigned i = 0; i < fs->nclones; i++)
        if (fs->clones[i].origin_id == id)
            return true;
    return false;
}

void cfs_tree_roots(struct cfs *fs, const struct cfs_snapshot *s, uint64_t *imap_root, uint64_t *next_ino)
{
    struct cfs_clone *c = (s->flags & CFS_SNAP_F_CLONE) ? cfs_clone_by_id(fs, s->id) : NULL;
    *imap_root = c ? c->imap_root : s->imap_root;
    *next_ino = c ? c->next_ino : s->next_ino;
}

int cfs_clone_create(struct cfs *fs, const char *origin, const char *name, struct cfs_snapshot *out)
{
    if (fs->sb.version < 12)
        return -EOPNOTSUPP;   /* a clone is an entry of the version-12 list */
    struct cfs_snapshot o;
    if (!cfs_snapshot_find(fs, origin, &o))
        return -ENOENT;
    if (o.flags & CFS_SNAP_F_CLONE)
        return -EINVAL;       /* a clone of a clone: not in B1 */
    if (cfs_snapshot_find(fs, name, NULL))
        return -EEXIST;
    struct cfs_clone *grown = krealloc(fs->clones, (size_t)(fs->nclones + 1) * sizeof(*grown), 0);
    if (grown == NULL)
        return -ENOMEM;
    fs->clones = grown;
    fs->clone_cap = fs->nclones + 1;
    int rc = cfs_commit(fs);
    if (rc)
        return rc;
    struct cfs_snapshot c;
    rc = cfs_snapshot_add_clone(fs, &o, name, &c);
    if (rc)
        return rc;
    struct cfs_clone *t = &fs->clones[fs->nclones++];
    memset(t, 0, sizeof(*t));
    t->id = c.id;
    t->origin_id = o.id;
    t->imap_root = c.imap_root;
    t->next_ino = c.next_ino;
    t->inode_count = c.inode_count;
    rc = cfs_commit(fs);
    if (rc && !fs->failed) {
        fs->failed = rc;
        kerror("cosmofs: a clone could not be committed (%d); the mount is now read-only", rc);
    }
    if (rc == 0) {
        if (out)
            *out = c;
        kinfo("cosmofs: clone '%s' of snapshot '%s' made (id %llu)", c.name, o.name, (unsigned long long)c.id);
    }
    return rc;
}

/* Deleting a clone: every block its tree reaches and its origin's does
 * not goes back, exempt (no snapshot's tree reaches one), in the
 * transaction that removes the entry. */
struct clone_del {
    struct cfs *fs;
    const uint8_t *origin;
    uint64_t freed;
};

static void free_unless_origin(void *arg, uint64_t lin)
{
    struct clone_del *d = arg;
    if (reached(d->origin, lin))
        return;
    uint64_t dva = cfs_lin_dva(d->fs, lin);
    if (dva == CFS_DVA_NONE)
        return;
    cfs_free_block_exempt(d->fs, dva);
    d->freed++;
}

int cfs_clone_delete(struct cfs *fs, const struct cfs_snapshot *doomed, uint64_t *freed)
{
    *freed = 0;
    struct cfs_clone *c = cfs_clone_by_id(fs, doomed->id);
    if (c == NULL)
        return -EIO;
    const uint8_t *bits = origin_reach(fs, c);
    if (bits == NULL)
        return -EIO;   /* refuse rather than guess what is the origin's */
    struct clone_del d = { .fs = fs, .origin = bits };
    int rc = cfs_tree_walk(fs, c->imap_root, c->next_ino, free_unless_origin, &d);
    *freed = d.freed;
    if (rc)
        return rc;   /* the caller abandons: some frees are in the transaction */
    kfree(c->origin_reach);
    unsigned at = (unsigned)(c - fs->clones);
    fs->clones[at] = fs->clones[fs->nclones - 1];
    fs->nclones--;
    return 0;
}
