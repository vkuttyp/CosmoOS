/*
 * cosmo-snap - snapshots of a cosmofs mount (roadmap 1.2 B1;
 * docs/kernel-services/filesystem/cosmofs/design.md, "Format version 12";
 * docs/guide/snapshots.md).
 *
 *   cosmo-snap [-m MOUNTPOINT] create NAME
 *   cosmo-snap [-m MOUNTPOINT] list
 *   cosmo-snap [-m MOUNTPOINT] delete NAME
 *   cosmo-snap [-m MOUNTPOINT] diff A B
 *
 * MOUNTPOINT is `/` unless given. The operations go through /dev/fsctl
 * against the mount found at that path in this namespace's listing.
 * `diff` names the paths that differ between two snapshots (`-` for the
 * live tree) by walking them under MOUNTPOINT/.snapshots: added (+),
 * removed (-) or changed (M) -- by type, mode, owner, size, link target
 * or contents; it compares no blocks.
 *
 * Exit status: 0 done, 1 refused or failed, 2 usage.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <uapi/cosmo/fsctl.h>

static const char *g_mp = "/";

static void usage(void)
{
    fprintf(stderr, "usage: cosmo-snap [-m MOUNTPOINT] create NAME | list | delete NAME | diff A B\n");
    exit(2);
}

/* One command, whole; the result read back from the same file. */
static long run(int fd, unsigned op, unsigned long long id, unsigned flags, const char *name, const char *name2,
                void *buf, size_t cap)
{
    struct cosmo_fsctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_FSCTL_VERSION;
    cmd.op = (unsigned short)op;
    cmd.flags = flags;
    cmd.mount_id = id;
    if (name != NULL)
        strncpy(cmd.name, name, sizeof(cmd.name) - 1);
    if (name2 != NULL)
        strncpy(cmd.name2, name2, sizeof(cmd.name2) - 1);
    if (write(fd, &cmd, sizeof(cmd)) != (long)sizeof(cmd))
        return -1;
    return read(fd, buf, cap);
}

/* The mount id at `g_mp`, or exit. */
static unsigned long long mount_id(int fd)
{
    static char buf[sizeof(struct cosmo_fsctl_result) + 128 * sizeof(struct cosmo_fsctl_mount)];
    long n = run(fd, COSMO_FSCTL_LIST, 0, 0, NULL, NULL, buf, sizeof(buf));
    if (n < (long)sizeof(struct cosmo_fsctl_result)) {
        perror("cosmo-snap: list mounts");
        exit(1);
    }
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_mount *m = (struct cosmo_fsctl_mount *)(buf + sizeof(*h));
    for (unsigned i = 0; i < h->count; i++) {
        if (strcmp(m[i].path, g_mp) != 0)
            continue;
        if (!(m[i].caps & COSMO_FSCTL_CAP_SNAP)) {
            fprintf(stderr, "cosmo-snap: %s is %s, which has no snapshots\n", g_mp, m[i].fstype);
            exit(1);
        }
        return m[i].id;
    }
    fprintf(stderr, "cosmo-snap: nothing is mounted at %s\n", g_mp);
    exit(1);
}

static void fail(const char *what, const char *name)
{
    int e = errno;
    if (e == EOPNOTSUPP)
        fprintf(stderr, "cosmo-snap: %s %s: this filesystem's format is older than 11 and has no snapshot identity\n",
                what, name ? name : "");
    else if (e == EBUSY)
        fprintf(stderr, "cosmo-snap: %s %s: in use (something inside it is open)\n", what, name ? name : "");
    else
        fprintf(stderr, "cosmo-snap: %s %s: %s\n", what, name ? name : "", strerror(e));
    exit(1);
}

static void uuid_str(const uint8_t *u, char out[37])
{
    static const char hex[] = "0123456789abcdef";
    unsigned k = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[k++] = '-';
        out[k++] = hex[u[i] >> 4];
        out[k++] = hex[u[i] & 15];
    }
    out[k] = '\0';
}

