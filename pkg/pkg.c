/*
 * pkg.c - The package manager (docs/pkg/): update, install, remove,
 * upgrade, list, info, search, verify. Everything untrusted (packages,
 * the index) is verified before a byte of it is acted on; installation
 * writes through temporary names and rename and rolls one package back
 * on failure; the database is text under /var/db/pkg.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pkg.h"
#include "verify.h"

static bool g_dry_run, g_force, g_reinstall;
static struct index g_index;
static bool g_index_loaded;

/* --- files ------------------------------------------------------------------ */

static int read_whole(const char *path, uint8_t **out, size_t *len, size_t limit)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size > limit) {
        close(fd);
        errno = st.st_size > limit ? EFBIG : errno;
        return -1;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size + 1);
    if (buf == NULL) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) {
            free(buf);
            close(fd);
            errno = n == 0 ? EIO : errno;
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    buf[size] = '\0';
    *out = buf;
    *len = size;
    return 0;
}

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

/* Write `path` through "<path>.pkgtmp" and rename. */
static int write_atomic(const char *path, const void *data, size_t len, unsigned mode)
{
    char tmp[PKG_PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.pkgtmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    int rc = write_all(fd, data, len);
    close(fd);
    if (rc == 0)
        rc = rename(tmp, path);
    if (rc)
        unlink(tmp);
    return rc;
}

struct dirlist {
    char paths[PKG_MAX_DIRS][PKG_PATH_MAX];
    int n;
};

/* mkdir -p for the directory of `path`, recording the directories created. */
static int ensure_parent_dirs(const char *path, struct dirlist *created)
{
    char buf[PKG_PATH_MAX];
    strlcpy(buf, path, sizeof(buf));
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        struct stat st;
        if (stat(buf, &st) < 0) {
            if (mkdir(buf, 0755) < 0 && errno != EEXIST)
                return -1;
            if (created && created->n < PKG_MAX_DIRS) {
                strlcpy(created->paths[created->n], buf, PKG_PATH_MAX);
                created->n++;
            }
        } else if (!S_ISDIR(st.st_type)) {
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static void remove_dirs(const struct dirlist *dirs)
{
    for (int i = dirs->n - 1; i >= 0; i--)
        rmdir(dirs->paths[i]);   /* only empty ones go */
}

/* --- the database ----------------------------------------------------------- */

static int db_init(void)
{
    struct dirlist d = { .n = 0 };
    if (ensure_parent_dirs(PKG_DB_INSTALLED "/x", &d) < 0) {
        fprintf(stderr, "pkg: cannot create %s: %s\n", PKG_DB_INSTALLED, strerror(errno));
        return -1;
    }
    return 0;
}

static int lock_acquire(void)
{
    int fd = open(PKG_DB_LOCK, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            fprintf(stderr, "pkg: another pkg is running (remove %s if not)\n", PKG_DB_LOCK);
        else
            fprintf(stderr, "pkg: cannot lock %s: %s\n", PKG_DB_LOCK, strerror(errno));
        return -1;
    }
    close(fd);
    return 0;
}

static void lock_release(void)
{
    unlink(PKG_DB_LOCK);
}

static void installed_path(const char *name, const char *what, char *out, size_t n)
{
    snprintf(out, n, PKG_DB_INSTALLED "/%s/%s", name, what);
}

static bool installed_load(const char *name, struct manifest *m)
{
    char path[PKG_PATH_MAX];
    installed_path(name, "MANIFEST", path, sizeof(path));
    uint8_t *text;
    size_t len;
    if (read_whole(path, &text, &len, PKG_INDEX_LIMIT) < 0)
        return false;
    char err[PKG_ERR_MAX];
    int rc = manifest_parse((const char *)text, len, m, err, sizeof(err));
    free(text);
    if (rc) {
        fprintf(stderr, "pkg: %s: damaged record: %s\n", name, err);
        return false;
    }
    return true;
}

static int installed_names(char names[][PKG_NAME_MAX], int max)
{
    DIR *d = opendir(PKG_DB_INSTALLED);
    if (d == NULL)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.' || !name_valid(e->d_name))
            continue;
        strlcpy(names[n++], e->d_name, PKG_NAME_MAX);
    }
    closedir(d);
    return n;
}

static int installed_store(const char *name, const char *manifest_text, size_t len, const struct dirlist *dirs)
{
    char dir[PKG_PATH_MAX], path[PKG_PATH_MAX];
    snprintf(dir, sizeof(dir), PKG_DB_INSTALLED "/%s", name);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -1;
    installed_path(name, "MANIFEST", path, sizeof(path));
    if (write_atomic(path, manifest_text, len, 0644) < 0)
        return -1;
    char buf[PKG_MAX_DIRS * 64];
    size_t used = 0;
    for (int i = 0; i < dirs->n && used + strlen(dirs->paths[i]) + 2 < sizeof(buf); i++)
        used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%s\n", dirs->paths[i]);
    installed_path(name, "DIRS", path, sizeof(path));
    return write_atomic(path, buf, used, 0644);
}

static void installed_dirs(const char *name, struct dirlist *dirs)
{
    dirs->n = 0;
    char path[PKG_PATH_MAX];
    installed_path(name, "DIRS", path, sizeof(path));
    uint8_t *text;
    size_t len;
    if (read_whole(path, &text, &len, 1 << 20) < 0)
        return;
    char *save;
    for (char *l = strtok_r((char *)text, "\n", &save); l && dirs->n < PKG_MAX_DIRS; l = strtok_r(NULL, "\n", &save))
        strlcpy(dirs->paths[dirs->n++], l, PKG_PATH_MAX);
    free(text);
}

static void installed_drop(const char *name)
{
    char path[PKG_PATH_MAX];
    installed_path(name, "MANIFEST", path, sizeof(path));
    unlink(path);
    installed_path(name, "DIRS", path, sizeof(path));
    unlink(path);
    snprintf(path, sizeof(path), PKG_DB_INSTALLED "/%s", name);
    rmdir(path);
}

/* --- the index and the key ring --------------------------------------------- */

static int keys_load(void)
{
    int n = ring_load(PKG_KEYS_DIR);
    if (n <= 0) {
        fprintf(stderr, "pkg: no signing keys in %s\n", PKG_KEYS_DIR);
        return -1;
    }
    return 0;
}

static int index_load(void)
{
    if (g_index_loaded)
        return 0;
    uint8_t *text;
    size_t len;
    if (read_whole(PKG_DB_INDEX, &text, &len, PKG_INDEX_LIMIT) < 0) {
        fprintf(stderr, "pkg: no index: run 'pkg update' first\n");
        return -1;
    }
    char err[PKG_ERR_MAX];
    int rc = index_parse((const char *)text, len, &g_index, err, sizeof(err));
    free(text);
    if (rc) {
        fprintf(stderr, "pkg: stored index: %s\n", err);
        return -1;
    }
    g_index_loaded = true;
    return 0;
}

/* Newest index entry of `name` satisfying `d` (or any version when d is NULL). */
static struct index_entry *index_best(const char *name, const struct depend *d)
{
    struct index_entry *best = NULL;
    for (int i = 0; i < g_index.n; i++) {
        struct index_entry *e = &g_index.entries[i];
        if (strcmp(e->name, name) != 0)
            continue;
        if (d && d->op != OP_NONE && !depend_satisfied(d, e->version))
            continue;
        if (best == NULL || version_cmp(e->version, best->version) > 0)
            best = e;
    }
    return best;
}

/* Repository directories from /etc/pkg/repos.conf. */
static int repos_load(char repos[][PKG_PATH_MAX], int max)
{
    uint8_t *text;
    size_t len;
    if (read_whole(PKG_REPOS_CONF, &text, &len, 1 << 16) < 0) {
        fprintf(stderr, "pkg: cannot read %s: %s\n", PKG_REPOS_CONF, strerror(errno));
        return -1;
    }
    int n = 0;
    char *save;
    for (char *l = strtok_r((char *)text, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ')
            l++;
        if (*l == '#' || *l == '\0')
            continue;
        strlcpy(repos[n++], l, PKG_PATH_MAX);
    }
    free(text);
    return n;
}

static int cmd_update(void)
{
    char repos[8][PKG_PATH_MAX];
    int nrepos = repos_load(repos, 8);
    if (nrepos <= 0) {
        fprintf(stderr, "pkg: no repositories configured\n");
        return EXIT_FAILED;
    }
    int total = 0;
    /* Phase 10: one repository is the common case; several are
     * concatenated in configuration order after each is verified. */
    char *merged = NULL;
    size_t mlen = 0;
    for (int i = 0; i < nrepos; i++) {
        char path[PKG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/INDEX", repos[i]);
        uint8_t *blob;
        size_t len, payload;
        if (read_whole(path, &blob, &len, PKG_INDEX_LIMIT) < 0) {
            fprintf(stderr, "pkg: %s: %s\n", path, strerror(errno));
            free(merged);
            return EXIT_FAILED;
        }
        char err[PKG_ERR_MAX];
        if (verify_signed(blob, len, &payload, err, sizeof(err)) < 0) {
            fprintf(stderr, "pkg: %s: %s\n", path, err);
            free(blob);
            free(merged);
            return EXIT_REFUSED;
        }
        struct index ix;
        if (index_parse((const char *)blob, payload, &ix, err, sizeof(err)) < 0) {
            fprintf(stderr, "pkg: %s: %s\n", path, err);
            free(blob);
            free(merged);
            return EXIT_REFUSED;
        }
        total += ix.n;
        index_free(&ix);
        char *m = realloc(merged, mlen + payload + 1);
        if (m == NULL) {
            free(blob);
            free(merged);
            return EXIT_FAILED;
        }
        merged = m;
        if (mlen && merged[mlen - 1] != '\n')
            merged[mlen++] = '\n';
        if (mlen)
            merged[mlen++] = '\n';   /* stanza separator between repositories */
        memcpy(merged + mlen, blob, payload);
        mlen += payload;
        free(blob);
    }
    if (g_dry_run) {
        printf("%d packages available\n", total);
        free(merged);
        return 0;
    }
    if (write_atomic(PKG_DB_INDEX, merged, mlen, 0644) < 0) {
        fprintf(stderr, "pkg: cannot write %s: %s\n", PKG_DB_INDEX, strerror(errno));
        free(merged);
        return EXIT_FAILED;
    }
    free(merged);
    printf("pkg: index updated: %d packages from %d repositor%s\n", total, nrepos, nrepos == 1 ? "y" : "ies");
    return 0;
}

/* --- loading and checking a package ------------------------------------------ */

struct loaded {
    uint8_t *blob;
    size_t len, payload;
    struct manifest m;
    const uint8_t **data;   /* per manifest file, into blob */
    char *manifest_text;
    size_t manifest_len;
};

static void loaded_free(struct loaded *l)
{
    free(l->blob);
    free(l->data);
    free(l->manifest_text);
    manifest_free(&l->m);
    memset(l, 0, sizeof(*l));
}

/* Read, verify the signature, parse and cross-check the manifest against
 * the members. Returns EXIT_* on failure with a message printed. */
static int load_package(const char *path, const struct index_entry *expect, struct loaded *l)
{
    memset(l, 0, sizeof(*l));
    char err[PKG_ERR_MAX];
    int rc = EXIT_REFUSED;
    char *text = NULL;
    if (read_whole(path, &l->blob, &l->len, PKG_PACKAGE_LIMIT) < 0) {
        fprintf(stderr, "pkg: %s: %s\n", path, strerror(errno));
        return EXIT_FAILED;
    }
    if (expect) {
        uint8_t sha[SHA512_LEN];
        sha512_of(l->blob, l->len, sha);
        if (l->len != expect->size || memcmp(sha, expect->sha, SHA512_LEN) != 0) {
            fprintf(stderr, "pkg: %s: checksum does not match the index (corrupt or replaced)\n", path);
            goto out;
        }
    }
    if (verify_signed(l->blob, l->len, &l->payload, err, sizeof(err)) < 0) {
        fprintf(stderr, "pkg: %s: %s\n", path, err);
        goto out;
    }
    struct tar_reader r;
    tar_open(&r, l->blob, l->payload);
    struct tar_member mem;
    int got = tar_next(&r, &mem, err, sizeof(err));
    if (got <= 0 || strcmp(mem.name, "+MANIFEST") != 0) {
        fprintf(stderr, "pkg: %s: %s\n", path, got < 0 ? err : "no +MANIFEST as the first member");
        goto out;
    }
    size_t manifest_size = (size_t)mem.size;
    text = malloc(manifest_size + 1);
    if (text == NULL) {
        rc = EXIT_FAILED;
        goto out;
    }
    memcpy(text, mem.data, manifest_size);
    text[manifest_size] = '\0';
    if (manifest_parse(text, manifest_size, &l->m, err, sizeof(err)) < 0) {
        fprintf(stderr, "pkg: %s: %s\n", path, err);
        goto out;
    }
    if (expect && (strcmp(l->m.name, expect->name) != 0 || strcmp(l->m.version, expect->version) != 0)) {
        fprintf(stderr, "pkg: %s: manifest names %s-%s, the index %s-%s\n", path, l->m.name, l->m.version,
                expect->name, expect->version);
        goto out;
    }
    l->data = calloc((size_t)l->m.nfiles + 1, sizeof(*l->data));
    if (l->data == NULL) {
        rc = EXIT_FAILED;
        goto out;
    }
    /* Members and manifest lines in lockstep. */
    for (int i = 0; i < l->m.nfiles; i++) {
        struct file_entry *f = &l->m.files[i];
        got = tar_next(&r, &mem, err, sizeof(err));
        if (got < 0) {
            fprintf(stderr, "pkg: %s: %s\n", path, err);
            goto out;
        }
        if (got == 0 || strcmp(mem.name, f->path) != 0 || mem.size != f->size || mem.mode != f->mode) {
            fprintf(stderr, "pkg: %s: member %d does not match manifest entry %s\n", path, i + 1, f->path);
            goto out;
        }
        uint8_t sha[SHA512_LEN];
        sha512_of(mem.data, mem.size, sha);
        if (memcmp(sha, f->sha, SHA512_LEN) != 0) {
            fprintf(stderr, "pkg: %s: checksum mismatch for %s\n", path, f->path);
            goto out;
        }
        l->data[i] = mem.data;
    }
    got = tar_next(&r, &mem, err, sizeof(err));
    if (got != 0) {
        fprintf(stderr, "pkg: %s: %s\n", path, got < 0 ? err : "member not listed in the manifest");
        goto out;
    }
    l->manifest_text = text;   /* owned by `l` from here (loaded_free) */
    l->manifest_len = manifest_size;
    return 0;
out:
    free(text);
    free(l->blob);
    free(l->data);
    manifest_free(&l->m);
    memset(l, 0, sizeof(*l));
    return rc;
}

/* --- installing one package -------------------------------------------------- */

/* Another installed package owning `path`, or NULL. */
static bool owned_by_other(const char *path, const char *self, char *owner, size_t ownerlen)
{
    char names[128][PKG_NAME_MAX];
    int n = installed_names(names, 128);
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], self) == 0)
            continue;
        struct manifest m;
        if (!installed_load(names[i], &m))
            continue;
        bool found = false;
        for (int k = 0; k < m.nfiles && !found; k++)
            found = strcmp(m.files[k].path, path) == 0;
        manifest_free(&m);
        if (found) {
            strlcpy(owner, names[i], ownerlen);
            return true;
        }
    }
    return false;
}

