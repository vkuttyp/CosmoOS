/*
 * cosmo-upgrade - move an installed system to another release and back
 * (shell completeness S7; docs/userland/design.md, "Updating").
 *
 *   cosmo-upgrade [--check] [--stop-after=written|loader|set] DIR
 *   cosmo-upgrade [--check] [--stop-after=...] --fetch TAG|URL
 *   cosmo-upgrade --check
 *   cosmo-upgrade --rollback
 *   cosmo-upgrade --status
 *
 * DIR holds the release's kernel.elf, boot.tar, cmdline and manifest
 * (or cosmoos-ARCH-<name>), its SHA256SUMS and SHA256SUMS.sig. The sums
 * are checked against /etc/pkg/keys and the files against the sums, then
 * the manifest against the ring, before anything is written; the loader
 * inside boot.tar is checked with them. The ESP is
 * changed by one-sector switches of directory entries, the loader's entry
 * first and then \cosmo's, so every state an interruption can leave boots.
 * The previous set stays in \cosmo\prev. Then the root's base system
 * (bin/, sbin/, etc/, usr/ and the package repository) is brought from the
 * old archive to the new one. --rollback is the same in the other
 * direction. --fetch (roadmap 1.1 P6) first fetches the set over HTTPS
 * through /bin/cosmo-fetch from TAG under the release URL of
 * /etc/cosmo-release.conf, or from URL: SHA256SUMS and its signature,
 * checked before anything else is fetched, then the four files; then it
 * proceeds as with DIR. --check alone says whether that URL's newest
 * release is newer than the running one. Exit 0 done, 1 failed, 2 usage,
 * 3 refused (nothing written), 4 stopped by --stop-after.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <kernel/crypto.h>
#include <uapi/cosmo/fsctl.h>

#include "../../boot/protocol/cosmomanifest.h"
#include "../lib/fetch.h"
#include "../lib/rootsnap.h"
#include "blkdevs.h"
#include "relsig.h"
#include "upgrade_be.h"
#include "upgrade_fat.h"

#define KEYS_DIR RELSIG_KEYS_DIR
#define REPO_DIR "/var/lib/pkg/repo"
#define TMP_SUFFIX ".cosmo-upgrade"
#define SET_FILES 4   /* kernel.elf, boot.tar, cmdline, manifest */
#define MAX_MEMBERS 4096

#if defined(__x86_64__)
#define ARCH "x86_64"
#define LOADER "BOOTX64.EFI"
#elif defined(__aarch64__)
#define ARCH "aarch64"
#define LOADER "BOOTAA64.EFI"
#else
#error "cosmo-upgrade: unknown architecture"
#endif

enum { EXIT_DONE = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_REFUSED = 3, EXIT_STOPPED = 4 };

static const char *const g_set_names[SET_FILES] = { "kernel.elf", "boot.tar", "cmdline", "manifest" };

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("cosmo-upgrade: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

/* --- a boot set --------------------------------------------------------------------- */

struct set {
    uint8_t *file[SET_FILES];
    size_t len[SET_FILES];
};

static void set_free(struct set *s)
{
    for (int i = 0; i < SET_FILES; i++) {
        free(s->file[i]);
        s->file[i] = NULL;
    }
}

/* The manifest's own checks: signed by a key in the ring (when `ring`),
 * its layout, and each file's size and SHA-512. 0, or -1 with `err`. */
static int set_verify(const struct set *s, bool ring, char *keyid, char *err, size_t errlen)
{
    const uint8_t *m = s->file[3];
    snprintf(keyid, 17, "(none)");   /* until the trailer names one */
    if (s->len[3] != COSMO_MANIFEST_FILE_SIZE) {
        snprintf(err, errlen, "manifest: %zu bytes, not %zu", s->len[3], (size_t)COSMO_MANIFEST_FILE_SIZE);
        return -1;
    }
    struct cosmo_manifest man;
    struct cosmo_manifest_sig sig;
    memcpy(&man, m, sizeof(man));
    memcpy(&sig, m + sizeof(man), sizeof(sig));
    if (memcmp(sig.magic, COSMO_MANIFEST_SIG_MAGIC, 8) != 0 || sig.version != COSMO_MANIFEST_SIG_VERSION ||
        sig.algo != COSMO_MANIFEST_SIG_ED25519) {
        snprintf(err, errlen, "manifest: not signed (no COSMOSIG trailer of version 1, Ed25519)");
        return -1;
    }
    relsig_hex(sig.key_id, 8, keyid);
    if (ring) {
        const struct relsig_key *k = relsig_key_find(sig.key_id);
        if (k == NULL) {
            snprintf(err, errlen, "manifest: unknown signing key %s (not in %s)", keyid, KEYS_DIR);
            return -1;
        }
        if (!ed25519_verify(sig.sig, m, sizeof(man), k->pub)) {
            snprintf(err, errlen, "manifest: bad signature (key %s)", k->name);
            return -1;
        }
    }
    if (memcmp(man.magic, COSMO_MANIFEST_MAGIC, 8) != 0 || man.version != COSMO_MANIFEST_VERSION ||
        man.nfiles != COSMO_MANIFEST_FILES) {
        snprintf(err, errlen, "manifest: bad magic, version or file count");
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        const struct cosmo_manifest_file *f = &man.files[i];
        char name[COSMO_MANIFEST_NAME_MAX + 1];
        memcpy(name, f->name, COSMO_MANIFEST_NAME_MAX);
        name[COSMO_MANIFEST_NAME_MAX] = '\0';
        if (strcmp(name, g_set_names[i]) != 0) {
            snprintf(err, errlen, "manifest: file %d is '%s', not %s", i, name, g_set_names[i]);
            return -1;
        }
        uint8_t digest[SHA512_DIGEST_SIZE];
        if (f->size != s->len[i]) {
            snprintf(err, errlen, "%s does not match the manifest (%zu bytes, the manifest says %llu)", name,
                     s->len[i], (unsigned long long)f->size);
            return -1;
        }
        sha512(s->file[i], s->len[i], digest);
        if (memcmp(digest, f->sha512, sizeof(digest)) != 0) {
            snprintf(err, errlen, "%s does not match the manifest (SHA-512)", name);
            return -1;
        }
    }
    return 0;
}

static int read_whole(const char *path, uint8_t **out, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_type)) {
        close(fd);
        return -EINVAL;
    }
    uint8_t *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (buf == NULL) {
        close(fd);
        return -ENOMEM;
    }
    size_t done = 0;
    while (done < st.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
        if (n <= 0) {
            int e = n < 0 ? errno : EIO;
            close(fd);
            free(buf);
            return -e;
        }
        done += (size_t)n;
    }
    close(fd);
    *out = buf;
    *len = done;
    return 0;
}

static int set_read_dir(const char *dir, struct set *s)
{
    for (int i = 0; i < SET_FILES; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, g_set_names[i]);
        int rc = read_whole(path, &s->file[i], &s->len[i]);
        if (rc == -ENOENT) {
            snprintf(path, sizeof(path), "%s/cosmoos-" ARCH "-%s", dir, g_set_names[i]);
            rc = read_whole(path, &s->file[i], &s->len[i]);
        }
        if (rc < 0) {
            fprintf(stderr, "cosmo-upgrade: %s/%s: %s\n", dir, g_set_names[i], strerror(-rc));
            return -1;
        }
    }
    return 0;
}

/*
 * The release's SHA256SUMS, signed by a key in the ring (SHA256SUMS.sig,
 * relsig.h), and the four files of the set against it, under the names
 * the release gives them. This is what a release asset is checked by
 * before anything else reads it; the manifest then binds the set to the
 * key the loader trusts. 0, or -1 with `err`.
 */
static int set_check_sums(const char *dir, const struct set *s, char keyid[17], char *err, size_t errlen)
{
    snprintf(keyid, 17, "(none)");
    uint8_t *sums = NULL, *sig = NULL;
    size_t slen = 0, glen = 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/SHA256SUMS", dir);
    int rc = read_whole(path, &sums, &slen);
    if (rc == 0) {
        snprintf(path, sizeof(path), "%s/SHA256SUMS.sig", dir);
        rc = read_whole(path, &sig, &glen);
    }
    if (rc < 0) {
        snprintf(err, errlen, "%s: %s (a release is checked against its signed SHA256SUMS)", path, strerror(-rc));
        free(sums);
        return -1;
    }
    int out = relsig_sums_verify(sums, slen, sig, glen, keyid, err, errlen);
    for (int i = 0; i < SET_FILES && out == 0; i++) {
        char name[64];
        uint8_t want[RELSIG_SHA256_SIZE], got[RELSIG_SHA256_SIZE];
        snprintf(name, sizeof(name), "cosmoos-" ARCH "-%s", g_set_names[i]);
        out = relsig_sums_lookup(sums, slen, name, want, err, errlen);
        if (out == 0) {
            relsig_sha256(s->file[i], s->len[i], got);
            if (memcmp(got, want, sizeof(got)) != 0) {
                snprintf(err, errlen, "%s does not match SHA256SUMS", name);
                out = -1;
            }
        }
    }
    free(sums);
    free(sig);
    return out;
}

/* --- the archive ---------------------------------------------------------------------- */

struct member {
    char name[256];
    char link[101];
    bool is_link;
    const uint8_t *data;
    size_t size;
};

static uint64_t octal(const uint8_t *f, size_t n, bool *ok)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && f[i] == ' ')
        i++;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(f[i] - '0');
    for (; i < n; i++)
        if (f[i] != '\0' && f[i] != ' ')
            *ok = false;
    return v;
}

/* Does `name` have a component that is exactly ".."? */
static bool has_dotdot(const char *name)
{
    for (const char *c = name; *c;) {
        size_t n = strcspn(c, "/");
        if (n == 2 && c[0] == '.' && c[1] == '.')
            return true;
        c += n;
        while (*c == '/')
            c++;
    }
    return false;
}

/* The archive's regular files and links (mkbootarchive.py's ustar). The
 * count, or -1 with `err`. */
static int archive_index(const uint8_t *buf, size_t len, struct member **out, char *err, size_t errlen)
{
    struct member *m = calloc(MAX_MEMBERS, sizeof(*m));
    if (m == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int n = 0;
    for (size_t pos = 0; pos + 512 <= len;) {
        const uint8_t *h = buf + pos;
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++)
            zero = h[i] == 0;
        if (zero)
            break;
        bool ok = memcmp(h + 257, "ustar", 5) == 0;
        uint64_t sum = octal(h + 148, 8, &ok), want = 0;
        for (int i = 0; i < 512; i++)
            want += (i >= 148 && i < 156) ? ' ' : h[i];
        uint64_t size = octal(h + 124, 12, &ok);
        size_t padded = (size_t)((size + 511) / 512 * 512);
        if (!ok || sum != want || size > len || pos + 512 + padded > len || n == MAX_MEMBERS) {
            snprintf(err, errlen, "boot.tar: bad member at offset %zu", pos);
            free(m);
            return -1;
        }
        char type = (char)h[156];
        if (type == '0' || type == '\0' || type == '2') {
            struct member *e = &m[n++];
            char prefix[156] = "";
            memcpy(prefix, h + 345, 155);
            char name[101] = "";
            memcpy(name, h, 100);
            snprintf(e->name, sizeof(e->name), "%s%s%s", prefix, prefix[0] ? "/" : "", name);
            e->is_link = type == '2';
            memcpy(e->link, h + 157, 100);
            e->link[100] = '\0';
            e->data = buf + pos + 512;
            e->size = (size_t)size;
            if (e->name[0] == '/' || has_dotdot(e->name)) {
                snprintf(err, errlen, "boot.tar: member name refused: %s", e->name);
                free(m);
                return -1;
            }
        }
        pos += 512 + padded;
    }
    *out = m;
    return n;
}

static const struct member *member_find(const struct member *m, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(m[i].name, name) == 0)
            return &m[i];
    return NULL;
}

/* --- the ESP ------------------------------------------------------------------------- */

/*
 * The layout cosmo-upgrade keeps (docs/userland/design.md, "Updating"):
 * \cosmo holds the set, the loader the set came with (\cosmo\<loader>)
 * and prev; \cosmo\prev holds the previous set and its loader; the loader
 * the firmware starts, \EFI\BOOT\<loader>, is a copy of one of them in
 * clusters of its own. A set's loader moves with the set, in the same
 * one-sector switch, so nothing an interruption leaves can lose it.
 */
struct esp {
    struct fat f;
    const struct cosmo_blkctl_dev *dev;
    struct fat_ent boot;            /* \EFI\BOOT\<loader> */
    uint32_t cosmo;                 /* \cosmo's cluster */
    uint64_t lba;                   /* the sector holding \cosmo's entries */
    struct fat_ent set[SET_FILES];  /* \cosmo\... */
    bool has_sloader;
    struct fat_ent sloader;         /* \cosmo\<loader> (none on an ESP no upgrade has touched) */
    bool has_prev;
    struct fat_ent prev;            /* \cosmo\prev */
    bool prev_whole;                /* prev holds a set and its loader */
    struct fat_ent pset[SET_FILES];
    struct fat_ent ploader;
    bool has_be;
    struct fat_ent be;              /* \cosmo\be, the boot environments (roadmap 1.2 B4) */
};

static int64_t g_now;

static int esp_find(const struct cosmo_blkctl_dev **out)
{
    int rc = blkdevs_list();
    if (rc < 0) {
        fprintf(stderr, "cosmo-upgrade: /dev/blkctl: %s\n", strerror(-rc));
        return -1;
    }
    const struct cosmo_blkctl_dev *esp = NULL;
    for (unsigned i = 0; i < g_nblkdevs; i++)
        if ((g_blkdevs[i].flags & (COSMO_BLKCTL_BOOT | COSMO_BLKCTL_PART)) == (COSMO_BLKCTL_BOOT | COSMO_BLKCTL_PART))
            esp = &g_blkdevs[i];
    if (esp == NULL) {
        fprintf(stderr, "cosmo-upgrade: no partition is flagged BOOT (the one the loader was read from)\n");
        return -1;
    }
    /* The root must be on the same disk: this is an installed system, not
     * the live image. */
    FILE *mf = fopen("/proc/mounts", "r");
    char line[512], root[64] = "";
    while (mf != NULL && fgets(line, sizeof(line), mf) != NULL) {
        char *sp = strchr(line, ' ');
        if (sp && strncmp(sp, " / ", 3) == 0 && strncmp(line, "/dev/", 5) == 0) {
            *sp = '\0';
            snprintf(root, sizeof(root), "%s", line + 5);
        }
    }
    if (mf != NULL)
        fclose(mf);
    const struct cosmo_blkctl_dev *r = root[0] ? blkdevs_find(root) : NULL;
    if (r == NULL || strcmp(r->disk, esp->disk) != 0) {
        fprintf(stderr, "cosmo-upgrade: the root is not on %s's disk: not an installed system\n", esp->name);
        return -1;
    }
    *out = esp;
    return 0;
}

