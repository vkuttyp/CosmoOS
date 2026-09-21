/* socket.c - Sockets over the native calls, inet_pton/inet_ntop. */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "libc.h"

int socket(int family, int type, int proto) { return (int)__syscall_ret(cosmo_socket(family, type, proto)); }
int listen(int fd, int backlog) { return (int)__syscall_ret(cosmo_listen(fd, backlog)); }
int shutdown(int fd, int how) { return (int)__syscall_ret(cosmo_shutdown(fd, how)); }

/* A unix address is as long as its name; an inet one is the whole
 * struct. The kernel reads the family first and the right shape after. */
static int addr_len_ok(const struct sockaddr *sa, socklen_t len)
{
    if (sa == NULL || len < 2)
        return 0;
    if (sa->sa_family == AF_UNIX)
        return len <= sizeof(struct cosmo_sockaddr_un);
    return len >= sizeof(struct cosmo_sockaddr);
}

int bind(int fd, const struct sockaddr *sa, socklen_t len)
{
    if (!addr_len_ok(sa, len)) {
        errno = EINVAL;
        return -1;
    }
    return (int)__syscall_ret(cosmo_bind_len(fd, sa, len));
}

int connect(int fd, const struct sockaddr *sa, socklen_t len)
{
    if (!addr_len_ok(sa, len)) {
        errno = EINVAL;
        return -1;
    }
    return (int)__syscall_ret(cosmo_connect_len(fd, sa, len));
}

int socketpair(int family, int type, int proto, int sv[2])
{
    (void)proto;
    return (int)__syscall_ret(cosmo_socketpair(family, type, sv));
}

int getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
    size_t l = len ? *len : 0;
    long r = __syscall_ret(cosmo_getsockopt(fd, level, opt, val, len ? &l : NULL));
    if (r >= 0 && len)
        *len = (socklen_t)l;
    return (int)r;
}

int accept(int fd, struct sockaddr *peer, socklen_t *len)
{
    size_t l = len ? *len : 0;
    long r = __syscall_ret(cosmo_accept(fd, (struct cosmo_sockaddr *)peer, peer ? &l : NULL));
    if (r >= 0 && len)
        *len = (socklen_t)l;
    return (int)r;
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t len)
{
    (void)flags;
    if (to && !addr_len_ok(to, len)) {
        errno = EINVAL;
        return -1;
    }
    return __syscall_ret(cosmo_sendto_len(fd, buf, n, to, to ? len : 0));
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len)
{
    (void)flags;
    size_t l = len ? *len : 0;
    long r = __syscall_ret(cosmo_recvfrom(fd, buf, n, (struct cosmo_sockaddr *)from, from ? &l : NULL));
    if (r >= 0 && len)
        *len = (socklen_t)l;
    return r;
}

ssize_t send(int fd, const void *buf, size_t n, int flags) { return sendto(fd, buf, n, flags, NULL, 0); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, NULL, NULL); }

int getsockname(int fd, struct sockaddr *sa, socklen_t *len)
{
    size_t l = len ? *len : 0;
    long r = __syscall_ret(cosmo_getsockname(fd, (struct cosmo_sockaddr *)sa, &l));
    if (r >= 0 && len)
        *len = (socklen_t)l;
    return (int)r;
}

/* --- text addresses --- */

static int pton4(const char *s, uint8_t *out)
{
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*s))
            return 0;
        int v = 0;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (*s++ - '0');
            if (v > 255)
                return 0;
        }
        out[i] = (uint8_t)v;
        if (i < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    return *s == '\0';
}

static int pton6(const char *s, uint8_t *out)
{
    uint16_t words[8];
    int n = 0, gap = -1;
    if (s[0] == ':' && s[1] == ':') {
        gap = 0;
        s += 2;
    }
    while (*s && n < 8) {
        if (!isxdigit((unsigned char)*s))
            return 0;
        unsigned v = 0;
        int digits = 0;
        while (isxdigit((unsigned char)*s) && digits < 4) {
            char c = (char)tolower((unsigned char)*s++);
            v = v * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10);
            digits++;
        }
        words[n++] = (uint16_t)v;
        if (*s == ':') {
            s++;
            if (*s == ':') {
                if (gap >= 0)
                    return 0;
                gap = n;
                s++;
            } else if (*s == '\0') {
                return 0;
            }
        } else if (*s != '\0') {
            return 0;
        }
    }
    if (*s)
        return 0;
    if (gap < 0 ? n != 8 : n == 8)
        return 0;   /* eight groups need no "::"; fewer need it */
    memset(out, 0, 16);
    int tail = gap < 0 ? 0 : n - gap;
    for (int i = 0; i < (gap < 0 ? n : gap); i++) {
        out[2 * i] = (uint8_t)(words[i] >> 8);
        out[2 * i + 1] = (uint8_t)words[i];
    }
    for (int i = 0; i < tail; i++) {
        int w = 8 - tail + i;
        out[2 * w] = (uint8_t)(words[gap + i] >> 8);
        out[2 * w + 1] = (uint8_t)words[gap + i];
    }
    return 1;
}

int inet_pton(int family, const char *src, void *dst)
{
    if (family == AF_INET)
        return pton4(src, dst);
    if (family == AF_INET6)
        return pton6(src, dst);
    errno = EAFNOSUPPORT;
    return -1;
}

const char *inet_ntop(int family, const void *src, char *dst, socklen_t size)
{
    const uint8_t *a = src;
    char tmp[INET6_ADDRSTRLEN];
    if (family == AF_INET) {
        snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else if (family == AF_INET6) {
        /* Longest run of zero words becomes "::". */
        uint16_t w[8];
        for (int i = 0; i < 8; i++)
            w[i] = (uint16_t)((a[2 * i] << 8) | a[2 * i + 1]);
        int best = -1, best_len = 0;
        for (int i = 0; i < 8;) {
            if (w[i] == 0) {
                int j = i;
                while (j < 8 && w[j] == 0)
                    j++;
                if (j - i > best_len && j - i >= 2) {
                    best = i;
                    best_len = j - i;
                }
                i = j;
            } else {
                i++;
            }
        }
        char *p = tmp;
        for (int i = 0; i < 8; i++) {
            if (i == best) {
                *p++ = ':';
                if (i == 0)
                    *p++ = ':';
                i += best_len - 1;
                continue;
            }
            p += sprintf(p, "%x", w[i]);
            if (i < 7)
                *p++ = ':';
        }
        *p = '\0';
    } else {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (strlen(tmp) + 1 > size) {
        errno = ENOSPC;
        return NULL;
    }
    strcpy(dst, tmp);
    return dst;
}
