/*
 * cosmofs_buf.c - Checksums and headers, and the buffer cache: every
 * metadata block's seal and its check, the superblock's, the cache that
 * holds blocks between the reads and the commit that writes them, the
 * copy-on-write of a block, the set-aside blocks a change that must not
 * stop half way takes, and the reservation the commit hands out from.
 * Moved out of cosmofs_core.c (roadmap 2.0 SA7) so that it can be held to
 * a digest and made CeeSafe on its own.
 *
 * Rules (docs/kernel-services/vfs/design.md): a metadata block from a
 * committed generation is never modified in place: it is copied first,
 * and the copy is sealed with the open transaction's generation.
 */

#include <kernel/blake2b.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- checksums and headers -------------------------------------------- */


/*
 * The version-14 tag (design.md, "The metadata MAC"): a 64-bit keyed
 * BLAKE2b over the domain byte and the block, with the eight bytes at
 * `off` -- where the CRC and the word after it were -- taken as zero.
 */
void cfs_mac_tag(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off, uint8_t tag[CFS_MAC_SIZE])
{
    static const uint8_t zero8[CFS_MAC_SIZE] = { 0 };
    uint8_t d = (uint8_t)domain;
    struct blake2b s;
    blake2b_init_key(&s, CFS_MAC_SIZE, m->key, sizeof(m->key));
    blake2b_update(&s, &d, 1);
    blake2b_update(&s, block, off);
    blake2b_update(&s, zero8, CFS_MAC_SIZE);
    blake2b_update(&s, block + off + CFS_MAC_SIZE, CFS_BLOCK - off - CFS_MAC_SIZE);
    blake2b_final(&s, tag);
}

/* Constant time: a comparison that stops early says how much of a
 * forgery was right. */
bool cfs_mac_ok(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off)
{
    uint8_t want[CFS_MAC_SIZE];
    cfs_mac_tag(m, domain, block, off, want);
    uint8_t diff = 0;
    for (unsigned i = 0; i < CFS_MAC_SIZE; i++)
        diff |= (uint8_t)(want[i] ^ block[off + i]);
    memset(want, 0, sizeof(want));
    return diff == 0;
}

void cfs_mac_seal(const struct cfs_mac *m, char domain, uint8_t *block, size_t off)
{
    uint8_t tag[CFS_MAC_SIZE];
    cfs_mac_tag(m, domain, block, off, tag);
    memcpy(block + off, tag, CFS_MAC_SIZE);
}

/* Seal a header: the CRC32C and its algorithm, or on a version-14
 * encrypted pool the tag in their place. */
static void mhdr_seal_mac(const struct cfs_mac *mac, uint8_t *block, uint32_t kind, uint64_t blkno, uint64_t gen)
{
    struct cfs_mhdr *h = (struct cfs_mhdr *)block;
    h->magic = CFS_MHDR_MAGIC;
    h->kind = kind;
    h->generation = gen;
    h->blkno = blkno;
    if (mac != NULL && mac->on) {
        cfs_mac_seal(mac, CFS_MAC_HDR, block, offsetof(struct cfs_mhdr, crc));
        return;
    }
    h->csum_algo = CFS_CSUM_CRC32C;   /* the algorithm `crc` is computed with */
    h->crc = 0;
    h->crc = cfs_block_crc(block, offsetof(struct cfs_mhdr, crc));
}

void cfs_mhdr_seal(struct cfs *fs, uint8_t *block, uint32_t kind, uint64_t blkno)
{
    mhdr_seal_mac(&fs->mac, block, kind, blkno, fs->gen);
}


/*
 * Why a metadata block did not verify, for the message below.
 *
 * The five are very different findings and the error said only "bad
 * metadata header or checksum" for all of them: a wrong magic is a block
 * that was never sealed as metadata, a wrong `blkno` is one block's
 * content sitting at another's address, a wrong kind is the right block
 * misused, an unsupported csum_algo is a block checksummed by an algorithm
 * this build does not have (a format or algorithm skew, not damage), and a
 * bad CRC is content that changed after it was sealed. Chasing the writeback
 * thread that committed during a mount needed to know which; the message is
 * kept because the next one will too. The enum is in cosmofs_internal.h.
 * Under a MAC (version 14) the last is MHDR_MAC: a tag that does not
 * verify, which is a forgery as readily as damage.
 */