static bool is_dot(const struct fat_ent *e)
{
    return e->raw[0] == '.' && (e->raw[1] == ' ' || (e->raw[1] == '.' && e->raw[2] == ' '));
}

/* The four set files, the loader and prev among a directory's entries;
 * anything else is refused. */
static int find_set(const struct fat_ent *ents, int n, struct fat_ent set[SET_FILES], struct fat_ent *loader,
                    bool *has_loader, struct fat_ent *prev, bool *has_prev, struct fat_ent *be, bool *has_be,
                    char *err, size_t errlen)
{
    uint8_t names[SET_FILES][11], lname[11], pname[11], bname[11], nt;
    for (int i = 0; i < SET_FILES; i++)
        fat_name83(g_set_names[i], names[i], &nt);
    fat_name83(LOADER, lname, &nt);
    fat_name83("prev", pname, &nt);
    fat_name83("be", bname, &nt);
    bool seen[SET_FILES] = { false };
    *has_loader = false;
    if (has_prev)
        *has_prev = false;
    if (has_be)
        *has_be = false;
    for (int j = 0; j < n; j++) {
        const struct fat_ent *e = &ents[j];
        bool known = is_dot(e);
        for (int i = 0; i < SET_FILES && !known; i++)
            if (memcmp(e->raw, names[i], 11) == 0 && !(e->raw[11] & FAT_ATTR_DIR)) {
                set[i] = *e;
                seen[i] = known = true;
            }
        if (!known && memcmp(e->raw, lname, 11) == 0 && !(e->raw[11] & FAT_ATTR_DIR)) {
            *loader = *e;
            *has_loader = known = true;
        }
        if (!known && has_prev && memcmp(e->raw, pname, 11) == 0 && (e->raw[11] & FAT_ATTR_DIR)) {
            *prev = *e;
            *has_prev = known = true;
        }
        if (!known && has_be && memcmp(e->raw, bname, 11) == 0 && !(e->raw[11] & FAT_ATTR_DIR)) {
            *be = *e;
            *has_be = known = true;
        }
        if (!known) {
            snprintf(err, errlen, "unexpected entry '%.11s' in the set's directory", (const char *)e->raw);
            return -1;
        }
    }
    for (int i = 0; i < SET_FILES; i++)
        if (!seen[i]) {
            snprintf(err, errlen, "no %s in the set's directory", g_set_names[i]);
            return -1;
        }
    return 0;
}

static int esp_load(struct esp *e, char *err, size_t errlen)
{
    memset(e, 0, sizeof(*e));
    if (esp_find(&e->dev) < 0)
        return -1;
    if (fat_open(&e->f, e->dev, err, errlen) < 0)
        return -1;
    uint8_t n83[11], nt;
    struct fat_ent d;
    fat_name83("EFI", n83, &nt);
    if (fat_lookup(&e->f, e->f.root, n83, &d) != 1 || !(d.raw[11] & FAT_ATTR_DIR)) {
        snprintf(err, errlen, "%s: no \\EFI", e->dev->name);
        return -1;
    }
    fat_name83("BOOT", n83, &nt);
    if (fat_lookup(&e->f, d.cluster, n83, &d) != 1 || !(d.raw[11] & FAT_ATTR_DIR)) {
        snprintf(err, errlen, "%s: no \\EFI\\BOOT", e->dev->name);
        return -1;
    }
    fat_name83(LOADER, n83, &nt);
    if (fat_lookup(&e->f, d.cluster, n83, &e->boot) != 1) {
        snprintf(err, errlen, "%s: no \\EFI\\BOOT\\" LOADER, e->dev->name);
        return -1;
    }
    fat_name83("cosmo", n83, &nt);
    if (fat_lookup(&e->f, e->f.root, n83, &d) != 1 || !(d.raw[11] & FAT_ATTR_DIR)) {
        snprintf(err, errlen, "%s: no \\cosmo", e->dev->name);
        return -1;
    }
    e->cosmo = d.cluster;
    e->lba = fat_cluster_lba(&e->f, e->cosmo);
    /* \cosmo's entries in one sector, none of them long-name parts: the
     * switch is one write (docs/userland/design.md). */
    uint8_t sec[4096];
    if (fat_read_sector(&e->f, e->lba, sec) < 0) {
        snprintf(err, errlen, "%s: reading \\cosmo", e->dev->name);
        return -1;
    }
    for (unsigned i = 0; i < e->f.bps / FAT_DIRENT && sec[i * FAT_DIRENT] != 0; i++)
        if (sec[i * FAT_DIRENT] != 0xE5 && (sec[i * FAT_DIRENT + 11] & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
            snprintf(err, errlen, "%s: \\cosmo has long-name entries: cannot switch it in one write", e->dev->name);
            return -1;
        }
    struct fat_ent ents[32];
    int n = fat_list(&e->f, e->cosmo, ents, 32);
    if (n < 0) {
        snprintf(err, errlen, "%s: \\cosmo: %s", e->dev->name, strerror(-n));
        return -1;
    }
    char why[160];
    if (find_set(ents, n, e->set, &e->sloader, &e->has_sloader, &e->prev, &e->has_prev, &e->be, &e->has_be, why,
                 sizeof(why)) < 0) {
        snprintf(err, errlen, "%s: \\cosmo: %s", e->dev->name, why);
        return -1;
    }
    for (int j = 0; j < n; j++)
        if (ents[j].lba != e->lba) {
            snprintf(err, errlen, "%s: \\cosmo's entries span sectors: cannot switch them in one write",
                     e->dev->name);
            return -1;
        }
    if (e->has_prev) {
        n = fat_list(&e->f, e->prev.cluster, ents, 32);
        bool has_loader = false;
        e->prev_whole = n >= 0 && find_set(ents, n, e->pset, &e->ploader, &has_loader, NULL, NULL, NULL, NULL, why,
                                           sizeof(why)) == 0 && has_loader;
    }
    return 0;
}

static int esp_read(struct esp *e, const struct fat_ent *ent, uint8_t **out)
{
    int rc = fat_read_file(&e->f, ent->cluster, ent->size, out);
    if (rc < 0)
        fprintf(stderr, "cosmo-upgrade: %s: reading %.11s: %s\n", e->dev->name, (const char *)ent->raw,
                strerror(-rc));
    return rc;
}

static int esp_read_set(struct esp *e, const struct fat_ent ents[SET_FILES], struct set *s)
{
    for (int i = 0; i < SET_FILES; i++) {
        if (esp_read(e, &ents[i], &s->file[i]) < 0)
            return -1;
        s->len[i] = ents[i].size;
    }
    return 0;
}

/* A prev directory's one cluster: '.', '..', the set and its loader. */
static int write_prev_dir(struct esp *e, const struct fat_ent set[SET_FILES], const struct fat_ent *loader,
                          uint32_t *cluster)
{
    uint32_t cb = fat_cluster_bytes(&e->f);
    uint8_t *buf = calloc(1, cb);
    if (buf == NULL)
        return -ENOMEM;
    uint8_t dot[11], dotdot[11];
    memset(dot, ' ', 11);
    memset(dotdot, ' ', 11);
    dot[0] = dotdot[0] = dotdot[1] = '.';
    fat_make_ent(buf, dot, 0, FAT_ATTR_DIR, 0, 0, g_now);
    fat_make_ent(buf + 32, dotdot, 0, FAT_ATTR_DIR, e->cosmo, 0, g_now);
    for (int i = 0; i < SET_FILES; i++)
        memcpy(buf + 64 + 32 * i, set[i].raw, FAT_DIRENT);
    memcpy(buf + 64 + 32 * SET_FILES, loader->raw, FAT_DIRENT);
    int rc = fat_write_new(&e->f, buf, cb, cluster);
    if (rc == 0) {
        /* '.' names the cluster just allocated: the first sector again. */
        fat_ent_set(buf, *cluster, 0, g_now);
        rc = fat_write_sector(&e->f, fat_cluster_lba(&e->f, *cluster), buf);
    }
    free(buf);
    return rc;
}

/* One change to a directory sector: an existing entry (slot >= 0) gets a
 * new chain and size; slot -1 adds an entry named `name` in a free slot. */
struct edit {
    int slot;
    uint32_t cluster, size;
    const char *name;
    uint8_t attr;
};

/* Every edit in one sector write, then FLUSH: the switch. */
static int switch_sector(struct esp *e, uint64_t lba, struct edit *ed, int n)
{
    uint8_t sec[4096];
    int rc = fat_read_sector(&e->f, lba, sec);
    if (rc < 0)
        return rc;
    unsigned per = e->f.bps / FAT_DIRENT;
    for (int i = 0; i < n; i++) {
        if (ed[i].slot >= 0) {
            fat_ent_set(sec + ed[i].slot * FAT_DIRENT, ed[i].cluster, ed[i].size, g_now);
            continue;
        }
        /* A deleted slot, or the end marker when another slot follows it
         * in this sector (which becomes the end marker). */
        int slot = -1;
        for (unsigned j = 0; j < per && slot < 0; j++) {
            uint8_t c = sec[j * FAT_DIRENT];
            if (c == 0xE5 || (c == 0x00 && j + 1 < per))
                slot = (int)j;
        }
        if (slot < 0)
            return -ENOSPC;
        if (sec[slot * FAT_DIRENT] == 0x00)
            sec[(slot + 1) * FAT_DIRENT] = 0x00;
        uint8_t n83[11], nt;
        fat_name83(ed[i].name, n83, &nt);
        fat_make_ent(sec + slot * FAT_DIRENT, n83, nt, ed[i].attr, ed[i].cluster, ed[i].size, g_now);
        ed[i].slot = slot;
    }
    rc = fat_write_sector(&e->f, lba, sec);
    return rc < 0 ? rc : blkdevs_flush(e->dev);
}

static int sweep_commit(struct esp *e)
{
    int freed = fat_sweep(&e->f);
    if (freed < 0)
        return freed;
    int rc = fat_commit(&e->f);
    return rc < 0 ? rc : freed;
}

/* \EFI\BOOT\<loader> becomes a fresh copy of `data`: written, committed,
 * then its entry switched. The old copy is left for the sweep. */
static int switch_boot_loader(struct esp *e, const uint8_t *data, size_t len)
{
    uint32_t c;
    int rc = fat_write_new(&e->f, data, len, &c);
    if (rc == 0)
        rc = fat_commit(&e->f);
    if (rc < 0)
        return rc;
    struct edit ed = { (int)e->boot.slot, c, (uint32_t)len, NULL, 0 };
    return switch_sector(e, e->boot.lba, &ed, 1);
}

/* --- the root -------------------------------------------------------------------------- */

struct counts {
    unsigned replaced, added, removed, kept, same;
};

/* The path a member lands at on the root, or NULL for one that does not. */
static bool target_of(const char *name, char *out, size_t len)
{
    if (strncmp(name, "bin/", 4) == 0 || strncmp(name, "sbin/", 5) == 0 || strncmp(name, "etc/", 4) == 0 ||
        strncmp(name, "usr/", 4) == 0 || strncmp(name, "lib/", 4) == 0) {
        snprintf(out, len, "/%s", name);
        return true;
    }
    if (strncmp(name, "repo/", 5) == 0) {
        snprintf(out, len, REPO_DIR "/%s", name + 5);
        return true;
    }
    return false;
}

/* The mode the kernel gives an archive path (ramfs_populate_boot). */
static unsigned archive_mode(const char *name)
{
    if (strncmp(name, "bin/", 4) == 0 || strncmp(name, "sbin/", 5) == 0 || strncmp(name, "usr/bin/", 8) == 0 ||
        strncmp(name, "usr/sbin/", 9) == 0 || strncmp(name, "usr/libexec/", 12) == 0 || strncmp(name, "lib/", 4) == 0 ||
        strncmp(name, "usr/lib/", 8) == 0)
        return 0755;
    return 0644;
}

/* Does the root's `path` hold exactly member `m`? */
static bool holds(const char *path, const struct member *m)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return false;
    if (m->is_link) {
        char t[256];
        long n = S_ISLNK(st.st_type) ? readlink(path, t, sizeof(t) - 1) : -1;
        if (n < 0)
            return false;
        t[n] = '\0';
        return strcmp(t, m->link) == 0;
    }
    if (!S_ISREG(st.st_type) || st.st_size != m->size)
        return false;
    uint8_t *buf;
    size_t len;
    if (read_whole(path, &buf, &len) < 0)
        return false;
    bool same = len == m->size && memcmp(buf, m->data, len) == 0;
    free(buf);
    return same;
}

static void mkparents(const char *path)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/')
            continue;
        *s = '\0';
        mkdir(p, 0755);
        *s = '/';
    }
}

/* Write member `m` to `path`: a temporary beside it, renamed over it. */
static int put(const char *path, const struct member *m, unsigned mode)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s" TMP_SUFFIX, path);
    mkparents(path);
    unlink(tmp);
    if (m->is_link) {
        if (symlink(m->link, tmp) < 0)
            goto fail;
    } else {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (fd < 0)
            goto fail;
        for (size_t done = 0; done < m->size;) {
            ssize_t n = write(fd, m->data + done, m->size - done);
            if (n <= 0) {
                close(fd);
                goto fail;
            }
            done += (size_t)n;
        }
        if (fsync(fd) < 0 || close(fd) < 0)
            goto fail;
    }
    if (rename(tmp, path) < 0)
        goto fail;
    return 0;
fail:
    fprintf(stderr, "cosmo-upgrade: %s: %s\n", path, strerror(errno));
    unlink(tmp);
    return -1;
}

/* Bring the root from archive `from` to archive `to` (docs/userland/design.md,
 * "The root: the base system"). */
