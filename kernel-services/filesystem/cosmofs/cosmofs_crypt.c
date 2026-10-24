/*
 * cosmofs_crypt.c - Keys, and what a block's nonce is made of
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 7").
 *
 * Three keys, each with one job. The user's key never encrypts a file:
 * it derives a wrapping key, which encrypts the master key, which
 * derives a key per file. That is what makes rotation cheap -- rewrap
 * one block -- and what keeps a per-block nonce small enough to fit
 * beside the tag.
 */

#include <kernel/blake2b.h>
#include <kernel/blk.h>
#include <kernel/chacha20.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/spinlock.h>
#include <kernel/crypto.h>
#include <kernel/string.h>

#include <uapi/cosmo/fsctl.h>

#include "cosmofs_internal.h"

/* The wrapping key: SHA-512 over the salt and the user's key, truncated.
 *
 * Deliberately not a password KDF -- there is no work factor here, and
 * none is claimed. What arrives through the unlock channel is a key, not
 * a passphrase; stretching a passphrase into one is the job of whatever
 * feeds that channel, and this says so rather than pretending 512 bits
 * of SHA make a weak secret strong. */
static void wrapping_key(const uint8_t salt[16], const void *user_key, size_t len, uint8_t out[CHACHA20_KEY_SIZE])
{
    struct sha512_ctx ctx;
    uint8_t digest[SHA512_DIGEST_SIZE];
    sha512_init(&ctx);
    sha512_update(&ctx, salt, 16);
    sha512_update(&ctx, user_key, len);
    sha512_final(&ctx, digest);
    memcpy(out, digest, CHACHA20_KEY_SIZE);
    memset(digest, 0, sizeof(digest));
    memset(&ctx, 0, sizeof(ctx));
}

/* The nonce that wraps the master key: fixed, because the wrapping key
 * is itself derived from a salt that is fresh for every wrap. */
static void wrap_nonce(uint8_t nonce[CHACHA20_NONCE_SIZE])
{
    memset(nonce, 0, CHACHA20_NONCE_SIZE);
    nonce[0] = 'k';
    nonce[1] = 'e';
    nonce[2] = 'y';
}

/*
 * A file's key. The master key is the ChaCha20 key and the inode number
 * is the nonce, so every file gets an independent stream from one
 * secret, and a block's nonce then only has to be unique within its own
 * file.
 */
void cfs_file_key(const struct cfs *fs, uint64_t ino, uint8_t out[CHACHA20_KEY_SIZE])
{
    uint8_t nonce[CHACHA20_NONCE_SIZE];
    memset(nonce, 0, sizeof(nonce));
    for (unsigned i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(ino >> (8 * i));
    uint8_t zeros[CHACHA20_KEY_SIZE];
    memset(zeros, 0, sizeof(zeros));
    chacha20_xor(fs->master_key, 0, nonce, zeros, out, CHACHA20_KEY_SIZE);
    memset(zeros, 0, sizeof(zeros));
}

/*
 * A block's nonce is drawn at random when the block is written, and
 * stored beside its tag.
 *
 * It was derived from the block number and the generation that wrote it,
 * on the argument that copy-on-write writes a block once per generation.
 * That argument is wrong, and Greptile found where: the older-root
 * fallback at mount resumes from a *previous* superblock, so a
 * generation is used again while the ciphertext the first attempt wrote
 * is still on the disk. A second write of the same block in the reused
 * generation would then repeat a (key, nonce) pair -- and for a stream
 * cipher that hands an attacker the xor of the two plaintexts.
 *
 * Deriving it from something else the filesystem controls only moves the
 * question. Ninety-six random bits per block do not need the filesystem
 * to promise anything: with a key per file, a file would have to reach
 * some 2^48 blocks before a repeat is worth thinking about, and it can
 * hold 2^32.
 */
void cfs_block_nonce(uint8_t nonce[CHACHA20_NONCE_SIZE])
{
    random_get_bytes(nonce, CHACHA20_NONCE_SIZE);
}

/*
 * Key material, salts and block nonces come from a seeded pool only
 * (docs/kernel/security/design.md §6, invariant S18). Wait a bounded
 * time for seeding; refuse with -EAGAIN, before anything is written,
 * when the pool is still unseeded. Loading a key passes through here
 * too: a loaded key is what lets cfs_block_nonce draw.
 */
int cfs_need_seeded(const char *what)
{
    int rc = random_wait_ready(RANDOM_KEYGEN_WAIT_NS);
    if (rc == 0)
        return 0;
    kwarn("cosmofs: cannot %s: the entropy pool is not seeded (waited %llu s)", what,
          (unsigned long long)(RANDOM_KEYGEN_WAIT_NS / 1000000000ull));
    return rc == -EINTR ? -EINTR : -EAGAIN;
}

/* --- the key block -------------------------------------------------------- */

int cfs_keys_write(struct spool *pool, uint64_t dva, uint64_t generation, const uint8_t master[CHACHA20_KEY_SIZE],
                   const void *user_key, size_t user_len)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_keys *k = (struct cfs_keys *)(block + CFS_MHDR_SIZE);
    k->version = 1;
    k->kdf = CFS_KDF_SHA512;
    random_get_bytes(k->salt, sizeof(k->salt));
    memcpy(k->wrapped, master, CHACHA20_KEY_SIZE);

    uint8_t wk[CHACHA20_KEY_SIZE], nonce[CHACHA20_NONCE_SIZE];
    wrapping_key(k->salt, user_key, user_len, wk);
    wrap_nonce(nonce);
    chacha20_seal(wk, nonce, NULL, 0, k->wrapped, sizeof(k->wrapped), k->tag);
    memset(wk, 0, sizeof(wk));

    cfs_mhdr_seal_raw(NULL, block, CFS_KIND_KEYS, dva, generation);
    int rc = pool_write(pool, dva, block);
    memset(block, 0, CFS_BLOCK);
    kfree(block);
    return rc;
}

