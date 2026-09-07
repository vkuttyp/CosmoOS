/*
 * utsns.h - The uts namespace: what a process believes it is running on
 * (docs/kernel/security/design.md §1e, invariant S13).
 *
 * One string, which is the point: software asks the machine its name and
 * believes the answer, so a contained process that reports the host's
 * name tells every log line and every peer something false about where
 * it is running.
 */

#ifndef KERNEL_UTSNS_H
#define KERNEL_UTSNS_H

#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <uapi/cosmo/syscall.h>

struct uts_ns {
    /* Processes holding it. Atomic rather than under a lock, because a
     * child inherits its parent's while the parent's spinlock is held
     * and a mutex may not be taken there. */
    uint32_t refs;
    spinlock_t lock;                       /* `name` only; held only to copy it */
    char name[COSMO_HOST_NAME_MAX];        /* NUL-terminated */
};

/* The namespace the system boots in. Never freed. */
struct uts_ns *utsns_initial(void);

/* The caller's namespace: its process's, or the initial one for a
 * kernel thread and for anything before there are processes. Borrowed
 * -- valid because the caller is running in it. */
struct uts_ns *utsns_current(void);

/* A new namespace holding a copy of `parent`'s name as it is now. */
int utsns_create(struct uts_ns *parent, struct uts_ns **out);

struct uts_ns *utsns_get(struct uts_ns *ns);
void utsns_put(struct uts_ns *ns);

/*
 * Copy the name out and in. Both go through a local buffer under the
 * lock rather than handing the stored string to a caller, so a
 * concurrent set is never observed half-written -- and so no user copy
 * happens with a spinlock held.
 */
size_t utsns_gethostname(struct uts_ns *ns, char *out, size_t n);
/* `name` is `len` bytes, not necessarily terminated. -EINVAL if it does
 * not fit or holds a NUL or a control character: a name reaches logs
 * and peers, and one that can hold a newline can forge a log line. */
int utsns_sethostname(struct uts_ns *ns, const char *name, size_t len);

#endif /* KERNEL_UTSNS_H */
