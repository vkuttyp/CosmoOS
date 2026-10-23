/*
 * cosmo-pool - a mirrored pool's copies: their state, replacing them and
 * spares (roadmap 1.2 B3; docs/kernel-services/filesystem/cosmofs/design.md,
 * "Format version 13"; docs/guide/mirrors.md).
 *
 *   cosmo-pool status [MOUNTPOINT]
 *   cosmo-pool [-m MOUNTPOINT] offline COPY
 *   cosmo-pool [-m MOUNTPOINT] online COPY
 *   cosmo-pool [-m MOUNTPOINT] replace COPY DEVICE
 *   cosmo-pool [-m MOUNTPOINT] attach COPY DEVICE
 *   cosmo-pool [-m MOUNTPOINT] detach COPY
 *   cosmo-pool [-m MOUNTPOINT] add-spare DEVICE
 *   cosmo-pool [-m MOUNTPOINT] remove-spare DEVICE
 *
 * A COPY is the block device holding it (`vdb2`, or `/dev/vdb2`), or
 * MEMBER:COPY as `status` numbers them for a copy whose device is gone.
 * MOUNTPOINT is `/` unless given; `status` without one reports every
 * cosmofs mount. `replace` resilvers DEVICE as the copy and detaches the
 * old one when it is done; `attach` makes DEVICE one more copy of COPY's
 * member (a single disk becomes a mirror); `online` brings a copy back,
 * resilvering what it missed. A spare takes the place of a copy that
 * fails or goes missing by itself.
 *
 * Exit status: 0 done (status: the pool is whole), 1 refused or failed
 * (status: a pool is degraded), 2 usage.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/cosmo/fsctl.h>

static const char *g_mp = "/";

static void usage(void)
{
    fprintf(stderr, "usage: cosmo-pool status [MOUNTPOINT]\n"
                    "       cosmo-pool [-m MOUNTPOINT] offline COPY | online COPY | replace COPY DEVICE |\n"
                    "                  attach COPY DEVICE | detach COPY | add-spare DEVICE | remove-spare DEVICE\n");
    exit(2);
}

static int run(int fd, unsigned op, unsigned long long id, const char *name, const char *name2, void *buf,
               size_t cap)
{
    struct cosmo_fsctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_FSCTL_VERSION;
    cmd.op = (unsigned short)op;
    cmd.mount_id = id;
    if (name)
        strncpy(cmd.name, name, sizeof(cmd.name) - 1);
    if (name2)
        strncpy(cmd.name2, name2, sizeof(cmd.name2) - 1);
    if (write(fd, &cmd, sizeof(cmd)) != (long)sizeof(cmd))
        return -1;
    if (buf == NULL)
        return 0;
    long n = read(fd, buf, cap);
    return n < 0 ? -1 : (int)n;
}

/* A device name as the kernel knows it: without /dev/, and short enough
 * to reach it whole, or exit -- one cut short would name another device. */
static const char *dev_arg(const char *s)
{
    if (strncmp(s, "/dev/", 5) == 0)
        s += 5;
    if (*s == '\0' || strlen(s) > COSMO_FSCTL_NAME_MAX) {
        fprintf(stderr, "cosmo-pool: not a device name: %s\n", s);
        exit(2);
    }
    return s;
}

struct mnt {
    unsigned long long id;
    char path[1024];
};

/* The cosmofs mounts with pool operations, whole; *count of them. */
static struct mnt *mounts(int fd, unsigned *count)
{
    size_t room = 64;
    char *buf = NULL;
    long n = -1;
    for (int tries = 0; tries < 12; tries++, room *= 2) {
        free(buf);
        size_t cap = sizeof(struct cosmo_fsctl_result) + room * sizeof(struct cosmo_fsctl_mount);
        buf = malloc(cap);
        if (buf == NULL)
            break;
        n = run(fd, COSMO_FSCTL_LIST, 0, NULL, NULL, buf, cap);
        if (n >= 0 || errno != ERANGE)
            break;
    }
    if (buf == NULL || n < (long)sizeof(struct cosmo_fsctl_result)) {
        perror("cosmo-pool: list mounts");
        exit(1);
    }
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_mount *m = (struct cosmo_fsctl_mount *)(buf + sizeof(*h));
    unsigned total = h->count;
    size_t fits = ((size_t)n - sizeof(*h)) / sizeof(*m);
    if (total > fits)
        total = (unsigned)fits;
    struct mnt *out = calloc(total ? total : 1, sizeof(*out));
    unsigned k = 0;
    for (unsigned i = 0; out && i < total; i++) {
        if (!(m[i].caps & COSMO_FSCTL_CAP_POOL))
            continue;
        out[k].id = m[i].id;
        strncpy(out[k].path, m[i].path, sizeof(out[k].path) - 1);
        k++;
    }
    free(buf);
    *count = k;
    return out;
}