/*
 * Unwrap the master key with the user's. -EKEYREJECTED when the tag does
 * not match, which is the whole reason there is a tag: a wrong key must
 * be told so, not handed a filesystem of noise.
 */
int cfs_keys_unwrap(const struct cfs_keys *k, const void *user_key, size_t user_len, uint8_t master[CHACHA20_KEY_SIZE])
{
    if (k->version != 1 || k->kdf != CFS_KDF_SHA512)
        return -EINVAL;
    uint8_t wk[CHACHA20_KEY_SIZE], nonce[CHACHA20_NONCE_SIZE];
    wrapping_key(k->salt, user_key, user_len, wk);
    wrap_nonce(nonce);
    uint8_t buf[CHACHA20_KEY_SIZE];
    memcpy(buf, k->wrapped, sizeof(buf));
    bool ok = chacha20_open(wk, nonce, NULL, 0, buf, sizeof(buf), k->tag);
    memset(wk, 0, sizeof(wk));
    if (!ok) {
        memset(buf, 0, sizeof(buf));
        return -EKEYREJECTED;
    }
    memcpy(master, buf, CHACHA20_KEY_SIZE);
    memset(buf, 0, sizeof(buf));
    return 0;
}

/* Read the key block and unwrap it. */
int cfs_keys_load(struct cfs *fs, const void *user_key, size_t user_len)
{
    int rc = cfs_need_seeded("load a key");
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    rc = pool_read(fs->pool, fs->sb.key_root, block);
    if (rc == 0 && !cfs_mhdr_ok(&fs->mac, block, fs->sb.key_root, CFS_KIND_KEYS))
        rc = -EIO;
    if (rc == 0)
        rc = cfs_keys_unwrap((const struct cfs_keys *)(block + CFS_MHDR_SIZE), user_key, user_len, fs->master_key);
    if (rc == 0)
        fs->have_key = true;
    memset(block, 0, CFS_BLOCK);
    kfree(block);
    return rc;
}

/*
 * Rotation: a new salt for the same master key. No file is rewritten,
 * because the user's key never encrypted one -- which is the point of
 * having a master key at all.
 */
