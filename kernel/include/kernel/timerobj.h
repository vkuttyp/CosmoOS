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

#include <stdbool.h>
#include <stdint.h>

struct kobject;

/* Create a timer armed to first expire after `initial_ns` (which must be
 * non-zero) and, if `interval_ns` is non-zero, to re-arm for that interval
 * after each expiry. On success *out owns one reference; the caller installs
 * a handle and drops it. -EINVAL if initial_ns is zero. */
int timer_obj_create(uint64_t initial_ns, uint64_t interval_ns, struct kobject **out);

/* Create a disarmed timer object (a timerfd starts disarmed). `nonblock` sets
 * the object's non-blocking mode; `realtime` records that the fd's clock is
 * the wall clock, so timerfd_settime converts an absolute deadline against the
 * right `now`. On success *out owns one reference. -ENOMEM on failure. */
int timer_obj_create_disarmed(bool nonblock, bool realtime, struct kobject **out);

/* Re-arm (`initial_ns != 0`) or disarm (`initial_ns == 0`) an existing timer
 * object, resetting its expiration count (Linux resets on settime). Reports
 * the previous remaining time and interval through the out-params (either may
 * be NULL). All times are monotonic ns; the caller converts any wall-clock
 * deadline to a monotonic delay. */
void timer_obj_settime(struct kobject *obj, uint64_t initial_ns, uint64_t interval_ns,
                       uint64_t *old_remaining_ns, uint64_t *old_interval_ns);

/* The remaining time until the next expiry (0 if disarmed) and the interval. */
void timer_obj_gettime(struct kobject *obj, uint64_t *remaining_ns, uint64_t *interval_ns);

/* Whether the fd's clock is the wall clock, for the absolute settime path. */
bool timer_obj_is_realtime(struct kobject *obj);

/* `obj` iff it is a timer object, else NULL -- a Linux door confirms an fd is
 * a timerfd before a settime/gettime. */
struct kobject *timer_obj_from_kobject(struct kobject *obj);

#endif /* KERNEL_TIMEROBJ_H */
