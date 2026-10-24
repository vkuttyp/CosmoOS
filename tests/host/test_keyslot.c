/*
 * test_keyslot.c - Argon2id (RFC 9106's test vector) and an encrypted
 * pool's key slots (userland/lib/keyslot.c; roadmap 1.2 B5) on the host
 * under ASan/UBSan; and the passphrase reader's echo (userland/lib/secret.c).
 */

#define _XOPEN_SOURCE 700   /* posix_openpt and ptsname under -std=c11 */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "harness.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <uapi/cosmo/syscall.h>

#include "../../userland/lib/argon2.h"
#include "../../userland/lib/keyslot.h"

static void unhex(uint8_t *out, const char *hex, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_argon2id_rfc9106(void)
{
    /* RFC 9106 section 5.3: Argon2id, version 0x13. */
    uint8_t pwd[32], salt[16], secret[8], ad[12], out[32], want[32];
    memset(pwd, 1, sizeof(pwd));
    memset(salt, 2, sizeof(salt));
    memset(secret, 3, sizeof(secret));
    memset(ad, 4, sizeof(ad));
    struct argon2_params p = { 3, 32, 4, secret, sizeof(secret), ad, sizeof(ad) };
    EXPECT(argon2id(&p, pwd, sizeof(pwd), salt, sizeof(salt), out, sizeof(out)) == 0);
    unhex(want, "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", 32);
    EXPECT(memcmp(out, want, 32) == 0);
    /* What RFC 9106 does not allow is refused, not computed. */
    struct argon2_params bad = { 0, 32, 4, NULL, 0, NULL, 0 };
    EXPECT(argon2id(&bad, pwd, sizeof(pwd), salt, sizeof(salt), out, sizeof(out)) == -1);
    EXPECT(argon2id(&p, pwd, sizeof(pwd), salt, 4, out, sizeof(out)) == -1);   /* a salt under 8 bytes */
}

/* Small parameters: what is tested is the sealing, not the work factor. */
#define T 1u
#define M 64u

static void test_keyslot_passphrase(void)
{
    uint8_t uuid[16], salt[16], master[COSMO_KEY_SIZE], got[COSMO_KEY_SIZE];
    for (unsigned i = 0; i < 16; i++) {
        uuid[i] = (uint8_t)(0x10 + i);
        salt[i] = (uint8_t)(0x80 + i);
    }
    for (unsigned i = 0; i < COSMO_KEY_SIZE; i++)
        master[i] = (uint8_t)(i * 7 + 3);
    struct cosmo_keyslot s;
    EXPECT(keyslot_seal_passphrase(uuid, 2, T, M, 1, "hunter2", 7, salt, master, &s) == 0);
    EXPECT(s.kind == COSMO_KEYSLOT_PASSPHRASE && s.t_cost == T && s.m_cost_kib == M && s.lanes == 1);
    EXPECT(memcmp(s.wrapped, master, sizeof(master)) != 0);   /* sealed, not stored */
    memset(got, 0, sizeof(got));
    EXPECT(keyslot_open(uuid, 2, &s, "hunter2", 7, got) == 0 && memcmp(got, master, sizeof(got)) == 0);
    /* A wrong passphrase is told by the tag. */
    EXPECT(keyslot_open(uuid, 2, &s, "hunter3", 7, got) == -COSMO_EKEYREJECTED);
    /* The same slot at another index, or of another pool, does not open. */
    EXPECT(keyslot_open(uuid, 3, &s, "hunter2", 7, got) == -COSMO_EKEYREJECTED);
    uint8_t other[16];
    memcpy(other, uuid, 16);
    other[0] ^= 1;
    EXPECT(keyslot_open(other, 2, &s, "hunter2", 7, got) == -COSMO_EKEYREJECTED);
    /* Its work factor lowered behind its back: refused, not computed faster. */
    struct cosmo_keyslot weak = s;
    weak.m_cost_kib = M / 2;
    EXPECT(keyslot_open(uuid, 2, &weak, "hunter2", 7, got) == -COSMO_EKEYREJECTED);
    /* An empty slot is not a wrong passphrase. */
    struct cosmo_keyslot empty;
    memset(&empty, 0, sizeof(empty));
    EXPECT(keyslot_open(uuid, 2, &empty, "hunter2", 7, got) == -COSMO_EINVAL);
}

static void test_keyslot_keyfile(void)
{
    uint8_t uuid[16], salt[16], file[KEYFILE_SIZE], master[COSMO_KEY_SIZE], got[COSMO_KEY_SIZE];
    memset(uuid, 0x42, sizeof(uuid));
    memset(salt, 0x24, sizeof(salt));
    for (unsigned i = 0; i < sizeof(file); i++)
        file[i] = (uint8_t)(255 - i);
    memset(master, 0x5a, sizeof(master));
    struct cosmo_keyslot s;
    EXPECT(keyslot_seal_keyfile(uuid, file, salt, master, &s) == 0);
    EXPECT(s.kind == COSMO_KEYSLOT_KEYFILE && s.t_cost == 0);
    EXPECT(keyslot_open(uuid, 0, &s, file, sizeof(file), got) == 0 && memcmp(got, master, sizeof(got)) == 0);
    file[5] ^= 1;
    EXPECT(keyslot_open(uuid, 0, &s, file, sizeof(file), got) == -COSMO_EKEYREJECTED);
    EXPECT(keyslot_open(uuid, 0, &s, file, 31, got) == -COSMO_EKEYREJECTED);   /* not a key file's size */
}

/* The reader's first flush is its prompt's: was echo already off then? */
static int g_flushes, g_prompt_echo;

static int prompt_fflush(FILE *f)
{
    struct termios t;
    if (f == stdout && g_flushes++ == 0)
        g_prompt_echo = tcgetattr(0, &t) == 0 && (t.c_lflag & ECHO);
    return fflush(f);
}

#define fflush prompt_fflush
#include "../../userland/lib/secret.c"
#undef fflush

static void test_secret_echo_off_before_prompt(void)
{
    /* A reply sent as soon as the prompt is seen (a paste, the boot
     * harness) must not be echoed: echo is off before the prompt is out,
     * and back on after the line. */
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    EXPECT(m >= 0);
    EXPECT(grantpt(m) == 0 && unlockpt(m) == 0);
    int s = open(ptsname(m), O_RDWR | O_NOCTTY);
    EXPECT(s >= 0);
    struct termios t;
    EXPECT(tcgetattr(s, &t) == 0);
    t.c_lflag |= ECHO | ICANON;
    EXPECT(tcsetattr(s, TCSANOW, &t) == 0);
    int saved = dup(0);
    EXPECT(dup2(s, 0) == 0);
    EXPECT(write(m, "pw\n", 3) == 3);
    char buf[SECRET_MAX];
    g_flushes = 0;
    g_prompt_echo = -1;
    int len = secret_read("secret: ", buf, sizeof(buf));
    struct termios after;
    bool echo_back = tcgetattr(0, &after) == 0 && (after.c_lflag & ECHO);
    EXPECT(dup2(saved, 0) == 0);
    close(saved);
    close(s);
    close(m);
    EXPECT(len == 2 && strcmp(buf, "pw") == 0);
    EXPECT(g_flushes >= 1);
    EXPECT(g_prompt_echo == 0);
    EXPECT(echo_back);
}

static const struct host_test tests[] = {
    { "argon2id-rfc9106", test_argon2id_rfc9106 },
    { "keyslot-passphrase", test_keyslot_passphrase },
    { "keyslot-keyfile", test_keyslot_keyfile },
    { "secret-echo-off-before-prompt", test_secret_echo_off_before_prompt },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