static unsigned long long mount_id(int fd, const char *path)
{
    unsigned n = 0;
    struct mnt *m = mounts(fd, &n);
    for (unsigned i = 0; m && i < n; i++) {
        if (strcmp(m[i].path, path) == 0) {
            unsigned long long id = m[i].id;
            free(m);
            return id;
        }
    }
    free(m);
    fprintf(stderr, "cosmo-pool: no cosmofs pool is mounted at %s\n", path);
    exit(1);
}

static const char *state_name(unsigned s)
{
    static const char *const names[] = { "ONLINE", "MISSING", "OFFLINE", "FAULTED", "RESILVERING", "SPARE" };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

/* One pool's report; 0 whole, 1 degraded, -1 not asked. */
static int status_one(int fd, unsigned long long id, const char *path)
{
    size_t cap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_pool) +
                 (255 * 4 + 4) * sizeof(struct cosmo_fsctl_copy);   /* every copy and spare a pool can have */
    char *buf = malloc(cap);
    if (buf == NULL)
        return -1;
    int n = run(fd, COSMO_FSCTL_POOL_STATUS, id, NULL, NULL, buf, cap);
    if (n < (int)(sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_pool))) {
        fprintf(stderr, "cosmo-pool: status %s: %s\n", path, strerror(errno));
        free(buf);
        return -1;
    }
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_pool *p = (struct cosmo_fsctl_pool *)(buf + sizeof(*h));
    struct cosmo_fsctl_copy *c = (struct cosmo_fsctl_copy *)(buf + sizeof(*h) + sizeof(*p));
    unsigned count = h->count;
    size_t fits = ((size_t)n - sizeof(*h) - sizeof(*p)) / sizeof(*c);
    if (count > fits)
        count = (unsigned)fits;
    char label[sizeof(p->label) + 1];
    memcpy(label, p->label, sizeof(p->label));
    label[sizeof(p->label)] = '\0';
    printf("pool %s%s%s%s: %s, format %u, generation %llu\n", path, label[0] ? " (" : "", label,
           label[0] ? ")" : "", (p->flags & COSMO_FSCTL_POOL_DEGRADED) ? "DEGRADED" : "ONLINE", p->format,
           (unsigned long long)p->generation);
    unsigned member = 0xffffffffu;
    bool spares = false;
    for (unsigned i = 0; i < count; i++) {
        const struct cosmo_fsctl_copy *k = &c[i];
        if (k->state == COSMO_FSCTL_COPY_SPARE) {
            if (!spares)
                printf("  spares:\n");
            spares = true;
            printf("    %-10s %llu blocks\n", k->dev, (unsigned long long)k->blocks);
            continue;
        }
        if (k->member != member) {
            member = k->member;
            printf("  member %u (%llu blocks):\n", member, (unsigned long long)k->blocks);
        }
        printf("    copy %u  %-10s %-11s", k->copy, k->dev[0] ? k->dev : "-", state_name(k->state));
        if (k->state == COSMO_FSCTL_COPY_RESILVERING) {
            unsigned pct = k->blocks ? (unsigned)(k->cursor * 100 / k->blocks) : 0;
            printf(" %u%% (block %llu of %llu, about %llu to copy, %u blocks/s bound)", pct,
                   (unsigned long long)k->cursor, (unsigned long long)k->blocks, (unsigned long long)k->to_do,
                   p->resilver_rate);
        } else if (k->state != COSMO_FSCTL_COPY_ONLINE) {
            if (k->flags & COSMO_FSCTL_COPY_F_WHOLE)
                printf(" needs a whole resilver");
            else
                printf(" since generation %llu, about %llu blocks to resilver", (unsigned long long)k->since,
                       (unsigned long long)k->to_do);
        }
        if (k->flags & COSMO_FSCTL_COPY_F_DETACH)
            printf(" (replaced: detached when its replacement is ONLINE)");
        if (k->read_errors || k->write_errors || k->csum_errors)
            printf(" errors: %u read, %u write, %u checksum", k->read_errors, k->write_errors, k->csum_errors);
        printf("\n");
    }
    free(buf);
    return (p->flags & COSMO_FSCTL_POOL_DEGRADED) ? 1 : 0;
}

