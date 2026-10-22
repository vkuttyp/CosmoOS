/*
 * pool.c - Storage pool over one or more member block devices.
 *
 * A block is named by a DVA (member, block). A member is a mirror group
 * of one or more devices holding the same blocks at the same offsets,
 * so the address does not change when a group gains a copy. Member 0 at
 * block b is the same sector as the single-member pool always
 * addressed, so a filesystem written before members existed reads back
 * unchanged.
 *
 * Each copy has a state, and a copy that is not ONLINE has a record of
 * the regions it missed (docs/kernel-services/filesystem/cosmofs/design.md,
 * "Format version 13"). The pool keeps both in memory; the filesystem
 * writes them into its own format.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/storage.h>
#include <kernel/string.h>

uint32_t g_pool_fault_errors = 16;

static const char *const g_state_names[POOL_COPY_NSTATES] = {
    [POOL_COPY_ONLINE] = "ONLINE",
    [POOL_COPY_MISSING] = "MISSING",
    [POOL_COPY_OFFLINE] = "OFFLINE",
    [POOL_COPY_FAULTED] = "FAULTED",
    [POOL_COPY_RESILVERING] = "RESILVERING",
};

const char *pool_copy_state_name(enum pool_copy_state s)
{
    return (unsigned)s < POOL_COPY_NSTATES ? g_state_names[s] : "?";
}

/* The member and offset a DVA names, or NULL when it names neither. */
static struct spool_member *member_of(struct spool *p, uint64_t dva, uint64_t *blk)
{
    unsigned v = POOL_DVA_VDEV(dva);
    if (v >= p->nmembers)
        return NULL;
    uint64_t b = POOL_DVA_BLK(dva);
    if (b >= p->m[v].nblocks)
        return NULL;
    *blk = b;
    return &p->m[v];
}

static bool readable(const struct pool_copy *c)
{
    return c->dev != NULL && c->state == POOL_COPY_ONLINE;
}

static bool writable(const struct pool_copy *c)
{
    return c->dev != NULL && (c->state == POOL_COPY_ONLINE || c->state == POOL_COPY_RESILVERING);
}

/* A hole: a block this ONLINE copy failed to take while another copy
 * took it (its bit in the copy's bitmap). Not read from until rebuilt. */
static bool hole(const struct spool_member *m, const struct pool_copy *c, uint64_t blk)
{
    if (c->dirty == NULL)
        return false;
    uint64_t r = blk / m->region_blocks;
    return r < POOL_DIRTY_BITS && ((c->dirty[r / 8] >> (r % 8)) & 1u);
}

static bool readable_at(const struct spool_member *m, const struct pool_copy *c, uint64_t blk)
{
    return readable(c) && !hole(m, c, blk);
}

static bool any_bit(const struct pool_copy *c)
{
    if (c->dirty)
        for (unsigned i = 0; i < POOL_DIRTY_BYTES; i++)
            if (c->dirty[i])
                return true;
    return false;
}

static unsigned online_copies(const struct spool_member *m)
{
    unsigned n = 0;
    for (unsigned c = 0; c < m->ncopies; c++)
        n += readable(&m->copy[c]) ? 1 : 0;
    return n;
}

static void region_set(uint8_t *bits, const struct spool_member *m, uint64_t blk)
{
    uint64_t r = blk / m->region_blocks;
    if (r >= POOL_DIRTY_BITS)
        r = POOL_DIRTY_BITS - 1;   /* cannot happen: region_blocks covers nblocks */
    bits[r / 8] |= (uint8_t)(1u << (r % 8));
}

/* Blocks per dirty bit: POOL_DIRTY_BITS regions cover the member. */
static uint32_t region_blocks_for(uint64_t nblocks)
{
    uint64_t r = (nblocks + POOL_DIRTY_BITS - 1) / POOL_DIRTY_BITS;
    return r ? (uint32_t)r : 1u;
}

/* The bitmaps a member needs once it has a second slot: one per copy and
 * the since-the-last-root one. */
