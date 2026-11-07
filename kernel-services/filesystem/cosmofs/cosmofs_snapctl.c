/*
 * cosmofs_snapctl.c - Snapshots through /dev/fsctl (roadmap 1.2 B1): list,
 * take, delete, roll back, clone, cancel and the boot's verdict, and what
 * each snapshot is worth. Moved out of cosmofs.c (roadmap 2.0 SA7) so
 * that it can be held to a digest and made CeeSafe on its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

/* --- snapshots through /dev/fsctl (roadmap 1.2 B1) ---------------------------
 *
 * design.md, "Format version 12". The .snapshots directory stays as it
 * was; this is the operator's interface, and the one that can say what a
 * snapshot is worth and roll back to one.
 */

/* A byte per block, saturating: how many trees reach it. Chunked like
 * the checker's maps, so a large pool does not need one large allocation. */
#define TALLY_CHUNK 4096u   /* a page each: no large allocation, however big the pool */

struct tally {
    uint8_t **chunk;
    unsigned nchunks;
};

static void tally_free(struct tally *t)
{
    for (unsigned i = 0; i < t->nchunks; i++)
        kfree(t->chunk[i]);
    kfree(t->chunk);
    t->chunk = NULL;
    t->nchunks = 0;
}

static int tally_alloc(struct tally *t, uint64_t n)
{
    t->nchunks = (unsigned)((n + TALLY_CHUNK - 1) / TALLY_CHUNK);
    t->chunk = kzalloc((size_t)t->nchunks * sizeof(*t->chunk));
    if (t->chunk == NULL) {
        t->nchunks = 0;
        return -ENOMEM;
    }
    for (unsigned i = 0; i < t->nchunks; i++) {
        t->chunk[i] = kzalloc(TALLY_CHUNK);
        if (t->chunk[i] == NULL) {
            tally_free(t);
            return -ENOMEM;
        }
    }
    return 0;
}

static uint8_t *tally_at(struct tally *t, uint64_t lin)
{
    unsigned k = (unsigned)(lin / TALLY_CHUNK);
    return k < t->nchunks ? &t->chunk[k][lin % TALLY_CHUNK] : NULL;
}

static void tally_count(void *arg, uint64_t lin)
{
    uint8_t *c = tally_at(arg, lin);
    if (c != NULL && *c < 255)
        (*c)++;
}

struct tally_size {
    struct tally *t;
    uint64_t referenced, unique;
};

static void tally_size(void *arg, uint64_t lin)
{
    struct tally_size *s = arg;
    uint8_t *c = tally_at(s->t, lin);
    s->referenced++;
    if (c != NULL && *c == 1)
        s->unique++;
}

/* A mark called once per block per root: `seen` is cleared per root. */
struct once {
    struct tally *t;
    uint8_t *seen;
    void (*fn)(void *arg, uint64_t lin);
    void *arg;
};

static void once_mark(void *arg, uint64_t lin)
{
    struct once *o = arg;
    uint8_t bit = (uint8_t)(1u << (lin % 8));
    if (o->seen[lin / 8] & bit)
        return;
    o->seen[lin / 8] |= bit;
    o->fn(o->arg, lin);
}

static void snap_info_fill(struct cfs *fs, struct cosmofs_snap_info *o, const struct cfs_snapshot *s)
{
    memset(o, 0, sizeof(*o));
    o->id = s->id;
    o->generation = s->generation;
    o->created_ns = s->created_ns;
    o->origin_id = s->origin_id;
    memcpy(o->uuid, s->uuid, sizeof(o->uuid));
    o->flags = (s->flags & CFS_SNAP_F_CLONE) ? COSMOFS_SNAP_I_CLONE : 0u;
    o->format = fs->sb.version;
    strlcpy(o->name, s->name, sizeof(o->name));
}

/*
 * Every snapshot, with what it reaches and what only it reaches. Two
 * walks per snapshot and one of the live tree: the first pass counts, per
 * block, how many trees reach it; the second sums each snapshot's blocks
 * and the ones whose count is one. Under the mount's lock, as the check
 * is. Oldest first.
 */
/* What a rollback record says about one snapshot, for the listing. */
static uint32_t snap_rollback_flags(const struct cfs_rollback *r, bool have, const struct cfs_snapshot *s)
{
    if (!have)
        return 0;
    uint32_t f = 0;
    bool target = memcmp(r->target_uuid, s->uuid, 16) == 0, safety = memcmp(r->safety_uuid, s->uuid, 16) == 0;
    if (r->state == CFS_ROLLBACK_PENDING) {
        f |= target ? COSMOFS_SNAP_I_TARGET : 0u;
        f |= safety ? COSMOFS_SNAP_I_SAFETY : 0u;
    } else {
        f |= safety ? COSMOFS_SNAP_I_HELD : 0u;
        /* The tree the live one was rolled back to, while the hold lasts:
         * how init tells a package transaction undone from one still
         * waiting to be (roadmap 1.2 B4). */
        f |= target ? COSMOFS_SNAP_I_LIVE : 0u;
    }
    return f;
}

