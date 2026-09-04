/* dirent.c - Directory streams over getdents. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libc.h"

#define DIRBUF 4096

struct _DIR {
    int fd;
    unsigned char buf[DIRBUF];
    size_t len, pos;
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;
    DIR *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (d->pos >= d->len) {
        long n = __syscall_ret(cosmo_getdents(d->fd, d->buf, sizeof(d->buf)));
        if (n <= 0)
            return NULL;
        d->len = (size_t)n;
        d->pos = 0;
    }
    const struct cosmo_dirent *e = (const struct cosmo_dirent *)(d->buf + d->pos);
    if (d->pos + sizeof(*e) > d->len || e->reclen < sizeof(*e) || d->pos + e->reclen > d->len) {
        errno = EIO;
        return NULL;
    }
    d->ent.d_ino = e->ino;
    d->ent.d_type = e->type;
    size_t nl = e->namelen < sizeof(d->ent.d_name) - 1 ? e->namelen : sizeof(d->ent.d_name) - 1;
    memcpy(d->ent.d_name, e->name, nl);
    d->ent.d_name[nl] = '\0';
    d->pos += e->reclen;
    return &d->ent;
}

int closedir(DIR *d)
{
    int rc = close(d->fd);
    free(d);
    return rc;
}