/* UTC, from seconds since 1970 (Howard Hinnant's civil_from_days). */
static void time_str(unsigned long long ns, char out[24])
{
    long long s = (long long)(ns / 1000000000ull);
    long long days = s / 86400, rem = s % 86400;
    long long z = days + 719468, era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    snprintf(out, 24, "%04lld-%02u-%02uT%02u:%02u:%02uZ", y, m, d, (unsigned)(rem / 3600), (unsigned)(rem / 60 % 60),
             (unsigned)(rem % 60));
}

static int cmd_create(int fd, unsigned long long id, const char *name)
{
    char buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_snap)];
    if (run(fd, COSMO_FSCTL_SNAP_CREATE, id, 0, name, NULL, buf, sizeof(buf)) < 0)
        fail("create", name);
    struct cosmo_fsctl_snap *s = (struct cosmo_fsctl_snap *)(buf + sizeof(struct cosmo_fsctl_result));
    char u[37];
    uuid_str(s->uuid, u);
    printf("cosmo-snap: created %s (id %llu, uuid %s, generation %llu)\n", s->name, (unsigned long long)s->id, u,
           (unsigned long long)s->generation);
    return 0;
}

static int cmd_list(int fd, unsigned long long id)
{
    size_t cap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_snap_pool) +
                 1024 * sizeof(struct cosmo_fsctl_snap);
    char *buf = malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "cosmo-snap: out of memory\n");
        return 1;
    }
    long n = run(fd, COSMO_FSCTL_SNAP_LIST, id, 0, NULL, NULL, buf, cap);
    if (n < (long)(sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_snap_pool)))
        fail("list", NULL);
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_snap_pool *p = (struct cosmo_fsctl_snap_pool *)(buf + sizeof(*h));
    struct cosmo_fsctl_snap *s = (struct cosmo_fsctl_snap *)(buf + sizeof(*h) + sizeof(*p));
    printf("cosmo-snap: %s: format %u%s%s\n", g_mp, p->format,
           (p->flags & COSMO_FSCTL_POOL_F_PENDING) ? ", a rollback is pending" : "",
           (p->flags & COSMO_FSCTL_POOL_F_HELD) ? ", held after a rollback until this boot completes" : "");
    printf("%-20s %5s %-20s %10s %10s %-36s %s\n", "NAME", "ID", "CREATED", "REFER", "UNIQUE", "UUID", "FLAGS");
    for (unsigned i = 0; i < h->count; i++) {
        char u[37], t[24], flags[64] = "";
        uuid_str(s[i].uuid, u);
        time_str(s[i].created_ns, t);
        if (s[i].flags & COSMO_FSCTL_SNAP_F_CLONE)
            snprintf(flags + strlen(flags), sizeof(flags) - strlen(flags), "clone-of:%llu ",
                     (unsigned long long)s[i].origin_id);
        if (s[i].flags & COSMO_FSCTL_SNAP_F_TARGET)
            strncat(flags, "rollback-target ", sizeof(flags) - strlen(flags) - 1);
        if (s[i].flags & COSMO_FSCTL_SNAP_F_SAFETY)
            strncat(flags, "rollback-safety ", sizeof(flags) - strlen(flags) - 1);
        if (s[i].flags & COSMO_FSCTL_SNAP_F_HELD)
            strncat(flags, "held ", sizeof(flags) - strlen(flags) - 1);
        printf("%-20s %5llu %-20s %9lluK %9lluK %-36s %s\n", s[i].name, (unsigned long long)s[i].id, t,
               (unsigned long long)(s[i].referenced / 1024), (unsigned long long)(s[i].unique / 1024), u,
               flags[0] ? flags : "-");
    }
    if (h->total > h->count)
        printf("cosmo-snap: %u of %u snapshots shown\n", h->count, h->total);
    free(buf);
    return 0;
}

