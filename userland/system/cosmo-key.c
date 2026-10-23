/* cosmo-key - the key slots of an encrypted pool (roadmap 1.2 B5;
 * docs/kernel-services/filesystem/cosmofs/design.md, "The key hierarchy").
 *
 *   cosmo-key list [PATH]                the slots of the pool mounted at PATH (/)
 *   cosmo-key unlock [--boot] DEVICE     unwrap its master key and hand it to the kernel
 *   cosmo-key add [OPTS] [PATH]          a new passphrase slot
 *   cosmo-key change [OPTS] [PATH]       the passphrase of the slot the current one opens
 *   cosmo-key remove SLOT [PATH]         empty a slot (never the last)
 *   cosmo-key bench [KIB [PASSES]]       time one Argon2id derivation
 *
 *   OPTS: --passes N, --memory KIB (Argon2id's; 3 and 65536 unless given)
 *
 * A passphrase is read from the standard input with echo off when it is
 * a terminal: init runs `unlock --boot` on its console, which is every
 * console output the machine has, serial lines included. The master key
 * is opened here, in user space -- Argon2id wants tens of MiB for a second
 * -- and only the key itself goes to the kernel, which checks it opens
 * the pool before keeping it.
 *
 * Exit status: 0 done; 1 failed; 2 usage; 3 (unlock) three wrong
 * passphrases, which init answers with a restart.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cosmo/syscall.h>

#include "../lib/argon2.h"
#include "../lib/fskey.h"
#include "../lib/keyslot.h"
#include "../lib/rootsnap.h"
#include "../lib/secret.h"
#include "blkdevs.h"
#include "upgrade_fat.h"

#define PASS_MAX SECRET_MAX
#define TRIES 3

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* A pool's name in what this prints: its label, or the device. */
static void pool_name(const struct cosmo_fsctl_keyinfo *k, const char *dev, char *out, size_t n)
{
    size_t l = strnlen(k->label, sizeof(k->label));
    if (l == 0)
        snprintf(out, n, "%s", dev ? dev : "the pool");
    else
        snprintf(out, n, "%.*s", (int)l, k->label);
}

/* Try `secret` on every slot of `kind`: the slot that opened, its time,
 * and the master key; -1 when none did (errno EKEYREJECTED or the error). */
static int open_any(const struct cosmo_fsctl_keyinfo *k, uint32_t kind, const void *secret, size_t len,
                    uint8_t master[COSMO_KEY_SIZE], double *seconds)
{
    int last = -EKEYREJECTED;
    for (unsigned i = 0; i < COSMO_KEY_SLOTS; i++) {
        if (k->slot[i].kind != kind)
            continue;
        double t0 = now();
        int rc = keyslot_open(k->uuid, i, &k->slot[i], secret, len, master);
        if (seconds)
            *seconds = now() - t0;
        if (rc == 0)
            return (int)i;
        if (rc != -EKEYREJECTED)
            last = rc;
    }
    errno = -last;
    return -1;
}

/* Slot 0's key file, from the ESP the loader was read from: 1 read, 0
 * there is none. */
static int keyfile_read(uint8_t out[KEYFILE_SIZE])
{
    if (blkdevs_list() < 0)
        return 0;
    const struct cosmo_blkctl_dev *esp = NULL;
    for (unsigned i = 0; i < g_nblkdevs && esp == NULL; i++)
        if (g_blkdevs[i].flags & COSMO_BLKCTL_BOOT)
            esp = &g_blkdevs[i];
    if (esp == NULL)
        return 0;
    struct fat f;
    char err[120];
    if (fat_open(&f, esp, err, sizeof(err)) < 0)
        return 0;
    uint8_t n83[11], nt;
    struct fat_ent e;
    uint8_t *data = NULL;
    int got = 0;
    if (fat_name83(KEYFILE_NAME, n83, &nt) && fat_lookup(&f, f.root, n83, &e) == 1 && e.size == KEYFILE_SIZE &&
        fat_read_file(&f, e.cluster, e.size, &data) == 0) {
        memcpy(out, data, KEYFILE_SIZE);
        secret_wipe(data, KEYFILE_SIZE);
        got = 1;
    }
    free(data);
    fat_close(&f);
    return got;
}