static int root_pass(const struct member *from, int nfrom, const struct member *to, int nto, struct counts *c)
{
    memset(c, 0, sizeof(*c));
    int failed = 0;
    for (int i = 0; i < nto; i++) {
        const struct member *m = &to[i];
        char path[1024];
        if (!target_of(m->name, path, sizeof(path)))
            continue;
        struct stat st;
        bool exists = lstat(path, &st) == 0;
        if (exists && holds(path, m)) {
            c->same++;
            continue;
        }
        unsigned mode = exists && S_ISREG(st.st_type) ? (st.st_mode & 07777) : archive_mode(m->name);
        const struct member *old = member_find(from, nfrom, m->name);
        if (strncmp(m->name, "etc/", 4) == 0 && exists && (old == NULL || !holds(path, old))) {
            /* Configuration someone changed: theirs stays, ours beside it. */
            char np[1100];
            snprintf(np, sizeof(np), "%s.new", path);
            if (!holds(np, m) && put(np, m, mode) < 0)
                failed++;
            c->kept++;
            continue;
        }
        if (put(path, m, mode) < 0)
            failed++;
        else if (exists)
            c->replaced++;
        else
            c->added++;
    }
    for (int i = 0; i < nfrom; i++) {
        const struct member *m = &from[i];
        char path[1024];
        if (strncmp(m->name, "etc/", 4) == 0 || member_find(to, nto, m->name) != NULL ||
            !target_of(m->name, path, sizeof(path)))
            continue;
        struct stat st;
        if (lstat(path, &st) < 0)
            continue;
        if (unlink(path) < 0) {
            fprintf(stderr, "cosmo-upgrade: removing %s: %s\n", path, strerror(errno));
            failed++;
        } else {
            c->removed++;
        }
    }
    sync();
    return failed ? -1 : 0;
}

static void root_set_mark(const uint8_t *manifest);

static int base_pass(const struct set *from, const struct set *to)
{
    char err[160];
    struct member *a = NULL, *b = NULL;
    int na = archive_index(from->file[1], from->len[1], &a, err, sizeof(err));
    int nb = na < 0 ? -1 : archive_index(to->file[1], to->len[1], &b, err, sizeof(err));
    if (na < 0 || nb < 0) {
        fprintf(stderr, "cosmo-upgrade: %s\n", err);
        free(a);
        return -1;
    }
    struct counts c;
    int rc = root_pass(a, na, b, nb, &c);
    say("root: %u files replaced, %u added, %u removed, %u unchanged, %u kept as changed here (the new one beside "
        "each as FILE.new)", c.replaced, c.added, c.removed, c.same, c.kept);
    free(a);
    free(b);
    if (rc == 0)
        root_set_mark(to->file[3]);   /* this root's base system is now `to`'s */
    return rc;
}

/* The loader the new archive carries: efi/<loader>. */
static const struct member *archive_loader(const struct set *s, struct member **idx)
{
    char err[160];
    int n = archive_index(s->file[1], s->len[1], idx, err, sizeof(err));
    if (n < 0)
        return NULL;
    return member_find(*idx, n, "efi/" LOADER);
}

/* Does loader image `ldr` trust the key that signed set `s`? The loader's
 * ring is a table of { id[8], pub[32], name } (kernel/keyring.h), built
 * from KEYRING_PUBS with revoked keys refused (scripts/gen-keyring.py), so
 * an id followed by a public key that verifies the set's manifest is that
 * key in the loader's ring. */
static bool loader_trusts(const uint8_t *ldr, size_t len, const struct set *s)
{
    struct cosmo_manifest_sig sig;
    memcpy(&sig, s->file[3] + sizeof(struct cosmo_manifest), sizeof(sig));
    for (size_t i = 0; i + 8 + 32 <= len; i++)
        if (ldr[i] == sig.key_id[0] && memcmp(ldr + i, sig.key_id, 8) == 0 &&
            ed25519_verify(sig.sig, s->file[3], sizeof(struct cosmo_manifest), ldr + i + 8))
            return true;
    return false;
}

/* --- the commands --------------------------------------------------------------------- */

/*
 * The pending marker: written before a set switch, naming (by its
 * manifest's SHA-512) the set being switched to, and removed when the
 * root pass after it is done. A run that finds it naming the current set
 * finishes that run -- the loader made the set's, the root brought to the
 * set -- rather than starting another, so an interrupted --rollback is not
 * turned into a step forward by running it again.
 */
#define STATE_DIR "/var/lib/cosmo-upgrade"
#define PENDING STATE_DIR "/pending"

static void manifest_tag(const uint8_t *manifest, char out[129])
{
    uint8_t d[SHA512_DIGEST_SIZE];
    sha512(manifest, COSMO_MANIFEST_FILE_SIZE, d);
    relsig_hex(d, sizeof(d), out);
}

static int pending_set(const uint8_t *manifest)
{
    char tag[129];
    manifest_tag(manifest, tag);
    mkdir("/var/lib", 0755);
    mkdir(STATE_DIR, 0755);
    int fd = open(PENDING TMP_SUFFIX, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, tag, 128) != 128 || fsync(fd) < 0 || close(fd) < 0 ||
        rename(PENDING TMP_SUFFIX, PENDING) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s\n", PENDING, strerror(errno));
        return -1;
    }
    sync();
    return 0;
}

/* True when the marker names this manifest's set. */
static bool pending_is(const uint8_t *manifest)
{
    char tag[129], have[129] = "";
    manifest_tag(manifest, tag);
    int fd = open(PENDING, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, have, 128);
    close(fd);
    return n == 128 && memcmp(have, tag, 128) == 0;
}

static void pending_clear(void)
{
    unlink(PENDING);
    sync();
}

static int oldest_format_mark(const struct set *a, const struct set *b);

static int fail(const char *what, int rc)
{
    fprintf(stderr, "cosmo-upgrade: %s: %s\n", what, strerror(rc < 0 ? -rc : rc));
    return EXIT_FAILED;
}

/* --- boot environments (roadmap 1.2 B4; docs/userland/design.md, "Boot environments") ---- *
 *
 * An environment is a root snapshot paired with the boot set it ran on:
 * an upgrade takes be-<release> of the root before it writes anything,
 * and the switch to the new set records the pair in \cosmo\be in the same
 * one-sector write (upgrade_be.h). --rollback returns both halves: the
 * root through cosmofs's pending rollback (applied at the next mount) and
 * the set by the switch -- the intent in \cosmo\be first, then the
 * record, then the set, so every crash leaves a pairing --repair can
 * finish. The root records which set's base system it holds (ROOT_SET),
 * which is how a mixed state is told: a snapshot carries its own.
 */
#define ROOT_SET STATE_DIR "/root-set"

/* ROOT_SET names `manifest`'s set: the base system the root now has. */
static void root_set_mark(const uint8_t *manifest)
{
    char tag[129];
    manifest_tag(manifest, tag);
    mkdir("/var/lib", 0755);
    mkdir(STATE_DIR, 0755);
    int fd = open(ROOT_SET TMP_SUFFIX, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, tag, 128) != 128 || write(fd, "\n", 1) != 1 || fsync(fd) < 0 || close(fd) < 0 ||
        rename(ROOT_SET TMP_SUFFIX, ROOT_SET) < 0)
        fprintf(stderr, "cosmo-upgrade: %s: %s\n", ROOT_SET, strerror(errno));
}

/* The set the root's base system came from, or "" when it predates the
 * mark (a root upgraded only by v1.0.7 or older). */
static void root_set_read(char out[129])
{
    out[0] = '\0';
    int fd = open(ROOT_SET, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, out, 128);
    close(fd);
    out[n == 128 ? 128 : 0] = '\0';
}

static void uuid_hex(const uint8_t u[16], char out[33])
{
    relsig_hex(u, 16, out);
}

/* \cosmo\be as read: 0, with no environments for an ESP without one;
 * -1 (said) for one that cannot be read or does not parse, which is not
 * taken for an empty one: what it paired is unknown, not absent. */
static int be_load(struct esp *e, struct be_file *b)
{
    memset(b, 0, sizeof(*b));
    if (!e->has_be)
        return 0;
    uint8_t *text;
    if (fat_read_file(&e->f, e->be.cluster, e->be.size, &text) < 0) {
        fprintf(stderr, "cosmo-upgrade: \\cosmo\\be cannot be read\n");
        return -1;
    }
    char err[120];
    int rc = be_parse((const char *)text, e->be.size, b, err, sizeof(err));
    free(text);
    if (rc < 0) {
        fprintf(stderr, "cosmo-upgrade: \\cosmo\\be: %s\n", err);
        memset(b, 0, sizeof(*b));
        return -1;
    }
    return 0;
}

/* Whether a set's own cosmo-upgrade reads \cosmo\be (upgrade_be.h). */
static bool set_reads_be(const struct set *s)
{
    char err[160];
    struct member *m = NULL;
    int n = archive_index(s->file[1], s->len[1], &m, err, sizeof(err));
    const struct member *x = n < 0 ? NULL : member_find(m, n, "sbin/cosmo-upgrade");
    bool yes = x != NULL && !x->is_link && be_aware(x->data, x->size);
    free(m);
    return yes;
}

#define BE_DAMAGED "cosmo-upgrade --prune writes it again, empty, keeping every snapshot; nothing written"

/* `b` written to free clusters, named by nothing yet: the edit that names
 * it goes into the caller's switch. */
static int be_stage(struct esp *e, const struct be_file *b, struct edit *ed)
{
    char text[BE_MAX * 200 + 200];
    size_t len = be_format(b, text, sizeof(text));
    if (len == 0)
        return -ENOSPC;
    uint32_t cl;
    int rc = fat_write_new(&e->f, text, len, &cl);
    if (rc < 0)
        return rc;
    *ed = (struct edit){ e->has_be ? (int)e->be.slot : -1, cl, (uint32_t)len, "be", FAT_ATTR_ARCHIVE };
    return 0;
}

struct snaps {
    struct cosmo_fsctl_snap_pool pool;
    struct cosmo_fsctl_snap *v;
    unsigned n;
};

static bool snaps_taken(const char *name, void *arg)
{
    const struct snaps *s = arg;
    return rootsnap_find(s->v, s->n, name) != NULL;
}

/* The environment's snapshot, when it is on the root with its uuid. */
static const struct cosmo_fsctl_snap *be_snapshot(const struct snaps *s, const struct be_env *env)
{
    const struct cosmo_fsctl_snap *x = env ? rootsnap_find(s->v, s->n, env->name) : NULL;
    char u[33];
    if (x == NULL)
        return NULL;
    uuid_hex(x->uuid, u);
    return strcmp(u, env->uuid) == 0 ? x : NULL;
}

static int running_tag(char *out, size_t n);

/* The end of a run that switched to the current set: \EFI\BOOT's loader
 * made the set's, the root brought from the previous set to it. */
/* As finish, and when `root` is false the root is left alone: a
 * boot-environment rollback replaces it whole at the next mount. */
