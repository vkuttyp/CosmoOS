#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define AF_INET COSMO_AF_INET
#define AF_INET6 COSMO_AF_INET6
#define SOCK_STREAM COSMO_SOCK_STREAM
#define SOCK_DGRAM COSMO_SOCK_DGRAM
#define SHUT_RD COSMO_SHUT_RD
#define SHUT_WR COSMO_SHUT_WR
#define SHUT_RDWR COSMO_SHUT_RDWR
/* The native, family-tagged address is the only address shape. */
struct sockaddr {
    uint16_t sa_family;
    uint16_t sa_port;       /* host order */
    uint32_t sa_flowinfo;
    uint8_t sa_addr[16];    /* 4 bytes for AF_INET, 16 for AF_INET6, network order */
    uint32_t sa_scope;
};
int socket(int family, int type, int proto);
int bind(int fd, const struct sockaddr *sa, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *peer, socklen_t *len);
int connect(int fd, const struct sockaddr *sa, socklen_t len);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *sa, socklen_t *len);
#endif
