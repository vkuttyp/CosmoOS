/* convert.c - Pure Linux/native conversions (compiled in the kernel and by tests/host/test_linux.c). */

#include <kernel/errno.h>
#include <kernel/string.h>
#include <stddef.h>

#include "convert.h"

int lx_open_flags(unsigned lx, unsigned *native)
{
    unsigned n = 0;
    switch (lx & LX_O_ACCMODE) {
    case LX_O_RDONLY: n = COSMO_O_RDONLY; break;
    case LX_O_WRONLY: n = COSMO_O_WRONLY; break;
    case LX_O_RDWR: n = COSMO_O_RDWR; break;
    default: return -1;
    }
    if (lx & LX_O_CREAT)
        n |= COSMO_O_CREAT;
    if (lx & LX_O_EXCL)
        n |= COSMO_O_EXCL;
    if (lx & LX_O_TRUNC)
        n |= COSMO_O_TRUNC;
    if (lx & LX_O_APPEND)
        n |= COSMO_O_APPEND;
    if (lx & LX_O_DIRECTORY)
        n |= COSMO_O_DIRECTORY;
    /* Accepted and dropped: no effect on this kernel. */
    unsigned known = LX_O_ACCMODE | LX_O_CREAT | LX_O_EXCL | LX_O_TRUNC | LX_O_APPEND | LX_O_DIRECTORY | LX_O_CLOEXEC |
                     LX_O_NONBLOCK | LX_O_NOCTTY | LX_O_LARGEFILE | LX_O_NOFOLLOW;
    if (lx & ~known)
        return -1;
    *native = n;
    return 0;
}

void lx_stat_from_native(const struct cosmo_stat *st, struct lx_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_ino = st->ino;
    out->st_nlink = st->nlink;
    uint32_t type;
    switch (st->type) {
    case COSMO_DT_DIR: type = LX_S_IFDIR; break;
    case COSMO_DT_CHR: type = LX_S_IFCHR; break;
    case COSMO_DT_FIFO: type = LX_S_IFIFO; break;
    case COSMO_DT_SOCK: type = LX_S_IFSOCK; break;
    default: type = LX_S_IFREG; break;
    }
    out->st_mode = type | (st->mode & 07777u);
    out->st_uid = st->uid;
    out->st_gid = st->gid;
    out->st_size = (int64_t)st->size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((st->size + 511) / 512);
    out->st_mtime = (int64_t)(st->mtime_ns / 1000000000ull);
    out->st_mtime_nsec = (int64_t)(st->mtime_ns % 1000000000ull);
    out->st_ctime = (int64_t)(st->ctime_ns / 1000000000ull);
    out->st_ctime_nsec = (int64_t)(st->ctime_ns % 1000000000ull);
    out->st_atime = out->st_mtime;
    out->st_atime_nsec = out->st_mtime_nsec;
}

int lx_wait_status(int native_status)
{
    if (native_status == COSMO_EXIT_FAULT)
        return LX_SIGSEGV;                  /* terminated by SIGSEGV */
    if (native_status > 128 && native_status < 128 + 64)
        return native_status - 128;         /* terminated by that signal */
    return (native_status & 0xff) << 8;     /* exited normally */
}

int lx_sockaddr_to_netaddr(const void *sa, size_t len, struct netaddr *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 2)
        return -EINVAL;
    uint16_t family;
    memcpy(&family, sa, 2);
    if (family == LX_AF_INET) {
        if (len < sizeof(struct lx_sockaddr_in))
            return -EINVAL;
        const struct lx_sockaddr_in *in = sa;
        out->family = COSMO_AF_INET;
        out->port = ntohs(in->sin_port);
        out->v4 = in->sin_addr;
        return 0;
    }
    if (family == LX_AF_INET6) {
        if (len < sizeof(struct lx_sockaddr_in6))
            return -EINVAL;
        const struct lx_sockaddr_in6 *in6 = sa;
        out->family = COSMO_AF_INET6;
        out->port = ntohs(in6->sin6_port);
        memcpy(out->v6.s6_addr, in6->sin6_addr, 16);
        return 0;
    }
    return -EAFNOSUPPORT;
}

size_t lx_sockaddr_from_netaddr(const struct netaddr *in, void *out, size_t cap)
{
    if (in->family == COSMO_AF_INET6) {
        struct lx_sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = LX_AF_INET6;
        a6.sin6_port = htons(in->port);
        memcpy(a6.sin6_addr, in->v6.s6_addr, 16);
        memcpy(out, &a6, cap < sizeof(a6) ? cap : sizeof(a6));
        return sizeof(a6);
    }
    struct lx_sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = LX_AF_INET;
    a4.sin_port = htons(in->port);
    a4.sin_addr = in->v4;
    memcpy(out, &a4, cap < sizeof(a4) ? cap : sizeof(a4));
    return sizeof(a4);
}

uint8_t lx_dirent_type(uint8_t native)
{
    switch (native) {
    case COSMO_DT_REG: return LX_DT_REG;
    case COSMO_DT_DIR: return LX_DT_DIR;
    case COSMO_DT_CHR: return LX_DT_CHR;
    case COSMO_DT_FIFO: return LX_DT_FIFO;
    case COSMO_DT_SOCK: return LX_DT_SOCK;
    default: return LX_DT_UNKNOWN;
    }
}

size_t lx_dirents_from_native(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap)
{
    size_t ip = 0, op = 0;
    while (ip + sizeof(struct cosmo_dirent) <= inlen) {
        struct cosmo_dirent d;
        memcpy(&d, in + ip, sizeof(d));
        const size_t hdr = offsetof(struct cosmo_dirent, name);   /* 12: the name follows the packed header */
        if (d.reclen < hdr || ip + d.reclen > inlen || d.namelen > d.reclen - hdr)
            break;
        size_t rec = (19 + (size_t)d.namelen + 1 + 7) & ~(size_t)7;   /* header 19 bytes + name + NUL, 8-aligned */
        if (op + rec > outcap)
            break;
        struct lx_dirent64 h = { .d_ino = d.ino, .d_off = (int64_t)(op + rec), .d_reclen = (uint16_t)rec,
                                 .d_type = lx_dirent_type(d.type) };
        memcpy(out + op, &h, 19);
        memcpy(out + op + 19, in + ip + hdr, d.namelen);
        memset(out + op + 19 + d.namelen, 0, rec - 19 - d.namelen);
        op += rec;
        ip += d.reclen;
    }
    return op;
}

int lx_prot(unsigned lx, int *native)
{
    if (lx & ~(LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC))
        return -1;
    int n = 0;
    if (lx & LX_PROT_READ)
        n |= COSMO_PROT_READ;
    if (lx & LX_PROT_WRITE)
        n |= COSMO_PROT_WRITE;
    if (lx & LX_PROT_EXEC)
        n |= COSMO_PROT_EXEC;
    *native = n;
    return 0;
}