static int snap_list_sized(struct cfs *fs, struct cosmofs_snap_req *req)
{
    req->format = fs->sb.version;
    req->pool_flags = 0;
    /* Room, as data can use it (the reserve is the metadata's), and the
     * pool's size: what a caller about to write asks before it does. */
    req->free_bytes = (fs->free_blocks > fs->reserve ? fs->free_blocks - fs->reserve : 0) * (uint64_t)CFS_BLOCK;
    req->total_bytes = fs->nblocks * (uint64_t)CFS_BLOCK;
    struct cfs_rollback rb;
    bool have_rb = cfs_rollback_read(fs, &rb) == 0;
    if (have_rb)
        req->pool_flags = rb.state == CFS_ROLLBACK_PENDING ? COSMOFS_POOL_PENDING : COSMOFS_POOL_HELD;
    /* Why this mount left a pending rollback pending (CFS-20); only while
     * one is, so a later request does not inherit an old reason. */
    req->rollback_error = have_rb && rb.state == CFS_ROLLBACK_PENDING ? fs->rollback_error : 0;
    unsigned total = 0;
    int rc = cfs_snapshot_list(fs, NULL, 0, &total);
    if (rc)
        return rc;
    req->total = total;
    req->count = 0;
    if (total == 0)
        return 0;
    struct cfs_snapshot *all = kmalloc((size_t)total * sizeof(*all), 0);
    if (all == NULL)
        return -ENOMEM;
    unsigned n = 0;
    rc = cfs_snapshot_list(fs, all, total, &n);
    if (rc == 0 && n != total)
        rc = -EIO;
    /* Oldest first: a short insertion sort by id, which only rises. */
    for (unsigned i = 1; rc == 0 && i < n; i++)
        for (unsigned j = i; j > 0 && all[j - 1].id > all[j].id; j--) {
            struct cfs_snapshot tmp = all[j];
            all[j] = all[j - 1];
            all[j - 1] = tmp;
        }
    if (req->no_sizes) {
        for (unsigned i = 0; rc == 0 && i < n && req->count < req->max; i++) {
            if (all[i].id <= req->after)
                continue;
            snap_info_fill(fs, &req->out[req->count], &all[i]);
            req->out[req->count++].flags |= snap_rollback_flags(&rb, have_rb, &all[i]);
        }
        kfree(all);
        return rc;
    }
    /* Each root counted once per block, whatever reaches the block twice
     * inside it (a deadlist names blocks its own tree reaches). */
    struct tally t = { 0 };
    struct once o1 = { .t = &t };
    if (rc == 0)
        rc = tally_alloc(&t, fs->nblocks);
    if (rc == 0) {
        o1.seen = kmalloc((size_t)((fs->nblocks + 7) / 8), 0);
        if (o1.seen == NULL)
            rc = -ENOMEM;
    }
    size_t seen_bytes = (size_t)((fs->nblocks + 7) / 8);
    if (rc == 0) {
        memset(o1.seen, 0, seen_bytes);
        o1.fn = tally_count;
        o1.arg = &t;
        rc = cfs_tree_walk(fs, fs->sb.imap_root, fs->sb.next_ino, once_mark, &o1);
        if (rc == 0)
            rc = cfs_alloc_walk(fs, fs->sb.version >= 4 ? fs->sb.members : fs->mem[0].alloc_root, once_mark, &o1);
        if (rc)
            kwarn("cosmofs: snapshot sizes: the live tree does not walk (%d)", rc);
    }
    for (unsigned i = 0; rc == 0 && i < n; i++) {
        uint64_t root, next;
        cfs_tree_roots(fs, &all[i], &root, &next);   /* a clone's as the open transaction has them */
        memset(o1.seen, 0, seen_bytes);
        rc = cfs_tree_walk(fs, root, next, once_mark, &o1);
        if (rc == 0)
            rc = cfs_snapshot_walk_held(fs, &all[i], once_mark, &o1);
        if (rc)
            kwarn("cosmofs: snapshot sizes: snapshot '%s' does not walk (%d)", all[i].name, rc);
    }
    for (unsigned i = 0; rc == 0 && i < n && req->count < req->max; i++) {
        if (all[i].id <= req->after)
            continue;   /* the caller's cursor: listed in an earlier page */
        struct tally_size sz = { .t = &t };
        uint64_t root, next;
        cfs_tree_roots(fs, &all[i], &root, &next);
        memset(o1.seen, 0, seen_bytes);
        o1.fn = tally_size;
        o1.arg = &sz;
        rc = cfs_tree_walk(fs, root, next, once_mark, &o1);
        if (rc == 0)
            rc = cfs_snapshot_walk_held(fs, &all[i], once_mark, &o1);
        struct cosmofs_snap_info *o = &req->out[req->count++];
        snap_info_fill(fs, o, &all[i]);
        o->flags |= snap_rollback_flags(&rb, have_rb, &all[i]);
        o->referenced = sz.referenced * CFS_BLOCK;
        o->unique = sz.unique * CFS_BLOCK;
    }
    kfree(o1.seen);
    tally_free(&t);
    kfree(all);
    return rc;
}

