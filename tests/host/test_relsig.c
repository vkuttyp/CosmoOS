/*
 * test_relsig.c - userland/system/relsig.c on the host under ASan/UBSan:
 * SHA-256 (FIPS 180-4 examples), a release's SHA256SUMS against its
 * detached signature, and the lookup cosmo-upgrade and cosmo-verify make
 * in it (docs/userland/testing.md, "test-upgrade").
 */

#include "harness.h"

#include "../../userland/system/relsig.h"

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

static bool sha256_is(const void *msg, size_t len, const char *hex)
{
    uint8_t got[32], want[32];
    relsig_sha256(msg, len, got);
    unhex(want, hex, 32);
    return memcmp(got, want, 32) == 0;
}

static void test_sha256_vectors(void)
{
    EXPECT(sha256_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    EXPECT(sha256_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* 448 bits: the padding needs a second block. */
    static const char two[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT(sha256_is(two, sizeof(two) - 1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    static char million[1000000];
    memset(million, 'a', sizeof(million));
    EXPECT(sha256_is(million, sizeof(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

/* scripts/modsign.py sign-detached with RFC 8032 section 7.1 TEST 1's
 * secret key, over SUMS. */
static const char SUMS[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  cosmoos-x86_64-kernel.elf\n"
    "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03  cosmoos-x86_64-cosmo-upgrade\n";
static const char PUB[] = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char SIG[] = "40c25995edaf28811cf940cc957b0a5925853014c1681f3cf9f4207db14352592a720a251c38e7ea0a4b2b2d9e7d0"
                          "9b2e1e00d5529ce1f63038f84e81ae2570c0e02a50225b4baaa0100000001000000434f534d4f534947";

static void ring_one(void)
{
    relsig_nkeys = 1;
    unhex(relsig_keys[0].pub, PUB, 32);
    unhex(relsig_keys[0].id, "0e02a50225b4baaa", 8);
    snprintf(relsig_keys[0].name, sizeof(relsig_keys[0].name), "test.pub");
}

static void test_sums_signature(void)
{
    uint8_t sig[RELSIG_SIG_SIZE];
    unhex(sig, SIG, sizeof(sig));
    char keyid[17], err[200];
    const uint8_t *sums = (const uint8_t *)SUMS;
    size_t len = sizeof(SUMS) - 1;
    relsig_nkeys = 0;
    EXPECT(relsig_sums_verify(sums, len, sig, sizeof(sig), keyid, err, sizeof(err)) == -1);
    EXPECT(strcmp(err, "SHA256SUMS: unknown signing key 0e02a50225b4baaa (not in /etc/pkg/keys)") == 0);
    ring_one();
    EXPECT(relsig_sums_verify(sums, len, sig, sizeof(sig), keyid, err, sizeof(err)) == 0);
    EXPECT(strcmp(keyid, "0e02a50225b4baaa") == 0);
    /* One digit of SHA256SUMS changed: the signature no longer covers it. */
    uint8_t bad[sizeof(SUMS)];
    memcpy(bad, SUMS, sizeof(SUMS));
    bad[3] = 'b';
    EXPECT(relsig_sums_verify(bad, len, sig, sizeof(sig), keyid, err, sizeof(err)) == -1);
    EXPECT(strcmp(err, "SHA256SUMS: bad signature (key test.pub)") == 0);
    /* A signature of the wrong size or without the trailer's magic. */
    EXPECT(relsig_sums_verify(sums, len, sig, 64, keyid, err, sizeof(err)) == -1);
    EXPECT(strcmp(keyid, "(none)") == 0);
    sig[RELSIG_SIG_SIZE - 1] ^= 1;
    EXPECT(relsig_sums_verify(sums, len, sig, sizeof(sig), keyid, err, sizeof(err)) == -1);
    relsig_nkeys = 0;
}

static void test_sums_lookup(void)
{
    uint8_t d[32], want[32];
    char err[200];
    const uint8_t *sums = (const uint8_t *)SUMS;
    size_t len = sizeof(SUMS) - 1;
    EXPECT(relsig_sums_lookup(sums, len, "cosmoos-x86_64-cosmo-upgrade", d, err, sizeof(err)) == 0);
    relsig_sha256("hello\n", 6, want);
    EXPECT(memcmp(d, want, 32) == 0);
    /* A name is the whole rest of its line, not a prefix of it. */
    EXPECT(relsig_sums_lookup(sums, len, "cosmoos-x86_64-cosmo", d, err, sizeof(err)) == -1);
    EXPECT(strcmp(err, "cosmoos-x86_64-cosmo is not in SHA256SUMS") == 0);
    static const char twice[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  f\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  f\n";
    EXPECT(relsig_sums_lookup((const uint8_t *)twice, sizeof(twice) - 1, "f", d, err, sizeof(err)) == -1);
    EXPECT(strcmp(err, "SHA256SUMS lists f twice") == 0);
    static const char malformed[] = "aaaa f\n";
    EXPECT(relsig_sums_lookup((const uint8_t *)malformed, sizeof(malformed) - 1, "f", d, err, sizeof(err)) == -1);
    static const char nothex[] = "zzaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  f\n";
    EXPECT(relsig_sums_lookup((const uint8_t *)nothex, sizeof(nothex) - 1, "f", d, err, sizeof(err)) == -1);
}

static const struct host_test tests[] = {
    { "sha256-vectors", test_sha256_vectors },
    { "sums-signature", test_sums_signature },
    { "sums-lookup", test_sums_lookup },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
