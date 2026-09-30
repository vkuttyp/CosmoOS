/*
 * timerobj.h - a timer as a submittable I/O object (a timerfd).
 *
 * The object is a kobject with a readiness operation, so it rides the I/O
 * ring's existing POLL/READ path (no new AIO op), and answers poll/select
 * and a blocking read like any other I/O object. It becomes readable when
 * it has expired at least once; a read returns the expiration count since
 * the last read and resets it. See docs/kernel/io/.
 */
#ifndef KERNEL_TIMEROBJ_H
#define KERNEL_TIMEROBJ_H

#include <stdint.h>

struct kobject;

/* Create a timer armed to first expire after `initial_ns` (which must be
 * non-zero) and, if `interval_ns` is non-zero, to re-arm for that interval
 * after each expiry. On success *out owns one reference; the caller installs
 * a handle and drops it. -EINVAL if initial_ns is zero. */
int timer_obj_create(uint64_t initial_ns, uint64_t interval_ns, struct kobject **out);

#endif /* KERNEL_TIMEROBJ_H */