static int member_bitmaps(struct spool_member *m)
{
    if (m->recent == NULL) {
        m->recent = kzalloc(POOL_DIRTY_BYTES);
        if (m->recent == NULL)
            return -ENOMEM;
    }
    for (unsigned c = 0; c < m->ncopies; c++) {
        if (m->copy[c].dirty == NULL) {
            m->copy[c].dirty = kzalloc(POOL_DIRTY_BYTES);
            if (m->copy[c].dirty == NULL)
                return -ENOMEM;
        }
    }
    return 0;
}

static int attach(struct pool_copy *c, struct blkdev *bd)
{
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    blkdev_get(bd);
    c->dev = bd;
    c->sectors_per_block = POOL_BLOCK / bd->sector_size;
    return 0;
}

int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev)
{
    if (p->nmembers >= POOL_MAX_MEMBERS)
        return -ENOSPC;
    if (p->nmembers == p->cap) {
        unsigned cap = p->cap ? p->cap * 2 : 1;
        if (cap > POOL_MAX_MEMBERS)
            cap = POOL_MAX_MEMBERS;
        struct spool_member *m = kzalloc((size_t)cap * sizeof(*m));
        if (m == NULL)
            return -ENOMEM;
        if (p->m)
            memcpy(m, p->m, (size_t)p->nmembers * sizeof(*m));
        kfree(p->m);
        p->m = m;
        p->cap = cap;
    }
    unsigned v = p->nmembers;
    struct spool_member *m = &p->m[v];
    int rc = attach(&m->copy[0], bd);
    if (rc)
        return rc;
    m->ncopies = 1;
    m->nblocks = bd->capacity / m->copy[0].sectors_per_block;
    m->region_blocks = region_blocks_for(m->nblocks);
    p->nmembers = v + 1;
    if (v == 0)
        p->nblocks = p->m[0].nblocks;
    if (vdev)
        *vdev = v;
    return 0;
}

/* A copy must cover the group's blocks. A larger device is allowed and
 * its tail is unused: the group's size is decided by the member table,
 * not by whichever disk happens to be biggest. */
static int fits(const struct spool_member *m, struct blkdev *bd)
{
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    if (bd->capacity / (POOL_BLOCK / bd->sector_size) < m->nblocks)
        return -EINVAL;
    return 0;
}

int pool_add_copy(struct spool *p, unsigned vdev, struct blkdev *bd)
{
    if (vdev >= p->nmembers)
        return -EINVAL;
    struct spool_member *m = &p->m[vdev];
    if (m->ncopies >= POOL_MAX_COPIES)
        return -ENOSPC;
    int rc = fits(m, bd);
    if (rc)
        return rc;
    m->ncopies++;
    rc = member_bitmaps(m);
    if (rc == 0)
        rc = attach(&m->copy[m->ncopies - 1], bd);
    if (rc) {
        m->ncopies--;
        return rc;
    }
    m->copy[m->ncopies - 1].state = POOL_COPY_ONLINE;
    return 0;
}

int pool_set_copies(struct spool *p, unsigned vdev, unsigned n)
{
    if (vdev >= p->nmembers || n == 0 || n > POOL_MAX_COPIES)
        return -EINVAL;
    struct spool_member *m = &p->m[vdev];
    if (n < m->ncopies)
        return -EINVAL;   /* slots are only added here; a detach is its own operation */
    unsigned was = m->ncopies;
    m->ncopies = n;
    if (n > 1) {
        int rc = member_bitmaps(m);
        if (rc) {
            m->ncopies = was;
            return rc;
        }
    }
    for (unsigned c = was; c < n; c++) {
        struct pool_copy *cp = &m->copy[c];
        cp->state = POOL_COPY_MISSING;
        cp->whole = true;   /* nobody knows what it missed */
        cp->since = 0;
    }
    if (n > was)
        p->state_seq++;
    return 0;
}

