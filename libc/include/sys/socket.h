#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define AF_UNIX COSMO_AF_UNIX
#define AF_LOCAL COSMO_AF_UNIX
#define AF_INET COSMO_AF_INET
#define AF_INET6 COSMO_AF_INET6
#define SOL_SOCKET COSMO_SOL_SOCKET
#define SO_ERROR COSMO_SO_ERROR
#define SO_PEERCRED COSMO_SO_PEERCRED
#define MSG_DONTWAIT COSMO_MSG_DONTWAIT
#define MSG_TRUNC COSMO_MSG_TRUNC
#define MSG_CTRUNC COSMO_MSG_HTRUNC
#define SOCK_STREAM COSMO_SOCK_STREAM
#define SOCK_DGRAM COSMO_SOCK_DGRAM
#define SOCK_NONBLOCK COSMO_SOCK_NONBLOCK
#define SHUT_RD COSMO_SHUT_RD
#define SHUT_WR COSMO_SHUT_WR
#define SHUT_RDWR COSMO_SHUT_RDWR
/* The native, family-tagged inet address; a unix address is struct
 * sockaddr_un (sys/un.h), passed with its own length. */
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
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
/* Two connected unix sockets, no name (AF_UNIX only). */
int socketpair(int family, int type, int proto, int sv[2]);
struct ucred {
    int32_t pid;
    uint32_t uid, gid;
};
#endif