enum mhdr_fault cfs_mhdr_fault_of(const struct cfs_mac *mac, const void *block, uint64_t blkno, uint32_t kind)
{
    /* The header's own checks are cfsparse's; a MAC needs the pool's key. */
    bool mac_on = mac != NULL && mac->on;
    enum mhdr_fault f = (enum mhdr_fault)cfs_mhdr_decode(block, blkno, kind, mac_on);
    if (f == MHDR_OK && mac_on)
        return cfs_mac_ok(mac, CFS_MAC_HDR, block, offsetof(struct cfs_mhdr, crc)) ? MHDR_OK : MHDR_MAC;
    return f;
}

static int mhdr_check(const struct cfs_mac *mac, const uint8_t *block, uint64_t blkno, uint32_t kind)
{
    return cfs_mhdr_fault_of(mac, block, blkno, kind) == MHDR_OK ? 0 : -EIO;
}

struct mhdr_want {
    const struct cfs_mac *mac;
    uint64_t dva;
    uint32_t kind;
};

void cfs_mhdr_seal_raw(const struct cfs_mac *mac, void *block, uint32_t kind, uint64_t dva, uint64_t generation)
{
    mhdr_seal_mac(mac, block, kind, dva, generation);
}

bool cfs_mhdr_ok(const struct cfs_mac *mac, const void *block, uint64_t dva, uint32_t kind)
{
    return mhdr_check(mac, block, dva, kind) == 0;
}

/* A superblock's seal: CRC32C, or the tag when it says it is a version-14
 * encrypted pool's. */
void cfs_super_seal(const struct cfs_mac *mac, uint8_t *block)
{
    struct cfs_super *sb = (struct cfs_super *)block;
    if (cfs_super_is_mac(sb)) {
        cfs_mac_seal(mac, CFS_MAC_SUPER, block, offsetof(struct cfs_super, crc));
        return;
    }
    sb->crc = 0;
    sb->crc = cfs_block_crc(block, offsetof(struct cfs_super, crc));
}

/*
 * A superblock's own check: magic, a version this kernel reads, and its
 * seal. A MAC'd one needs its pool's key from the key table (`mac` is
 * filled from it): -ENOKEY when the pool is locked. A CRC'd one is
 * refused when a key is loaded for its uuid -- a pool whose key this
 * kernel holds does not get to stop authenticating (design.md, "The
 * metadata MAC"). -EIO for anything else that does not verify.
 */
int cfs_super_verify(const void *block, struct cfs_mac *mac)
{
    const struct cfs_super *sb = block;
    if (!cfs_super_ident_ok(sb))
        return -EIO;
    struct cfs_mac m;
    int rc = cfs_keytab_mac(sb->uuid, &m);
    if (cfs_super_is_mac(sb)) {
        if (rc)
            return -ENOKEY;
        rc = cfs_mac_ok(&m, CFS_MAC_SUPER, block, offsetof(struct cfs_super, crc)) ? 0 : -EIO;
    } else {
        rc = rc == 0 ? -EIO : cfs_super_crc_ok(block) ? 0 : -EIO;
        m.on = false;
    }
    if (rc == 0 && mac != NULL)
        *mac = m;
    memset(&m, 0, sizeof(m));
    return rc;
}

bool cfs_super_ok(const void *block)
{
    return cfs_super_verify(block, NULL) == 0;
}

/* The verifier cfs_read_repair calls for a metadata block. */
static bool mhdr_ok(const void *block, void *arg)
{
    const struct mhdr_want *w = arg;
    return mhdr_check(w->mac, block, w->dva, w->kind) == 0;
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b)
{
    return (struct cfs_mhdr *)b->data;
}

int cfs_data_read(struct cfs *fs, uint64_t blk, void *buf)
{
    if (!cfs_dva_valid(fs, blk))
        return -EIO;
    return pool_read(fs->pool, blk, buf);
}

int cfs_data_write(struct cfs *fs, uint64_t blk, const void *buf)
{
    if (!cfs_dva_valid(fs, blk))
        return -EIO;
    return pool_write(fs->pool, blk, buf);
}

/* --- buffer cache -------------------------------------------------------- */

struct cfs_buf *cfs_buf_find(struct cfs *fs, uint64_t blkno)
{
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (b->blkno == blkno)
            return b;
    }
    return NULL;
}

static void buf_evict_clean(struct cfs *fs)
{
    if (fs->nr_bufs < CFS_BUF_CACHE)
        return;
    struct cfs_buf *b;
    list_for_each_entry_reverse(b, &fs->bufs, link) {
        if (!b->dirty && b->refs == 0) {
            list_remove(&b->link);
            fs->nr_bufs--;
            kfree(b->data);
            kfree(b);
            return;
        }
    }
}