static int install_loaded(struct loaded *l)
{
    struct manifest old;
    bool had_old = installed_load(l->m.name, &old);
    struct dirlist old_dirs = { .n = 0 };
    if (had_old)
        installed_dirs(l->m.name, &old_dirs);

    for (int i = 0; i < l->m.nfiles; i++) {
        char owner[PKG_NAME_MAX];
        if (owned_by_other(l->m.files[i].path, l->m.name, owner, sizeof(owner))) {
            fprintf(stderr, "pkg: %s: /%s is owned by %s\n", l->m.name, l->m.files[i].path, owner);
            if (had_old)
                manifest_free(&old);
            return EXIT_FAILED;
        }
    }
    printf("pkg: installing %s-%s (%d files)\n", l->m.name, l->m.version, l->m.nfiles);
    if (g_dry_run) {
        if (had_old)
            manifest_free(&old);
        return 0;
    }

    struct dirlist created = { .n = 0 };
    int done = 0;
    int rc = 0;
    for (; done < l->m.nfiles; done++) {
        struct file_entry *f = &l->m.files[done];
        char path[PKG_PATH_MAX];
        snprintf(path, sizeof(path), "/%s", f->path);
        if (ensure_parent_dirs(path, &created) < 0 || write_atomic(path, l->data[done], (size_t)f->size, f->mode) < 0) {
            fprintf(stderr, "pkg: %s: cannot write %s: %s\n", l->m.name, path, strerror(errno));
            rc = EXIT_FAILED;
            break;
        }
    }
    if (rc) {
        /* Roll back this package's files; a previous version's files with the same paths are lost too. */
        for (int i = 0; i < done; i++) {
            char path[PKG_PATH_MAX];
            snprintf(path, sizeof(path), "/%s", l->m.files[i].path);
            unlink(path);
        }
        remove_dirs(&created);
        if (had_old)
            manifest_free(&old);
        return rc;
    }
    if (had_old) {
        for (int i = 0; i < old.nfiles; i++) {
            bool still = false;
            for (int k = 0; k < l->m.nfiles && !still; k++)
                still = strcmp(old.files[i].path, l->m.files[k].path) == 0;
            if (!still) {
                char path[PKG_PATH_MAX];
                snprintf(path, sizeof(path), "/%s", old.files[i].path);
                unlink(path);
            }
        }
        remove_dirs(&old_dirs);
        /* Directories still in use stay; carry them into the new record. */
        for (int i = 0; i < old_dirs.n && created.n < PKG_MAX_DIRS; i++) {
            struct stat st;
            if (stat(old_dirs.paths[i], &st) == 0)
                strlcpy(created.paths[created.n++], old_dirs.paths[i], PKG_PATH_MAX);
        }
        manifest_free(&old);
    }
    if (installed_store(l->m.name, l->manifest_text, l->manifest_len, &created) < 0) {
        fprintf(stderr, "pkg: %s: cannot record installation: %s\n", l->m.name, strerror(errno));
        return EXIT_FAILED;
    }
    return 0;
}

