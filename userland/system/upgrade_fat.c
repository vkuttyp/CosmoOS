/* upgrade_fat.c - FAT32 through /dev/blkctl for cosmo-upgrade (upgrade_fat.h). */
#include "upgrade_fat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAT_MASK 0x0FFFFFFFu
#define FAT_EOC 0x0FFFFFFFu
#define FAT_BAD 0x0FFFFFF7u
#define FAT_DIR_DEPTH 16

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

uint64_t fat_cluster_lba(const struct fat *f, uint32_t c)
{
    return f->data_lba + (uint64_t)(c - 2) * f->spc;
}

uint32_t fat_cluster_bytes(const struct fat *f)
{
    return f->spc * f->bps;
}

int fat_read_sector(const struct fat *f, uint64_t lba, void *buf)
{
    return blkdevs_read(f->dev, lba, 1, buf);
}

int fat_write_sector(const struct fat *f, uint64_t lba, const void *buf)
{
    return blkdevs_write(f->dev, lba, 1, buf);
}

static bool valid(const struct fat *f, uint32_t c)
{
    return c >= 2 && c < f->nclusters + 2;
}

static uint32_t next_of(const struct fat *f, uint32_t c)
{
    return f->fat[c] & FAT_MASK;
}

static void set_next(struct fat *f, uint32_t c, uint32_t v)
{
    f->fat[c] = (f->fat[c] & ~FAT_MASK) | (v & FAT_MASK);
    f->dirty[(size_t)c * 4 / f->bps] = 1;
}

int fat_open(struct fat *f, const struct cosmo_blkctl_dev *d, char *err, size_t errlen)
{
    memset(f, 0, sizeof(*f));
    f->dev = d;
    uint8_t bs[4096];
    if (d->sector_size < 512 || d->sector_size > sizeof(bs)) {
        snprintf(err, errlen, "%s: %u-byte sectors", d->name, d->sector_size);
        return -EINVAL;
    }
    int rc = blkdevs_read(d, 0, 1, bs);
    if (rc < 0) {
        snprintf(err, errlen, "%s: reading the boot sector: %s", d->name, strerror(-rc));
        return rc;
    }
    f->bps = le16(bs + 11);
    f->spc = bs[13];
    f->rsvd = le16(bs + 14);
    f->nfats = bs[16];
    uint32_t root_entries = le16(bs + 17), total16 = le16(bs + 19), fatsz16 = le16(bs + 22);
    uint32_t total = total16 ? total16 : le32(bs + 32);
    f->fatsz = le32(bs + 36);
    uint16_t ext = le16(bs + 40);
    f->root = le32(bs + 44);
    f->fsinfo = le16(bs + 48);
    if (bs[510] != 0x55 || bs[511] != 0xAA || root_entries != 0 || fatsz16 != 0 || f->fatsz == 0 ||
        memcmp(bs + 82, "FAT32   ", 8) != 0) {
        snprintf(err, errlen, "%s: not FAT32", d->name);
        return -EINVAL;
    }
    if (f->bps != d->sector_size || f->spc == 0 || (f->spc & (f->spc - 1)) != 0 || f->nfats == 0 || f->nfats > 2 ||
        f->rsvd == 0 || total > d->sectors) {
        snprintf(err, errlen, "%s: FAT32 with %u-byte sectors, %u per cluster, %u FATs on %u-byte sectors: refused",
                 d->name, f->bps, f->spc, f->nfats, d->sector_size);
        return -EINVAL;
    }
    f->data_lba = f->rsvd + (uint64_t)f->nfats * f->fatsz;
    if (f->data_lba >= total) {
        snprintf(err, errlen, "%s: FAT32 layout past its end", d->name);
        return -EINVAL;
    }
    f->nclusters = (uint32_t)((total - f->data_lba) / f->spc);
    uint64_t entries = (uint64_t)f->fatsz * f->bps / 4;
    if (entries < (uint64_t)f->nclusters + 2 || !valid(f, f->root)) {
        snprintf(err, errlen, "%s: FAT32 too small for its clusters", d->name);
        return -EINVAL;
    }
    f->active = (ext & 0x80) ? (int)(ext & 0x0F) : -1;
    if (f->active >= (int)f->nfats) {
        snprintf(err, errlen, "%s: active FAT %d of %u", d->name, f->active, f->nfats);
        return -EINVAL;
    }
    f->fat = malloc((size_t)f->fatsz * f->bps);
    f->dirty = calloc(f->fatsz, 1);
    if (f->fat == NULL || f->dirty == NULL) {
        snprintf(err, errlen, "out of memory for the FAT");
        return -ENOMEM;
    }
    uint32_t which = f->active < 0 ? 0 : (uint32_t)f->active;
    rc = blkdevs_read(d, f->rsvd + (uint64_t)which * f->fatsz, f->fatsz, f->fat);
    if (rc < 0) {
        snprintf(err, errlen, "%s: reading the FAT: %s", d->name, strerror(-rc));
        return rc;
    }
    f->hint = 2;
    return 0;
}