static int unlock(const char *dev, bool boot)
{
    struct cosmo_fsctl_keyinfo k;
    if (fskey_info(dev, 0, &k) < 0) {
        if (boot && errno == EINVAL)
            return 0;   /* not a cosmofs: nothing to unlock */
        fprintf(stderr, "cosmo-key: %s: %s\n", dev, strerror(errno));
        return 1;
    }
    char name[40];
    pool_name(&k, dev, name, sizeof(name));
    if (!(k.flags & COSMO_FSCTL_KEY_F_MAC)) {
        if (boot)
            return 0;   /* a plain pool, or a version-7 one this does not open */
        fprintf(stderr, "cosmo-key: %s is not an encrypted pool cosmo-key opens (format %u)\n", name, k.format);
        return 1;
    }
    if (k.flags & COSMO_FSCTL_KEY_F_UNLOCKED) {
        if (!boot)
            printf("cosmo-key: %s is already unlocked\n", name);
        return 0;
    }
    uint8_t master[COSMO_KEY_SIZE];
    double secs = 0;
    if (k.slot[0].kind == COSMO_KEYSLOT_KEYFILE) {
        uint8_t file[KEYFILE_SIZE];
        int got = keyfile_read(file);
        int slot = got ? open_any(&k, COSMO_KEYSLOT_KEYFILE, file, sizeof(file), master, &secs) : -1;
        secret_wipe(file, sizeof(file));
        if (slot == 0 && fskey_load(dev, master) == 0) {
            secret_wipe(master, sizeof(master));
            printf("cosmo-key: %s unlocked with the key file on the ESP\n", name);
            return 0;
        }
        printf("cosmo-key: %s: %s; asking for a passphrase\n", name,
               got ? "the key file on the ESP does not open it" : "no key file on the ESP");
    }
    bool any = false;
    for (unsigned i = 0; i < COSMO_KEY_SLOTS; i++)
        any = any || k.slot[i].kind == COSMO_KEYSLOT_PASSPHRASE;
    if (!any) {
        fprintf(stderr, "cosmo-key: %s has no passphrase slot\n", name);
        return 1;
    }
    char prompt[64], pass[PASS_MAX];
    snprintf(prompt, sizeof(prompt), "passphrase for %s: ", name);
    for (int t = 0; t < TRIES; t++) {
        int len = secret_read(prompt, pass, sizeof(pass));
        if (len < 0) {
            fprintf(stderr, "cosmo-key: no passphrase (end of input)\n");
            return 1;
        }
        int slot = open_any(&k, COSMO_KEYSLOT_PASSPHRASE, pass, (size_t)len, master, &secs);
        secret_wipe(pass, sizeof(pass));
        if (slot < 0 && errno != EKEYREJECTED) {
            fprintf(stderr, "cosmo-key: %s: %s\n", name, strerror(errno));
            return 1;
        }
        if (slot >= 0) {
            int rc = fskey_load(dev, master);
            int err = errno;
            secret_wipe(master, sizeof(master));
            if (rc < 0) {
                fprintf(stderr, "cosmo-key: %s: the kernel did not take the key: %s\n", name, strerror(err));
                return 1;
            }
            const struct cosmo_keyslot *s = &k.slot[slot];
            printf("cosmo-key: %s unlocked with slot %d (argon2id %u KiB, %u passes: %.2f s)\n", name, slot,
                   s->m_cost_kib, s->t_cost, secs);
            return 0;
        }
        printf("cosmo-key: that passphrase opens no slot of %s\n", name);
    }
    printf("cosmo-key: %u wrong passphrases for %s\n", TRIES, name);
    return 3;
}

/* The mount at `path`: its id and key information. */
static int mounted(const char *path, uint64_t *id, struct cosmo_fsctl_keyinfo *k, char *name, size_t n)
{
    struct rootsnap r;
    if (rootsnap_open(&r, path) < 0) {
        fprintf(stderr, "cosmo-key: %s: %s\n", path, strerror(errno));
        return -1;
    }
    *id = r.mount_id;
    rootsnap_close(&r);
    if (fskey_info(NULL, *id, k) < 0) {
        fprintf(stderr, "cosmo-key: %s: %s\n", path, strerror(errno));
        return -1;
    }
    pool_name(k, path, name, n);
    if (!(k->flags & COSMO_FSCTL_KEY_F_MAC)) {
        fprintf(stderr, "cosmo-key: %s is not an encrypted pool (format %u)\n", name, k->format);
        return -1;
    }
    return 0;
}

