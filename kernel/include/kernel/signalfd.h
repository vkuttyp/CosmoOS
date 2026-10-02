/* signalfd.h - a signalfd as an I/O object (signalfd(2)).
 *
 * A kobject carrying a signal mask. It becomes readable when a signal in the
 * mask is pending for the reading process or thread, and read() drains those
 * as signalfd_siginfo records. It owns no process reference: ready/read use
 * the current process, and it polls on the process's signalfd_wqh (the signal
 * path wakes it), so a descriptor that outlives its creator reports the
 * reader's signals. See docs/audit/next-subsystem-signalfd.md.
 */
#ifndef KERNEL_SIGNALFD_H
#define KERNEL_SIGNALFD_H

#include <kernel/types.h>

struct kobject;

/* Create a signalfd reporting the signals in `mask` (the caller strips
 * SIGKILL/SIGSTOP); `nonblock` sets its initial O_NONBLOCK mode. */
int signalfd_obj_create(uint64_t mask, bool nonblock, struct kobject **out);

/* Replace an existing signalfd's mask. -EINVAL if `obj` is not a signalfd. */
int signalfd_obj_set_mask(struct kobject *obj, uint64_t mask);

#endif /* KERNEL_SIGNALFD_H */