/* A buffer not yet in the cache: the half of buf_alloc that can fail,
 * so a set-aside (cfs_prep_meta) can do it before anything changes. */
static struct cfs_buf *buf_make(struct cfs *fs, uint64_t blkno)
{
    /* A clean buffer goes back first: memory the cache holds for nobody
     * is what a new buffer should be made from (Codex on #433). */
    buf_evict_clean(fs);
    struct cfs_buf *b = kzalloc(sizeof(*b));
    if (b == NULL)
        return NULL;
    b->data = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (b->data == NULL) {
        kfree(b);
        return NULL;
    }
    list_init(&b->link);
    b->blkno = blkno;
    b->refs = 1;
    return b;
}

/* And the half that cannot. */
static void buf_insert(struct cfs *fs, struct cfs_buf *b)
{
    list_push_front(&fs->bufs, &b->link);
    fs->nr_bufs++;
}

struct cfs_buf *cfs_buf_alloc(struct cfs *fs, uint64_t blkno)
{
    struct cfs_buf *b = buf_make(fs, blkno);
    if (b != NULL)
        buf_insert(fs, b);
    return b;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    if (!cfs_dva_valid(fs, blkno))
        return -EIO;
    struct cfs_buf *b = cfs_buf_find(fs, blkno);
    if (b) {
        b->refs++;
        list_remove(&b->link);
        list_push_front(&fs->bufs, &b->link);
        if (kind && cfs_buf_hdr(b)->kind != kind) {
            cfs_buf_put(fs, b);   /* undo the ref taken above: a kind mismatch is not a hand-out */
            return -EIO;
        }
        *out = b;
        return 0;
    }
    b = cfs_buf_alloc(fs, blkno);
    if (b == NULL)
        return -ENOMEM;
    /* A metadata block checks itself, so a mirrored member can answer
     * from another copy and put the bad one right (design.md, "A mirror
     * is only as good as its verifier"). */
    struct mhdr_want want = { .mac = &fs->mac, .dva = blkno, .kind = kind };
    bool read_ok = false;
    int rc = cfs_read_repair(fs, blkno, b->data, mhdr_ok, &want, NULL, &read_ok);
    if (rc) {
        if (rc == -EIO && read_ok) {
            /* Say which of the four checks failed and what it found:
             * "bad header or checksum" covers four different faults with
             * four different causes. */
            const struct cfs_mhdr *h = (const struct cfs_mhdr *)b->data;
            static const char *const why[] = { "ok", "magic", "blkno", "kind", "algo", "crc", "mac" };
            enum mhdr_fault f = cfs_mhdr_fault_of(&fs->mac, b->data, blkno, kind);
            if (fs->mac.on)
                /* No computed value: printing the right tag beside a
                 * forged one would hand the forger the answer. */
                kerror("cosmofs: block %llu: metadata %s fault: magic 0x%08x kind %u gen %llu blkno %llu "
                       "(wanted kind %u at blkno %llu): it does not authenticate",
                       (unsigned long long)blkno, why[f], h->magic, h->kind, (unsigned long long)h->generation,
                       (unsigned long long)h->blkno, kind, (unsigned long long)blkno);
            else
                kerror("cosmofs: block %llu: metadata %s fault: magic 0x%08x kind %u gen %llu blkno %llu algo %u "
                       "crc 0x%08x (wanted kind %u at blkno %llu, computed crc 0x%08x)",
                       (unsigned long long)blkno, why[f], h->magic, h->kind, (unsigned long long)h->generation,
                       (unsigned long long)h->blkno, h->csum_algo, h->crc, kind, (unsigned long long)blkno,
                       cfs_block_crc(b->data, offsetof(struct cfs_mhdr, crc)));
        } else {
            /* Nothing was read, so b->data describes nothing: saying
             * which header field is wrong would be inventing one. */
            kerror("cosmofs: block %llu: read error (%d)", (unsigned long long)blkno, rc);
        }
        list_remove(&b->link);
        fs->nr_bufs--;
        kfree(b->data);
        kfree(b);
        return rc;
    }
    *out = b;
    return 0;
}

/* Whether the open transaction holds `blkno` dirty: written in memory
 * and not yet on any device, so a read of the device says nothing about
 * it (the scrub's question). */