static int list(const char *path)
{
    uint64_t id;
    struct cosmo_fsctl_keyinfo k;
    char name[40];
    if (mounted(path, &id, &k, name, sizeof(name)) < 0)
        return 1;
    printf("%s: format %u, encrypted, unlocked\n", name, k.format);
    for (unsigned i = 0; i < COSMO_KEY_SLOTS; i++) {
        const struct cosmo_keyslot *s = &k.slot[i];
        if (s->kind == COSMO_KEYSLOT_PASSPHRASE)
            printf("slot %u: passphrase, argon2id %u KiB, %u passes, %u lane%s\n", i, s->m_cost_kib, s->t_cost,
                   s->lanes, s->lanes == 1 ? "" : "s");
        else if (s->kind == COSMO_KEYSLOT_KEYFILE)
            printf("slot %u: key file (\\%s on the ESP)\n", i, KEYFILE_NAME);
        else
            printf("slot %u: empty\n", i);
    }
    return 0;
}

/* Ask for a passphrase that opens the pool: the slot it opened, or -1. */
static int ask_open(const struct cosmo_fsctl_keyinfo *k, const char *prompt, uint8_t master[COSMO_KEY_SIZE])
{
    char pass[PASS_MAX];
    int len = secret_read(prompt, pass, sizeof(pass));
    if (len < 0)
        return -1;
    int slot = open_any(k, COSMO_KEYSLOT_PASSPHRASE, pass, (size_t)len, master, NULL);
    secret_wipe(pass, sizeof(pass));
    if (slot < 0)
        fprintf(stderr, "cosmo-key: that passphrase opens no slot\n");
    return slot;
}

/* Seal `master` into slot `idx` with a new passphrase, and write it. */
static int write_slot(uint64_t id, const struct cosmo_fsctl_keyinfo *k, unsigned idx, uint32_t t, uint32_t m,
                      const uint8_t master[COSMO_KEY_SIZE], const char *name)
{
    char pass[PASS_MAX];
    int len = secret_read_new("cosmo-key", pass, sizeof(pass));
    if (len < 0) {
        secret_wipe(pass, sizeof(pass));
        return 1;
    }
    uint8_t salt[16];
    struct cosmo_keyslot rec;
    int rc = cosmo_getrandom(salt, sizeof(salt)) == (long)sizeof(salt) ? 0 : -EIO;
    double t0 = now();
    if (rc == 0)
        rc = keyslot_seal_passphrase(k->uuid, idx, t, m, KEYSLOT_LANES_DEFAULT, pass, (size_t)len, salt, master, &rec);
    double secs = now() - t0;
    secret_wipe(pass, sizeof(pass));
    if (rc == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        rec.created_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        rc = fskey_slot(id, idx, master, &rec) < 0 ? -errno : 0;
    }
    if (rc < 0) {
        fprintf(stderr, "cosmo-key: slot %u of %s not written: %s\n", idx, name, strerror(-rc));
        return 1;
    }
    printf("cosmo-key: slot %u of %s written (argon2id %u KiB, %u passes: %.2f s)\n", idx, name, m, t, secs);
    return 0;
}

static int add_or_change(bool change, const char *path, uint32_t t, uint32_t m)
{
    uint64_t id;
    struct cosmo_fsctl_keyinfo k;
    char name[40];
    if (mounted(path, &id, &k, name, sizeof(name)) < 0)
        return 1;
    int idx = -1;
    if (!change) {
        for (unsigned i = 0; i < COSMO_KEY_SLOTS && idx < 0; i++)
            if (k.slot[i].kind == COSMO_KEYSLOT_EMPTY)
                idx = (int)i;
        if (idx < 0) {
            fprintf(stderr, "cosmo-key: every slot of %s is in use; remove one first\n", name);
            return 1;
        }
    }
    uint8_t master[COSMO_KEY_SIZE];
    char prompt[64];
    snprintf(prompt, sizeof(prompt), change ? "current passphrase for %s: " : "a passphrase for %s: ", name);
    int opened = ask_open(&k, prompt, master);
    if (opened < 0)
        return 1;
    if (change)
        idx = opened;
    int rc = write_slot(id, &k, (unsigned)idx, t, m, master, name);
    secret_wipe(master, sizeof(master));
    return rc;
}