int pool_attach_copy(struct spool *p, unsigned vdev, unsigned c, struct blkdev *bd, enum pool_copy_state state)
{
    if (vdev >= p->nmembers || c >= p->m[vdev].ncopies || (unsigned)state >= POOL_COPY_NSTATES)
        return -EINVAL;
    struct spool_member *m = &p->m[vdev];
    struct pool_copy *cp = &m->copy[c];
    if (cp->dev != NULL)
        return -EBUSY;
    int rc = fits(m, bd);
    if (rc == 0)
        rc = attach(cp, bd);
    if (rc)
        return rc;
    if (cp->state != state)
        p->state_seq++;
    cp->state = state;
    return 0;
}

void pool_swap_copies(struct spool *p, unsigned vdev, unsigned a, unsigned b)
{
    if (vdev >= p->nmembers || a == b || a >= p->m[vdev].ncopies || b >= p->m[vdev].ncopies)
        return;
    struct pool_copy t = p->m[vdev].copy[a];
    p->m[vdev].copy[a] = p->m[vdev].copy[b];
    p->m[vdev].copy[b] = t;
}

unsigned pool_copies(const struct spool *p, uint64_t dva)
{
    unsigned v = POOL_DVA_VDEV(dva);
    return v < p->nmembers ? p->m[v].ncopies : 0;
}

bool pool_copy_readable(const struct spool *p, uint64_t dva, unsigned c)
{
    unsigned v = POOL_DVA_VDEV(dva);
    uint64_t blk = POOL_DVA_BLK(dva);
    return v < p->nmembers && c < p->m[v].ncopies && blk < p->m[v].nblocks &&
           readable_at(&p->m[v], &p->m[v].copy[c], blk);
}

bool pool_copy_online(const struct spool *p, unsigned vdev, unsigned c)
{
    return vdev < p->nmembers && c < p->m[vdev].ncopies && readable(&p->m[vdev].copy[c]);
}

/* A copy out, or an ONLINE one with holes: either way the member is not
 * whole, and its record is kept (design.md, "Degraded operation"). */
bool pool_member_degraded(const struct spool *p, unsigned vdev)
{
    if (vdev >= p->nmembers)
        return false;
    const struct spool_member *m = &p->m[vdev];
    for (unsigned c = 0; c < m->ncopies; c++)
        if (!readable(&m->copy[c]) || (m->ncopies > 1 && any_bit(&m->copy[c])))
            return true;
    return false;
}

void pool_set_member_blocks(struct spool *p, unsigned vdev, uint64_t nblocks)
{
    if (vdev >= p->nmembers || nblocks == 0 || nblocks > p->m[vdev].nblocks)
        return;
    p->m[vdev].nblocks = nblocks;
    p->m[vdev].region_blocks = region_blocks_for(nblocks);
    if (vdev == 0)
        p->nblocks = nblocks;
}

bool pool_degraded(const struct spool *p)
{
    for (unsigned v = 0; v < p->nmembers; v++)
        if (pool_member_degraded(p, v))
            return true;
    return false;
}

int pool_open(struct blkdev *bd, struct spool **out)
{
    struct spool *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return -ENOMEM;
    p->block_size = POOL_BLOCK;
    int rc = pool_add_member(p, bd, NULL);
    if (rc) {
        kfree(p->m);
        kfree(p);
        return rc;
    }
    *out = p;
    return 0;
}

void pool_close(struct spool *p)
{
    for (unsigned v = 0; v < p->nmembers; v++) {
        struct spool_member *m = &p->m[v];
        for (unsigned c = 0; c < m->ncopies; c++) {
            if (m->copy[c].dev)
                blkdev_put(m->copy[c].dev);
            kfree(m->copy[c].dirty);
        }
        kfree(m->recent);
    }
    kfree(p->m);
    kfree(p);
}

static const char *dev_name_of(const struct pool_copy *c)
{
    return c->dev ? c->dev->name : "no device";
}

