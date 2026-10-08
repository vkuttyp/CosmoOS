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
struct unix_sock;

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
    int family;                 /* COSMO_AF_INET / COSMO_AF_INET6 / COSMO_AF_UNIX */
    int type;                   /* COSMO_SOCK_STREAM / COSMO_SOCK_DGRAM */
    enum socket_state state;
    struct udp_pcb udp;         /* SOCK_DGRAM, inet */
    struct tcp_pcb *tcp;        /* SOCK_STREAM, inet */
    struct unix_sock *un;       /* COSMO_AF_UNIX: the transport in kernel/ipc/unix.c; every ksock_* dispatches on the family */
    struct waitqueue wait;
    /* Pending asynchronous error, delivered once (invariant N21). Written
     * by sock_set_error from any context -- including packet receive,
     * where s->lock cannot be taken -- and read by ksock_error, which
     * exchanges it for 0. Neither holds s->lock: three of the five
     * readers hold that mutex and two do not, so it cannot serve as this
     * field's rule. Atomic accessors only, and never by hand: the low 32
     * bits are the errno and the high 32 a generation bumped by every
     * write, so a delivery can tell the verdict it carried from an
     * identical errno that arrived while it was being copied out. */
    uint64_t error;
    unsigned shut;              /* 1 = RD, 2 = WR */
    bool nonblock;              /* a property of the object, shared by every handle to it */
    struct mutex lock;
    uint32_t uid;               /* creator's effective uid, informational (reserved ports are judged on the
                                   caller's credentials at bind time) */
};

#define SOCK_IO_CHUNK 4096u

void socket_init(void);
int ksock_create(int family, int type, uint32_t uid, struct socket **out);
/* The inet entry points below refuse a unix socket with -EAFNOSUPPORT (an
 * inet address cannot name it); the doors call the unix_* entry points
 * of kernel/unix.h for a unix address. The address-free ones -- listen,
 * accept, a connected send or receive, shutdown, ready -- dispatch. */
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
/* Protocol side: a reference held across a wake made after the protocol's
 * own lock is dropped. A tryget, because a socket's release detaches it
 * from its pcb only after its count reached zero: NULL for a NULL or dying
 * socket. Counted while held, so the self-test census can tell a socket a
 * test left behind from one a worker is still waking (socket_wake_refs). */
struct socket *sock_wake_ref(struct socket *s);
void sock_wake_unref(struct socket *s);
void sock_set_error(struct socket *s, int err);   /* and wake */
/* The pending asynchronous error, read once: returns it and clears it, as
 * SO_ERROR does, so two readers cannot both be told the same verdict. A
 * stream socket's own pcb error is reported *without* clearing, because a
 * dead connection must keep failing. 0 when there is none. Takes no lock
 * (invariant N21): safe with or without s->lock held, and against a
 * writer in packet context. */
int ksock_error(struct socket *s);
/* For a caller whose delivery can fail -- a syscall copying the verdict to
 * user memory, where a range check is not a promise the copy succeeds.
 * `peek` reports without clearing and hands back an opaque `token`;
 * `delivered` commits the clear only once the value has reached the
 * caller, and only if the token still names what is there. Taking first
 * and putting it back on failure would leave a window in which a
 * concurrent asker sees 0 while a verdict is pending and undelivered,
 * which is a worse answer than the one this pair can give: if two askers
 * race, both are told the truth and one of them clears it. The token
 * carries a generation rather than only the errno, because two ICMP
 * messages for one flow can carry the same one. */
int ksock_error_peek(struct socket *s, uint64_t *token);
void ksock_error_delivered(struct socket *s, uint64_t token);

unsigned socket_count(void);
/* Wake references held right now (sock_wake_ref). Read with acquire: at a
 * zero, every socket a wake reference was the last holder of has been
 * released and uncounted. */
unsigned socket_wake_refs(void);

/*
 * The held-wake seam (`net-census-wake-ref`). CONFIG_DEBUG only; every
 * entry point is a no-op, and _wait false, otherwise.
 *
 * Armed for a socket, the next sock_wake_unref of it -- on whichever
 * thread made the wake -- stops after the wake and before its put, holding
 * the reference, until _release. The hold is bounded (10 s) and a timeout
 * is recorded, never hidden: _release returns false if the held side gave
 * up waiting. _wait blocks until a wake is held, to `ns`.
 */
void sock_test_wake_hold_arm(struct socket *s);
bool sock_test_wake_hold_wait(uint64_t ns);
bool sock_test_wake_hold_release(void);   /* and disarm; waits for the held side to resume */

#endif /* KERNEL_SOCKET_H */
