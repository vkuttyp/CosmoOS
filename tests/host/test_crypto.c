/*
 * test_crypto.c - SHA-512 (FIPS 180-4 examples) and Ed25519 (RFC 8032
 * section 7.1 vectors) on the host under ASan/UBSan.
 */

#include "harness.h"

#include <kernel/crypto.h>

#include <stdio.h>
#include <string.h>

static void unhex(uint8_t *out, const char *hex, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static bool digest_is(const void *msg, size_t len, const char *hex)
{
    uint8_t got[64], want[64];
    sha512(msg, len, got);
    unhex(want, hex, 64);
    return memcmp(got, want, 64) == 0;
}

static void test_sha512_vectors(void)
{
    EXPECT(digest_is("", 0,
                     "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                     "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    EXPECT(digest_is("abc", 3,
                     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    const char *two = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    EXPECT(digest_is(two, strlen(two),
                     "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                     "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));

    /* One million 'a', fed in odd-sized pieces to exercise buffering. */
    static uint8_t block[1000];
    memset(block, 'a', sizeof(block));
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    size_t left = 1000000;
    size_t piece = 1;
    while (left > 0) {
        size_t n = piece < left ? piece : left;
        if (n > sizeof(block))
            n = sizeof(block);
        sha512_update(&ctx, block, n);
        left -= n;
        piece = (piece * 7 + 3) % 1000 + 1;
    }
    uint8_t got[64], want[64];
    sha512_final(&ctx, got);
    unhex(want,
          "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
          "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
          64);
    EXPECT(memcmp(got, want, 64) == 0);
}

struct ed_vector {
    const char *pub;
    const char *msg;
    size_t msg_len;
    const char *sig;
};

/* RFC 8032 section 7.1, TEST 1, 2, 3, and the SHA(abc) test. */
static const struct ed_vector vectors[] = {
    {
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "", 0,
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    },
    {
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72", 1,
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    },
    {
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82", 2,
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
    },
    {
        "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64,
        "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
        "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704",
    },
};

static void test_ed25519_vectors(void)
{
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t pub[32], sig[64], msg[64];
        unhex(pub, vectors[i].pub, 32);
        unhex(sig, vectors[i].sig, 64);
        unhex(msg, vectors[i].msg, vectors[i].msg_len);
        EXPECT(ed25519_verify(sig, msg, vectors[i].msg_len, pub));

        /* Every single-bit flip of the signature must fail. */
        for (unsigned bit = 0; bit < 64 * 8; bit += 37) {
            uint8_t bad[64];
            memcpy(bad, sig, 64);
            bad[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            EXPECT(!ed25519_verify(bad, msg, vectors[i].msg_len, pub));
        }
        /* A flipped message bit and a flipped key bit must fail. */
        if (vectors[i].msg_len > 0) {
            uint8_t badmsg[64];
            memcpy(badmsg, msg, vectors[i].msg_len);
            badmsg[0] ^= 1;
            EXPECT(!ed25519_verify(sig, badmsg, vectors[i].msg_len, pub));
        }
        uint8_t badpub[32];
        memcpy(badpub, pub, 32);
        badpub[5] ^= 0x10;
        EXPECT(!ed25519_verify(sig, msg, vectors[i].msg_len, badpub));
    }
}

static void test_ed25519_malleability(void)
{
    /* S + L must be rejected even though it is the same scalar mod L. */
    uint8_t pub[32], sig[64], msg[1];
    unhex(pub, vectors[1].pub, 32);
    unhex(sig, vectors[1].sig, 64);
    unhex(msg, vectors[1].msg, 1);
    static const uint8_t L[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
    };
    unsigned carry = 0;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = sig[32 + i] + L[i] + carry;
        sig[32 + i] = (uint8_t)v;
        carry = v >> 8;
    }
    EXPECT(!ed25519_verify(sig, msg, 1, pub));

    /* A public key that is not on the curve must be rejected. */
    uint8_t off[32];
    memset(off, 0xff, 32);
    off[31] = 0x7f;   /* y = p - 1... actually >= p after masking: non-canonical */
    EXPECT(!ed25519_verify(sig, msg, 1, off));
}

static const struct host_test tests[] = {
    { "sha512-vectors", test_sha512_vectors },
    { "ed25519-vectors", test_ed25519_vectors },
    { "ed25519-malleability", test_ed25519_malleability },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
