/*
 * handle.c - Per-process handle table.
 */

#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/panic.h>
#include <kernel/string.h>

void handle_table_init(struct handle_table *t)
{
    spinlock_init(&t->lock, "handles");
    memset(t->entries, 0, sizeof(t->entries));
    t->count = 0;
    t->limit = HANDLE_TABLE_SIZE;
    t->exiting = false;
}

/*
 * Closing every handle is not enough on its own: a thread of this
 * process could be inside a syscall, between its own lookup and its
 * install, and would put a reference back into a table nobody will
 * close again. The gate is raised first, so every later lookup and
 * install fails, and only then are the slots emptied.
 */
void handle_table_destroy(struct handle_table *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    t->exiting = true;
    spin_unlock_irqrestore(&t->lock, s);
    for (int h = 0; h < HANDLE_TABLE_SIZE; h++)
        handle_close(t, h);
    KASSERT(t->count == 0);
}

static int install_slot(struct handle_table *t, int h, struct kobject *obj, unsigned rights)
{
    t->entries[h].obj = obj;
    t->entries[h].rights = rights;
    t->count++;
    return h;
}

int handle_install(struct handle_table *t, struct kobject *obj, unsigned rights)
{
    KASSERT(obj != NULL);
    kobject_get(obj);

    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    if (t->exiting) {
        spin_unlock_irqrestore(&t->lock, s);
        kobject_put(obj);
        return -EBADF;   /* the table is going away; nothing would close it */
    }
    if (t->count >= t->limit) {   /* the process's NOFILE limit */
        spin_unlock_irqrestore(&t->lock, s);
        kobject_put(obj);
        return -EMFILE;
    }
    for (int h = 0; h < HANDLE_TABLE_SIZE; h++) {
        if (t->entries[h].obj == NULL) {
            int r = install_slot(t, h, obj, rights);
            spin_unlock_irqrestore(&t->lock, s);
            return r;
        }
    }
    spin_unlock_irqrestore(&t->lock, s);

    kobject_put(obj);
    return -EMFILE;
}

int handle_install_at(struct handle_table *t, int h, struct kobject *obj, unsigned rights)
{
    KASSERT(obj != NULL);
    if (h < 0 || h >= HANDLE_TABLE_SIZE)
        return -EBADF;
    kobject_get(obj);

    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    if (t->exiting) {
        spin_unlock_irqrestore(&t->lock, s);
        kobject_put(obj);
        return -EBADF;
    }
    if (t->entries[h].obj != NULL) {
        spin_unlock_irqrestore(&t->lock, s);
        kobject_put(obj);
        return -EBUSY;
    }
    install_slot(t, h, obj, rights);
    spin_unlock_irqrestore(&t->lock, s);
    return h;
}

struct kobject *handle_lookup_rights(struct handle_table *t, int h, unsigned rights_needed, bool *missing_rights)
{
    if (missing_rights)
        *missing_rights = false;
    if (h < 0 || h >= HANDLE_TABLE_SIZE)
        return NULL;

    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    struct kobject *obj = t->exiting ? NULL : t->entries[h].obj;
    if (obj != NULL && (t->entries[h].rights & rights_needed) != rights_needed) {
        /* The handle is real and does not carry what was asked for:
         * a different answer than "no such handle", and one the caller
         * is entitled to tell apart. */
        if (missing_rights)
            *missing_rights = true;
        obj = NULL;
    } else if (obj != NULL) {
        kobject_get(obj);
    }
    spin_unlock_irqrestore(&t->lock, s);
    return obj;
}

struct kobject *handle_lookup(struct handle_table *t, int h, unsigned rights_needed)
{
    return handle_lookup_rights(t, h, rights_needed, NULL);
}

struct kobject *handle_get(struct handle_table *t, int h, unsigned *rights_out)
{
    if (h < 0 || h >= HANDLE_TABLE_SIZE)
        return NULL;
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    struct kobject *obj = t->exiting ? NULL : t->entries[h].obj;
    if (obj != NULL) {
        kobject_get(obj);
        *rights_out = t->entries[h].rights;
    }
    spin_unlock_irqrestore(&t->lock, s);
    return obj;
}

int handle_close(struct handle_table *t, int h)
{
    if (h < 0 || h >= HANDLE_TABLE_SIZE)
        return -EBADF;

    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    struct kobject *obj = t->entries[h].obj;
    if (obj == NULL) {
        spin_unlock_irqrestore(&t->lock, s);
        return -EBADF;
    }
    t->entries[h].obj = NULL;
    t->entries[h].rights = 0;
    t->count--;
    spin_unlock_irqrestore(&t->lock, s);

    kobject_put(obj); /* outside the lock: release may block */
    return 0;
}

unsigned handle_table_count(struct handle_table *t)
{
    arch_irq_state_t s = spin_lock_irqsave(&t->lock);
    unsigned n = t->count;
    spin_unlock_irqrestore(&t->lock, s);
    return n;
}