static int finish_ex(struct esp *e, const struct set *cur, bool root)
{
    if (e->has_sloader) {
        uint8_t *sl, *bl;
        if (esp_read(e, &e->sloader, &sl) < 0 || esp_read(e, &e->boot, &bl) < 0)
            return EXIT_FAILED;
        if (e->sloader.size != e->boot.size || memcmp(sl, bl, e->boot.size) != 0) {
            int rc = switch_boot_loader(e, sl, e->sloader.size);
            if (rc == 0)
                rc = sweep_commit(e);
            if (rc < 0)
                return fail("switching the loader", rc);
            say("loader switched: \\EFI\\BOOT\\" LOADER " is the set's own");
        }
        free(sl);
        free(bl);
    }
    if (!root)
        return EXIT_DONE;
    if (!e->prev_whole) {
        if (pending_is(cur->file[3])) {
            /* The switch wrote prev with the set; without it the root
             * pass has nothing to start from. The marker stays. */
            fprintf(stderr, "cosmo-upgrade: a root pass is pending but \\cosmo\\prev is incomplete; the root is "
                            "not brought to the current set\n");
            return EXIT_FAILED;
        }
        say("no previous set in \\cosmo\\prev: the root is left as it is");
        return EXIT_DONE;
    }
    struct set prev = { 0 };
    char key[17], err[200];
    if (esp_read_set(e, e->pset, &prev) < 0)
        return EXIT_FAILED;
    if (set_verify(&prev, false, key, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: \\cosmo\\prev: %s\n", err);
        return EXIT_FAILED;
    }
    (void)oldest_format_mark(cur, &prev);   /* a run finishing another's: the record as the switch left it */
    int rc = base_pass(&prev, cur);
    set_free(&prev);
    if (rc < 0)
        return EXIT_FAILED;
    pending_clear();
    return EXIT_DONE;
}

static int finish(struct esp *e, const struct set *cur)
{
    return finish_ex(e, cur, true);
}

/* The environment an upgrade is taking, named before the snapshot and
 * removed once the switch pairs it: the one snapshot a sweep may delete
 * for being unpaired. Nothing else is judged by its absence from
 * \cosmo\be, so a lost or rewritten file loses no environment. */
#define BE_TAKING STATE_DIR "/be-taking"

static void be_taking_read(char out[BE_NAME_MAX + 1])
{
    out[0] = '\0';
    int fd = open(BE_TAKING, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, out, BE_NAME_MAX);
    close(fd);
    out[n > 0 ? n : 0] = '\0';
    char *nl = strchr(out, '\n');
    if (nl)
        *nl = '\0';
}

static int be_taking_mark(const char *name)
{
    int fd = open(BE_TAKING, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        mkdir(STATE_DIR, 0755);
        fd = open(BE_TAKING, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    bool ok = fd >= 0 && write(fd, name, strlen(name)) == (ssize_t)strlen(name) && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    sync();
    return ok ? 0 : -1;
}

static void be_taking_clear(void)
{
    unlink(BE_TAKING);
    sync();
}

static bool be_paired(const struct be_file *b, const char *name)
{
    for (int k = 0; k < b->n; k++)
        if (strcmp(b->env[k].name, name) == 0)
            return true;
    return false;
}

/* The environment an interrupted upgrade took and never paired goes,
 * unless the kernel keeps it for a rollback; with `prune`, the ones
 * beyond BE_KEEP, oldest first, from both. */
static void be_sweep(struct rootsnap *rs, struct snaps *s, struct be_file *b, bool prune)
{
    char taking[BE_NAME_MAX + 1];
    be_taking_read(taking);
    const struct cosmo_fsctl_snap *x = taking[0] ? rootsnap_find(s->v, s->n, taking) : NULL;
    const uint32_t kept = COSMO_FSCTL_SNAP_F_TARGET | COSMO_FSCTL_SNAP_F_SAFETY | COSMO_FSCTL_SNAP_F_HELD |
                          COSMO_FSCTL_SNAP_F_LIVE;
    if (x != NULL && !be_paired(b, taking) && !(x->flags & kept) && rootsnap_delete(rs, taking) == 0)
        say("deleted %s, an environment an interrupted run took and never paired", taking);
    if (taking[0])
        be_taking_clear();
    while (prune && b->n > BE_KEEP) {
        char nm[BE_NAME_MAX + 1];
        snprintf(nm, sizeof(nm), "%s", b->env[0].name);
        if (rootsnap_find(s->v, s->n, nm) != NULL && rootsnap_delete(rs, nm) < 0) {
            say("keeping environment %s: %s", nm, errno == EBUSY ? "a rollback holds it" : strerror(errno));
            break;
        }
        say("deleted environment %s (the newest %d are kept)", nm, BE_KEEP);
        be_drop(b, 0);
    }
}

/* --status's half for the root: the pairing, and a rollback the last
 * mount did not apply (a refused one: no room, a bad record; backlog
 * CFS-20) -- said here and at boot by init. */
static void status_envs(struct esp *e)
{
    struct be_file be;
    if (be_load(e, &be) < 0)
        say("boot environments: unknown (%s)", "cosmo-upgrade --prune writes \\cosmo\\be again, empty, keeping "
            "every snapshot");
    struct set cur = { 0 };
    char curtag[129] = "", rootset[129];
    if (esp_read_set(e, e->set, &cur) == 0)
        manifest_tag(cur.file[3], curtag);
    set_free(&cur);
    root_set_read(rootset);
    say("boot environments: %d paired%s%s", be.n, be.intent[0] ? "; intent: " : "",
        !be.intent[0] ? "" : strcmp(be.intent, curtag) == 0 ? "the current set" : "another set");
    if (be.intent[0] && rootset[0])
        say("the root %s the intended set's base system%s", strcmp(rootset, be.intent) == 0 ? "has" : "does not have",
            strcmp(rootset, be.intent) == 0 && strcmp(curtag, be.intent) == 0 ? "" : ": cosmo-upgrade --repair");
    struct rootsnap rs;
    struct snaps sn = { 0 };
    if (rootsnap_open(&rs, "/") < 0 || rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0)
        return;
    for (unsigned i = 0; i < sn.n; i++)
        if (sn.v[i].flags & COSMO_FSCTL_SNAP_F_TARGET)
            say("a rollback of the root to %s is pending%s", sn.v[i].name,
                "; if this boot's mount should have applied it, it refused: see the kernel log and fsctl check /");
    free(sn.v);
    rootsnap_close(&rs);
}

static int status(void)
{
    struct esp e;
    char err[200], keyid[17];
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s\n", err);
        return EXIT_FAILED;
    }
    struct set cur = { 0 };
    if (esp_read_set(&e, e.set, &cur) < 0)
        return EXIT_FAILED;
    int v = set_verify(&cur, true, keyid, err, sizeof(err));
    say("current: %s, kernel.elf %zu bytes, boot.tar %zu bytes, key %s%s%s", e.dev->name, cur.len[0], cur.len[1],
        keyid, v == 0 ? "" : ": ", v == 0 ? "" : err);
    if (pending_is(cur.file[3]))
        say("an interrupted run is pending: run cosmo-upgrade again (with the same DIR, or --rollback) to finish it");
    set_free(&cur);
    if (!e.prev_whole) {
        say("prev: %s", e.has_prev ? "incomplete" : "none");
        status_envs(&e);
        return EXIT_DONE;
    }
    struct set prev = { 0 };
    if (esp_read_set(&e, e.pset, &prev) < 0)
        return EXIT_FAILED;
    v = set_verify(&prev, true, keyid, err, sizeof(err));
    say("prev: kernel.elf %zu bytes, boot.tar %zu bytes, key %s%s%s", prev.len[0], prev.len[1], keyid,
        v == 0 ? "" : ": ", v == 0 ? "" : err);
    set_free(&prev);
    status_envs(&e);
    return EXIT_DONE;
}

/*
 * Whether the running kernel's /dev/fsctl is older than version 3: it
 * takes the 16-byte command of versions 1 and 2. Version 3 came with
 * format 12, so such a kernel reads format 11 at most -- and since it
 * mounted the root, the root is not 12.
 */
static bool fsctl_predates_v3(int fd)
{
    struct {
        uint16_t version, op;
        uint32_t flags;
        uint64_t mount_id;
    } old;
    for (uint16_t v = 1; v <= 2; v++) {
        memset(&old, 0, sizeof(old));
        old.version = v;
        old.op = COSMO_FSCTL_LIST;
        if (write(fd, &old, sizeof(old)) == (long)sizeof(old))
            return true;
    }
    return false;
}

/*
 * The cosmofs format the root is at: 0 when the root is not a cosmofs
 * with snapshots (the live image) or the running kernel predates format
 * 12, -1 when it cannot be told. A pool is
 * raised from 11 to 12 by its first snapshot
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version
 * 12"), and a kernel that reads 11 at most cannot mount it; the gate
 * fails closed, so not knowing refuses the switch.
 */
static int root_format(void)
{
    int fd = open("/dev/fsctl", O_RDWR);
    if (fd < 0)
        return -1;
    struct cosmo_fsctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_FSCTL_VERSION;
    cmd.op = COSMO_FSCTL_LIST;
    /* The listing is read whole: a buffer too small is -ERANGE, and the
     * next try is twice the size. */
    uint8_t *buf = NULL;
    long n = -1;
    for (size_t room = 64, tries = 0; tries < 12; tries++, room *= 2) {
        free(buf);
        size_t cap = sizeof(struct cosmo_fsctl_result) + room * sizeof(struct cosmo_fsctl_mount);
        buf = malloc(cap);
        if (buf == NULL)
            break;
        if (write(fd, &cmd, sizeof(cmd)) != (long)sizeof(cmd)) {
            int e = errno;
            if (e == EINVAL && fsctl_predates_v3(fd)) {
                free(buf);
                close(fd);
                return 0;
            }
            errno = e;
            break;
        }
        n = read(fd, buf, cap);
        if (n >= 0 || errno != ERANGE)
            break;
    }
    int format = -1;
    if (buf != NULL && n >= (long)sizeof(struct cosmo_fsctl_result)) {
        const struct cosmo_fsctl_result *h = (const struct cosmo_fsctl_result *)buf;
        const struct cosmo_fsctl_mount *m = (const struct cosmo_fsctl_mount *)(buf + sizeof(*h));
        unsigned count = h->count;
        size_t fits = ((size_t)n - sizeof(*h)) / sizeof(*m);
        if (count > fits)
            count = (unsigned)fits;   /* never past what was read */
        format = 0;
        for (unsigned i = 0; i < count; i++) {
            if (strcmp(m[i].path, "/") != 0 || !(m[i].caps & COSMO_FSCTL_CAP_SNAP))
                continue;
            cmd.op = COSMO_FSCTL_SNAP_LIST;
            cmd.flags = COSMO_FSCTL_F_NO_SIZES;
            cmd.mount_id = m[i].id;
            /* Only the pool record is wanted: a page of one snapshot. */
            size_t cap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_snap_pool) +
                         COSMO_FSCTL_SNAP_PAGE * sizeof(struct cosmo_fsctl_snap);
            uint8_t *lb = malloc(cap);
            format = -1;
            if (lb != NULL && write(fd, &cmd, sizeof(cmd)) == (long)sizeof(cmd) &&
                read(fd, lb, cap) >= (long)(sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_snap_pool)))
                format = (int)((const struct cosmo_fsctl_snap_pool *)(lb + sizeof(struct cosmo_fsctl_result)))->format;
            free(lb);
            break;
        }
    }
    free(buf);
    close(fd);
    return format;
}

/* The newest cosmofs format a kernel image reads: the marker it carries
 * (cosmofs_core.c, cosmofs_init), or 11 for one from before format 12. */
static unsigned kernel_reads(const uint8_t *elf, size_t len)
{
    static const char tag[] = "cosmofs-format-max=";
    size_t tl = sizeof(tag) - 1;
    for (size_t i = 0; i + tl + 1 < len; i++) {
        if (elf[i] != 'c' || memcmp(elf + i, tag, tl) != 0)
            continue;
        unsigned v = 0;
        for (size_t k = i + tl; k < len && elf[k] >= '0' && elf[k] <= '9' && v < 1000; k++)
            v = v * 10 + (unsigned)(elf[k] - '0');
        return v;
    }
    return 11;
}

/*
 * OLDEST_FORMAT: the newest cosmofs format every boot set kept on the ESP
 * can read -- the least of what the current set's and \cosmo\prev's
 * kernels read -- rewritten after each switch. pkg reads it before a
 * transaction would raise a format-11 root to 12 (B1's lazy raise): a raise
 * that a kept set could not mount would take away the way back to it
 * (roadmap 1.2 B4; docs/pkg/design.md, "Transactions"). cosmo-install
 * writes "none": a fresh install keeps no other set.
 */
#define OLDEST_FORMAT "/var/lib/cosmo-upgrade/oldest-format"

static int oldest_format_mark(const struct set *a, const struct set *b)
{
    unsigned ra = kernel_reads(a->file[0], a->len[0]), rb = b ? kernel_reads(b->file[0], b->len[0]) : ra;
    char text[16];
    int n = snprintf(text, sizeof(text), "%u\n", ra < rb ? ra : rb);
    mkdir("/var/lib", 0755);
    mkdir("/var/lib/cosmo-upgrade", 0755);
    int fd = open(OLDEST_FORMAT TMP_SUFFIX, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, text, (size_t)n) != n || fsync(fd) < 0 || close(fd) < 0 ||
        rename(OLDEST_FORMAT TMP_SUFFIX, OLDEST_FORMAT) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s\n", OLDEST_FORMAT, strerror(errno));
        return -1;
    }
    sync();
    return 0;
}

/* Refuse a set whose kernel could not mount this root (the lazy raise's
 * price, design.md, "Format version 12"). */
static int format_gate(const struct set *s, const char *what)
{
    int rf = root_format();
    unsigned kr = kernel_reads(s->file[0], s->len[0]);
    if (rf < 0) {
        fprintf(stderr, "cosmo-upgrade: the root's cosmofs format could not be read from /dev/fsctl: %s; "
                        "%s's kernel might not mount it, so nothing written\n", strerror(errno), what);
        return EXIT_REFUSED;
    }
    if ((unsigned)rf <= kr)
        return 0;
    fprintf(stderr, "cosmo-upgrade: %s's kernel reads cosmofs format %u at most, and the root is format %u (a "
                    "snapshot raised it): that kernel could not mount the root; nothing written\n", what, kr, (unsigned)rf);
    return EXIT_REFUSED;
}

