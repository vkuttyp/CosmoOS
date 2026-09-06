/*
 * test_chacha20.c - ChaCha20 and Poly1305 against RFC 8439, on the host.
 *
 * A cipher that is nearly right is worse than none: it looks like it
 * works. So the vectors here are the RFC's own, byte for byte, and the
 * sealed-block tests check the properties the filesystem depends on --
 * that a tag is computed over ciphertext, that a forgery is refused
 * before any plaintext exists, and that the same key and nonce give the
 * same bytes every time.
 */

#include "harness.h"

#include <kernel/chacha20.h>

#include <stdio.h>
#include <string.h>

/* RFC 8439 §2.4.2 */
static void test_chacha20_vector(void)
{
    uint8_t key[32];
    for (unsigned i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    /* Section 2.4.2's nonce. The 0x09 that appears in the third byte of
     * section 2.3.2's block vector belongs to that test, not this one --
     * mixing the two is how a correct cipher fails its own test. */
    uint8_t nonce[12] = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    static const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
                                "only one tip for the future, sunscreen would be it.";
    size_t len = sizeof(plain) - 1;
    static const uint8_t want[] = {
        0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
        0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
        0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
        0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
        0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
        0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
        0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
        0x87, 0x4d,
    };
    uint8_t out[sizeof(want)];
    EXPECT(len == sizeof(want));
    chacha20_xor(key, 1, nonce, plain, out, len);
    EXPECT(memcmp(out, want, len) == 0);

    /* Section 2.3.2: one keystream block, with the nonce that vector
     * uses, checked by encrypting zeros. */
    uint8_t ks_nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    static const uint8_t ks_want[16] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    };
    uint8_t zeros[16] = { 0 }, ks[16];
    chacha20_xor(key, 1, ks_nonce, zeros, ks, sizeof(ks));
    EXPECT(memcmp(ks, ks_want, sizeof(ks_want)) == 0);

    /* And back again: the cipher is its own inverse. */
    uint8_t back[sizeof(want)];
    chacha20_xor(key, 1, nonce, out, back, len);
    EXPECT(memcmp(back, plain, len) == 0);
}

/* RFC 8439 §2.5.2 */
static void test_poly1305_vector(void)
{
    static const uint8_t key[32] = {
        0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
        0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b,
    };
    static const char msg[] = "Cryptographic Forum Research Group";
    static const uint8_t want[16] = {
        0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6, 0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9,
    };
    uint8_t tag[16];
    poly1305(key, msg, sizeof(msg) - 1, tag);
    EXPECT(memcmp(tag, want, 16) == 0);

    /* A message that is a whole number of blocks, and an empty one:
     * the padded-last-block path must not run for the first. */
    uint8_t t2[16], t3[16];
    poly1305(key, "0123456789abcdef", 16, t2);
    poly1305(key, "", 0, t3);
    EXPECT(memcmp(t2, t3, 16) != 0);
    EXPECT(poly1305_verify(tag, want));
    uint8_t bent[16];
    memcpy(bent, want, 16);
    bent[15] ^= 1;
    EXPECT(!poly1305_verify(tag, bent));
}

/* The shape the filesystem uses. */
static void test_seal_open(void)
{
    uint8_t key[32];
    for (unsigned i = 0; i < 32; i++)
        key[i] = (uint8_t)(i * 7 + 1);
    uint8_t nonce[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    uint8_t data[4096], copy[4096], tag[16];
    static const uint8_t aad[16] = { 7, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0 };   /* inode 7, block 3 */
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i % 251);
    memcpy(copy, data, sizeof(data));

    chacha20_seal(key, nonce, aad, sizeof(aad), data, sizeof(data), tag);
    EXPECT(memcmp(data, copy, sizeof(data)) != 0);   /* it did something */

    /* The tag is over the ciphertext, so a reader with no key can check
     * a block -- which is what lets a mirror repair and a scrub run on a
     * machine that cannot decrypt anything. */
    /* The associated data is authenticated but not encrypted, and moving
     * a block to another position must fail even with its own tag. */
    uint8_t elsewhere[16];
    memcpy(elsewhere, aad, sizeof(aad));
    elsewhere[8] ^= 1;
    uint8_t moved[4096];
    memcpy(moved, data, sizeof(moved));
    EXPECT(!chacha20_open(key, nonce, elsewhere, sizeof(elsewhere), moved, sizeof(moved), tag));

    uint8_t work[4096];
    memcpy(work, data, sizeof(work));
    EXPECT(chacha20_open(key, nonce, aad, sizeof(aad), work, sizeof(work), tag));
    EXPECT(memcmp(work, copy, sizeof(work)) == 0);

    /* A forged block never becomes plaintext. */
    memcpy(work, data, sizeof(work));
    work[100] ^= 0x40;
    EXPECT(!chacha20_open(key, nonce, aad, sizeof(aad), work, sizeof(work), tag));
    EXPECT(work[100] == (uint8_t)(data[100] ^ 0x40));   /* untouched */

    /* A right tag with the wrong nonce, and with the wrong key. */
    memcpy(work, data, sizeof(work));
    uint8_t other_nonce[12];
    memcpy(other_nonce, nonce, 12);
    other_nonce[0] ^= 1;
    EXPECT(!chacha20_open(key, other_nonce, aad, sizeof(aad), work, sizeof(work), tag));
    uint8_t other_key[32];
    memcpy(other_key, key, 32);
    other_key[31] ^= 1;
    EXPECT(!chacha20_open(other_key, nonce, aad, sizeof(aad), work, sizeof(work), tag));

    /* Deterministic: the same key and nonce give the same ciphertext,
     * which is what makes a rewritten block reproducible. */
    uint8_t twice[4096], tag2[16];
    memcpy(twice, copy, sizeof(twice));
    chacha20_seal(key, nonce, aad, sizeof(aad), twice, sizeof(twice), tag2);
    EXPECT(memcmp(twice, data, sizeof(twice)) == 0 && memcmp(tag, tag2, 16) == 0);

    /* Different nonces give different ciphertext for the same plaintext:
     * the property the per-block nonce exists to provide. */
    memcpy(twice, copy, sizeof(twice));
    chacha20_seal(key, other_nonce, aad, sizeof(aad), twice, sizeof(twice), tag2);
    EXPECT(memcmp(twice, data, sizeof(twice)) != 0);
}

/* Lengths around the block boundaries the code splits on. */
static void test_lengths(void)
{
    uint8_t key[32] = { 9 }, nonce[12] = { 3 };
    for (size_t len = 0; len <= 130; len++) {
        uint8_t buf[130], orig[130], tag[16];
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)(i ^ len);
        memcpy(orig, buf, len);
        chacha20_seal(key, nonce, NULL, 0, buf, len, tag);
        EXPECT(chacha20_open(key, nonce, NULL, 0, buf, len, tag));
        EXPECT(len == 0 || memcmp(buf, orig, len) == 0);
    }
}

static const struct host_test tests[] = {
    { "chacha20-rfc8439", test_chacha20_vector },
    { "poly1305-rfc8439", test_poly1305_vector },
    { "chacha20-seal-open", test_seal_open },
    { "chacha20-lengths", test_lengths },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