/* --- resolution ------------------------------------------------------------- */

struct plan {
    struct index_entry *entries[PKG_MAX_INDEX];
    int n;
    const char *visiting[PKG_MAX_INDEX];
    int nvisiting;
};

static bool plan_has(const struct plan *p, const char *name, struct index_entry **out)
{
    for (int i = 0; i < p->n; i++) {
        if (strcmp(p->entries[i]->name, name) == 0) {
            if (out)
                *out = p->entries[i];
            return true;
        }
    }
    return false;
}

static int resolve(struct plan *p, const struct depend *d, const char *why)
{
    struct index_entry *chosen;
    if (plan_has(p, d->name, &chosen)) {
        if (!depend_satisfied(d, chosen->version)) {
            fprintf(stderr, "pkg: %s needs %s %s %s but %s-%s is planned\n", why, d->name, op_text(d->op), d->version,
                    chosen->name, chosen->version);
            return EXIT_FAILED;
        }
        return 0;
    }
    for (int i = 0; i < p->nvisiting; i++) {
        if (strcmp(p->visiting[i], d->name) == 0) {
            fprintf(stderr, "pkg: dependency cycle through %s\n", d->name);
            return EXIT_FAILED;
        }
    }
    struct manifest inst;
    if (!g_reinstall && installed_load(d->name, &inst)) {
        bool ok = depend_satisfied(d, inst.version);
        char ver[PKG_VERSION_MAX];
        strlcpy(ver, inst.version, sizeof(ver));
        manifest_free(&inst);
        if (ok)
            return 0;   /* already there and good enough */
        struct index_entry *e = index_best(d->name, d);
        if (e == NULL) {
            fprintf(stderr, "pkg: %s needs %s %s %s; installed %s, nothing better in the index\n", why, d->name,
                    op_text(d->op), d->version, ver);
            return EXIT_FAILED;
        }
        printf("pkg: %s will be upgraded from %s to %s\n", d->name, ver, e->version);
    }
    chosen = index_best(d->name, d);
    if (chosen == NULL) {
        if (d->op == OP_NONE)
            fprintf(stderr, "pkg: %s: no such package in the index\n", d->name);
        else
            fprintf(stderr, "pkg: %s needs %s %s %s: no such version in the index\n", why, d->name, op_text(d->op),
                    d->version);
        return EXIT_FAILED;
    }
    p->visiting[p->nvisiting++] = d->name;
    for (int i = 0; i < chosen->ndepends; i++) {
        int rc = resolve(p, &chosen->depends[i], chosen->name);
        if (rc)
            return rc;
    }
    p->nvisiting--;
    p->entries[p->n++] = chosen;   /* dependencies first */
    return 0;
}

