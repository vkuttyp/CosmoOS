/*
 * poll.h - Wait for readiness on several objects at once
 * (docs/kernel/io/design.md, "Polling"). The AIO ring's multi-queue wait
 * as a plain call; Linux poll/ppoll translate onto it.
 */

#ifndef KERNEL_POLL_H
#define KERNEL_POLL_H

#include <kernel/types.h>

struct kobject;

struct io_pollfd {
    struct kobject *obj;   /* NULL: the entry is ignored (a negative fd) */
    unsigned events;       /* COSMO_IO_* bits of interest */
    unsigned revents;      /* out: ready & events, plus HANGUP/ERROR whenever set */
};

/* Fill every entry's revents; return how many entries have any bit set.
 * With none set: sleep on every object's poll_wq until one may have
 * changed, `timeout_ns` passes (0: return at once; IO_POLL_FOREVER: no
 * timeout) or the process is killed or has a signal to take (-EINTR).
 * Objects without a poll_wq are re-checked only when another entry wakes
 * the caller or the timeout ends. Thread context. */
#define IO_POLL_FOREVER (~0ull)
int io_poll(struct io_pollfd *fds, unsigned n, uint64_t timeout_ns);

#endif /* KERNEL_POLL_H */
