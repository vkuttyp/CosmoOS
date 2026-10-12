/*
 * verify.c - Verified boot: the signed manifest (roadmap M5;
 * docs/boot/design.md, "Verified boot").
 *
 * \cosmo\manifest lists the size and SHA-512 of kernel.elf, boot.tar and
 * cmdline under one Ed25519 signature (cosmomanifest.h). The loader checks
 * the signature against the keys compiled into it -- the same ring the
 * kernel trusts for modules, generated from KEYRING_PUBS -- before it
 * reads anything else, then checks each file against its entry as soon as
 * the file is in memory. Any failure is fatal and names the file; there is
 * no runtime switch. The hashing and the signature check are the kernel's
 * own sources (kernel/security/sha512.c, ed25519.c, keyring.c), built
 * freestanding for the loader by boot.mk.
 *
 * A loader built with VERIFY_BOOT=0 (CONFIG_VERIFY_BOOT 0) compiles none
 * of the checks: verify_manifest() says so on the console and the boot
 * protocol reports the boot unverified.
 */

#include "loader.h"
#include "cosmoboot.h"
#include "cosmomanifest.h"

#include <kernel/crypto.h>
#include <kernel/keyring.h>

#ifndef CONFIG_VERIFY_BOOT
#error "CONFIG_VERIFY_BOOT must be defined (boot.mk)"
#endif

static const char *const g_paths[COSMO_MANIFEST_FILES] = {
    [COSMO_MANIFEST_KERNEL] = "\\cosmo\\kernel.elf",
    [COSMO_MANIFEST_ARCHIVE] = "\\cosmo\\boot.tar",
    [COSMO_MANIFEST_CMDLINE] = "\\cosmo\\cmdline",
};

const char *verify_path(unsigned which)
{
    return which < COSMO_MANIFEST_FILES ? g_paths[which] : "?";
}

#if CONFIG_VERIFY_BOOT

static const char *const g_names[COSMO_MANIFEST_FILES] = {
    [COSMO_MANIFEST_KERNEL] = "kernel.elf",
    [COSMO_MANIFEST_ARCHIVE] = "boot.tar",
    [COSMO_MANIFEST_CMDLINE] = "cmdline",
};

static struct cosmo_manifest g_manifest;
static bool g_checked[COSMO_MANIFEST_FILES];

static void refuse(const char *what, const char *path, const char *why)
{
    lprintf("verified boot: %s %s: %s\n", what, path, why);
    die("verified boot: refusing to boot", EFI_SECURITY_VIOLATION);
}

static void hex(char *out, const uint8_t *b, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 15];
    }
    out[2 * n] = '\0';
}

void verify_manifest(const uint8_t *file, size_t size, uint8_t key_id[COSMOBOOT_KEY_ID_SIZE])
{
    static const char path[] = "\\cosmo\\manifest";
    if (file == NULL)
        refuse("cannot read", path, "a verifying loader boots nothing without the signed manifest");
    if (size != COSMO_MANIFEST_FILE_SIZE)
        refuse("rejected", path, "not the size of a manifest");

    struct cosmo_manifest_sig sig;
    memcpy(&g_manifest, file, sizeof(g_manifest));
    memcpy(&sig, file + sizeof(g_manifest), sizeof(sig));
    if (memcmp(sig.magic, COSMO_MANIFEST_SIG_MAGIC, 8) != 0 || sig.version != COSMO_MANIFEST_SIG_VERSION ||
        sig.algo != COSMO_MANIFEST_SIG_ED25519)
        refuse("rejected", path, "no Ed25519 signature trailer");

    char id[2 * KEYRING_ID_SIZE + 1];
    hex(id, sig.key_id, KEYRING_ID_SIZE);
    const struct trusted_key *key = keyring_find(sig.key_id);
    if (key == NULL) {
        lprintf("verified boot: %s is signed by key %s, which this loader does not trust\n", path, id);
        die("verified boot: refusing to boot", EFI_SECURITY_VIOLATION);
    }
    /* The signature covers the payload alone, as for a module. */
    if (!ed25519_verify(sig.sig, file, sizeof(g_manifest), key->pub))
        refuse("rejected", path, "the signature does not verify");

    /* Signed, so the structure is the build's; checked anyway, since a
     * signing tool that wrote the wrong layout must not boot either. */
    if (memcmp(g_manifest.magic, COSMO_MANIFEST_MAGIC, 8) != 0 || g_manifest.version != COSMO_MANIFEST_VERSION ||
        g_manifest.nfiles != COSMO_MANIFEST_FILES)
        refuse("rejected", path, "not a version 1 manifest of three files");
    for (unsigned i = 0; i < COSMO_MANIFEST_FILES; i++) {
        char name[COSMO_MANIFEST_NAME_MAX];
        memset(name, 0, sizeof(name));
        memcpy(name, g_names[i], strlen(g_names[i]));
        if (memcmp(name, g_manifest.files[i].name, sizeof(name)) != 0)
            refuse("rejected", path, "the files are not kernel.elf, boot.tar, cmdline");
    }

    memcpy(key_id, sig.key_id, KEYRING_ID_SIZE);
    lprintf("verified boot: %s signed by key %s (%s)\n", path, id, key->name);
}

void verify_file(unsigned which, const uint8_t *data, size_t size)
{
    const char *path = verify_path(which);
    if (data == NULL)
        refuse("cannot read", path, "the signed manifest lists it");
    const struct cosmo_manifest_file *want = &g_manifest.files[which];
    if (size != want->size)
        refuse("rejected", path, "its size does not match the signed manifest");
    uint8_t digest[SHA512_DIGEST_SIZE];
    sha512(data, size, digest);
    if (memcmp(digest, want->sha512, sizeof(digest)) != 0)
        refuse("rejected", path, "its SHA-512 does not match the signed manifest");
    g_checked[which] = true;
    lprintf("verified boot: %s matches the manifest\n", path);
}

bool verify_complete(void)
{
    for (unsigned i = 0; i < COSMO_MANIFEST_FILES; i++)
        if (!g_checked[i])
            return false;
    return true;
}

#else /* !CONFIG_VERIFY_BOOT */

void verify_manifest(const uint8_t *file, size_t size, uint8_t key_id[COSMOBOOT_KEY_ID_SIZE])
{
    (void)file;
    (void)size;
    (void)key_id;
    lputs("verified boot: OFF (this loader was built with VERIFY_BOOT=0); nothing is checked\n");
}

void verify_file(unsigned which, const uint8_t *data, size_t size)
{
    (void)which;
    (void)data;
    (void)size;
}

bool verify_complete(void)
{
    return false;
}

#endif