static void fail(const char *what, const char *name)
{
    int e = errno;
    const char *why = strerror(e);
    if (e == EBUSY)
        why = "in use (the member's last good copy, or a device another mount holds)";
    else if (e == ENOENT)
        why = "not a copy of this pool";
    else if (e == ENOSPC)
        why = "no room (the device is smaller than the member, or the member has four copies)";
    else if (e == EEXIST)
        why = "already the pool's";
    else if (e == EOPNOTSUPP)
        why = "this pool's format is older than 11 and cannot record a copy's state";
    else if (e == EROFS)
        why = "the pool is mounted read-only";
    fprintf(stderr, "cosmo-pool: %s %s: %s\n", what, name ? name : "", why);
    exit(1);
}

int main(int argc, char **argv)
{
    int i = 1;
    bool have_mp = false;
    if (i + 1 < argc && strcmp(argv[i], "-m") == 0) {
        g_mp = argv[i + 1];
        have_mp = true;
        i += 2;
    }
    if (i >= argc)
        usage();
    const char *verb = argv[i++];
    int left = argc - i;
    int fd = open("/dev/fsctl", O_RDWR);
    if (fd < 0) {
        perror("cosmo-pool: /dev/fsctl");
        return 1;
    }
    if (strcmp(verb, "status") == 0) {
        if (left > 1)
            usage();
        if (left == 1 || have_mp) {
            const char *mp = left == 1 ? argv[i] : g_mp;
            int r = status_one(fd, mount_id(fd, mp), mp);
            return r < 0 ? 1 : r;
        }
        unsigned n = 0;
        struct mnt *m = mounts(fd, &n);
        int worst = 0;
        for (unsigned k = 0; m && k < n; k++) {
            int r = status_one(fd, m[k].id, m[k].path);
            if (r != 0)
                worst = 1;
        }
        free(m);
        return worst;
    }
    struct {
        const char *verb;
        unsigned op, args;
    } const verbs[] = {
        { "offline", COSMO_FSCTL_POOL_OFFLINE, 1 },     { "online", COSMO_FSCTL_POOL_ONLINE, 1 },
        { "replace", COSMO_FSCTL_POOL_REPLACE, 2 },     { "attach", COSMO_FSCTL_POOL_ATTACH, 2 },
        { "detach", COSMO_FSCTL_POOL_DETACH, 1 },       { "add-spare", COSMO_FSCTL_POOL_SPARE_ADD, 1 },
        { "remove-spare", COSMO_FSCTL_POOL_SPARE_REMOVE, 1 },
    };
    for (unsigned k = 0; k < sizeof(verbs) / sizeof(verbs[0]); k++) {
        if (strcmp(verb, verbs[k].verb) != 0)
            continue;
        if (left != (int)verbs[k].args)
            usage();
        const char *a = dev_arg(argv[i]);
        const char *b = verbs[k].args == 2 ? dev_arg(argv[i + 1]) : NULL;
        unsigned long long id = mount_id(fd, g_mp);
        if (run(fd, verbs[k].op, id, a, b, NULL, 0) != 0)
            fail(verb, b ? b : a);
        printf("cosmo-pool: %s %s%s%s: done\n", verb, a, b ? " " : "", b ? b : "");
        return 0;
    }
    usage();
    return 2;
}