int cfs_keys_rotate(struct cfs *fs, const void *new_key, size_t new_len)
{
    if (!fs->have_key)
        return -ENOKEY;
    struct cfs_buf *b;
    int rc = cfs_buf_get(fs, fs->sb.key_root, CFS_KIND_KEYS, &b);
    if (rc)
        return rc;
    rc = cfs_buf_cow(fs, &b, &fs->sb.key_root);
    if (rc) {
        cfs_buf_put(fs, b);
        return rc;
    }
    struct cfs_keys *k = (struct cfs_keys *)(b->data + CFS_MHDR_SIZE);
    k->version = 1;
    k->kdf = CFS_KDF_SHA512;
    random_get_bytes(k->salt, sizeof(k->salt));
    memcpy(k->wrapped, fs->master_key, CHACHA20_KEY_SIZE);
    uint8_t wk[CHACHA20_KEY_SIZE], nonce[CHACHA20_NONCE_SIZE];
    wrapping_key(k->salt, new_key, new_len, wk);
    wrap_nonce(nonce);
    chacha20_seal(wk, nonce, NULL, 0, k->wrapped, sizeof(k->wrapped), k->tag);
    memset(wk, 0, sizeof(wk));
    cfs_buf_mark_dirty(fs, b);
    cfs_buf_put(fs, b);
    return 0;
}

/* --- version 14: the MAC key, the key block's slots, the key table -------- */

/* The metadata MAC's key, from the master: BLAKE2b keyed by the master
 * over a fixed string, so it is independent of every file key. */
void cfs_mac_derive(const uint8_t master[CHACHA20_KEY_SIZE], struct cfs_mac *out)
{
    static const char what[] = "cosmofs metadata MAC v1";
    struct blake2b s;
    blake2b_init_key(&s, sizeof(out->key), master, CHACHA20_KEY_SIZE);
    blake2b_update(&s, what, sizeof(what) - 1);
    blake2b_final(&s, out->key);
    out->on = true;
}

/* A slot as user space sealed it is taken as it is -- the kernel cannot
 * open one, which needs the passphrase -- but its shape is checked. */
bool cfs_keyslot_valid(unsigned index, const struct cfs_keyslot *s)
{
    switch (s->kind) {
    case CFS_KEYSLOT_EMPTY:
        return true;
    case CFS_KEYSLOT_PASSPHRASE:
        return s->t_cost >= 1 && s->lanes >= 1 && s->m_cost_kib >= 8 * s->lanes;
    case CFS_KEYSLOT_KEYFILE:
        return index == 0;
    default:
        return false;
    }
}

/* Write a version-2 key block: the pool's uuid and the slots. */
int cfs_keys2_write(struct spool *pool, const struct cfs_mac *mac, uint64_t dva, uint64_t generation,
                    const uint8_t uuid[16], const struct cfs_keyslot slots[CFS_KEY_SLOTS])
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_keys2 *k = (struct cfs_keys2 *)(block + CFS_MHDR_SIZE);
    k->version = 2;
    k->nslots = CFS_KEY_SLOTS;
    memcpy(k->uuid, uuid, 16);
    memcpy(k->slot, slots, sizeof(k->slot));
    cfs_mhdr_seal_raw(mac, block, CFS_KIND_KEYS, dva, generation);
    int rc = pool_write(pool, dva, block);
    kfree(block);
    return rc;
}

/*
 * The key table (design.md, "The key handle"): the pools whose master
 * key user space has handed over, by uuid. A mount copies its entry and
 * holds it; the last holder's unmount wipes it. A maintenance mount (the
 * check on an unmounted device) reads it and holds nothing, so the check
 * at boot does not drop the key the root's mount is about to need.
 */
#define CFS_KEYTAB 8u

static struct {
    bool used;
    unsigned holders;          /* live mounts of the pool */
    bool mounted;              /* a mount took it: its last unmount drops it */
    uint8_t uuid[16];
    uint8_t master[CHACHA20_KEY_SIZE];
    struct cfs_mac mac;
} g_keytab[CFS_KEYTAB];
static spinlock_t g_keytab_lock = SPINLOCK_INIT("cosmofs-keys");

static int keytab_find(const uint8_t uuid[16])
{
    static const uint8_t zero[16] = { 0 };
    if (memcmp(uuid, zero, 16) == 0)
        return -1;
    for (unsigned i = 0; i < CFS_KEYTAB; i++)
        if (g_keytab[i].used && memcmp(g_keytab[i].uuid, uuid, 16) == 0)
            return (int)i;
    return -1;
}

