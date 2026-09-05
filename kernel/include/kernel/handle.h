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

#define HANDLE_RIGHT_READ  (1u << 0)
#define HANDLE_RIGHT_WRITE (1u << 1)
#define HANDLE_RIGHT_ALL   (HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE)

struct handle_entry {
    struct kobject *obj;   /* NULL = free */
    unsigned rights;
};

struct handle_table {
    spinlock_t lock;
    struct handle_entry entries[HANDLE_TABLE_SIZE];
    unsigned count;
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
 * `rights_needed`; NULL otherwise. */
struct kobject *handle_lookup(struct handle_table *t, int h, unsigned rights_needed);
/* Referenced object and its rights, for dup and spawn; NULL when free. */
struct kobject *handle_get(struct handle_table *t, int h, unsigned *rights_out);

/* Drop the table's reference. -EBADF if the slot is empty or invalid. */
int handle_close(struct handle_table *t, int h);

unsigned handle_table_count(struct handle_table *t);

#endif /* KERNEL_HANDLE_H */
