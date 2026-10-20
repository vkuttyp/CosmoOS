/*
 * rootsnap.c -- a mount's snapshots through /dev/fsctl (rootsnap.h).
 */
#include "rootsnap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One command, whole, and its result read back from the same file: a
 * result is read whole or not at all (-ERANGE), so the buffer grows. */
static long run(struct rootsnap *r, unsigned op, unsigned flags, const char *name, const char *name2,
                unsigned long long cursor, char **buf)
{
    struct cosmo_fsctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_FSCTL_VERSION;
    cmd.op = (uint16_t)op;
    cmd.flags = flags;
    cmd.mount_id = r->mount_id;
    cmd.cursor = cursor;
    if ((name && strlen(name) > COSMO_FSCTL_NAME_MAX) || (name2 && strlen(name2) > COSMO_FSCTL_NAME_MAX)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (name)
        memcpy(cmd.name, name, strlen(name));
    if (name2)
        memcpy(cmd.name2, name2, strlen(name2));
    if (write(r->fd, &cmd, sizeof(cmd)) != (long)sizeof(cmd))
        return -1;
    size_t cap = 4096;
    for (int tries = 0; tries < 16; tries++, cap *= 2) {
        char *b = malloc(cap);
        if (b == NULL) {
            errno = ENOMEM;
            return -1;
        }
        long n = read(r->fd, b, cap);
        if (n >= (long)sizeof(struct cosmo_fsctl_result)) {
            *buf = b;
            return n;
        }
        int e = n < 0 ? errno : EIO;
        free(b);
        if (e != ERANGE) {
            errno = e;
            return -1;
        }
    }
    errno = E2BIG;
    return -1;
}

int rootsnap_open(struct rootsnap *r, const char *path)
{
    r->fd = open("/dev/fsctl", O_RDWR);
    r->mount_id = 0;
    if (r->fd < 0)
        return -1;
    char *buf = NULL;
    long n = run(r, COSMO_FSCTL_LIST, 0, NULL, NULL, 0, &buf);
    if (n < 0) {
        /* A kernel whose /dev/fsctl refuses the version-3 command predates
         * snapshots (v1.0.7 and older): no snapshots, not an error -- the
         * release's cosmo-upgrade runs on the release it upgrades from. */
        int e = errno == EINVAL ? EOPNOTSUPP : errno;
        rootsnap_close(r);
        errno = e;
        return -1;
    }
    const struct cosmo_fsctl_result *h = (const struct cosmo_fsctl_result *)buf;
    const struct cosmo_fsctl_mount *m = (const struct cosmo_fsctl_mount *)(buf + sizeof(*h));
    size_t fits = ((size_t)n - sizeof(*h)) / sizeof(*m);
    unsigned count = h->count < fits ? h->count : (unsigned)fits;
    for (unsigned i = 0; i < count; i++)
        if (strcmp(m[i].path, path) == 0 && (m[i].caps & COSMO_FSCTL_CAP_SNAP))
            r->mount_id = m[i].id;
    free(buf);
    if (r->mount_id == 0) {
        rootsnap_close(r);
        errno = EOPNOTSUPP;
        return -1;
    }
    return 0;
}

void rootsnap_close(struct rootsnap *r)
{
    if (r->fd >= 0)
        close(r->fd);
    r->fd = -1;
}

int rootsnap_list(struct rootsnap *r, struct cosmo_fsctl_snap_pool *pool, struct cosmo_fsctl_snap **out,
                  unsigned *nout)
{
    struct cosmo_fsctl_snap *all = NULL;
    unsigned n = 0;
    unsigned long long cursor = 0;
    for (;;) {
        char *buf = NULL;
        long len = run(r, COSMO_FSCTL_SNAP_LIST, COSMO_FSCTL_F_NO_SIZES, NULL, NULL, cursor, &buf);
        if (len < (long)(sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_snap_pool))) {
            int e = len < 0 ? errno : EIO;
            free(buf);
            free(all);
            errno = e;
            return -1;
        }
        const struct cosmo_fsctl_result *h = (const struct cosmo_fsctl_result *)buf;
        const struct cosmo_fsctl_snap_pool *p = (const struct cosmo_fsctl_snap_pool *)(buf + sizeof(*h));
        const struct cosmo_fsctl_snap *s = (const struct cosmo_fsctl_snap *)(buf + sizeof(*h) + sizeof(*p));
        if (cursor == 0 && pool)
            *pool = *p;
        size_t fits = ((size_t)len - sizeof(*h) - sizeof(*p)) / sizeof(*s);
        unsigned count = h->count < fits ? h->count : (unsigned)fits;
        if (count) {
            struct cosmo_fsctl_snap *grown = realloc(all, (n + count) * sizeof(*all));
            if (grown == NULL) {
                free(buf);
                free(all);
                errno = ENOMEM;
                return -1;
            }
            all = grown;
            memcpy(all + n, s, count * sizeof(*s));
            n += count;
            cursor = s[count - 1].id;
        }
        free(buf);
        if (count < COSMO_FSCTL_SNAP_PAGE)
            break;
    }
    if (out)
        *out = all;
    else
        free(all);
    if (nout)
        *nout = n;
    return 0;
}

static int one(struct rootsnap *r, unsigned op, unsigned flags, const char *name, const char *name2,
               struct cosmo_fsctl_snap *out)
{
    char *buf = NULL;
    long len = run(r, op, flags, name, name2, 0, &buf);
    if (len < 0)
        return -1;
    const struct cosmo_fsctl_result *h = (const struct cosmo_fsctl_result *)buf;
    if (out) {
        if (h->count < 1 || (size_t)len < sizeof(*h) + sizeof(*out)) {
            free(buf);
            errno = EIO;
            return -1;
        }
        memcpy(out, buf + sizeof(*h), sizeof(*out));
    }
    free(buf);
    return 0;
}

int rootsnap_create(struct rootsnap *r, const char *name, struct cosmo_fsctl_snap *out)
{
    return one(r, COSMO_FSCTL_SNAP_CREATE, 0, name, NULL, out);
}

int rootsnap_delete(struct rootsnap *r, const char *name)
{
    return one(r, COSMO_FSCTL_SNAP_DELETE, 0, name, NULL, NULL);
}

int rootsnap_rollback(struct rootsnap *r, const char *name, const char *safety, struct cosmo_fsctl_snap *out)
{
    return one(r, COSMO_FSCTL_SNAP_ROLLBACK, safety ? COSMO_FSCTL_F_SAFETY_NAME : 0, name, safety, out);
}

int rootsnap_cancel(struct rootsnap *r)
{
    return one(r, COSMO_FSCTL_SNAP_CANCEL, 0, NULL, NULL, NULL);
}

int rootsnap_clone(struct rootsnap *r, const char *name, const char *clone, struct cosmo_fsctl_snap *out)
{
    return one(r, COSMO_FSCTL_SNAP_CLONE, 0, name, clone, out);
}

const struct cosmo_fsctl_snap *rootsnap_find(const struct cosmo_fsctl_snap *v, unsigned n, const char *name)
{
    for (unsigned i = 0; i < n; i++)
        if (strncmp(v[i].name, name, sizeof(v[i].name)) == 0)
            return &v[i];
    return NULL;
}