static void keytab_wipe(unsigned i)
{
    memset(&g_keytab[i], 0, sizeof(g_keytab[i]));
}

int cfs_keytab_mac(const uint8_t uuid[16], struct cfs_mac *mac)
{
    spin_lock(&g_keytab_lock);
    int i = keytab_find(uuid);
    if (i >= 0)
        *mac = g_keytab[i].mac;
    spin_unlock(&g_keytab_lock);
    return i >= 0 ? 0 : -ENOKEY;
}

/* The master key for a mount, and the mount's hold on the entry. */
int cfs_keytab_take(const uint8_t uuid[16], bool hold, uint8_t master[CHACHA20_KEY_SIZE])
{
    spin_lock(&g_keytab_lock);
    int i = keytab_find(uuid);
    if (i >= 0) {
        memcpy(master, g_keytab[i].master, CHACHA20_KEY_SIZE);
        if (hold) {
            g_keytab[i].holders++;
            g_keytab[i].mounted = true;
        }
    }
    spin_unlock(&g_keytab_lock);
    return i >= 0 ? 0 : -ENOKEY;
}

void cfs_keytab_release(const uint8_t uuid[16])
{
    spin_lock(&g_keytab_lock);
    int i = keytab_find(uuid);
    if (i >= 0 && g_keytab[i].holders > 0 && --g_keytab[i].holders == 0 && g_keytab[i].mounted)
        keytab_wipe((unsigned)i);
    spin_unlock(&g_keytab_lock);
}

/* Keep `master` for pool `uuid`. The caller has checked it. */
int cfs_keytab_install(const uint8_t uuid[16], const uint8_t master[CHACHA20_KEY_SIZE])
{
    spin_lock(&g_keytab_lock);
    int i = keytab_find(uuid);
    int rc = 0;
    if (i < 0) {
        for (unsigned j = 0; j < CFS_KEYTAB && i < 0; j++)
            if (!g_keytab[j].used)
                i = (int)j;
        if (i < 0) {
            rc = -ENOSPC;
        } else {
            g_keytab[i].used = true;
            memcpy(g_keytab[i].uuid, uuid, 16);
            memcpy(g_keytab[i].master, master, CHACHA20_KEY_SIZE);
            cfs_mac_derive(master, &g_keytab[i].mac);
        }
    } else if (memcmp(g_keytab[i].master, master, CHACHA20_KEY_SIZE) != 0) {
        rc = -EKEYREJECTED;   /* a different key for a pool already unlocked: impossible for a real one */
    }
    spin_unlock(&g_keytab_lock);
    return rc;
}

/* Drop the entry for `uuid` if no mount holds it. */
int cfs_keytab_unload(const uint8_t uuid[16])
{
    spin_lock(&g_keytab_lock);
    int i = keytab_find(uuid);
    int rc = i < 0 ? -ENOKEY : g_keytab[i].holders ? -EBUSY : 0;
    if (rc == 0)
        keytab_wipe((unsigned)i);
    spin_unlock(&g_keytab_lock);
    return rc;
}

bool cfs_keytab_has(const uint8_t uuid[16])
{
    spin_lock(&g_keytab_lock);
    bool has = keytab_find(uuid) >= 0;
    spin_unlock(&g_keytab_lock);
    return has;
}

/* --- the key operations (/dev/fsctl KEY_*; design.md, "The key handle") --- */

_Static_assert(sizeof(struct cosmo_keyslot) == sizeof(struct cfs_keyslot), "a uapi slot is an on-disk slot");
_Static_assert(offsetof(struct cosmo_keyslot, wrapped) == offsetof(struct cfs_keyslot, wrapped), "slot layout");
_Static_assert(offsetof(struct cosmo_keyslot, created_ns) == offsetof(struct cfs_keyslot, created_ns), "slot layout");
_Static_assert(COSMO_KEY_SLOTS == CFS_KEY_SLOTS && COSMO_KEY_SIZE == CHACHA20_KEY_SIZE, "slot count, key size");