int pool_copy_set_state(struct spool *p, unsigned vdev, unsigned c, enum pool_copy_state state, int why)
{
    if (vdev >= p->nmembers || c >= p->m[vdev].ncopies || (unsigned)state >= POOL_COPY_NSTATES)
        return -EINVAL;
    struct spool_member *m = &p->m[vdev];
    struct pool_copy *cp = &m->copy[c];
    if (cp->state == state)
        return 0;
    if (state == POOL_COPY_OFFLINE && readable(cp) && online_copies(m) == 1)
        return -EBUSY;   /* the operator may not take the last copy away */
    if (state == POOL_COPY_ONLINE) {
        /* Only a copy that missed nothing: anything else is a resilver. */
        if (cp->dev == NULL)
            return -ENODEV;
        if (cp->whole || cp->since != p->epoch)
            return -EAGAIN;
        if (cp->dirty)
            for (unsigned i = 0; i < POOL_DIRTY_BYTES; i++)
                if (cp->dirty[i])
                    return -EAGAIN;
    }
    enum pool_copy_state was = cp->state;
    if (was == POOL_COPY_ONLINE) {
        /* From here it misses writes. Whatever reached it since the last
         * root may be only in its cache, so that is added to the holes it
         * already had. */
        cp->since = p->epoch;
        cp->whole = false;
        if (cp->dirty && m->recent)
            for (unsigned i = 0; i < POOL_DIRTY_BYTES; i++)
                cp->dirty[i] |= m->recent[i];
    }
    if (state == POOL_COPY_ONLINE && cp->dirty)
        memset(cp->dirty, 0, POOL_DIRTY_BYTES);
    cp->state = state;
    p->state_seq++;
    if (p->notify)
        p->notify(p->notify_arg);
    const char *deg = pool_member_degraded(p, vdev) ? "degraded" : "whole";
    const char *pn = p->name ? p->name : "", *sep = p->name ? ": " : "";
    if (why)
        kwarn("pool: %s%smember %u copy %u (%s) %s (%d): the member is %s", pn, sep, vdev, c, dev_name_of(cp),
              pool_copy_state_name(state), why, deg);
    else
        kwarn("pool: %s%smember %u copy %u (%s) %s, was %s: the member is %s", pn, sep, vdev, c, dev_name_of(cp),
              pool_copy_state_name(state), pool_copy_state_name(was), deg);
    return 0;
}

/* One failure on copy `c`: -ENODEV takes it out at once, anything else
 * counts towards FAULTED -- but never the member's last ONLINE copy,
 * which is still the best there is. */
static void copy_failed(struct spool *p, unsigned vdev, unsigned c, int rc)
{
    struct spool_member *m = &p->m[vdev];
    struct pool_copy *cp = &m->copy[c];
    if (rc == -ENODEV) {
        (void)pool_copy_set_state(p, vdev, c, POOL_COPY_MISSING, rc);
        return;
    }
    uint32_t limit = __atomic_load_n(&g_pool_fault_errors, __ATOMIC_RELAXED);
    uint32_t total = cp->read_errors + cp->write_errors;
    if (limit == 0 || total <= limit || cp->state == POOL_COPY_FAULTED)
        return;
    if (cp->state == POOL_COPY_ONLINE && online_copies(m) == 1)
        return;
    (void)pool_copy_set_state(p, vdev, c, POOL_COPY_FAULTED, rc);
}

/* Counted, and not towards FAULTED: a copy that answers with the wrong
 * bytes is one read repair and the scrub put right, and a faulted copy
 * is one they could no longer write to. */
void pool_copy_csum_error(struct spool *p, uint64_t dva, unsigned c)
{
    unsigned v = POOL_DVA_VDEV(dva);
    if (v >= p->nmembers || c >= p->m[v].ncopies)
        return;
    p->m[v].copy[c].csum_errors++;
}

void pool_mark_missed(struct spool *p, uint64_t dva)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return;
    for (unsigned c = 0; c < m->ncopies; c++)
        if (!writable(&m->copy[c]) && m->copy[c].dirty)
            region_set(m->copy[c].dirty, m, blk);
}