bool cfs_buf_is_dirty(struct cfs *fs, uint64_t blkno)
{
    struct cfs_buf *b = cfs_buf_find(fs, blkno);
    return b != NULL && b->dirty;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    KASSERT(b->refs > 0);
    b->refs--;
}

void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    if (!b->dirty) {
        b->dirty = true;
        fs->nr_dirty++;
    }
    cfs_note_dirty(fs);
}

/*
 * The inverse, for a rollback. A buffer whose block is being given back
 * must not stay dirty: the commit that eventually succeeds writes every
 * dirty buffer, and by then that block may belong to something else --
 * two owners writing one block, which is a corruption rather than a
 * leak.
 */
void cfs_buf_mark_clean(struct cfs *fs, struct cfs_buf *b)
{
    if (b->dirty) {
        b->dirty = false;
        KASSERT(fs->nr_dirty > 0);
        fs->nr_dirty--;
    }
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    uint64_t blk;
    int rc = cfs_alloc_block(fs, &blk);
    if (rc)
        return rc;
    struct cfs_buf *b = cfs_buf_alloc(fs, blk);
    if (b == NULL) {
        cfs_free_block_deferred(fs, blk);
        return -ENOMEM;
    }
    memset(b->data, 0, CFS_BLOCK);
    cfs_mhdr_seal(fs, b->data, kind, blk);
    cfs_buf_mark_dirty(fs, b);
    *out = b;
    return 0;
}

/*
 * `exempt` says how the superseded block is given back: deferred, the
 * way every tree block is, or exempt -- freed for the new root with no
 * snapshot filter. Exempt is for a block no snapshot's tree can reach,
 * which is every block hanging off a superblock field a snapshot does
 * not copy: the free record, and from this unit the snapshot list and
 * its deadlists. Sending one of those through the filter would have the
 * snapshot hold it and append it to the deadlist it is a copy of
 * (docs/audit/next-subsystem-snap-deadlist.md, "The exemption, and the
 * rule it rests on").
 */
uint64_t cfs_res_take(struct cfs_res *r)
{
    return (r != NULL && r->used < r->n) ? r->blk[r->used++] : 0;
}

void cfs_res_untake(struct cfs_res *r)
{
    if (r != NULL && r->used > 0)
        r->used--;
}

static int buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, bool exempt, struct cfs_res *res)
{
    struct cfs_buf *b = *bp;
    struct cfs_mhdr *h = cfs_buf_hdr(b);
    if (h->generation == fs->gen) {
        cfs_buf_mark_dirty(fs, b);
        return 0;
    }
    uint64_t nblk;
    if (res != NULL) {
        /* Inside the commit's window, where allocating is the defect
         * this unit removes: the block comes from what was set aside
         * before the fixpoint. Nothing left means the bound was wrong,
         * which fails the commit rather than quietly allocating. */
        nblk = cfs_res_take(res);
        if (nblk == 0) {
            kerror("cosmofs: the commit's reservation is short; refusing to allocate after the bitmap");
            return -ENOSPC;
        }
    } else {
        /* The copy stays on the member the block was on where that
         * member has room. Otherwise a metadata tree drifts onto
         * whichever member is emptiest, and a member's own allocation
         * index could end up on a different device than the blocks it
         * describes. */
        unsigned v = CFS_DVA_VDEV(b->blkno);
        uint64_t hint = v < fs->nmembers ? CFS_DVA(v, fs->mem[v].first_usable) : 0;
        uint64_t got;
        int rc = cfs_alloc_run(fs, CFS_ALLOC_META, hint, 1, &nblk, &got);
        if (rc)
            return rc;
    }
    struct cfs_buf *nb = cfs_buf_alloc(fs, nblk);
    if (nb == NULL) {
        if (res != NULL)
            cfs_res_untake(res);
        else
            cfs_free_block_deferred(fs, nblk);
        return -ENOMEM;
    }
    memcpy(nb->data, b->data, CFS_BLOCK);
    cfs_mhdr_seal(fs, nb->data, h->kind, nblk);
    cfs_buf_mark_dirty(fs, nb);
    if (exempt)
        cfs_free_block_exempt(fs, b->blkno);
    else
        cfs_free_block_deferred(fs, b->blkno);
    cfs_buf_put(fs, b);
    *parent_slot = nblk;
    *bp = nb;
    return 0;
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    return buf_cow(fs, bp, parent_slot, false, NULL);
}

