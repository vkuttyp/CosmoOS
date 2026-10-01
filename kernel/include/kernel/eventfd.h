/*
 * eventfd.h - an eventfd counter as an I/O object.
 *
 * A kobject with a readiness operation (like timerobj), so it rides the I/O
 * ring's POLL/READ path and serves poll/select, a blocking read and a
 * blocking write with no new mechanism. It holds a uint64 counter: a write
 * adds to it (blocking while it would overflow), a read drains it -- the whole
 * count, or one at a time in semaphore mode -- and it is readable while the
 * count is non-zero and writable while it is below the maximum. See
 * docs/audit/next-subsystem-eventfd.md.
 */
#ifndef KERNEL_EVENTFD_H
#define KERNEL_EVENTFD_H

#include <stdbool.h>
#include <stdint.h>

struct kobject;

/* Create an eventfd with initial count `initval`; `semaphore` selects the
 * one-at-a-time read. On success *out owns one reference; the caller installs
 * a handle and drops it. -ENOMEM on allocation failure. */
int eventfd_obj_create(uint64_t initval, bool semaphore, struct kobject **out);

#endif /* KERNEL_EVENTFD_H */