static int upgrade(const char *dir, bool check, const char *stop)
{
    struct set nw = { 0 };
    char err[200], keyid[17];
    if (set_read_dir(dir, &nw) < 0)
        return EXIT_FAILED;
    char sumkey[17];
    if (set_check_sums(dir, &nw, sumkey, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s; nothing written\n", dir, err);
        return EXIT_REFUSED;
    }
    say("verified SHA256SUMS, signed by key %s in " KEYS_DIR ", and the set's four files against it", sumkey);
    if (set_verify(&nw, true, keyid, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s; nothing written\n", dir, err);
        return EXIT_REFUSED;
    }
    struct member *idx = NULL;
    const struct member *loader = archive_loader(&nw, &idx);
    if (loader == NULL) {
        fprintf(stderr, "cosmo-upgrade: %s: boot.tar carries no efi/" LOADER " (a release before S7); nothing "
                        "written\n", dir);
        return EXIT_REFUSED;
    }
    for (int i = 0; i < SET_FILES; i++)
        if (nw.len[i] > UINT32_MAX) {
            fprintf(stderr, "cosmo-upgrade: %s: %s is larger than a FAT32 file can be; nothing written\n", dir,
                    g_set_names[i]);
            return EXIT_REFUSED;
        }
    if (loader->size > UINT32_MAX) {
        fprintf(stderr, "cosmo-upgrade: %s: efi/" LOADER " is larger than a FAT32 file can be; nothing written\n",
                dir);
        return EXIT_REFUSED;
    }
    if (!loader_trusts(loader->data, loader->size, &nw)) {
        fprintf(stderr, "cosmo-upgrade: %s: its loader does not trust key %s, which signed its set; nothing "
                        "written\n", dir, keyid);
        return EXIT_REFUSED;
    }
    say("verified kernel.elf, boot.tar and cmdline against the manifest, signed by key %s in " KEYS_DIR
        ", and the loader in boot.tar", keyid);
    if (format_gate(&nw, dir) != 0)
        return EXIT_REFUSED;
    struct esp e;
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s; nothing written\n", err);
        return EXIT_REFUSED;
    }
    struct set cur = { 0 };
    char curkey[17];
    if (esp_read_set(&e, e.set, &cur) < 0)
        return EXIT_FAILED;
    if (set_verify(&cur, false, curkey, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: the current set on %s: %s; nothing written\n", e.dev->name, err);
        return EXIT_REFUSED;
    }
    /* Between the two switches the new loader starts the current set. */
    if (!loader_trusts(loader->data, loader->size, &cur)) {
        fprintf(stderr, "cosmo-upgrade: %s: its loader does not trust key %s, which signed the current set: an "
                        "interruption after the loader switch would leave nothing that boots; nothing written\n",
                dir, curkey);
        return EXIT_REFUSED;
    }
    bool same_set = memcmp(cur.file[3], nw.file[3], COSMO_MANIFEST_FILE_SIZE) == 0;
    if (check) {
        say("check: %s; the ESP is %s", same_set ? "this set is already current" : "it would replace the current set",
            e.dev->name);
        return EXIT_DONE;
    }
    if (same_set) {
        /* An earlier run switched to this set: finish it (it converges). */
        say("the boot set on %s is already this one: finishing", e.dev->name);
        int rc = finish(&e, &nw);
        if (rc == EXIT_DONE)
            say("upgraded; reboot to run it, then pkg update && pkg upgrade");
        return rc;
    }
    if (pending_is(cur.file[3])) {
        say("an earlier run switched to the current set and was interrupted: finishing it first");
        int rc = finish(&e, &cur);
        if (rc != EXIT_DONE)
            return rc;
        if (esp_load(&e, err, sizeof(err)) < 0) {
            fprintf(stderr, "cosmo-upgrade: %s\n", err);
            return EXIT_FAILED;
        }
    }
    pending_clear();

    /* 0. The boot environment (roadmap 1.2 B4): the root as it is now,
     * be-<release>, paired with the current set. Taken before anything is
     * written; refused for want of room -- the base pass replaces the
     * base system beside it, and the snapshot keeps the old one -- with
     * nothing written. A root that keeps no snapshots gets none. */
    struct be_file be;
    if (be_load(&e, &be) < 0) {
        fprintf(stderr, "cosmo-upgrade: what \\cosmo\\be paired is unknown: %s\n", BE_DAMAGED);
        return EXIT_REFUSED;
    }
    struct rootsnap rs;
    struct snaps sn = { 0 };
    bool envs = rootsnap_open(&rs, "/") == 0;
    if (!envs && errno != EOPNOTSUPP) {
        fprintf(stderr, "cosmo-upgrade: /dev/fsctl: %s; nothing written\n", strerror(errno));
        return EXIT_REFUSED;
    }
    char envname[BE_NAME_MAX + 1] = "", dropped[BE_MAX][BE_NAME_MAX + 1];
    int ndropped = 0;
    char curtag[129], nwtag[129], have[129];
    manifest_tag(cur.file[3], curtag);
    manifest_tag(nw.file[3], nwtag);
    root_set_read(have);
    if (envs) {
        if (rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0) {
            fprintf(stderr, "cosmo-upgrade: the root's snapshots: %s; nothing written\n", strerror(errno));
            return EXIT_REFUSED;
        }
        /* A root that is not the current set's own -- a rollback pending
         * that will replace it, or one withdrawn or refused that left the
         * other half moved -- would be snapshotted under the wrong set and
         * brought forward from the wrong base system. */
        if (sn.pool.flags & COSMO_FSCTL_POOL_F_PENDING) {
            fprintf(stderr, "cosmo-upgrade: a rollback of the root is pending (cosmo-snap list --all): reboot to "
                            "apply it, or cosmo-snap rollback --cancel; nothing written\n");
            return EXIT_REFUSED;
        }
        if (have[0] && strcmp(have, curtag) != 0) {
            fprintf(stderr, "cosmo-upgrade: the root holds another set's base system than the current set's (a "
                            "rollback half done): cosmo-upgrade --repair first; nothing written\n");
            return EXIT_REFUSED;
        }
        /* No environment for a set whose own cosmo-upgrade would refuse
         * an ESP with \cosmo\be once rolled back to (v1.0.7 and older),
         * nor one whose snapshot would raise the root past what a kept
         * set's kernel mounts (B1's lazy raise; pkg's rule, PK11). */
        int rf = root_format();
        unsigned kn = kernel_reads(nw.file[0], nw.len[0]), kc = kernel_reads(cur.file[0], cur.len[0]);
        if (!set_reads_be(&cur)) {
            say("no boot environment: the current set's cosmo-upgrade predates them; --rollback returns the set "
                "and the base system only");
            envs = false;
        } else if (rf >= 0 && rf < 12 && (kn < 12 || kc < 12)) {
            say("no boot environment: its snapshot would raise the root to cosmofs format 12, which %s set's "
                "kernel cannot mount; --rollback returns the set and the base system only",
                kn < 12 ? "the new" : "the current");
            envs = false;
        }
    }
    if (envs) {
        be_sweep(&rs, &sn, &be, false);
        free(sn.v);
        if (rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0)
            return fail("the root's snapshots", errno);
        uint64_t want = (uint64_t)nw.len[1] + (1u << 20);
        if (sn.pool.free_bytes < want) {
            fprintf(stderr,
                    "cosmo-upgrade: not enough space on the root for the boot environment: about %llu KiB needed "
                    "(the new base system beside the old one, which the environment keeps), %llu KiB free; "
                    "nothing written\n", (unsigned long long)((want + 1023) / 1024),
                    (unsigned long long)(sn.pool.free_bytes / 1024));
            return EXIT_REFUSED;
        }
        char tag[64];
        if (running_tag(tag, sizeof(tag)) < 0)
            snprintf(tag, sizeof(tag), "unknown");
        be_name(tag, snaps_taken, &sn, envname);
        /* The root says which set's base system it holds, so that the
         * environment does: a fresh install does not say yet, and its
         * base system is the current set's. */
        if (!have[0])
            root_set_mark(cur.file[3]);
        if (be_taking_mark(envname) < 0) {
            fprintf(stderr, "cosmo-upgrade: %s: %s; nothing written\n", BE_TAKING, strerror(errno));
            return EXIT_REFUSED;
        }
        struct cosmo_fsctl_snap es;
        if (rootsnap_create(&rs, envname, &es) < 0) {
            fprintf(stderr, "cosmo-upgrade: the boot environment %s: %s; nothing written\n", envname,
                    errno == ENOSPC ? "no room on the root" : strerror(errno));
            be_taking_clear();
            return EXIT_REFUSED;
        }
        char u[33];
        uuid_hex(es.uuid, u);
        be_add(&be, envname, u, curtag);
        snprintf(be.intent, sizeof(be.intent), "%s", nwtag);
        while (be.n > BE_KEEP) {
            snprintf(dropped[ndropped++], BE_NAME_MAX + 1, "%s", be.env[0].name);
            be_drop(&be, 0);
        }
        say("boot environment %s: the root as it is now, paired with the current set", envname);
    }

    /* 1. The sweep: what an interrupted run left allocated. */
    int rc = sweep_commit(&e);
    if (rc < 0)
        return fail("sweeping the ESP", rc);
    if (rc > 0)
        say("freed %d clusters an earlier run left", rc);
    /* 2. A set with no loader of its own (an ESP no upgrade has touched)
     * gets a copy of the one the firmware starts, in the same directory:
     * from here on a set and its loader move together. */
    if (!e.has_sloader) {
        uint8_t *bl;
        uint32_t c;
        if (esp_read(&e, &e.boot, &bl) < 0)
            return EXIT_FAILED;
        rc = fat_write_new(&e.f, bl, e.boot.size, &c);
        free(bl);
        if (rc == 0)
            rc = fat_commit(&e.f);
        struct edit ed = { -1, c, e.boot.size, LOADER, FAT_ATTR_ARCHIVE };
        if (rc == 0)
            rc = switch_sector(&e, e.lba, &ed, 1);
        if (rc < 0)
            return fail("recording the set's loader in \\cosmo", rc);
        if (esp_load(&e, err, sizeof(err)) < 0 || !e.has_sloader) {
            fprintf(stderr, "cosmo-upgrade: %s\n", err);
            return EXIT_FAILED;
        }
    }
    /* 3. The old prev goes first: three sets need not fit. */
    if (e.has_prev) {
        uint8_t sec[4096];
        rc = fat_read_sector(&e.f, e.prev.lba, sec);
        if (rc == 0) {
            sec[e.prev.slot * FAT_DIRENT] = 0xE5;
            rc = fat_write_sector(&e.f, e.prev.lba, sec);
        }
        if (rc == 0)
            rc = blkdevs_flush(e.dev);
        if (rc == 0)
            rc = sweep_commit(&e);
        if (rc < 0)
            return fail("dropping the old \\cosmo\\prev", rc);
        e.has_prev = false;
    }
    /* 4. The new set and its loader, a second copy of that loader for
     * \EFI\BOOT, and the new prev naming the current set and its loader:
     * all in free clusters, named by nothing yet. */
    uint32_t cl[SET_FILES], scl, bcl, pcl;
    for (int i = 0; i < SET_FILES; i++) {
        rc = fat_write_new(&e.f, nw.file[i], nw.len[i], &cl[i]);
        if (rc < 0)
            return fail(g_set_names[i], rc);
    }
    rc = fat_write_new(&e.f, loader->data, loader->size, &scl);
    if (rc == 0)
        rc = fat_write_new(&e.f, loader->data, loader->size, &bcl);
    if (rc == 0)
        rc = write_prev_dir(&e, e.set, &e.sloader, &pcl);
    struct edit bed;
    if (rc == 0 && envs)
        rc = be_stage(&e, &be, &bed);   /* the pairing, switched with the set */
    if (rc == 0)
        rc = fat_commit(&e.f);
    if (rc < 0)
        return fail("writing the new set", rc);
    say("written: the new set, its loader and \\cosmo\\prev's directory, in free clusters of %s", e.dev->name);
    if (stop && strcmp(stop, "written") == 0)
        goto stopped;
    /* 5. The loader the firmware starts: the new one boots the old set
     * too, the old one may not trust the new set's key. */
    struct edit led = { (int)e.boot.slot, bcl, (uint32_t)loader->size, NULL, 0 };
    rc = switch_sector(&e, e.boot.lba, &led, 1);
    if (rc < 0)
        return fail("switching the loader", rc);
    say("loader switched: \\EFI\\BOOT\\" LOADER " is the new one");
    if (stop && strcmp(stop, "loader") == 0)
        goto stopped;
    /* 6. The set, its loader and prev: one sector, the commit point. The
     * oldest format a kept set reads first, for both sides of it (the old
     * prev is gone already): pkg must not raise the root past it. */
    if (oldest_format_mark(&nw, &cur) < 0)
        return EXIT_FAILED;
    if (pending_set(nw.file[3]) < 0)
        return EXIT_FAILED;
    struct edit sed[SET_FILES + 3];
    for (int i = 0; i < SET_FILES; i++)
        sed[i] = (struct edit){ (int)e.set[i].slot, cl[i], (uint32_t)nw.len[i], NULL, 0 };
    sed[SET_FILES] = (struct edit){ (int)e.sloader.slot, scl, (uint32_t)loader->size, NULL, 0 };
    sed[SET_FILES + 1] = (struct edit){ -1, pcl, 0, "prev", FAT_ATTR_DIR };
    if (envs)
        sed[SET_FILES + 2] = bed;
    rc = switch_sector(&e, e.lba, sed, SET_FILES + 2 + (envs ? 1 : 0));
    if (rc < 0)
        return fail("switching the boot set", rc);
    say("boot set switched: %s boots the new set; the previous one is in \\cosmo\\prev", e.dev->name);
    if (stop && strcmp(stop, "set") == 0)
        goto stopped;
    /* 7. What the switch left unnamed, then the root. */
    rc = sweep_commit(&e);
    if (rc < 0)
        return fail("sweeping the ESP", rc);
    if (envs)
        be_taking_clear();   /* paired by the switch */
    if (base_pass(&cur, &nw) < 0)
        return EXIT_FAILED;
    pending_clear();
    for (int i = 0; i < ndropped; i++) {
        if (rootsnap_delete(&rs, dropped[i]) == 0)
            say("deleted environment %s (the newest %d are kept)", dropped[i], BE_KEEP);
        else
            say("environment %s is no longer paired but stays: %s", dropped[i], strerror(errno));
    }
    free(sn.v);
    rootsnap_close(&rs);
    say("upgraded; reboot to run it, then pkg update && pkg upgrade%s%s", envs ? "; --rollback returns to " : "",
        envname);
    free(idx);
    return EXIT_DONE;
stopped:
    say("stopped after %s (--stop-after); run it again to finish", stop);
    return EXIT_STOPPED;
}

/* --repair's half for the ESP: the set switched back alone, the root
 * left to the pairing (it is already the intended one, or its rollback is
 * pending). */
static bool g_set_only;

/* After the root's rollback is armed, the tree it keeps (the safety
 * snapshot) is fixed: what is written after it is in neither tree. So, as
 * cosmo-snap rollback does, init is asked to restart now; --no-reboot
 * leaves that to the operator. */
static bool g_no_reboot;

static int reboot_to_apply(void)
{
    if (g_no_reboot) {
        say("reboot to run it (what is written before then is not kept)");
        return EXIT_DONE;
    }
    say("rebooting to apply it");
    fflush(stdout);
    sync();
    if (kill(1, SIGTERM) != 0)   /* init restarts on SIGTERM, as BusyBox's reboot asks */
        return fail("asking init to restart", errno);
    return EXIT_DONE;
}

static int rollback(const char *stop)
{
    struct esp e;
    char err[200], keyid[17], curkey[17];
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s; nothing written\n", err);
        return EXIT_REFUSED;
    }
    struct set prev = { 0 }, cur = { 0 };
    if (esp_read_set(&e, e.set, &cur) < 0)
        return EXIT_FAILED;
    if (set_verify(&cur, false, curkey, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: the current set: %s; nothing written\n", err);
        return EXIT_REFUSED;
    }
    if (pending_is(cur.file[3])) {
        say("an earlier run switched to the current set and was interrupted: finishing it, not rolling back");
        int rc = finish(&e, &cur);
        if (rc == EXIT_DONE)
            say("finished; reboot to run it");
        return rc;
    }
    if (!e.prev_whole || !e.has_sloader) {
        fprintf(stderr, "cosmo-upgrade: no previous set in \\cosmo\\prev; nothing written\n");
        return EXIT_REFUSED;
    }
    if (esp_read_set(&e, e.pset, &prev) < 0)
        return EXIT_FAILED;
    if (set_verify(&prev, true, keyid, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: \\cosmo\\prev: %s; nothing written\n", err);
        return EXIT_REFUSED;
    }
    if (format_gate(&prev, "\\cosmo\\prev") != 0)
        return EXIT_REFUSED;
    /* Between the two switches one loader starts the other set. The set
     * goes first when the loader the firmware starts trusts the previous
     * set; otherwise the loader does, when the previous set's own loader
     * trusts the current set (a rollback from v1.0.3 back to a newer
     * build: v1.0.3's loader lacks the newer key). Neither: refused. */
    uint8_t *bl, *pl;
    if (esp_read(&e, &e.boot, &bl) < 0)
        return EXIT_FAILED;
    if (esp_read(&e, &e.ploader, &pl) < 0) {
        free(bl);
        return EXIT_FAILED;
    }
    bool set_first = loader_trusts(bl, e.boot.size, &prev);
    bool loader_first = !set_first && loader_trusts(pl, e.ploader.size, &cur);
    free(bl);
    if (!set_first && !loader_first) {
        free(pl);
        fprintf(stderr, "cosmo-upgrade: neither \\EFI\\BOOT\\" LOADER " trusts key %s, which signed \\cosmo\\prev, "
                        "nor \\cosmo\\prev's loader key %s, which signed the current set: an interruption between "
                        "the switches would leave nothing that boots; nothing written\n", keyid, curkey);
        return EXIT_REFUSED;
    }
    say("verified \\cosmo\\prev, signed by key %s in " KEYS_DIR, keyid);
    /* The other half (roadmap 1.2 B4): the root, when \cosmo\be pairs the
     * previous set with an environment the root still has. */
    struct be_file be;
    if (be_load(&e, &be) < 0) {
        free(pl);
        fprintf(stderr, "cosmo-upgrade: whether \\cosmo\\prev has a boot environment is unknown: %s\n", BE_DAMAGED);
        return EXIT_REFUSED;
    }
    char prevtag[129], curtag[129];
    manifest_tag(prev.file[3], prevtag);
    manifest_tag(cur.file[3], curtag);
    struct rootsnap rs;
    struct snaps sn = { 0 };
    bool envs = rootsnap_open(&rs, "/") == 0;
    if (envs && rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0)
        envs = false;
    else if (!envs && errno == EOPNOTSUPP)
        errno = 0;
    if (!envs && errno != 0) {
        /* Snapshots the root keeps but that cannot be read now: an
         * environment may be there, and the set alone is not the rollback
         * asked for. */
        free(pl);
        fprintf(stderr, "cosmo-upgrade: the root's snapshots: %s; nothing written\n", strerror(errno));
        return EXIT_REFUSED;
    }
    const struct be_env *env = envs ? be_for_set(&be, prevtag) : NULL;
    const struct cosmo_fsctl_snap *envsnap = envs ? be_snapshot(&sn, env) : NULL;
    bool with_root = !g_set_only && envsnap != NULL;
    char target[BE_NAME_MAX + 1] = "";
    if (with_root)
        snprintf(target, sizeof(target), "%s", env->name);   /* `env` points into `be`, which changes below */
    if (!with_root && !g_set_only)
        say("no boot environment pairs \\cosmo\\prev with a snapshot of the root (it was installed by v1.0.7 or "
            "older, or the root keeps no snapshots): the set and the base system go back; installed packages and "
            "/etc stay as they are");
    if (with_root && (sn.pool.flags & COSMO_FSCTL_POOL_F_PENDING)) {
        free(pl);
        fprintf(stderr, "cosmo-upgrade: a rollback of the root is already pending (cosmo-snap list --all; "
                        "cosmo-snap rollback --cancel withdraws it); nothing written\n");
        return EXIT_REFUSED;
    }
    if (with_root && (sn.pool.flags & COSMO_FSCTL_POOL_F_HELD) && !(envsnap->flags & COSMO_FSCTL_SNAP_F_HELD)) {
        free(pl);
        fprintf(stderr, "cosmo-upgrade: the boot after the last rollback has not completed, and its hold allows "
                        "only a rollback back to the tree it replaced; nothing written\n");
        return EXIT_REFUSED;
    }
    int rc = sweep_commit(&e);
    if (rc < 0) {
        free(pl);
        return fail("sweeping the ESP", rc);
    }
    if (loader_first) {
        rc = switch_boot_loader(&e, pl, e.ploader.size);
        if (rc == 0)
            rc = sweep_commit(&e);
        if (rc < 0) {
            free(pl);
            return fail("switching the loader", rc);
        }
        say("loader switched: \\EFI\\BOOT\\" LOADER " is \\cosmo\\prev's, which starts both sets");
    }
    free(pl);
    char safety[BE_NAME_MAX + 1] = "";
    if (with_root) {
        /* i. The intent: \cosmo\be names the previous set as the one to
         * run, before either half moves -- every crash after this leaves
         * a pairing --repair can finish (docs/userland/design.md). */
        snprintf(be.intent, sizeof(be.intent), "%s", prevtag);
        struct edit ie;
        rc = be_stage(&e, &be, &ie);
        if (rc == 0)
            rc = fat_commit(&e.f);
        if (rc == 0)
            rc = switch_sector(&e, e.lba, &ie, 1);
        if (rc < 0)
            return fail("recording the intent in \\cosmo\\be", rc);
        if (esp_load(&e, err, sizeof(err)) < 0) {
            fprintf(stderr, "cosmo-upgrade: %s\n", err);
            return EXIT_FAILED;
        }
        say("intent recorded: \\cosmo\\be names the previous set and %s", target);
        if (stop && strcmp(stop, "intent") == 0)
            goto stopped;
        /* ii. The root: cosmofs's pending rollback, applied at the next
         * mount; the tree it replaces is kept as this release's
         * environment, paired with the current set below. */
        char tag[64];
        if (running_tag(tag, sizeof(tag)) < 0)
            snprintf(tag, sizeof(tag), "unknown");
        be_name(tag, snaps_taken, &sn, safety);
        char have[129];
        root_set_read(have);
        if (!have[0])
            root_set_mark(cur.file[3]);   /* as the upgrade does: the tree it keeps says which set it ran */
        struct cosmo_fsctl_snap ss;
        if (rootsnap_rollback(&rs, target, safety, &ss) < 0) {
            fprintf(stderr, "cosmo-upgrade: the root's rollback to %s: %s; \\cosmo\\be names the previous set as "
                            "intended: cosmo-upgrade --repair finishes it\n", target, strerror(errno));
            return EXIT_FAILED;
        }
        char u[33];
        uuid_hex(ss.uuid, u);
        be_add(&be, safety, u, curtag);
        /* At most BE_KEEP paired: the oldest others are let go, and the
         * next upgrade or --prune deletes their snapshots. */
        for (int i = 0; be.n > BE_KEEP && i < be.n;)
            if (strcmp(be.env[i].name, target) != 0 && strcmp(be.env[i].name, safety) != 0)
                be_drop(&be, i);
            else
                i++;
        /* The tree it keeps, paired at once in a switch of its own: a
         * crash before the set's switch leaves it an environment --repair
         * finds, not a snapshot nothing names. */
        struct edit pe;
        rc = be_stage(&e, &be, &pe);
        if (rc == 0)
            rc = fat_commit(&e.f);
        if (rc == 0)
            rc = switch_sector(&e, e.lba, &pe, 1);
        if (rc == 0 && esp_load(&e, err, sizeof(err)) < 0)
            rc = -EIO;
        if (rc < 0)
            return fail("pairing the kept tree in \\cosmo\\be", rc);
        say("the root rolls back to %s at the next boot; the tree it replaces is kept as %s", target, safety);
        if (stop && strcmp(stop, "record") == 0)
            goto stopped;
    }
    /* A new prev naming the current set and its loader; then the set and
     * its loader (the new loader still running boots the old set), then
     * the loader the firmware starts. */
    uint32_t pcl;
    rc = write_prev_dir(&e, e.set, &e.sloader, &pcl);
    if (rc == 0)
        rc = fat_commit(&e.f);
    if (rc < 0)
        return fail("writing \\cosmo\\prev", rc);
    struct edit bed;
    bool stage_be = with_root;
    if (stage_be) {
        rc = be_stage(&e, &be, &bed);   /* the current set's environment, switched with the set */
        if (rc == 0)
            rc = fat_commit(&e.f);
        if (rc < 0)
            return fail("writing \\cosmo\\be", rc);
    }
    if (oldest_format_mark(&prev, &cur) < 0)   /* the same two sets kept, before and after */
        return EXIT_FAILED;
    /* The pending marker asks the next run to bring the root's base system
     * to the set; with the root rolled back whole, or left to its pairing,
     * there is nothing to bring. */
    if (!with_root && !g_set_only && pending_set(prev.file[3]) < 0)
        return EXIT_FAILED;
    struct edit sed[SET_FILES + 3];
    for (int i = 0; i < SET_FILES; i++)
        sed[i] = (struct edit){ (int)e.set[i].slot, e.pset[i].cluster, e.pset[i].size, NULL, 0 };
    sed[SET_FILES] = (struct edit){ (int)e.sloader.slot, e.ploader.cluster, e.ploader.size, NULL, 0 };
    sed[SET_FILES + 1] = (struct edit){ (int)e.prev.slot, pcl, 0, NULL, 0 };
    if (stage_be)
        sed[SET_FILES + 2] = bed;
    rc = switch_sector(&e, e.lba, sed, SET_FILES + 2 + (stage_be ? 1 : 0));
    if (rc < 0)
        return fail("switching the boot set", rc);
    say("boot set switched back: %s boots the previous set; the one it replaced is in \\cosmo\\prev", e.dev->name);
    if (stop && strcmp(stop, "set") == 0)
        goto stopped;
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s\n", err);
        return EXIT_FAILED;
    }
    rc = finish_ex(&e, &prev, !with_root && !g_set_only);
    if (rc == EXIT_DONE && with_root) {
        say("rolled back: the boot set now, and the root -- packages, /etc and the base system as they were in %s -- "
            "at the next boot", target);
        return reboot_to_apply();
    } else if (rc == EXIT_DONE)
        say("rolled back; reboot to run it");
    return rc;
stopped:
    say("stopped after %s (--stop-after); cosmo-upgrade --repair finishes it", stop);
    return EXIT_STOPPED;
}

/* One run at a time: an abstract unix name bound for the process's life
 * (syslogd's hold_instance). Two runs would each plan the FAT from their
 * own snapshot and hand out the same free clusters. The kernel lets the
 * name go however the process ends, so a killed run leaves no lock. */
static int hold_lock(void)
{
    static const char name[] = "cosmo-upgrade";
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un la = { .sun_family = AF_UNIX };
    memcpy(la.sun_path + 1, name, sizeof(name) - 1);
    if (fd < 0 || bind(fd, (const struct sockaddr *)&la, (socklen_t)(2 + 1 + sizeof(name) - 1)) < 0) {
        if (errno == EADDRINUSE)
            fprintf(stderr, "cosmo-upgrade: another cosmo-upgrade is running; nothing written\n");
        else
            fprintf(stderr, "cosmo-upgrade: the lock: %s; nothing written\n", strerror(errno));
        return -1;
    }
    return 0;   /* fd stays open, and the name held, until exit */
}

/* --- fetching a release (roadmap 1.1 P6) ------------------------------------------------ */

#define RELEASE_CONF        "/etc/cosmo-release.conf"
#define RELEASE_URL_DEFAULT "https://vkuttyp.github.io/CosmoOS/releases"
#define FETCH_DIR           "/var/cache/cosmo-upgrade"
#define FETCH_MAX           (256ull << 20)

struct release_src {
    char url[512];   /* without a trailing '/' */
    char pin[72];    /* "sha256:HEX" or empty */
};

static void strip_slashes(char *u)
{
    size_t n = strlen(u);
    while (n > 8 && u[n - 1] == '/')
        u[--n] = '\0';
}

/* RELEASE_CONF: "url URL" and "pin sha256:HEX" lines, '#' comments; the
 * project's releases when it is absent. 0, or -1 with a message. */
static int release_conf(struct release_src *r)
{
    snprintf(r->url, sizeof(r->url), "%s", RELEASE_URL_DEFAULT);
    r->pin[0] = '\0';
    uint8_t *text;
    size_t len;
    int rc = read_whole(RELEASE_CONF, &text, &len);
    if (rc == -ENOENT)
        return 0;
    if (rc < 0) {
        fprintf(stderr, "cosmo-upgrade: " RELEASE_CONF ": %s\n", strerror(-rc));
        return -1;
    }
    uint8_t *t = realloc(text, len + 1);
    if (t == NULL) {
        free(text);
        return -1;
    }
    t[len] = '\0';
    int lineno = 0, bad = 0;
    char *save;
    for (char *l = strtok_r((char *)t, "\n", &save); l && !bad; l = strtok_r(NULL, "\n", &save)) {
        lineno++;
        while (*l == ' ' || *l == '\t')
            l++;
        if (*l == '#' || *l == '\0')
            continue;
        /* "KEY VALUE": the first word after KEY */
        char *key = l, *value = l + strcspn(l, " \t");
        if (*value)
            *value++ = '\0';
        value += strspn(value, " \t");
        value[strcspn(value, " \t")] = '\0';
        if (*value == '\0' || strlen(value) >= sizeof(r->url)) {
            bad = 1;
        } else if (strcmp(key, "url") == 0 && strncmp(value, "https://", 8) == 0) {
            snprintf(r->url, sizeof(r->url), "%s", value);
            strip_slashes(r->url);
        } else if (strcmp(key, "pin") == 0 && strncmp(value, "sha256:", 7) == 0 && strlen(value) == 71 &&
                   strspn(value + 7, "0123456789abcdef") == 64) {
            snprintf(r->pin, sizeof(r->pin), "%s", value);
        } else {
            bad = 1;
        }
    }
    free(t);
    if (bad) {
        fprintf(stderr, "cosmo-upgrade: " RELEASE_CONF " line %d: not 'url https://...' or 'pin sha256:HEX'\n",
                lineno);
        return -1;
    }
    return 0;
}

/* vMAJOR.MINOR.PATCH[-rcN][-COMMITS-gHASH][-dirty]: git describe's form,
 * the kernel's build id. rc 0: a release; after: commits past the tag. */
struct tag {
    unsigned v[3], rc, after;
};

static bool tag_parse(const char *s, struct tag *t)
{
    memset(t, 0, sizeof(*t));
    char *end;
    if (s[0] != 'v')
        return false;
    s++;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9')
            return false;
        t->v[i] = (unsigned)strtoul(s, &end, 10);
        s = end;
        if (i < 2 && *s++ != '.')
            return false;
    }
    if (strncmp(s, "-rc", 3) == 0 && s[3] >= '0' && s[3] <= '9') {
        t->rc = (unsigned)strtoul(s + 3, &end, 10);
        s = end;
    }
    if (*s == '-' && s[1] >= '0' && s[1] <= '9') {
        t->after = (unsigned)strtoul(s + 1, &end, 10);
        s = end;
        if (strncmp(s, "-g", 2) != 0)
            return false;
        s += 2 + strspn(s + 2, "0123456789abcdef");
    }
    if (strcmp(s, "-dirty") == 0)
        s += 6;
    return *s == '\0';
}

/* Whether release `a` is newer than what `b` runs. */
static bool tag_newer(const struct tag *a, const struct tag *b)
{
    for (int i = 0; i < 3; i++)
        if (a->v[i] != b->v[i])
            return a->v[i] > b->v[i];
    if (a->rc == b->rc)
        return false;          /* the same release, or b built after it */
    if (a->rc == 0)
        return true;           /* the release after b's candidate */
    if (b->rc == 0)
        return false;
    return a->rc > b->rc;
}

/* The build id this kernel reports: the last word of /proc/version. */
static int running_tag(char *out, size_t n)
{
    uint8_t *text;
    size_t len;
    if (read_whole("/proc/version", &text, &len) < 0)
        return -1;
    while (len && (text[len - 1] == '\n' || text[len - 1] == ' '))
        len--;
    size_t start = len;
    while (start && text[start - 1] != ' ')
        start--;
    snprintf(out, n, "%.*s", (int)(len - start), (const char *)text + start);
    free(text);
    return 0;
}

static int fetch_to(const struct release_src *r, const char *base, const char *name, const char *dir, bool resume,
                    uint64_t max)
{
    char url[1024], path[512], part[520];
    snprintf(url, sizeof(url), "%s/%s", base, name);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(part, sizeof(part), "%s.part", path);
    if (!resume)
        unlink(part);
    struct fetch_opts o = { .pin = r->pin, .resume = resume, .max = max };
    int st = fetch_url(url, part, &o);
    if (st != FETCH_OK) {
        if (st < 0)
            fprintf(stderr, "cosmo-upgrade: cannot run " FETCH_PROGRAM ": %s\n", strerror(errno));
        else
            fprintf(stderr, "cosmo-upgrade: %s: %s (above)\n", url,
                    st == FETCH_REFUSED ? "refused" : "cannot be fetched");
        return st == FETCH_REFUSED ? EXIT_REFUSED : EXIT_FAILED;
    }
    if (rename(part, path) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s\n", path, strerror(errno));
        return EXIT_FAILED;
    }
    return EXIT_DONE;
}

static int mkdirs(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1;; p++) {
        if (*p != '/' && *p != '\0')
            continue;
        char c = *p;
        *p = '\0';
        if (mkdir(buf, 0755) < 0 && errno != EEXIST)
            return -1;
        if (c == '\0')
            return 0;
        *p = c;
    }
}

