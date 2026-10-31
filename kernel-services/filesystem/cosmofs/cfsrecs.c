/*
 * cfsrecs.c - cosmofs's decoders of the records snapshots, rollback and
 * encryption keep (roadmap 2.0 SA5, the cosmofs family, batch 3): a
 * snapshot list's entries under both layouts, the rollback record, and a
 * key slot's shape. Moved unchanged from cosmofs_snap.c,
 * cosmofs_rollback.c and cosmofs_crypt.c: the C step, to be replaced in
 * cfsparse.cee once tests/host/test_cfsparse.c's recorded digest agrees.
 */

#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* The rollback record's CRC32C: over the record with its crc field zero. */
uint32_t cfs_rollback_crc(const struct cfs_rollback *r)
{
    struct cfs_rollback tmp = *r;
    tmp.crc = 0;
    return crc32c(&tmp, sizeof(tmp));
}

int cfs_rollback_parse(const void *payload, struct cfs_rollback *out)
{
    static const uint8_t zero[16];
    memcpy(out, payload, sizeof(*out));
    if (out->magic != CFS_ROLLBACK_MAGIC || out->version != 1 || cfs_rollback_crc(out) != out->crc ||
        (out->state != CFS_ROLLBACK_PENDING && out->state != CFS_ROLLBACK_HELD) ||
        (out->flags & ~CFS_ROLLBACK_F_FROM_HELD) != 0 ||
        (out->state == CFS_ROLLBACK_HELD && out->flags != 0))
        return -EIO;
    /* Checked for meaning too, not only for damage: a record whose safety
     * snapshot is its target keeps nothing of the tree it replaces. */
    if (memcmp(out->safety_uuid, zero, 16) == 0)
        return -EIO;
    if (out->state == CFS_ROLLBACK_PENDING &&
        (memcmp(out->target_uuid, zero, 16) == 0 || memcmp(out->target_uuid, out->safety_uuid, 16) == 0))
        return -EIO;
    return 0;
}

/* Entry `i` of a list block, false when the slot is empty. */
bool cfs_snaplist_get(const struct cfs *fs, const struct cfs_buf *b, unsigned i, struct cfs_snapshot *out)
{
    if (fs->sb.version >= 12) {
        const struct cfs_snap_block2 *sb = (const struct cfs_snap_block2 *)(b->data + CFS_MHDR_SIZE);
        if (i >= CFS_SNAPS_PER_BLOCK2 || sb->snap[i].name[0] == '\0')
            return false;
        *out = sb->snap[i];
        out->name[CFS_SNAP_NAME_MAX] = '\0';
        return true;
    }
    const struct cfs_snap_block *sb = (const struct cfs_snap_block *)(b->data + CFS_MHDR_SIZE);
    if (i >= CFS_SNAPS_PER_BLOCK || sb->snap[i].name[0] == '\0')
        return false;
    const struct cfs_snapshot_v11 *o = &sb->snap[i];
    memset(out, 0, sizeof(*out));
    out->generation = o->generation;
    out->imap_root = o->imap_root;
    out->alloc_root = o->alloc_root;
    out->next_ino = o->next_ino;
    out->inode_count = o->inode_count;
    out->deadlist = o->deadlist;
    out->created_ns = o->created_ns;
    out->id = o->id;
    memcpy(out->name, o->name, sizeof(o->name));
    out->name[CFS_SNAP_NAME_MAX] = '\0';
    return true;
}

/* A slot as user space sealed it is taken as it is -- the kernel cannot
 * open one, which needs the passphrase -- but its shape is checked: the
 * Argon2id parameters user space can derive with (userland/lib/argon2.c).
 * Lanes are at most 2^24 - 1 (RFC 9106), which also keeps 8 * lanes from
 * wrapping: 0x20000000 lanes would have passed with any memory cost. */
bool cfs_keyslot_valid(unsigned index, const struct cfs_keyslot *s)
{
    switch (s->kind) {
    case CFS_KEYSLOT_EMPTY:
        return true;
    case CFS_KEYSLOT_PASSPHRASE:
        return s->t_cost >= 1 && s->lanes >= 1 && s->lanes <= 0xffffff && s->m_cost_kib >= 8 * s->lanes;
    case CFS_KEYSLOT_KEYFILE:
        return index == 0;
    default:
        return false;
    }
}