static int install_entry(const struct index_entry *e)
{
    char repos[8][PKG_PATH_MAX];
    int nrepos = repos_load(repos, 8);
    struct loaded l;
    int rc = EXIT_FAILED;
    for (int i = 0; i < nrepos; i++) {
        char path[PKG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", repos[i], e->file);
        struct stat st;
        if (stat(path, &st) < 0)
            continue;
        rc = load_package(path, e, &l);
        if (rc)
            return rc;
        rc = install_loaded(&l);
        loaded_free(&l);
        return rc;
    }
    fprintf(stderr, "pkg: %s: not found in any repository\n", e->file);
    return EXIT_FAILED;
}

static int cmd_install(int argc, char **argv)
{
    if (argc == 0) {
        fprintf(stderr, "usage: pkg install [-n] [-f] [--reinstall] NAME[=VERSION]... | FILE.cpk...\n");
        return EXIT_USAGE;
    }
    int rc = 0;
    struct plan p = { .n = 0, .nvisiting = 0 };
    for (int i = 0; i < argc && rc == 0; i++) {
        if (strchr(argv[i], '/')) {
            /* A package file: its dependencies must already be installed or in the index. */
            struct loaded l;
            rc = load_package(argv[i], NULL, &l);
            if (rc)
                return rc;
            for (int k = 0; k < l.m.ndepends && rc == 0; k++) {
                struct manifest inst;
                bool ok = installed_load(l.m.depends[k].name, &inst);
                if (ok) {
                    ok = depend_satisfied(&l.m.depends[k], inst.version);
                    manifest_free(&inst);
                }
                if (!ok) {
                    if (index_load() == 0)
                        rc = resolve(&p, &l.m.depends[k], l.m.name);
                    else
                        rc = EXIT_FAILED;
                }
            }
            for (int k = 0; k < p.n && rc == 0; k++)
                rc = install_entry(p.entries[k]);
            p.n = 0;
            if (rc == 0)
                rc = install_loaded(&l);
            loaded_free(&l);
            continue;
        }
        struct depend d;
        char spec[PKG_NAME_MAX + PKG_VERSION_MAX + 4];
        const char *eq = strchr(argv[i], '=');
        if (eq)
            snprintf(spec, sizeof(spec), "%.*s = %s", (int)(eq - argv[i]), argv[i], eq + 1);   /* name=version */
        else
            strlcpy(spec, argv[i], sizeof(spec));
        if (!depend_parse(spec, &d)) {
            fprintf(stderr, "pkg: bad package specification '%s'\n", argv[i]);
            return EXIT_USAGE;
        }
        if (index_load())
            return EXIT_FAILED;
        rc = resolve(&p, &d, "the request");
    }
    for (int k = 0; k < p.n && rc == 0; k++)
        rc = install_entry(p.entries[k]);
    return rc;
}

/* --- remove ------------------------------------------------------------------ */

static int cmd_remove(int argc, char **argv)
{
    if (argc == 0) {
        fprintf(stderr, "usage: pkg remove [-f] NAME...\n");
        return EXIT_USAGE;
    }
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct manifest m;
        if (!installed_load(argv[i], &m)) {
            fprintf(stderr, "pkg: %s is not installed\n", argv[i]);
            rc = EXIT_FAILED;
            continue;
        }
        /* Dependants. */
        char names[128][PKG_NAME_MAX];
        int n = installed_names(names, 128);
        bool blocked = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(names[k], argv[i]) == 0)
                continue;
            struct manifest other;
            if (!installed_load(names[k], &other))
                continue;
            for (int j = 0; j < other.ndepends; j++) {
                if (strcmp(other.depends[j].name, argv[i]) == 0) {
                    fprintf(stderr, "pkg: %s: %s depends on it%s\n", argv[i], names[k], g_force ? " (removing anyway)" : "");
                    if (!g_force)
                        blocked = true;
                }
            }
            manifest_free(&other);
        }
        if (blocked) {
            manifest_free(&m);
            rc = EXIT_FAILED;
            continue;
        }
        printf("pkg: removing %s-%s (%d files)\n", m.name, m.version, m.nfiles);
        if (!g_dry_run) {
            for (int k = 0; k < m.nfiles; k++) {
                char path[PKG_PATH_MAX];
                snprintf(path, sizeof(path), "/%s", m.files[k].path);
                if (unlink(path) < 0 && errno != ENOENT)
                    fprintf(stderr, "pkg: %s: cannot remove %s: %s\n", m.name, path, strerror(errno));
            }
            struct dirlist dirs;
            installed_dirs(m.name, &dirs);
            remove_dirs(&dirs);
            installed_drop(m.name);
        }
        manifest_free(&m);
    }
    return rc;
}

