/*
 * handle.h - Per-process handle table: small integers to kernel objects
 * with rights. User space never sees a kobject pointer.
 *
 * Concurrency: the table spinlock (irqsave) protects slot contents.
 * A lookup returns a referenced object; the caller owns that reference
 * until kobject_put. All functions are non-blocking.
 */

#ifndef KERNEL_HANDLE_H
#define KERNEL_HANDLE_H

#include <kernel/object.h>
#include <kernel/spinlock.h>

#define HANDLE_TABLE_SIZE 64

/*
 * A handle is a capability: what a process may do with an object is what
 * its handle says (docs/kernel/object/architecture.md, "Rights"). Bits
 * 0..15 mean the same for every object kind; 16..31 are the type's own.
 * Rights only ever shrink -- nothing adds a right to a handle.
 */
#define HANDLE_RIGHT_READ     (1u << 0)   /* take data out of it */
#define HANDLE_RIGHT_WRITE    (1u << 1)   /* put data into it */
#define HANDLE_RIGHT_DUP      (1u << 2)   /* make another handle to it here */
#define HANDLE_RIGHT_TRANSFER (1u << 3)   /* give a handle to it to another process */
#define HANDLE_RIGHT_MANAGE   (1u << 4)   /* change how it behaves, not what it holds */

#define HANDLE_RIGHTS_GENERIC 0x0000FFFFu
#define HANDLE_RIGHTS_TYPE    0xFFFF0000u

/* What creating an object grants on top of the access rights that suit
 * it: the creator may copy it, pass it on and administer it. */
#define HANDLE_RIGHT_OWNER (HANDLE_RIGHT_DUP | HANDLE_RIGHT_TRANSFER | HANDLE_RIGHT_MANAGE)

/* What a creator gets: everything the generic vocabulary defines. A type
 * that adds rights of its own installs them alongside. */
#define HANDLE_RIGHT_ALL                                                       \
    (HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_DUP |               \
     HANDLE_RIGHT_TRANSFER | HANDLE_RIGHT_MANAGE)

struct handle_entry {
    struct kobject *obj;   /* NULL = free */
    unsigned rights;
};

struct handle_table {
    spinlock_t lock;
    struct handle_entry entries[HANDLE_TABLE_SIZE];
    unsigned count;
    unsigned limit;   /* COSMO_RLIMIT_NOFILE: handle_install refuses at or above it */
    /* Set before the table is torn down. A lookup or install after this
     * would hand out a reference to an object the exit is releasing, or
     * add one for nobody to close. */
    bool exiting;
};

void handle_table_init(struct handle_table *t);

/* Close every handle (drops the table's references). */
void handle_table_destroy(struct handle_table *t);

/* Take a reference on `obj` and store it in the lowest free slot.
 * Returns the handle or -EMFILE. */
int handle_install(struct handle_table *t, struct kobject *obj, unsigned rights);

/* Store at a specific slot (used for the standard handles of a new
 * process). -EBUSY if occupied, -EBADF if out of range. */
int handle_install_at(struct handle_table *t, int h, struct kobject *obj, unsigned rights);

/* Referenced object if `h` is valid and holds every right in
 * `rights_needed`; NULL otherwise. `missing_rights` distinguishes the
 * two failures for a caller that wants to say -EPERM rather than
 * -EBADF: it is set when the handle exists and the rights do not. */
struct kobject *handle_lookup(struct handle_table *t, int h, unsigned rights_needed);
struct kobject *handle_lookup_rights(struct handle_table *t, int h, unsigned rights_needed, bool *missing_rights);
/* Referenced object and its rights, for dup and spawn; NULL when free. */
struct kobject *handle_get(struct handle_table *t, int h, unsigned *rights_out);

/* Drop the table's reference. -EBADF if the slot is empty or invalid. */
int handle_close(struct handle_table *t, int h);

unsigned handle_table_count(struct handle_table *t);

#endif /* KERNEL_HANDLE_H */
