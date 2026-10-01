/*
 * epoll.h - an interest set of I/O objects, waited on together (epoll).
 *
 * A kobject holding a set of (fd, object) registrations, each with a wanted
 * readiness mask and an opaque data token. epoll_obj_wait reports which members
 * are ready, sleeping on every member's poll_wq and the set's own queue at once
 * -- the same multi-wait protocol as the aio ring (kernel/io/aio.c). Generic:
 * the Linux personality translates EPOLL* <-> COSMO_IO_* and carries the
 * epoll_event ABI. See docs/audit/next-subsystem-epoll.md.
 */
#ifndef KERNEL_EPOLL_H
#define KERNEL_EPOLL_H

#include <stdbool.h>
#include <stdint.h>

struct kobject;

#define EPOLL_WAIT_FOREVER UINT64_MAX

/* One ready member, kernel-side: the COSMO_IO_* bits that fired and the opaque
 * token the registration carried. */
struct epoll_ready {
    unsigned events;
    uint64_t data;
};

int epoll_obj_create(struct kobject **out);

/* `obj` iff it is an epoll object, else NULL -- a door confirms an fd is an
 * epoll before a ctl/wait, and rejects nesting an epoll in an epoll. */
struct kobject *epoll_obj_from_kobject(struct kobject *obj);

/* Register `target` (a referenced object; the add takes ownership of that
 * reference on success, the caller drops it on failure) under key `fd` with a
 * COSMO_IO_* `want` mask, opaque `data`, and one-shot flag. -EEXIST if `fd` is
 * already registered. */
int epoll_obj_add(struct kobject *ep, int fd, struct kobject *target,
                  unsigned want, uint64_t data, bool oneshot);
/* Update the registration keyed by `fd` and re-arm a one-shot. -ENOENT if not
 * registered. */
int epoll_obj_mod(struct kobject *ep, int fd, unsigned want, uint64_t data, bool oneshot);
/* Remove the registration keyed by `fd`, dropping its held reference. -ENOENT
 * if not registered. */
int epoll_obj_del(struct kobject *ep, int fd);

/* Wait until a member is ready, the timeout elapses, or a kill is pending.
 * Fills up to `max` ready members into `out` and returns the count (0 at the
 * deadline), or -EINTR. `timeout_ns == 0` polls; `EPOLL_WAIT_FOREVER` blocks. */
int64_t epoll_obj_wait(struct kobject *ep, struct epoll_ready *out, unsigned max, uint64_t timeout_ns);

#endif /* KERNEL_EPOLL_H */