static bool key_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < CHACHA20_KEY_SIZE; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void info_from_super(const struct cfs_super *sb, struct cosmo_fsctl_keyinfo *info)
{
    memset(info, 0, sizeof(*info));
    info->format = sb->version;
    memcpy(info->uuid, sb->uuid, 16);
    memcpy(info->label, sb->label, sizeof(info->label));
    info->generation = sb->generation;
    if (sb->version >= 7 && sb->key_root != 0)
        info->flags |= COSMO_FSCTL_KEY_F_ENCRYPTED;
    if (cfs_super_is_mac(sb))
        info->flags |= COSMO_FSCTL_KEY_F_MAC;
    if (cfs_keytab_has(sb->uuid))
        info->flags |= COSMO_FSCTL_KEY_F_UNLOCKED;
}

static void info_slots(const struct cfs_keys2 *k, struct cosmo_fsctl_keyinfo *info)
{
    info->nslots = CFS_KEY_SLOTS;
    memcpy(info->slot, k->slot, sizeof(info->slot));
}

/*
 * The device half: the pool is not mounted (or is, through another
 * device; the key table does not care), and is named by `bd`. Its key
 * block is read from member 0, which is where every pool this kernel makes
 * keeps it: a pool whose key block moved to another member says -EXDEV.
 */
static int keyctl_dev(struct blkdev *bd, struct cosmofs_key_req *req)
{
    struct spool *p;
    int rc = pool_open(bd, &p);
    if (rc)
        return rc;
    struct cfs_super sb;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    rc = block == NULL ? -ENOMEM : cfs_super_peek(p, &sb);
    if (rc == -ENOENT)
        rc = -EINVAL;   /* not a cosmofs at all */
    bool mac = rc == 0 && cfs_super_is_mac(&sb);
    if (rc == 0 && req->op == COSMOFS_KEY_INFO) {
        info_from_super(&sb, req->info);
        if (mac && CFS_DVA_VDEV(sb.key_root) != 0)
            rc = -EXDEV;
        else if (mac && (rc = pool_read(p, sb.key_root, block)) == 0) {
            const struct cfs_mhdr *h = (const struct cfs_mhdr *)block;
            const struct cfs_keys2 *k = (const struct cfs_keys2 *)(block + CFS_MHDR_SIZE);
            /* Unverified, as the superblock was: a slot authenticates
             * itself when it is opened, against this uuid. */
            if (h->magic != CFS_MHDR_MAGIC || h->kind != CFS_KIND_KEYS || h->blkno != sb.key_root ||
                k->version != 2 || memcmp(k->uuid, sb.uuid, 16) != 0)
                rc = -EIO;
            else
                info_slots(k, req->info);
        }
    } else if (rc == 0 && req->op == COSMOFS_KEY_LOAD) {
        rc = mac ? 0 : -EINVAL;   /* only a version-14 pool takes its key this way */
        struct cfs_mac m;
        cfs_mac_derive(req->key, &m);
        /* The key must open the pool before it is kept: a superblock slot
         * of this uuid that verifies under it, and the key block it names. */
        bool super_ok = false;
        uint64_t key_root = 0;
        for (unsigned slot = 0; slot < 2 && rc == 0; slot++) {
            if (pool_read(p, slot, block) != 0)
                continue;
            const struct cfs_super *s = (const struct cfs_super *)block;
            if (memcmp(s->magic, CFS_MAGIC, 8) == 0 && cfs_super_is_mac(s) && memcmp(s->uuid, sb.uuid, 16) == 0 &&
                cfs_mac_ok(&m, CFS_MAC_SUPER, block, offsetof(struct cfs_super, crc)) &&
                (!super_ok || s->generation == sb.generation)) {
                super_ok = true;
                key_root = s->key_root;
            }
        }
        if (rc == 0 && !super_ok) {
            rc = -EKEYREJECTED;
            kwarn("cosmofs: %s: that key does not open the pool", bd->name);
        }
        if (rc == 0 && CFS_DVA_VDEV(key_root) != 0)
            rc = -EXDEV;
        if (rc == 0)
            rc = pool_read(p, key_root, block);
        if (rc == 0) {
            const struct cfs_keys2 *k = (const struct cfs_keys2 *)(block + CFS_MHDR_SIZE);
            if (!cfs_mhdr_ok(&m, block, key_root, CFS_KIND_KEYS) || k->version != 2 ||
                memcmp(k->uuid, sb.uuid, 16) != 0) {
                kerror("cosmofs: %s: the key block does not authenticate; the pool is not unlocked", bd->name);
                rc = -EIO;
            }
        }
        memset(&m, 0, sizeof(m));
        if (rc == 0)
            rc = cfs_keytab_install(sb.uuid, req->key);
        if (rc == 0)
            kinfo("cosmofs: %s: unlocked", bd->name);
    } else if (rc == 0 && req->op == COSMOFS_KEY_UNLOAD) {
        rc = cfs_keytab_unload(sb.uuid);
    } else if (rc == 0) {
        rc = -EINVAL;
    }
    if (block != NULL) {
        memset(block, 0, CFS_BLOCK);
        kfree(block);
    }
    pool_close(p);
    return rc;
}