/* --- upgrade ---------------------------------------------------------------- */

static int cmd_upgrade(int argc, char **argv)
{
    if (index_load())
        return EXIT_FAILED;
    char names[128][PKG_NAME_MAX];
    int n = argc ? argc : installed_names(names, 128);
    int rc = 0, upgraded = 0;
    for (int i = 0; i < n && rc == 0; i++) {
        const char *name = argc ? argv[i] : names[i];
        struct manifest inst;
        if (!installed_load(name, &inst)) {
            if (argc) {
                fprintf(stderr, "pkg: %s is not installed\n", name);
                rc = EXIT_FAILED;
            }
            continue;
        }
        struct index_entry *best = index_best(name, NULL);
        bool newer = best && version_cmp(best->version, inst.version) > 0;
        manifest_free(&inst);
        if (!newer)
            continue;
        struct plan p = { .n = 0, .nvisiting = 0 };
        struct depend d = { .op = OP_EQ };
        strlcpy(d.name, name, sizeof(d.name));
        strlcpy(d.version, best->version, sizeof(d.version));
        rc = resolve(&p, &d, "upgrade");
        for (int k = 0; k < p.n && rc == 0; k++)
            rc = install_entry(p.entries[k]);
        if (rc == 0)
            upgraded++;
    }
    if (rc == 0)
        printf("pkg: %d package%s upgraded\n", upgraded, upgraded == 1 ? "" : "s");
    return rc;
}