static int cmd_delete(int fd, unsigned long long id, const char *name)
{
    char buf[sizeof(struct cosmo_fsctl_result)];
    if (run(fd, COSMO_FSCTL_SNAP_DELETE, id, 0, name, NULL, buf, sizeof(buf)) < 0)
        fail("delete", name);
    printf("cosmo-snap: deleted %s\n", name);
    return 0;
}

/* --- diff -------------------------------------------------------------- */

static unsigned long g_changes;
/* The device (mount id) of each tree's root: a directory on another
 * device is another filesystem mounted there (/proc, /dev, /tmp on the
 * live root), not part of the tree, and the walk does not enter it. */
static uint32_t g_dev_a, g_dev_b;

/* `root` + `rel`, with the live root's empty prefix made "/". */
static void join(char *out, size_t n, const char *root, const char *rel)
{
    snprintf(out, n, "%s%s", root, rel);
    if (out[0] == '\0')
        snprintf(out, n, "/");
}

/* Directory entries of `path`, sorted, without . and ..; NULL on error. */
static char **names_of(const char *path, unsigned *count)
{
    *count = 0;
    DIR *d = opendir(path);
    if (d == NULL)
        return NULL;
    unsigned cap = 16, n = 0;
    char **v = malloc(cap * sizeof(*v));
    struct dirent *e;
    while (v != NULL && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap *= 2;
            char **nv = realloc(v, cap * sizeof(*v));
            if (nv == NULL) {
                v = NULL;
                break;
            }
            v = nv;
        }
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (v == NULL)
        return NULL;
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && strcmp(v[j - 1], v[j]) > 0; j--) {
            char *t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    *count = n;
    return v;
}

static void names_free(char **v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

/* Same contents, byte for byte. */
static int same_file(const char *a, const char *b)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    int same = fa >= 0 && fb >= 0;
    static char ba[4096], bb[4096];
    while (same) {
        long na = read(fa, ba, sizeof(ba)), nb = read(fb, bb, sizeof(bb));
        if (na != nb || na < 0 || memcmp(ba, bb, (size_t)na) != 0)
            same = 0;
        else if (na == 0)
            break;
    }
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    return same;
}

static void report(char kind, const char *rel)
{
    printf("%c %s\n", kind, rel[0] ? rel : "/");
    g_changes++;
}

/* Everything under `rel` in one tree only, each path once. */
static void report_tree(char kind, const char *root, const char *rel, uint32_t dev)
{
    report(kind, rel);
    char path[1024];
    join(path, sizeof(path), root, rel);
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_type) || st.st_dev != dev)
        return;
    unsigned n;
    char **v = names_of(path, &n);
    for (unsigned i = 0; v != NULL && i < n; i++) {
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", rel, v[i]);
        report_tree(kind, root, sub, dev);
    }
    if (v != NULL)
        names_free(v, n);
}

