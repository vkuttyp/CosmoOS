/*
 * unix.h - Unix domain sockets: the transport behind COSMO_AF_UNIX
 * (docs/kernel/ipc/design.md, "Unix domain sockets"; the report is
 * docs/audit/next-subsystem-unix-sockets.md).
 *
 * `struct socket` (kernel/socket.h) owns one of these for the unix
 * family and every ksock_* entry point dispatches here before touching a
 * pcb. The network stack is not involved: a stream connection is two
 * bounded queues of sends, a datagram socket is one, a name is a
 * filesystem node or an abstract name scoped by the caller's root, and a
 * message may carry handles under spawn's transfer rule.
 */

#ifndef KERNEL_UNIX_H
#define KERNEL_UNIX_H

#include <kernel/object.h>
#include <kernel/types.h>
#include <uapi/cosmo/syscall.h>

struct socket;
struct vnode;

#define UNIX_PATH_MAX    COSMO_UNIX_PATH_MAX
#define UNIX_HANDLES_MAX COSMO_UNIX_HANDLES_MAX
#define UNIX_BUF         (64u * 1024u)     /* a stream direction: bytes queued and unread */
#define UNIX_MSG_MAX     UNIX_BUF          /* one datagram, or one stream send */
#define UNIX_DGRAM_MAX   64u               /* a datagram socket's queue: messages ... */
#define UNIX_DGRAM_BYTES (256u * 1024u)    /* ... and bytes */
#define UNIX_BACKLOG_MAX 128u

/* A unix name as the doors parse it: a path (NUL-terminated, `len` its
 * length without the NUL) or an abstract name (`len` bytes, any bytes).
 * `len == 0` is "no name". */
struct unix_addr {
    bool abstract;
    uint8_t len;
    char bytes[UNIX_PATH_MAX];
};

/* Handles riding in a message: referenced objects and the rights to
 * install them with, owned by the message once queued. */
struct unix_handles {
    unsigned nr;
    struct kobject *objs[UNIX_HANDLES_MAX];
    unsigned rights[UNIX_HANDLES_MAX];
};

int  unix_create(struct socket *s);          /* ksock_create, for COSMO_AF_UNIX */
void unix_release(struct socket *s);         /* the socket's release: name, connection, queue, references */

int  unix_bind(struct socket *s, const struct unix_addr *a);
int  unix_listen(struct socket *s, int backlog);
int  unix_accept(struct socket *s, struct socket **out);                 /* blocks */
int  unix_connect(struct socket *s, const struct unix_addr *a);          /* blocks on a full backlog */
/* A send: `to` names a datagram's destination (NULL: the connected peer);
 * `h` is the handles to carry (NULL or nr 0: none) and is consumed on
 * success -- its references belong to the message. Returns bytes sent;
 * a stream may send fewer than `len`. */
int64_t unix_send(struct socket *s, const void *buf, size_t len, const struct unix_addr *to,
                  struct unix_handles *h, bool dontwait);
/* A receive: `from` (optional) gets the sender's name; `h` (optional) has
 * `nr` set to the room on entry and to the handles delivered on return,
 * their references now the caller's; `flags` gets COSMO_MSG_TRUNC and
 * COSMO_MSG_HTRUNC. Returns bytes received, 0 at end of stream. */
int64_t unix_recv(struct socket *s, void *buf, size_t len, struct unix_addr *from, struct unix_handles *h,
                  unsigned *flags, bool dontwait);
int  unix_shutdown(struct socket *s, int how);   /* after the socket's shut bits are set: closes the queues, wakes */
int  unix_getsockname(struct socket *s, struct unix_addr *out);
int  unix_getpeername(struct socket *s, struct unix_addr *out);
int  unix_peercred(struct socket *s, struct cosmo_ucred *out);
unsigned unix_ready(struct socket *s);
int  unix_socketpair(int type, struct socket **a, struct socket **b);

/* The user shape of a name at either door -- a 16-bit family and then
 * the path -- is the same on both, so both parse and pack here. `plen`
 * is the bytes after the family: 0 means no name; a leading NUL means an
 * abstract name of the remaining bytes; otherwise a path, NUL-terminated
 * or exactly `plen` long. `pack` writes `family` and the name into
 * `out` (at least 2 + UNIX_PATH_MAX bytes) and returns the full size. */
int unix_addr_parse(const char *path, size_t plen, struct unix_addr *out);
size_t unix_addr_pack(const struct unix_addr *a, uint16_t family, void *out);

/* Drop the references a handle set still owns (a send that failed). */
void unix_handles_drop(struct unix_handles *h);

unsigned unix_socket_count(void);   /* live unix sockets, for the leak tests */

#endif /* KERNEL_UNIX_H */