void fat_close(struct fat *f)
{
    free(f->fat);
    free(f->dirty);
    f->fat = NULL;
    f->dirty = NULL;
}

bool fat_name83(const char *name, uint8_t out[11], uint8_t *nt)
{
    memset(out, ' ', 11);
    *nt = 0;
    const char *dot = strchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot && strchr(dot + 1, '.')))
        return false;
    bool lower_b = false, upper_b = false, lower_e = false, upper_e = false;
    for (size_t i = 0; i < base + (dot ? 1 + ext : 0); i++) {
        if (dot && name + i == dot)
            continue;
        char c = name[i];
        bool in_ext = dot && name + i > dot;
        if (c >= 'a' && c <= 'z') {
            *(in_ext ? &lower_e : &lower_b) = true;
            c = (char)(c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'Z') {
            *(in_ext ? &upper_e : &upper_b) = true;
        } else if (!((c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
        if (in_ext)
            out[8 + (size_t)(name + i - dot - 1)] = (uint8_t)c;
        else
            out[i] = (uint8_t)c;
    }
    if ((lower_b && upper_b) || (lower_e && upper_e))
        return false;   /* mixed case needs a long name */
    *nt = (uint8_t)((lower_b ? FAT_NT_LOWER_BASE : 0) | (lower_e ? FAT_NT_LOWER_EXT : 0));
    return true;
}

static void fill_ent(struct fat_ent *e, const uint8_t *raw, uint64_t lba, unsigned slot)
{
    memcpy(e->raw, raw, FAT_DIRENT);
    e->cluster = (uint32_t)le16(raw + 20) << 16 | le16(raw + 26);
    e->size = le32(raw + 28);
    e->lba = lba;
    e->slot = slot;
}

/* Walk the directory at `dir`: `fn` sees each live short entry (not
 * long-name parts, deleted entries or the volume label) and stops the walk
 * by returning nonzero. Ends at the first 0x00 entry. */
static int dir_walk(struct fat *f, uint32_t dir, int (*fn)(void *ctx, const struct fat_ent *e), void *ctx)
{
    uint8_t sec[4096];
    uint32_t c = dir;
    for (uint32_t hops = 0; valid(f, c); hops++) {
        if (hops > f->nclusters)
            return -EIO;   /* a loop in the chain */
        for (uint32_t s = 0; s < f->spc; s++) {
            uint64_t lba = fat_cluster_lba(f, c) + s;
            int rc = fat_read_sector(f, lba, sec);
            if (rc < 0)
                return rc;
            for (unsigned i = 0; i < f->bps / FAT_DIRENT; i++) {
                const uint8_t *r = sec + i * FAT_DIRENT;
                if (r[0] == 0x00)
                    return 0;
                if (r[0] == 0xE5 || (r[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN ||
                    ((r[11] & FAT_ATTR_VOLUME) && !(r[11] & FAT_ATTR_DIR)))
                    continue;
                struct fat_ent e;
                fill_ent(&e, r, lba, i);
                rc = fn(ctx, &e);
                if (rc)
                    return rc;
            }
        }
        uint32_t n = next_of(f, c);
        if (n >= 0x0FFFFFF8u)
            return 0;
        c = n;
    }
    return -EIO;
}

struct lookup_ctx {
    const uint8_t *name;
    struct fat_ent *out;
};

static int lookup_fn(void *p, const struct fat_ent *e)
{
    struct lookup_ctx *l = p;
    if (memcmp(e->raw, l->name, 11) != 0)
        return 0;
    *l->out = *e;
    return 1;
}

int fat_lookup(struct fat *f, uint32_t dir, const uint8_t name83[11], struct fat_ent *e)
{
    struct lookup_ctx l = { name83, e };
    return dir_walk(f, dir, lookup_fn, &l);
}

struct list_ctx {
    struct fat_ent *out;
    int max, n;
};

static int list_fn(void *p, const struct fat_ent *e)
{
    struct list_ctx *l = p;
    if (l->n == l->max)
        return -E2BIG;
    l->out[l->n++] = *e;
    return 0;
}

int fat_list(struct fat *f, uint32_t dir, struct fat_ent *out, int max)
{
    struct list_ctx l = { out, max, 0 };
    int rc = dir_walk(f, dir, list_fn, &l);
    return rc < 0 ? rc : l.n;
}

int fat_read_file(struct fat *f, uint32_t cluster, uint32_t size, uint8_t **out)
{
    uint32_t cb = fat_cluster_bytes(f);
    uint8_t *buf = malloc(size ? size : 1);
    uint8_t *tmp = malloc(cb);
    if (buf == NULL || tmp == NULL) {
        free(buf);
        free(tmp);
        return -ENOMEM;
    }
    uint32_t done = 0, c = cluster;
    int rc = 0;
    while (done < size) {
        if (!valid(f, c)) {
            rc = -EIO;
            break;
        }
        /* A run of consecutive clusters is one read. */
        uint32_t run = 1;
        while ((uint64_t)(run + 1) * cb <= size - done && next_of(f, c + run - 1) == c + run)
            run++;
        uint32_t whole = (size - done) / cb < run ? (size - done) / cb : run;
        if (whole) {
            rc = blkdevs_read(f->dev, fat_cluster_lba(f, c), (uint64_t)whole * f->spc, buf + done);
            if (rc < 0)
                break;
            done += whole * cb;
            c += whole - 1;
        } else {
            rc = blkdevs_read(f->dev, fat_cluster_lba(f, c), f->spc, tmp);
            if (rc < 0)
                break;
            memcpy(buf + done, tmp, size - done);
            done = size;
        }
        if (done < size)
            c = next_of(f, c);
    }
    free(tmp);
    if (rc < 0) {
        free(buf);
        return rc;
    }
    *out = buf;
    return 0;
}

static uint32_t alloc_one(struct fat *f)
{
    for (uint32_t i = 0; i < f->nclusters; i++) {
        uint32_t c = 2 + (f->hint - 2 + i) % f->nclusters;
        if (next_of(f, c) == 0) {
            f->hint = c + 1 >= f->nclusters + 2 ? 2 : c + 1;
            return c;
        }
    }
    return 0;
}

void fat_free_chain(struct fat *f, uint32_t c)
{
    for (uint32_t hops = 0; valid(f, c) && hops <= f->nclusters; hops++) {
        uint32_t n = next_of(f, c);
        if (n == 0 || n == FAT_BAD)
            return;
        set_next(f, c, 0);
        if (n >= 0x0FFFFFF8u)
            return;
        c = n;
    }
}

int fat_write_new(struct fat *f, const void *data, size_t len, uint32_t *first)
{
    *first = 0;
    if (len == 0)
        return 0;
    uint32_t cb = fat_cluster_bytes(f);
    uint64_t need = (len + cb - 1) / cb;
    if (need > f->nclusters)
        return -ENOSPC;
    uint8_t *tmp = malloc(cb);
    if (tmp == NULL)
        return -ENOMEM;
    const uint8_t *p = data;
    uint32_t prev = 0, run_start = 0, run_len = 0;
    size_t run_off = 0;
    int rc = 0;
    for (uint64_t i = 0; i < need; i++) {
        uint32_t c = alloc_one(f);
        if (c == 0) {
            rc = -ENOSPC;
            break;
        }
        set_next(f, c, FAT_EOC);
        if (prev)
            set_next(f, prev, c);
        else
            *first = c;
        /* Whole clusters are written in runs; the last, partial one alone. */
        bool partial = (i + 1) * cb > len;
        if (run_len && (c != run_start + run_len || partial)) {
            rc = blkdevs_write(f->dev, fat_cluster_lba(f, run_start), (uint64_t)run_len * f->spc, p + run_off);
            if (rc < 0)
                break;
            run_len = 0;
        }
        if (partial) {
            memset(tmp, 0, cb);
            memcpy(tmp, p + i * cb, len - i * cb);
            rc = blkdevs_write(f->dev, fat_cluster_lba(f, c), f->spc, tmp);
            if (rc < 0)
                break;
        } else {
            if (run_len == 0) {
                run_start = c;
                run_off = (size_t)(i * cb);
            }
            run_len++;
        }
        prev = c;
    }
    if (rc == 0 && run_len)
        rc = blkdevs_write(f->dev, fat_cluster_lba(f, run_start), (uint64_t)run_len * f->spc, p + run_off);
    free(tmp);
    if (rc < 0 && *first) {
        fat_free_chain(f, *first);
        *first = 0;
    }
    return rc;
}

/* --- the sweep ------------------------------------------------------------------- */

struct sweep {
    struct fat *f;
    uint8_t *mark;
    int depth;
    int err;
};

static int mark_chain(struct sweep *s, uint32_t c)
{
    for (uint32_t hops = 0; valid(s->f, c); hops++) {
        if (hops > s->f->nclusters || s->mark[c])
            return hops > s->f->nclusters ? -EIO : 0;
        s->mark[c] = 1;
        uint32_t n = next_of(s->f, c);
        if (n == 0 || n >= 0x0FFFFFF7u)
            return 0;
        c = n;
    }
    return 0;
}

static int sweep_dir(struct sweep *s, uint32_t dir);

static int sweep_fn(void *p, const struct fat_ent *e)
{
    struct sweep *s = p;
    if (e->raw[0] == '.' && (e->raw[1] == ' ' || (e->raw[1] == '.' && e->raw[2] == ' ')))
        return 0;
    if (!valid(s->f, e->cluster))
        return 0;
    if (e->raw[11] & FAT_ATTR_DIR) {
        if (s->mark[e->cluster])
            return 0;   /* already walked: a second name for a directory is not followed twice */
        int rc = sweep_dir(s, e->cluster);
        return rc < 0 ? rc : 0;
    }
    int rc = mark_chain(s, e->cluster);
    return rc < 0 ? rc : 0;
}

static int sweep_dir(struct sweep *s, uint32_t dir)
{
    if (++s->depth > FAT_DIR_DEPTH)
        return -EIO;   /* deeper than any ESP this system makes */
    int rc = mark_chain(s, dir);
    if (rc == 0)
        rc = dir_walk(s->f, dir, sweep_fn, s);
    s->depth--;
    return rc;
}

int fat_sweep(struct fat *f)
{
    struct sweep s = { f, calloc((size_t)f->nclusters + 2, 1), 0, 0 };
    if (s.mark == NULL)
        return -ENOMEM;
    int rc = sweep_dir(&s, f->root);
    int freed = 0;
    if (rc == 0) {
        for (uint32_t c = 2; c < f->nclusters + 2; c++) {
            uint32_t n = next_of(f, c);
            if (!s.mark[c] && n != 0 && n != FAT_BAD) {
                set_next(f, c, 0);
                freed++;
            }
        }
    }
    free(s.mark);
    return rc < 0 ? rc : freed;
}

int fat_commit(struct fat *f)
{
    uint8_t *base = (uint8_t *)f->fat;
    for (uint32_t s = 0; s < f->fatsz; s++) {
        if (!f->dirty[s])
            continue;
        for (uint32_t k = 0; k < f->nfats; k++) {
            if (f->active >= 0 && k != (uint32_t)f->active)
                continue;
            int rc = fat_write_sector(f, f->rsvd + (uint64_t)k * f->fatsz + s, base + (size_t)s * f->bps);
            if (rc < 0)
                return rc;
        }
        f->dirty[s] = 0;
    }
    if (f->fsinfo != 0 && f->fsinfo != 0xFFFF && f->fsinfo < f->rsvd) {
        uint8_t sec[4096];
        int rc = fat_read_sector(f, f->fsinfo, sec);
        if (rc < 0)
            return rc;
        if (le32(sec) == 0x41615252u && le32(sec + 484) == 0x61417272u) {
            uint32_t freec = 0;
            for (uint32_t c = 2; c < f->nclusters + 2; c++)
                freec += next_of(f, c) == 0;
            put32(sec + 488, freec);
            put32(sec + 492, f->hint);
            rc = fat_write_sector(f, f->fsinfo, sec);
            if (rc < 0)
                return rc;
        }
    }
    return blkdevs_flush(f->dev);
}

/* --- entries ---------------------------------------------------------------------- */

/* Seconds since 1970 to a FAT date and time (UTC; FAT has no zone). */
static void fat_stamp(int64_t now, uint16_t *date, uint16_t *time)
{
    if (now < 315532800)   /* 1980-01-01, FAT's epoch */
        now = 315532800;
    int64_t days = now / 86400, secs = now % 86400;
    /* Civil from days (Howard Hinnant's algorithm). */
    int64_t z = days + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y > 2107)
        y = 2107;
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)(((secs / 3600) << 11) | ((secs / 60 % 60) << 5) | (secs % 60 / 2));
}

void fat_ent_set(uint8_t raw[FAT_DIRENT], uint32_t cluster, uint32_t size, int64_t now)
{
    uint16_t date, time;
    fat_stamp(now, &date, &time);
    put16(raw + 20, cluster >> 16);
    put16(raw + 26, cluster & 0xFFFF);
    put32(raw + 28, size);
    put16(raw + 22, time);
    put16(raw + 24, date);
    put16(raw + 18, date);
}

void fat_make_ent(uint8_t raw[FAT_DIRENT], const uint8_t name83[11], uint8_t nt, uint8_t attr, uint32_t cluster,
                  uint32_t size, int64_t now)
{
    memset(raw, 0, FAT_DIRENT);
    memcpy(raw, name83, 11);
    raw[11] = attr;
    raw[12] = nt;
    uint16_t date, time;
    fat_stamp(now, &date, &time);
    put16(raw + 14, time);
    put16(raw + 16, date);
    fat_ent_set(raw, cluster, size, now);
}