static void diff_dir(const char *ra, const char *rb, const char *rel)
{
    char pa[1024], pb[1024];
    join(pa, sizeof(pa), ra, rel);
    join(pb, sizeof(pb), rb, rel);
    unsigned na, nb;
    char **va = names_of(pa, &na), **vb = names_of(pb, &nb);
    if (va == NULL || vb == NULL) {
        fprintf(stderr, "cosmo-snap: diff: cannot read %s\n", va == NULL ? pa : pb);
        exit(1);
    }
    unsigned i = 0, j = 0;
    while (i < na || j < nb) {
        int c = i == na ? 1 : j == nb ? -1 : strcmp(va[i], vb[j]);
        char sub[1024];
        if (c < 0) {
            snprintf(sub, sizeof(sub), "%s/%s", rel, va[i++]);
            report_tree('-', ra, sub, g_dev_a);
            continue;
        }
        if (c > 0) {
            snprintf(sub, sizeof(sub), "%s/%s", rel, vb[j++]);
            report_tree('+', rb, sub, g_dev_b);
            continue;
        }
        snprintf(sub, sizeof(sub), "%s/%s", rel, va[i]);
        i++;
        j++;
        char fa[1024], fb[1024];
        join(fa, sizeof(fa), ra, sub);
        join(fb, sizeof(fb), rb, sub);
        struct stat sa, sb;
        if (lstat(fa, &sa) != 0 || lstat(fb, &sb) != 0) {
            report('M', sub);
            continue;
        }
        if (sa.st_type != sb.st_type) {
            report_tree('-', ra, sub, g_dev_a);
            report_tree('+', rb, sub, g_dev_b);
            continue;
        }
        /* Another filesystem mounted on a directory of either tree is not
         * part of it: neither compared nor entered. */
        if (S_ISDIR(sa.st_type) && (sa.st_dev != g_dev_a || sb.st_dev != g_dev_b))
            continue;
        int changed = sa.st_mode != sb.st_mode || sa.st_uid != sb.st_uid || sa.st_gid != sb.st_gid;
        if (S_ISREG(sa.st_type))
            changed = changed || sa.st_size != sb.st_size || !same_file(fa, fb);
        else if (S_ISLNK(sa.st_type)) {
            char la[1024], lb[1024];
            long xa = readlink(fa, la, sizeof(la)), xb = readlink(fb, lb, sizeof(lb));
            changed = changed || xa != xb || (xa > 0 && memcmp(la, lb, (size_t)xa) != 0);
        }
        if (changed)
            report('M', sub);
        if (S_ISDIR(sa.st_type))
            diff_dir(ra, rb, sub);
    }
    names_free(va, na);
    names_free(vb, nb);
}

static void tree_root(const char *name, char *out, size_t n)
{
    const char *sep = strcmp(g_mp, "/") == 0 ? "" : "/";
    if (strcmp(name, "-") == 0)
        snprintf(out, n, "%s", strcmp(g_mp, "/") == 0 ? "" : g_mp);
    else
        snprintf(out, n, "%s%s.snapshots/%s", strcmp(g_mp, "/") == 0 ? "/" : g_mp, sep, name);
}

static int cmd_diff(const char *a, const char *b)
{
    char ra[512], rb[512], pa[512], pb[512];
    tree_root(a, ra, sizeof(ra));
    tree_root(b, rb, sizeof(rb));
    join(pa, sizeof(pa), ra, "");
    join(pb, sizeof(pb), rb, "");
    struct stat sa, sb;
    if (stat(pa, &sa) != 0 || stat(pb, &sb) != 0) {
        fprintf(stderr, "cosmo-snap: diff: no snapshot %s\n", stat(pa, &sa) != 0 ? a : b);
        return 1;
    }
    g_dev_a = sa.st_dev;
    g_dev_b = sb.st_dev;
    diff_dir(ra, rb, "");
    printf("cosmo-snap: %lu change(s) from %s to %s\n", g_changes, a, b);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-m") == 0) {
        g_mp = argv[i + 1];
        i += 2;
    }
    if (i >= argc)
        usage();
    const char *op = argv[i++];
    if (strcmp(op, "diff") == 0) {
        if (argc - i != 2)
            usage();
        return cmd_diff(argv[i], argv[i + 1]);
    }
    int fd = open("/dev/fsctl", O_RDWR);
    if (fd < 0) {
        perror("cosmo-snap: /dev/fsctl");
        return 1;
    }
    unsigned long long id = mount_id(fd);
    if (strcmp(op, "create") == 0 && argc - i == 1)
        return cmd_create(fd, id, argv[i]);
    if (strcmp(op, "list") == 0 && argc - i == 0)
        return cmd_list(fd, id);
    if (strcmp(op, "delete") == 0 && argc - i == 1)
        return cmd_delete(fd, id, argv[i]);
    usage();
    return 2;
}