static int snap_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return -EINVAL;
    size_t len = strlen(name);
    if (len > CFS_SNAP_NAME_MAX)
        return -ENAMETOOLONG;
    if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;
    return 0;
}

int cosmofs_snapctl(struct mount *mnt, struct cosmofs_snap_req *req)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    if (fs->failed)
        return -EIO;
    bool writes = req->op != COSMOFS_SNAP_LIST;
    if (writes && (mnt->flags & MOUNT_RDONLY))
        return -EROFS;
    int rc;
    switch (req->op) {
    case COSMOFS_SNAP_CREATE: {
        rc = snap_name_ok(req->name);
        if (rc)
            return rc;
        /* What has been written is in the snapshot: dirty pages are the
         * page cache's until written, and a commit alone does not. */
        rc = cfs_sync_vnodes(fs);
        if (rc)
            return rc;
        struct cfs_snapshot s;
        mutex_lock(&fs->lock);
        rc = cfs_snapshot_create(fs, req->name, true, &s);
        mutex_unlock(&fs->lock);
        if (rc == 0) {
            if (req->out != NULL && req->max > 0) {
                snap_info_fill(fs, &req->out[0], &s);
                req->count = 1;
            }
            kinfo("cosmofs: snapshot '%s' taken (id %llu, generation %llu)", s.name, (unsigned long long)s.id,
                  (unsigned long long)s.generation);
        }
        return rc;
    }
    case COSMOFS_SNAP_LIST:
        mutex_lock(&fs->lock);
        rc = snap_list_sized(fs, req);
        mutex_unlock(&fs->lock);
        return rc;
    case COSMOFS_SNAP_DELETE: {
        rc = snap_name_ok(req->name);
        if (rc)
            return rc;
        mutex_lock(&fs->lock);
        struct cfs_snapshot snap;
        unsigned tag;
        rc = cfs_snap_by_name(fs, req->name, strlen(req->name), &snap, &tag);
        /* Busy while anything inside it is in use, as rmdir on
         * .snapshots answers (design.md, "When something goes wrong"):
         * here no reference is the caller's own. */
        if (rc == 0 && cfs_snap_in_use(mnt, tag, NULL))
            rc = -EBUSY;
        if (rc == 0)
            rc = cfs_snapshot_delete(fs, req->name);
        mutex_unlock(&fs->lock);
        if (rc == 0)
            kinfo("cosmofs: snapshot '%s' deleted", req->name);
        return rc;
    }
    case COSMOFS_SNAP_ROLLBACK: {
        rc = snap_name_ok(req->name);
        if (rc == 0 && req->name2 != NULL)
            rc = snap_name_ok(req->name2);
        if (rc)
            return rc;
        /* The safety snapshot is the tree as written so far, dirty pages
         * included; what is written after the request is not in it, and
         * the switch replaces it with the target (design.md, "Rollback"). */
        rc = cfs_sync_vnodes(fs);
        if (rc)
            return rc;
        struct cfs_snapshot s;
        mutex_lock(&fs->lock);
        rc = cfs_rollback_request(fs, req->name, req->name2, &s);
        mutex_unlock(&fs->lock);
        if (rc == 0 && req->out != NULL && req->max > 0) {
            snap_info_fill(fs, &req->out[0], &s);
            req->out[0].flags |= COSMOFS_SNAP_I_SAFETY;
            req->count = 1;
        }
        return rc;
    }
    case COSMOFS_SNAP_CLONE: {
        rc = snap_name_ok(req->name);
        if (rc == 0)
            rc = req->name2 ? snap_name_ok(req->name2) : -EINVAL;
        if (rc)
            return rc;
        struct cfs_snapshot c;
        mutex_lock(&fs->lock);
        rc = cfs_clone_create(fs, req->name, req->name2, &c);
        mutex_unlock(&fs->lock);
        if (rc == 0 && req->out != NULL && req->max > 0) {
            snap_info_fill(fs, &req->out[0], &c);
            req->count = 1;
        }
        return rc;
    }
    case COSMOFS_SNAP_CANCEL:
        mutex_lock(&fs->lock);
        rc = cfs_rollback_cancel(fs);
        mutex_unlock(&fs->lock);
        return rc;
    case COSMOFS_SNAP_BOOT_OK: {
        bool released = false;
        mutex_lock(&fs->lock);
        rc = cfs_rollback_boot_ok(fs, &released);
        mutex_unlock(&fs->lock);
        req->count = 0;
        req->total = released ? 1 : 0;   /* fsctl does not report it; a test reads it */
        return rc;
    }
    default:
        return -EOPNOTSUPP;
    }
}