static int remove_slot(unsigned idx, const char *path)
{
    uint64_t id;
    struct cosmo_fsctl_keyinfo k;
    char name[40];
    if (mounted(path, &id, &k, name, sizeof(name)) < 0)
        return 1;
    if (idx >= COSMO_KEY_SLOTS || k.slot[idx].kind == COSMO_KEYSLOT_EMPTY) {
        fprintf(stderr, "cosmo-key: slot %u of %s is empty\n", idx, name);
        return 1;
    }
    uint8_t master[COSMO_KEY_SIZE];
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "a passphrase for %s: ", name);
    if (ask_open(&k, prompt, master) < 0)
        return 1;
    struct cosmo_keyslot empty;
    memset(&empty, 0, sizeof(empty));
    int rc = fskey_slot(id, idx, master, &empty);
    int err = errno;
    secret_wipe(master, sizeof(master));
    if (rc < 0) {
        fprintf(stderr, "cosmo-key: slot %u of %s not removed: %s\n", idx, name,
                err == EBUSY ? "it is the last one" : strerror(err));
        return 1;
    }
    printf("cosmo-key: slot %u of %s removed\n", idx, name);
    return 0;
}

static int bench(int argc, char **argv)
{
    struct argon2_params p = { .t_cost = KEYSLOT_T_DEFAULT, .m_cost_kib = KEYSLOT_M_DEFAULT,
                               .lanes = KEYSLOT_LANES_DEFAULT };
    if (argc > 2)
        p.m_cost_kib = (uint32_t)strtoul(argv[2], NULL, 10);
    if (argc > 3)
        p.t_cost = (uint32_t)strtoul(argv[3], NULL, 10);
    uint8_t out[32];
    double t0 = now();
    int rc = argon2id(&p, "passphrase", 10, "0123456789abcdef", 16, out, sizeof(out));
    double t = now() - t0;
    if (rc) {
        fprintf(stderr, "cosmo-key: argon2id %u KiB, %u passes: %s\n", p.m_cost_kib, p.t_cost,
                rc == -2 ? "out of memory" : "bad parameters");
        return 1;
    }
    printf("cosmo-key: argon2id %u KiB, %u passes: %.3f s\n", p.m_cost_kib, p.t_cost, t);
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: cosmo-key list [PATH]\n"
                    "       cosmo-key unlock [--boot] DEVICE\n"
                    "       cosmo-key add [--passes N] [--memory KIB] [PATH]\n"
                    "       cosmo-key change [--passes N] [--memory KIB] [PATH]\n"
                    "       cosmo-key remove SLOT [PATH]\n"
                    "       cosmo-key bench [KIB [PASSES]]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    if (strcmp(cmd, "bench") == 0)
        return bench(argc, argv);
    if (strcmp(cmd, "unlock") == 0) {
        bool boot = argc == 4 && strcmp(argv[2], "--boot") == 0;
        if (argc != 3 + boot)
            return usage();
        return unlock(argv[2 + boot], boot);
    }
    if (strcmp(cmd, "list") == 0) {
        if (argc > 3)
            return usage();
        return list(argc == 3 ? argv[2] : "/");
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3 || argc > 4)
            return usage();
        char *end;
        unsigned long idx = strtoul(argv[2], &end, 10);
        if (*end != '\0' || end == argv[2])
            return usage();
        return remove_slot((unsigned)idx, argc == 4 ? argv[3] : "/");
    }
    if (strcmp(cmd, "add") == 0 || strcmp(cmd, "change") == 0) {
        uint32_t t = KEYSLOT_T_DEFAULT, m = KEYSLOT_M_DEFAULT;
        const char *path = "/";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--passes") == 0 && i + 1 < argc)
                t = (uint32_t)strtoul(argv[++i], NULL, 10);
            else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc)
                m = (uint32_t)strtoul(argv[++i], NULL, 10);
            else if (argv[i][0] != '-' && i == argc - 1)
                path = argv[i];
            else
                return usage();
        }
        if (t < 1 || m < 8 * KEYSLOT_LANES_DEFAULT)
            return usage();
        return add_or_change(strcmp(cmd, "change") == 0, path, t, m);
    }
    return usage();
}