/* --check alone: the newest release at the release URL against the
 * running one. */
static int check_latest(void)
{
    struct release_src r;
    if (release_conf(&r) < 0)
        return EXIT_FAILED;
    /* Anyone may ask: a file of this process's in /tmp, not the cache. */
    char tmpdir[64], latest_path[96];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cosmo-upgrade-check.%d", (int)getpid());
    if (mkdir(tmpdir, 0700) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s\n", tmpdir, strerror(errno));
        return EXIT_FAILED;
    }
    snprintf(latest_path, sizeof(latest_path), "%s/LATEST", tmpdir);
    int rc = fetch_to(&r, r.url, "LATEST", tmpdir, false, 4096);
    uint8_t *text = NULL;
    size_t len = 0;
    if (rc == EXIT_DONE && read_whole(latest_path, &text, &len) < 0)
        rc = EXIT_FAILED;
    unlink(latest_path);
    rmdir(tmpdir);
    if (rc != EXIT_DONE)
        return rc;
    char latest[64] = "", published[32] = "", running[128] = "";
    uint8_t *t = realloc(text, len + 1);
    if (t == NULL) {
        free(text);
        return EXIT_FAILED;
    }
    t[len] = '\0';
    char *save;
    for (char *l = strtok_r((char *)t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (strncmp(l, "tag: ", 5) == 0)
            snprintf(latest, sizeof(latest), "%s", l + 5);
        else if (strncmp(l, "published: ", 11) == 0)
            snprintf(published, sizeof(published), "%s", l + 11);
    }
    free(t);
    struct tag lt, rt;
    if (!tag_parse(latest, &lt) || lt.after) {
        fprintf(stderr, "cosmo-upgrade: %s/LATEST: no 'tag: vX.Y.Z' line\n", r.url);
        return EXIT_FAILED;
    }
    if (running_tag(running, sizeof(running)) < 0) {
        fprintf(stderr, "cosmo-upgrade: /proc/version cannot be read; the newest release at %s is %s\n", r.url,
                latest);
        return EXIT_FAILED;
    }
    if (!tag_parse(running, &rt)) {
        say("the newest release at %s is %s%s%s; this system's build (%s) is not a release's", r.url, latest,
            published[0] ? ", published " : "", published, running);
        return EXIT_DONE;
    }
    if (tag_newer(&lt, &rt))
        say("%s is newer than this system's %s (published %s): cosmo-upgrade --fetch %s", latest, running,
            published[0] ? published : "?", latest);
    else
        say("this system's %s is up to date: the newest release at %s is %s", running, r.url, latest);
    return EXIT_DONE;
}

/* --fetch: the set into FETCH_DIR/<tag>, SHA256SUMS checked before the
 * files are fetched, then upgrade() as for a directory. */
static int fetch_set(const char *arg, bool check, const char *stop)
{
    struct release_src r;
    if (release_conf(&r) < 0)
        return EXIT_FAILED;
    char base[1024], dir[512];
    struct tag t;
    if (strstr(arg, "://")) {
        snprintf(base, sizeof(base), "%s", arg);
        strip_slashes(base);
        snprintf(dir, sizeof(dir), FETCH_DIR "/url");
    } else if (tag_parse(arg, &t) && t.after == 0 && strchr(arg, '/') == NULL) {
        snprintf(base, sizeof(base), "%s/%s", r.url, arg);
        snprintf(dir, sizeof(dir), FETCH_DIR "/%s", arg);
    } else {
        fprintf(stderr, "cosmo-upgrade: --fetch takes a release tag (vX.Y.Z) or an https:// URL, not '%s'\n", arg);
        return EXIT_USAGE;
    }
    if (mkdirs(dir) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s: %s\n", dir, strerror(errno));
        return EXIT_FAILED;
    }
    say("fetching the " ARCH " set from %s into %s", base, dir);
    int rc = fetch_to(&r, base, "SHA256SUMS", dir, false, 1 << 20);
    if (rc == EXIT_DONE)
        rc = fetch_to(&r, base, "SHA256SUMS.sig", dir, false, 4096);
    if (rc != EXIT_DONE)
        return rc;
    /* The sums first: nothing more is fetched for a set they do not cover. */
    uint8_t *sums = NULL, *sig = NULL;
    size_t slen = 0, glen = 0;
    char path[600], err[200], keyid[17];
    snprintf(path, sizeof(path), "%s/SHA256SUMS", dir);
    int r1 = read_whole(path, &sums, &slen);
    snprintf(path, sizeof(path), "%s/SHA256SUMS.sig", dir);
    int r2 = r1 == 0 ? read_whole(path, &sig, &glen) : r1;
    if (r1 < 0 || r2 < 0 || relsig_sums_verify(sums, slen, sig, glen, keyid, err, sizeof(err)) < 0) {
        if (r1 < 0 || r2 < 0)
            snprintf(err, sizeof(err), "%s: %s", r1 < 0 ? "SHA256SUMS" : "SHA256SUMS.sig",
                     strerror(-(r1 < 0 ? r1 : r2)));
        fprintf(stderr, "cosmo-upgrade: %s: %s; nothing more fetched, nothing written\n", base, err);
        free(sums);
        free(sig);
        return r1 < 0 || r2 < 0 ? EXIT_FAILED : EXIT_REFUSED;
    }
    say("verified SHA256SUMS, signed by key %s in " KEYS_DIR, keyid);
    for (int i = 0; i < SET_FILES && rc == EXIT_DONE; i++) {
        char name[64];
        uint8_t want[RELSIG_SHA256_SIZE], got[RELSIG_SHA256_SIZE];
        snprintf(name, sizeof(name), "cosmoos-" ARCH "-%s", g_set_names[i]);
        if (relsig_sums_lookup(sums, slen, name, want, err, sizeof(err)) < 0) {
            fprintf(stderr, "cosmo-upgrade: %s: %s; nothing written\n", base, err);
            rc = EXIT_REFUSED;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        /* A file kept from before, else fetched resuming a part; a copy
         * that does not match is fetched once more from byte zero (the
         * part may have come from another set), and then refused. */
        for (int attempt = 0;; attempt++) {
            uint8_t *data;
            size_t dlen;
            if (read_whole(path, &data, &dlen) == 0) {
                relsig_sha256(data, dlen, got);
                free(data);
                if (memcmp(got, want, sizeof(got)) == 0)
                    break;
                unlink(path);
            }
            if (attempt == 2) {
                fprintf(stderr, "cosmo-upgrade: %s/%s does not match SHA256SUMS; nothing written\n", base, name);
                rc = EXIT_REFUSED;
                break;
            }
            rc = fetch_to(&r, base, name, dir, attempt == 0, FETCH_MAX);
            if (rc != EXIT_DONE)
                break;
        }
    }
    free(sums);
    free(sig);
    if (rc != EXIT_DONE)
        return rc;
    say("fetched the set's four files, each matching SHA256SUMS");
    rc = upgrade(dir, check, stop);
    if (rc == EXIT_DONE && !check) {
        for (int i = 0; i < SET_FILES; i++) {
            snprintf(path, sizeof(path), "%s/cosmoos-" ARCH "-%s", dir, g_set_names[i]);
            unlink(path);
        }
        snprintf(path, sizeof(path), "%s/SHA256SUMS", dir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/SHA256SUMS.sig", dir);
        unlink(path);
        rmdir(dir);
    }
    return rc;
}

/* --list: the environments \cosmo\be pairs, and where each set and
 * snapshot stands. */
static int list_envs(void)
{
    struct esp e;
    char err[200];
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s\n", err);
        return EXIT_FAILED;
    }
    struct be_file be;
    if (be_load(&e, &be) < 0)
        say("what \\cosmo\\be paired is unknown; cosmo-upgrade --prune writes it again, empty, keeping every "
            "snapshot");
    struct set cur = { 0 }, prev = { 0 };
    char curtag[129] = "", prevtag[129] = "", rootset[129];
    if (esp_read_set(&e, e.set, &cur) == 0)
        manifest_tag(cur.file[3], curtag);
    if (e.prev_whole && esp_read_set(&e, e.pset, &prev) == 0)
        manifest_tag(prev.file[3], prevtag);
    set_free(&cur);
    set_free(&prev);
    root_set_read(rootset);
    struct rootsnap rs;
    struct snaps sn = { 0 };
    bool snaps = rootsnap_open(&rs, "/") == 0 && rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) == 0;
    printf("%-24s %-9s %-13s %s\n", "ENVIRONMENT", "SET", "SET-ID", "SNAPSHOT");
    for (int i = be.n - 1; i >= 0; i--) {
        const struct be_env *v = &be.env[i];
        const char *which = strcmp(v->set, curtag) == 0 ? "current" : strcmp(v->set, prevtag) == 0 ? "previous" : "gone";
        const struct cosmo_fsctl_snap *x = snaps ? be_snapshot(&sn, v) : NULL;
        printf("%-24s %-9s %.12s  %s\n", v->name, which, v->set,
               x == NULL ? "missing" : (x->flags & COSMO_FSCTL_SNAP_F_HELD) ? "on the root, held" : "on the root");
    }
    if (be.n == 0)
        printf("(none: no upgrade by this release has taken one yet)\n");
    say("the root's base system is %s", !rootset[0] ? "unrecorded (older than boot environments)"
                                       : strcmp(rootset, curtag) == 0 ? "the current set's"
                                       : strcmp(rootset, prevtag) == 0 ? "the previous set's" : "another set's");
    if (be.intent[0] && (strcmp(be.intent, curtag) != 0 || (rootset[0] && strcmp(rootset, be.intent) != 0)))
        say("\\cosmo\\be intends %s: cosmo-upgrade --repair", strcmp(be.intent, prevtag) == 0 ? "the previous set"
                                                                                       : "another pairing");
    if (snaps) {
        free(sn.v);
        rootsnap_close(&rs);
    }
    return EXIT_DONE;
}

/* --prune: environments beyond the newest BE_KEEP, and unpaired ones, go
 * (a held one stays); \cosmo\be is rewritten to match. */
static int prune(void)
{
    struct esp e;
    char err[200];
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s\n", err);
        return EXIT_FAILED;
    }
    struct be_file be;
    /* A file that does not parse is written again, empty: the environments
     * it paired are no longer known as such, and their snapshots all stay
     * (cosmo-snap list --all names them; cosmo-snap delete removes one). */
    bool damaged = be_load(&e, &be) < 0;
    struct rootsnap rs;
    struct snaps sn = { 0 };
    if (!damaged && (rootsnap_open(&rs, "/") < 0 || rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0)) {
        fprintf(stderr, "cosmo-upgrade: the root's snapshots: %s\n", strerror(errno));
        return EXIT_FAILED;
    }
    int before = be.n;
    if (!damaged) {
        be_sweep(&rs, &sn, &be, true);
        free(sn.v);
        rootsnap_close(&rs);
    }
    if (damaged)
        say("\\cosmo\\be written again, empty; every be-* snapshot stays");
    if ((be.n != before || damaged) && e.has_be) {
        struct edit ed;
        int rc = be_stage(&e, &be, &ed);
        if (rc == 0)
            rc = fat_commit(&e.f);
        if (rc == 0)
            rc = switch_sector(&e, e.lba, &ed, 1);
        if (rc == 0)
            rc = sweep_commit(&e);
        if (rc < 0)
            return fail("rewriting \\cosmo\\be", rc);
    }
    say("%d environment(s) paired", be.n);
    return EXIT_DONE;
}

