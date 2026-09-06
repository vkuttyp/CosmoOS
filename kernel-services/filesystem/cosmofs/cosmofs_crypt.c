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

#include <kernel/chacha20.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/crypto.h>
#include <kernel/string.h>

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
    chacha20_seal(wk, nonce, k->wrapped, sizeof(k->wrapped), k->tag);
    memset(wk, 0, sizeof(wk));

    cfs_mhdr_seal_raw(block, CFS_KIND_KEYS, dva, generation);
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
    bool ok = chacha20_open(wk, nonce, buf, sizeof(buf), k->tag);
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
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, fs->sb.key_root, block);
    if (rc == 0 && !cfs_mhdr_ok(block, fs->sb.key_root, CFS_KIND_KEYS))
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
    chacha20_seal(wk, nonce, k->wrapped, sizeof(k->wrapped), k->tag);
    memset(wk, 0, sizeof(wk));
    cfs_buf_mark_dirty(fs, b);
    cfs_buf_put(fs, b);
    return 0;
}
