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

/* One ready member, kernel-side. `io` is the COSMO_IO_* bits that fired;
 * `events` and `data` are the opaque personality tokens the registration
 * carried (the door interprets them); `fd` and `oneshot` let the door re-arm a
 * one-shot it could not deliver. */
struct epoll_ready {
    int fd;
    uint64_t id;              /* the registration instance, to re-arm exactly this one */
    unsigned io;
    uint32_t events;
    uint64_t data;
    bool oneshot;
};

int epoll_obj_create(struct kobject **out);

/* `obj` iff it is an epoll object, else NULL -- a door confirms an fd is an
 * epoll before a ctl/wait, and rejects nesting an epoll in an epoll. */
struct kobject *epoll_obj_from_kobject(struct kobject *obj);

/* Register `target` (a referenced object; the add takes ownership of that
 * reference on success, the caller drops it on failure) under key `fd` with a
 * COSMO_IO_* `want` mask for readiness filtering, an opaque `events` token and
 * opaque `data` (both echoed to the waiter), and a one-shot flag. -EEXIST if
 * `fd` is already registered. */
int epoll_obj_add(struct kobject *ep, int fd, struct kobject *target,
                  unsigned want, uint32_t events, uint64_t data, bool oneshot);
/* Update the registration keyed by `fd` and re-arm a one-shot. -ENOENT if not
 * registered. */
int epoll_obj_mod(struct kobject *ep, int fd, unsigned want, uint32_t events, uint64_t data, bool oneshot);
/* Remove the registration keyed by `fd`, dropping its held reference. -ENOENT
 * if not registered. */
int epoll_obj_del(struct kobject *ep, int fd);
/* Re-arm the one-shot registration `(fd, id)` whose event the door could not
 * deliver, and wake the set. A no-op if that exact registration is gone (the
 * fd was removed, or removed and re-added as a different instance), so a reused
 * fd's new registration is never wrongly re-enabled. */
void epoll_obj_rearm(struct kobject *ep, int fd, uint64_t id);

/* Wait until a member is ready, the timeout elapses, or a kill is pending.
 * Fills up to `max` ready members into `out` and returns the count (0 at the
 * deadline), or -EINTR. `timeout_ns == 0` polls; `EPOLL_WAIT_FOREVER` blocks. */
int64_t epoll_obj_wait(struct kobject *ep, struct epoll_ready *out, unsigned max, uint64_t timeout_ns);

#endif /* KERNEL_EPOLL_H */