static int rollback_set_only(void)
{
    g_set_only = true;
    int rc = rollback(NULL);
    g_set_only = false;
    return rc;
}

/* --repair: the root and the ESP made to run the pairing \cosmo\be
 * intends, after a crash left one half moved (docs/userland/design.md,
 * "Boot environments"): the set switched back, or the root's rollback
 * asked for again. Both mixed states boot; this ends them. */
/* Whatever else --repair did: \EFI\BOOT's loader made the set's (a
 * switch cut short before finish leaves the firmware's loader behind). */
static int repair_loader(void)
{
    struct esp e;
    char err[200];
    struct set cur = { 0 };
    if (esp_load(&e, err, sizeof(err)) < 0 || esp_read_set(&e, e.set, &cur) < 0)
        return EXIT_FAILED;
    int rc = finish_ex(&e, &cur, false);
    set_free(&cur);
    return rc;
}

static int repair(void)
{
    struct esp e;
    char err[200];
    if (esp_load(&e, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-upgrade: %s\n", err);
        return EXIT_FAILED;
    }
    struct be_file be;
    if (be_load(&e, &be) < 0) {
        fprintf(stderr, "cosmo-upgrade: the pairing \\cosmo\\be intends is unknown: %s\n", BE_DAMAGED);
        return EXIT_REFUSED;
    }
    struct set cur = { 0 }, prev = { 0 };
    char curtag[129] = "", prevtag[129] = "", rootset[129], key[17];
    if (esp_read_set(&e, e.set, &cur) < 0)
        return EXIT_FAILED;
    /* An upgrade or set-only rollback that switched the set and stopped
     * before its base pass: finished as running cosmo-upgrade again
     * would, which brings the root to the set (and its record with it). */
    if (pending_is(cur.file[3])) {
        say("an earlier run switched to the current set and was interrupted: finishing it");
        int rc = finish(&e, &cur);
        set_free(&cur);
        if (rc != EXIT_DONE)
            return rc;
        if (esp_load(&e, err, sizeof(err)) < 0 || esp_read_set(&e, e.set, &cur) < 0) {
            fprintf(stderr, "cosmo-upgrade: %s\n", err);
            return EXIT_FAILED;
        }
    }
    if (!be.intent[0]) {
        set_free(&cur);
        say("no boot environments recorded: nothing more to repair");
        return repair_loader();
    }
    manifest_tag(cur.file[3], curtag);
    set_free(&cur);
    root_set_read(rootset);
    bool esp_ok = strcmp(curtag, be.intent) == 0, root_ok = strcmp(rootset, be.intent) == 0;
    if (esp_ok && root_ok) {
        say("the root and the boot set are the pairing \\cosmo\\be intends: nothing to repair");
        return repair_loader();
    }
    /* The set half is checked before the root's rollback is asked for: a
     * root moved toward a set that then cannot follow is what this ends. */
    if (!esp_ok) {
        if (e.prev_whole && esp_read_set(&e, e.pset, &prev) == 0)
            manifest_tag(prev.file[3], prevtag);
        int v = prevtag[0] ? set_verify(&prev, true, key, err, sizeof(err)) : -1;
        set_free(&prev);
        if (strcmp(prevtag, be.intent) != 0 || v < 0) {
            fprintf(stderr, "cosmo-upgrade: the intended set is not a verified \\cosmo\\prev%s%s: nothing "
                            "repaired\n", v < 0 && prevtag[0] && strcmp(prevtag, be.intent) == 0 ? ": " : "",
                    v < 0 && prevtag[0] && strcmp(prevtag, be.intent) == 0 ? err : "");
            return EXIT_FAILED;
        }
    }
    bool changed = false, armed = false;
    if (!root_ok) {
        struct rootsnap rs;
        struct snaps sn = { 0 };
        if (rootsnap_open(&rs, "/") < 0 || rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0) {
            fprintf(stderr, "cosmo-upgrade: the root's snapshots: %s\n", strerror(errno));
            return EXIT_FAILED;
        }
        const struct be_env *env = be_for_set(&be, be.intent);
        const struct cosmo_fsctl_snap *x = be_snapshot(&sn, env);
        if (!rootset[0]) {
            fprintf(stderr, "cosmo-upgrade: the root records no set (older than boot environments): not repaired\n");
            return EXIT_FAILED;
        } else if (x != NULL && (x->flags & COSMO_FSCTL_SNAP_F_TARGET)) {
            say("the root's rollback to %s is pending: it applies at the next boot", x->name);
        } else if (x == NULL) {
            fprintf(stderr, "cosmo-upgrade: the intended environment's snapshot is not on the root: not repaired\n");
            return EXIT_FAILED;
        } else {
            char tag[64], safety[BE_NAME_MAX + 1], target[BE_NAME_MAX + 1];
            snprintf(target, sizeof(target), "%s", env->name);
            if (running_tag(tag, sizeof(tag)) < 0)
                snprintf(tag, sizeof(tag), "unknown");
            be_name(tag, snaps_taken, &sn, safety);
            struct cosmo_fsctl_snap ss;
            if (rootsnap_rollback(&rs, target, safety, &ss) < 0) {
                fprintf(stderr, "cosmo-upgrade: the root's rollback to %s: %s\n", target, strerror(errno));
                return EXIT_FAILED;
            }
            char u[33];
            uuid_hex(ss.uuid, u);
            be_add(&be, safety, u, rootset);
            struct edit ed;
            int rc = be_stage(&e, &be, &ed);
            if (rc == 0)
                rc = fat_commit(&e.f);
            if (rc == 0)
                rc = switch_sector(&e, e.lba, &ed, 1);
            if (rc < 0)
                return fail("writing \\cosmo\\be", rc);
            say("the root rolls back to %s at the next boot; the tree it replaces is kept as %s", target, safety);
            changed = armed = true;
        }
        free(sn.v);
        rootsnap_close(&rs);
    }
    if (!esp_ok) {
        int rc = rollback_set_only();
        if (rc != EXIT_DONE)
            return rc;
        changed = true;
    }
    int rc = repair_loader();
    if (rc != EXIT_DONE || !changed)
        return rc;
    say("repaired; reboot to run the pairing \\cosmo\\be intends");
    return armed ? reboot_to_apply() : EXIT_DONE;
}

/* --after-rollback, from init before any service, at the first boot after
 * a boot-environment rollback (the live tree is a held rollback's target,
 * be-*): the operator's files written under /root, /home and /var/log in
 * the tree it replaced -- added or changed since the environment was taken,
 * by B1's diff of the two -- are copied into /var/lib/cosmo/rollback/<that
 * tree's name>/. Nothing else is kept; once per rollback. */
#define KEEP_DIR "/var/lib/cosmo/rollback"

static bool kept_path(const char *p)
{
    static const char *const roots[] = { "/root/", "/home/", "/var/log/" };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (strncmp(p, roots[i], strlen(roots[i])) == 0)
            return true;
    return false;
}

static int mkdirs_for(const char *path)
{
    char *d = strdup(path);
    if (d == NULL)
        return -1;
    for (char *s = d + 1; *s; s++)
        if (*s == '/') {
            *s = '\0';
            if (mkdir(d, 0755) < 0 && errno != EEXIST) {
                free(d);
                return -1;
            }
            *s = '/';
        }
    free(d);
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    uint8_t *data;
    size_t len;
    int rc = read_whole(from, &data, &len);
    if (rc < 0)
        return rc;
    if (mkdirs_for(to) < 0) {
        free(data);
        return -errno;
    }
    int fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = fd >= 0;
    for (size_t off = 0; ok && off < len;) {
        ssize_t w = write(fd, data + off, len - off);
        ok = w > 0;
        off += ok ? (size_t)w : 0;
    }
    ok = ok && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    free(data);
    return ok ? 0 : -EIO;
}

static char *path_join(const char *a, const char *b, const char *c)
{
    size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
    char *s = malloc(n);
    if (s != NULL)
        snprintf(s, n, "%s%s%s", a, b, c);
    return s;
}

/* Exit status 0 only when every file is kept: init keeps the rollback's
 * hold otherwise (the replaced tree stays undeletable), and the next boot
 * tries again. `.done` marks a rollback whose files are all kept. */
static int after_rollback(void)
{
    struct rootsnap rs;
    struct snaps sn = { 0 };
    if (rootsnap_open(&rs, "/") < 0 || rootsnap_list(&rs, &sn.pool, &sn.v, &sn.n) < 0)
        return errno == EOPNOTSUPP ? EXIT_DONE : fail("the root's snapshots", errno);
    const struct cosmo_fsctl_snap *live = NULL, *held = NULL;
    for (unsigned i = 0; i < sn.n; i++) {
        if ((sn.v[i].flags & COSMO_FSCTL_SNAP_F_LIVE) && strncmp(sn.v[i].name, "be-", 3) == 0)
            live = &sn.v[i];
        if ((sn.v[i].flags & COSMO_FSCTL_SNAP_F_HELD) && strncmp(sn.v[i].name, "be-", 3) == 0)
            held = &sn.v[i];
    }
    if (live == NULL || held == NULL) {
        free(sn.v);
        rootsnap_close(&rs);
        return EXIT_DONE;   /* not the boot after an environment rollback */
    }
    char dest[256], done[300], diffs[] = "/tmp/cosmo-upgrade-diff";
    snprintf(dest, sizeof(dest), KEEP_DIR "/%s", held->name);
    snprintf(done, sizeof(done), "%s/.done", dest);
    struct stat st;
    if (stat(done, &st) == 0) {
        free(sn.v);
        rootsnap_close(&rs);
        return EXIT_DONE;
    }
    int out = open(diffs, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0)
        return fail(diffs, errno);
    const char *argv[] = { "cosmo-snap", "diff", live->name, held->name, NULL };
    struct spawn_handle map[] = { { .child = 0, .parent = 0 }, { .child = 1, .parent = out },
                                  { .child = 2, .parent = 2 } };
    pid_t pid = spawnve("/sbin/cosmo-snap", argv, NULL, map, 3);
    close(out);
    int ws = -1;
    if (pid <= 0 || waitpid(pid, &ws, 0) != pid || ws != 0) {
        fprintf(stderr, "cosmo-upgrade: cosmo-snap diff %s %s failed; the files written after the upgrade are in "
                        "snapshot %s, kept for the next boot to try again\n", live->name, held->name, held->name);
        return EXIT_FAILED;
    }
    uint8_t *text;
    size_t len;
    if (read_whole(diffs, &text, &len) < 0)
        return fail(diffs, errno);
    unsigned copied = 0, failed = 0;
    char snapdir[64];
    snprintf(snapdir, sizeof(snapdir), "/.snapshots/%s", held->name);
    for (size_t pos = 0; pos < len;) {
        char *l = (char *)text + pos;
        char *nl = memchr(l, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - l) : len - pos;
        pos += ll + 1;
        if (ll < 3 || (l[0] != '+' && l[0] != 'M') || l[1] != ' ')
            continue;
        char *path = strndup(l + 2, ll - 2);
        if (path == NULL) {
            failed++;
            continue;
        }
        char *from = kept_path(path) ? path_join(snapdir, path, "") : NULL;
        if (kept_path(path) && from == NULL)
            failed++;
        if (from != NULL && lstat(from, &st) == 0 && S_ISREG(st.st_type)) {
            char *to = path_join(dest, path, "");
            if (to != NULL && copy_file(from, to) == 0)
                copied++;
            else
                failed++;
            free(to);
        }
        free(from);
        free(path);
    }
    free(text);
    unlink(diffs);
    if (failed) {
        fprintf(stderr, "cosmo-upgrade: %u of the files written under /root, /home and /var/log after %s was taken "
                        "could not be copied to %s (%u were); they are in snapshot %s, kept for the next boot to "
                        "try again\n", failed, live->name, dest, copied, held->name);
        free(sn.v);
        rootsnap_close(&rs);
        return EXIT_FAILED;
    }
    int fd = mkdirs_for(done) == 0 ? open(done, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    if (fd < 0 || fsync(fd) < 0) {
        if (fd >= 0)
            close(fd);
        return fail(done, errno);
    }
    close(fd);
    sync();
    say("%u file(s) written under /root, /home and /var/log after %s was taken are kept in %s", copied, live->name,
        dest);
    free(sn.v);
    rootsnap_close(&rs);
    return EXIT_DONE;
}

static int usage(void)
{
    fprintf(stderr, "usage: cosmo-upgrade [--check] [--stop-after=written|loader|set] DIR\n"
                    "       cosmo-upgrade [--check] [--stop-after=written|loader|set] --fetch TAG|URL\n"
                    "       cosmo-upgrade --check\n"
                    "       cosmo-upgrade --rollback [--no-reboot] [--stop-after=intent|record|set]\n"
                    "       cosmo-upgrade --repair [--no-reboot]\n"
                    "       cosmo-upgrade --list | --status | --prune\n");
    return EXIT_USAGE;
}

int main(int argc, char **argv)
{
    bool check = false, roll = false, stat_only = false;
    const char *stop = NULL, *dir = NULL, *fetch = NULL, *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            check = true;
        else if (strcmp(argv[i], "--fetch") == 0 && i + 1 < argc && fetch == NULL)
            fetch = argv[++i];
        else if (strcmp(argv[i], "--rollback") == 0)
            roll = true;
        else if (strcmp(argv[i], "--status") == 0)
            stat_only = true;
        else if ((strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "--repair") == 0 ||
                  strcmp(argv[i], "--prune") == 0 || strcmp(argv[i], "--after-rollback") == 0) && only == NULL)
            only = argv[i];
        else if (strncmp(argv[i], "--stop-after=", 13) == 0)
            stop = argv[i] + 13;
        else if (strcmp(argv[i], "--no-reboot") == 0)
            g_no_reboot = true;
        else if (argv[i][0] == '-' || dir != NULL)
            return usage();
        else
            dir = argv[i];
    }
    if (only != NULL && argc != 2 + (g_no_reboot ? 1 : 0))
        return usage();
    if (g_no_reboot && !roll && !(only && strcmp(only, "--repair") == 0))
        return usage();
    if (stop && !roll && strcmp(stop, "written") != 0 && strcmp(stop, "loader") != 0 && strcmp(stop, "set") != 0)
        return usage();
    if (stop && roll && strcmp(stop, "intent") != 0 && strcmp(stop, "record") != 0 && strcmp(stop, "set") != 0)
        return usage();
    if (fetch && dir)
        return usage();
    if (only == NULL && ((roll || stat_only) ? (dir != NULL || fetch || check || (stop && !roll) || (roll && stat_only))
                                             : (dir == NULL && fetch == NULL && (!check || stop))))
        return usage();
    bool latest = check && dir == NULL && fetch == NULL;
    struct timespec ts;
    g_now = clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_sec : 0;
    bool reads_only = stat_only || (only && strcmp(only, "--list") == 0);
    if (!reads_only && !check && hold_lock() < 0)
        return EXIT_REFUSED;
    relsig_ring_load(KEYS_DIR);
    if (only && strcmp(only, "--list") == 0)
        return list_envs();
    if (only && strcmp(only, "--repair") == 0)
        return repair();
    if (only && strcmp(only, "--prune") == 0)
        return prune();
    if (only && strcmp(only, "--after-rollback") == 0)
        return after_rollback();
    if (latest)
        return check_latest();
    if (stat_only)
        return status();
    if (fetch)
        return fetch_set(fetch, check, stop);
    if (roll)
        return rollback(stop);
    return upgrade(dir, check, stop);
}