/* --- queries ----------------------------------------------------------------- */

static int cmd_list(void)
{
    char names[128][PKG_NAME_MAX];
    int n = installed_names(names, 128);
    for (int i = 0; i < n; i++) {
        struct manifest m;
        if (installed_load(names[i], &m)) {
            printf("%-20s %-10s %s\n", m.name, m.version, m.summary);
            manifest_free(&m);
        }
    }
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct manifest m;
        bool inst = installed_load(argv[i], &m);
        struct index_entry *e = NULL;
        if (index_load() == 0)
            e = index_best(argv[i], NULL);
        if (!inst && e == NULL) {
            fprintf(stderr, "pkg: %s: unknown package\n", argv[i]);
            rc = EXIT_FAILED;
            continue;
        }
        printf("name: %s\n", inst ? m.name : e->name);
        if (inst)
            printf("installed: %s\n", m.version);
        if (e)
            printf("available: %s (%s, %llu bytes)\n", e->version, e->file, (unsigned long long)e->size);
        printf("summary: %s\n", inst ? m.summary : e->summary);
        int nd = inst ? m.ndepends : e->ndepends;
        const struct depend *deps = inst ? m.depends : e->depends;
        for (int k = 0; k < nd; k++)
            printf("depends: %s %s %s\n", deps[k].name, op_text(deps[k].op), deps[k].version);
        if (inst) {
            for (int k = 0; k < m.nfiles; k++)
                printf("file: /%s %04o %llu\n", m.files[k].path, m.files[k].mode, (unsigned long long)m.files[k].size);
            manifest_free(&m);
        }
    }
    return rc;
}