bool pool_copy_missed(const struct spool *p, uint64_t dva, unsigned c)
{
    unsigned v = POOL_DVA_VDEV(dva);
    uint64_t blk = POOL_DVA_BLK(dva);
    if (v >= p->nmembers || c >= p->m[v].ncopies || blk >= p->m[v].nblocks)
        return false;
    const struct spool_member *m = &p->m[v];
    const struct pool_copy *cp = &m->copy[c];
    if (cp->whole)
        return true;
    if (cp->dirty == NULL)
        return false;
    uint64_t r = blk / m->region_blocks;
    return (cp->dirty[r / 8] >> (r % 8)) & 1u;
}

void pool_commit_done(struct spool *p, uint64_t gen)
{
    p->epoch = gen;
    for (unsigned v = 0; v < p->nmembers; v++)
        if (p->m[v].recent)
            memset(p->m[v].recent, 0, POOL_DIRTY_BYTES);
}

static int read_one(struct spool *p, unsigned v, uint64_t blk, unsigned c, void *buf)
{
    struct pool_copy *cp = &p->m[v].copy[c];
    p->reads++;
    int rc = blk_read(cp->dev, blk * cp->sectors_per_block, cp->sectors_per_block, buf);
    if (rc) {
        cp->read_errors++;
        copy_failed(p, v, c, rc);
    }
    /* A device that went away during the I/O is the copy's state now
     * (MISSING); to the caller it is an I/O error like any other (CF26):
     * -ENODEV from the pool means "that copy may not be read". */
    return rc == -ENODEV ? -EIO : rc;
}

int pool_read_copy(struct spool *p, uint64_t dva, unsigned copy, void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL || copy >= m->ncopies)
        return -EINVAL;
    if (!readable_at(m, &m->copy[copy], blk))
        return -ENODEV;
    return read_one(p, POOL_DVA_VDEV(dva), blk, copy, buf);
}

/* A write that did not reach copy `c`: its region is one the copy missed.
 * An ONLINE copy's are holes, set by the writer only when another copy
 * took the block (pool_write_flags): one leaving ONLINE adds the regions
 * written since the last root, which include this one. */
static void missed(struct spool_member *m, unsigned c, uint64_t blk)
{
    if (m->copy[c].dirty && m->copy[c].state != POOL_COPY_ONLINE)
        region_set(m->copy[c].dirty, m, blk);
}

static int write_one(struct spool *p, unsigned v, uint64_t blk, unsigned c, const void *buf, unsigned flags)
{
    struct spool_member *m = &p->m[v];
    struct pool_copy *cp = &m->copy[c];
    p->writes++;
    if (flags & BIO_PREFLUSH)
        p->flushes++;
    if (flags & BIO_FUA)
        p->flushes++;
    int rc = flags ? blk_write_flags(cp->dev, blk * cp->sectors_per_block, cp->sectors_per_block, buf, flags)
                   : blk_write(cp->dev, blk * cp->sectors_per_block, cp->sectors_per_block, buf);
    if (rc) {
        cp->write_errors++;
        missed(m, c, blk);
        copy_failed(p, v, c, rc);
    }
    return rc == -ENODEV ? -EIO : rc;   /* as read_one */
}

int pool_write_copy(struct spool *p, uint64_t dva, unsigned copy, const void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL || copy >= m->ncopies)
        return -EINVAL;
    if (!writable(&m->copy[copy]))
        return -ENODEV;
    bool was_online = m->copy[copy].state == POOL_COPY_ONLINE;
    int rc = write_one(p, POOL_DVA_VDEV(dva), blk, copy, buf, 0);
    /* A repair that did not take: the copy still lacks the block, and
     * another copy has it (it is what the repair was from). */
    if (rc && was_online && m->copy[copy].state == POOL_COPY_ONLINE && online_copies(m) > 1 &&
        m->copy[copy].dirty)
        region_set(m->copy[copy].dirty, m, blk);
    return rc;
}