int cfs_buf_cow_exempt(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_res *res)
{
    return buf_cow(fs, bp, parent_slot, true, res);
}

/*
 * Give back a block this transaction allocated and nothing has named or
 * written: its bit is cleared now, not at the next commit, so the retry
 * a set-aside's failure invites finds the space it had (Codex on #433;
 * a deferred free would hold it allocated until a commit). The on-disk
 * bitmap never had it.
 */
static void alloc_undo(struct cfs *fs, uint64_t dva)
{
    uint64_t lin = cfs_dva_lin(fs, dva);
    if (lin == CFS_DVA_NONE || !cfs_bit_test(fs->bitmap, lin))
        return;
    cfs_bit_clear(fs->bitmap, lin);
    fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
    fs->free_blocks++;
    fs->mem[CFS_DVA_VDEV(dva)].free_blocks++;
}

/*
 * Metadata blocks set aside before a change that must not stop half way
 * (CF17; CFS-6): each block is allocated and its buffer made now, so the
 * change itself only takes them and cannot fail. What it does not take
 * goes back with cfs_prep_release. All or nothing: a failure here gives
 * back what it had set aside and changes nothing else.
 */
int cfs_prep_meta(struct cfs *fs, struct cfs_prep *p, unsigned n)
{
    p->n = p->used = 0;
    if (n > CFS_PREP_MAX)
        return -EFBIG;
    for (unsigned i = 0; i < n; i++) {
        uint64_t got;
        int rc = cfs_alloc_run(fs, CFS_ALLOC_META, 0, 1, &p->blk[i], &got);
        if (rc == 0) {
            p->buf[i] = buf_make(fs, p->blk[i]);
            if (p->buf[i] == NULL) {
                alloc_undo(fs, p->blk[i]);
                rc = -ENOMEM;
            }
        }
        if (rc) {
            p->n = i;
            cfs_prep_release(fs, p);
            return rc;
        }
    }
    p->n = n;
    return 0;
}

void cfs_prep_release(struct cfs *fs, struct cfs_prep *p)
{
    for (unsigned i = p->used; i < p->n; i++) {
        kfree(p->buf[i]->data);
        kfree(p->buf[i]);
        alloc_undo(fs, p->blk[i]);
    }
    p->n = p->used = 0;
}

/* Copy-on-write into a set-aside block: cfs_buf_cow that cannot fail. */
void cfs_buf_cow_prep(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_prep *p)
{
    struct cfs_buf *b = *bp;
    struct cfs_mhdr *h = cfs_buf_hdr(b);
    if (h->generation == fs->gen) {
        cfs_buf_mark_dirty(fs, b);
        return;
    }
    KASSERT(p->used < p->n);
    struct cfs_buf *nb = p->buf[p->used];
    uint64_t nblk = p->blk[p->used++];
    buf_insert(fs, nb);
    memcpy(nb->data, b->data, CFS_BLOCK);
    cfs_mhdr_seal(fs, nb->data, h->kind, nblk);
    cfs_buf_mark_dirty(fs, nb);
    cfs_free_block_deferred(fs, b->blkno);
    cfs_buf_put(fs, b);
    *parent_slot = nblk;
    *bp = nb;
}

/* A new block from the set-aside: cfs_buf_new that cannot fail. */
struct cfs_buf *cfs_buf_new_prep(struct cfs *fs, uint32_t kind, struct cfs_prep *p)
{
    KASSERT(p->used < p->n);
    struct cfs_buf *b = p->buf[p->used];
    uint64_t blk = p->blk[p->used++];
    buf_insert(fs, b);
    memset(b->data, 0, CFS_BLOCK);
    cfs_mhdr_seal(fs, b->data, kind, blk);
    cfs_buf_mark_dirty(fs, b);
    return b;
}

int cfs_buf_new_at(struct cfs *fs, uint32_t kind, uint64_t blk, struct cfs_buf **out)
{
    /* Through buf_alloc, not cfs_buf_get: a block just handed out may
     * have a stale buffer from its previous life further down the list.
     * The same reason cfs_buf_cow and freelog_fill do it this way. */
    struct cfs_buf *b = cfs_buf_alloc(fs, blk);
    if (b == NULL)
        return -ENOMEM;
    memset(b->data, 0, CFS_BLOCK);
    cfs_mhdr_seal(fs, b->data, kind, blk);
    cfs_buf_mark_dirty(fs, b);
    *out = b;
    return 0;
}