static int cmd_search(int argc, char **argv)
{
    if (index_load())
        return EXIT_FAILED;
    for (int i = 0; i < g_index.n; i++) {
        struct index_entry *e = &g_index.entries[i];
        bool hit = argc == 0;
        for (int k = 0; k < argc && !hit; k++)
            hit = strstr(e->name, argv[k]) || strstr(e->summary, argv[k]);
        if (hit)
            printf("%-20s %-10s %s\n", e->name, e->version, e->summary);
    }
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    char names[128][PKG_NAME_MAX];
    int n = argc ? argc : installed_names(names, 128);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const char *name = argc ? argv[i] : names[i];
        struct manifest m;
        if (!installed_load(name, &m)) {
            fprintf(stderr, "pkg: %s is not installed\n", name);
            bad++;
            continue;
        }
        for (int k = 0; k < m.nfiles; k++) {
            char path[PKG_PATH_MAX];
            snprintf(path, sizeof(path), "/%s", m.files[k].path);
            uint8_t *data;
            size_t len;
            if (read_whole(path, &data, &len, PKG_PACKAGE_LIMIT) < 0) {
                printf("%s: %s: missing (%s)\n", name, path, strerror(errno));
                bad++;
                continue;
            }
            uint8_t sha[SHA512_LEN];
            sha512_of(data, len, sha);
            if (len != m.files[k].size || memcmp(sha, m.files[k].sha, SHA512_LEN) != 0) {
                printf("%s: %s: modified\n", name, path);
                bad++;
            }
            free(data);
        }
        manifest_free(&m);
    }
    printf("pkg: verify: %d problem%s\n", bad, bad == 1 ? "" : "s");
    return bad ? EXIT_FAILED : 0;
}

