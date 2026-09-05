/*
 * socket.h - Sockets: the kernel API shared by system calls and tests.
 *
 * struct socket is a kobject with the io type, so a connected stream
 * socket in a handle table answers read/write/close. Every ksock_*
 * call blocks (wait queues) and runs in thread context; sock->lock
 * serialises callers and protocol locks nest inside it.
 */

#ifndef KERNEL_SOCKET_H
#define KERNEL_SOCKET_H

#include <kernel/mutex.h>
#include <kernel/net/inet.h>
#include <kernel/net/udp.h>
#include <kernel/object.h>
#include <kernel/wait.h>

struct tcp_pcb;

enum socket_state {
    SS_UNCONNECTED,
    SS_BOUND,
    SS_LISTENING,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_CLOSED,
};

struct socket {
    struct kobject obj;
    int family;                 /* COSMO_AF_INET / COSMO_AF_INET6 */
    int type;                   /* COSMO_SOCK_STREAM / COSMO_SOCK_DGRAM */
    enum socket_state state;
    struct udp_pcb udp;         /* SOCK_DGRAM */
    struct tcp_pcb *tcp;        /* SOCK_STREAM */
    struct waitqueue wait;
    int error;                  /* pending asynchronous error, consumed by the next call */
    unsigned shut;              /* 1 = RD, 2 = WR */
    bool nonblock;              /* a property of the object, shared by every handle to it */
    struct mutex lock;
    uint32_t uid;               /* creator's effective uid, informational (reserved ports are judged on the
                                   caller's credentials at bind time) */
};

#define SOCK_IO_CHUNK 4096u

void socket_init(void);
int ksock_create(int family, int type, uint32_t uid, struct socket **out);
int ksock_bind(struct socket *s, const struct netaddr *addr);
int ksock_listen(struct socket *s, int backlog);
int ksock_accept(struct socket *s, struct socket **out, struct netaddr *peer);   /* blocks */
int ksock_connect(struct socket *s, const struct netaddr *addr);               /* blocks */
int64_t ksock_sendto(struct socket *s, const void *buf, size_t len, const struct netaddr *to);
int64_t ksock_recvfrom(struct socket *s, void *buf, size_t len, struct netaddr *from);   /* blocks */
int ksock_shutdown(struct socket *s, int how);
int ksock_getsockname(struct socket *s, struct netaddr *out);
int ksock_getpeername(struct socket *s, struct netaddr *out);
/* Non-blocking mode: accept -EAGAIN, connect -EINPROGRESS, recv -EAGAIN,
 * send what fits or -EAGAIN. */
void ksock_set_nonblock(struct socket *s, bool on);
/* COSMO_IO_* bits that would not block now. */
unsigned ksock_ready(struct socket *s);
static inline void ksock_get(struct socket *s) { kobject_get(&s->obj); }
static inline void ksock_put(struct socket *s) { kobject_put(&s->obj); }
struct socket *socket_from_kobject(struct kobject *obj);

/* Protocol side: wake every waiter on the socket (any context). */
void sock_wake(struct socket *s);
void sock_set_error(struct socket *s, int err);   /* and wake */

unsigned socket_count(void);

#endif /* KERNEL_SOCKET_H */