/* The mount half: its slots, and changing one. */
static int keyctl_mount(struct mount *mnt, struct cosmofs_key_req *req)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    if (req->op == COSMOFS_KEY_SLOT) {
        if (!fs->mac.on)
            return -EINVAL;
        if ((mnt->flags & MOUNT_RDONLY) || fs->rdonly)
            return -EROFS;
        if (req->slot >= CFS_KEY_SLOTS)
            return -EINVAL;
        const struct cfs_keyslot *rec = (const struct cfs_keyslot *)req->rec;
        if (!cfs_keyslot_valid(req->slot, rec))
            return -EINVAL;
    }
    mutex_lock(&fs->lock);
    int rc = 0;
    if (req->op == COSMOFS_KEY_SLOT && !key_equal(req->key, fs->master_key))
        rc = -EKEYREJECTED;   /* only a holder of a working passphrase changes the slots */
    if (rc == 0 && req->op == COSMOFS_KEY_INFO) {
        info_from_super(&fs->sb, req->info);
        if (!fs->mac.on) {
            mutex_unlock(&fs->lock);
            return 0;
        }
    } else if (rc == 0 && req->op != COSMOFS_KEY_SLOT) {
        rc = -EINVAL;
    }
    struct cfs_buf *b = NULL;
    if (rc == 0)
        rc = cfs_buf_get(fs, fs->sb.key_root, CFS_KIND_KEYS, &b);
    if (rc == 0 && req->op == COSMOFS_KEY_INFO) {
        info_slots((const struct cfs_keys2 *)(b->data + CFS_MHDR_SIZE), req->info);
        cfs_buf_put(fs, b);
    } else if (rc == 0) {
        struct cfs_keys2 *k = (struct cfs_keys2 *)(b->data + CFS_MHDR_SIZE);
        unsigned left = 0;
        for (unsigned i = 0; i < CFS_KEY_SLOTS; i++)
            if (i != req->slot && k->slot[i].kind != CFS_KEYSLOT_EMPTY)
                left++;
        if (req->rec->kind == CFS_KEYSLOT_EMPTY && left == 0) {
            rc = -EBUSY;   /* the last way in */
            cfs_buf_put(fs, b);
        } else {
            rc = cfs_buf_cow(fs, &b, &fs->sb.key_root);
            if (rc == 0) {
                k = (struct cfs_keys2 *)(b->data + CFS_MHDR_SIZE);
                memcpy(&k->slot[req->slot], req->rec, sizeof(k->slot[0]));
                cfs_buf_mark_dirty(fs, b);
            }
            cfs_buf_put(fs, b);
            /* Durable before the answer: a slot written is one a reboot
             * can use, and a slot cleared is one nobody can. */
            if (rc == 0)
                rc = cfs_commit(fs);
            if (rc == 0)
                kinfo("cosmofs: %s: key slot %u %s", fs->pool_name, req->slot,
                      req->rec->kind == CFS_KEYSLOT_EMPTY ? "cleared" : "written");
        }
    }
    mutex_unlock(&fs->lock);
    return rc;
}

int cosmofs_keyctl(struct blkdev *bd, struct mount *mnt, struct cosmofs_key_req *req)
{
    if ((bd == NULL) == (mnt == NULL))
        return -EINVAL;
    if (bd != NULL)
        return req->op == COSMOFS_KEY_SLOT ? -EINVAL : keyctl_dev(bd, req);
    return keyctl_mount(mnt, req);
}