/* The first ONLINE copy that answers. */
int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    int first = 0;
    for (unsigned c = 0; c < m->ncopies; c++) {
        if (!readable_at(m, &m->copy[c], blk))
            continue;
        int rc = read_one(p, POOL_DVA_VDEV(dva), blk, c, buf);
        if (rc == 0)
            return 0;
        if (first == 0)
            first = rc;
    }
    return first ? first : -EIO;
}

/*
 * Every writable copy, and every one is attempted even after a failure:
 * a device that refuses one write is not a reason to leave the others
 * holding an older block. A copy that does not take it -- not writable,
 * or failed -- has the block's region recorded as missed. The write
 * succeeds when an ONLINE copy took it (design.md, "Degraded
 * operation"); otherwise the caller is inside a transaction that has
 * not published a root, so a write that reached no good copy is never
 * something a later mount can find.
 */
int pool_write_flags(struct spool *p, uint64_t dva, const void *buf, unsigned flags)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    unsigned v = POOL_DVA_VDEV(dva);
    if (m->recent)
        region_set(m->recent, m, blk);
    int first = 0;
    bool landed = false;
    bool failed_online[POOL_MAX_COPIES] = { false };
    for (unsigned c = 0; c < m->ncopies; c++) {
        if (!writable(&m->copy[c])) {
            missed(m, c, blk);
            continue;
        }
        bool was_online = m->copy[c].state == POOL_COPY_ONLINE;
        int rc = write_one(p, v, blk, c, buf, flags);
        if (rc == 0 && was_online)
            landed = true;
        else if (rc && was_online)
            failed_online[c] = true;
        if (rc && first == 0)
            first = rc;
    }
    if (!landed)
        return first ? first : -EIO;
    /* An ONLINE copy that failed it, below its fault count: still ONLINE,
     * and holding the old block. That is a hole, recorded and not read
     * from until it is rebuilt -- forgetting it would let the copy serve
     * the stale block, or come back from a later fault as if it held it.
     * Only when another copy took the write: with none, the write failed
     * and nothing will name the block. */
    for (unsigned c = 0; c < m->ncopies; c++)
        if (failed_online[c] && m->copy[c].state == POOL_COPY_ONLINE && m->copy[c].dirty)
            region_set(m->copy[c].dirty, m, blk);
    return 0;
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    return pool_write_flags(p, dva, buf, 0);
}

/*
 * Every writable copy of every member. A copy whose flush fails has
 * missed everything since the last root -- which is what its bitmap
 * starts from when it leaves ONLINE -- so it is taken out: MISSING for
 * -ENODEV, FAULTED otherwise, except a member's last ONLINE copy. The
 * flush succeeds for a member when an ONLINE copy flushed: a commit
 * durable on one device of a member is durable, and on none is not.
 */
int pool_flush(struct spool *p)
{
    int first = 0;
    for (unsigned v = 0; v < p->nmembers; v++) {
        struct spool_member *m = &p->m[v];
        int mfirst = 0;
        bool flushed = false;   /* an ONLINE copy is durable */
        for (unsigned c = 0; c < m->ncopies; c++) {
            struct pool_copy *cp = &m->copy[c];
            if (!writable(cp))
                continue;
            bool was_online = cp->state == POOL_COPY_ONLINE;
            p->flushes++;
            int rc = blk_flush(cp->dev);
            if (rc == 0) {
                flushed = flushed || was_online;
                continue;
            }
            cp->write_errors++;
            if (mfirst == 0)
                mfirst = rc;
            if (rc == -ENODEV)
                (void)pool_copy_set_state(p, v, c, POOL_COPY_MISSING, rc);
            else if (!(cp->state == POOL_COPY_ONLINE && online_copies(m) == 1))
                (void)pool_copy_set_state(p, v, c, POOL_COPY_FAULTED, rc);
        }
        /* Durable on one ONLINE copy is durable; on none -- even the last
         * copy left ONLINE after its flush failed -- it is not. */
        if (!flushed && first == 0)
            first = mfirst ? (mfirst == -ENODEV ? -EIO : mfirst) : -EIO;
    }
    return first;
}