/* --- main -------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: pkg update\n"
            "       pkg install [-n] [-f] [--reinstall] NAME[=VERSION]... | FILE.cpk...\n"
            "       pkg remove [-n] [-f] NAME...\n"
            "       pkg upgrade [-n] [NAME...]\n"
            "       pkg list | info NAME... | search [TEXT...] | verify [NAME...]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return EXIT_USAGE;
    }
    const char *cmd = argv[1];
    int i = 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-n") == 0)
            g_dry_run = true;
        else if (strcmp(argv[i], "-f") == 0)
            g_force = true;
        else if (strcmp(argv[i], "--reinstall") == 0)
            g_reinstall = true;
        else {
            usage();
            return EXIT_USAGE;
        }
    }
    argc -= i;
    argv += i;

    bool mutating = strcmp(cmd, "update") == 0 || strcmp(cmd, "install") == 0 || strcmp(cmd, "remove") == 0 ||
                    strcmp(cmd, "upgrade") == 0;
    if (db_init() < 0)
        return EXIT_FAILED;
    if (keys_load() < 0)
        return EXIT_FAILED;
    if (mutating && !g_dry_run && lock_acquire() < 0)
        return EXIT_FAILED;
    int rc;
    if (strcmp(cmd, "update") == 0)
        rc = cmd_update();
    else if (strcmp(cmd, "install") == 0)
        rc = cmd_install(argc, argv);
    else if (strcmp(cmd, "remove") == 0)
        rc = cmd_remove(argc, argv);
    else if (strcmp(cmd, "upgrade") == 0)
        rc = cmd_upgrade(argc, argv);
    else if (strcmp(cmd, "list") == 0)
        rc = cmd_list();
    else if (strcmp(cmd, "info") == 0)
        rc = cmd_info(argc, argv);
    else if (strcmp(cmd, "search") == 0)
        rc = cmd_search(argc, argv);
    else if (strcmp(cmd, "verify") == 0)
        rc = cmd_verify(argc, argv);
    else {
        usage();
        rc = EXIT_USAGE;
    }
    if (mutating && !g_dry_run)
        lock_release();
    fflush(stdout);
    return rc;
}
