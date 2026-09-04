/*
 * shim_spinlock.c - Host spinlock with a held-lock stack.
 *
 * The kernel's spinlock is a one-word test-and-set; the only reason the
 * host does not use it verbatim is EXPECT_PANIC: a panic raised while a
 * lock is held longjmps out of the critical section, and the next
 * acquisition would then report a deadlock. The harness releases every
 * lock recorded here before unwinding, mirroring what "the CPU never
 * returns from panic" means for a kernel.
 */

#include <stdio.h>
#include <stdlib.h>

#include <kernel/spinlock.h>

#include "harness.h"

#define HELD_MAX 64

static spinlock_t *g_held[HELD_MAX];
static unsigned g_nheld;

void spinlock_init(spinlock_t *lock, const char *name)
{
    lock->locked = 0;
    lock->owner_cpu = SPINLOCK_NO_OWNER;
    lock->name = name;
}

void spin_lock(spinlock_t *lock)
{
    if (lock->locked) {
        fprintf(stderr, "host spinlock '%s' re-acquired (deadlock)\n", lock->name ? lock->name : "?");
        abort();
    }
    if (g_nheld >= HELD_MAX) {
        fprintf(stderr, "host spinlock: too many locks held\n");
        abort();
    }
    lock->locked = 1;
    lock->owner_cpu = 0;
    g_held[g_nheld++] = lock;
}

bool spin_trylock(spinlock_t *lock)
{
    if (lock->locked)
        return false;
    spin_lock(lock);
    return true;
}

void spin_unlock(spinlock_t *lock)
{
    if (!lock->locked || g_nheld == 0 || g_held[g_nheld - 1] != lock) {
        fprintf(stderr, "host spinlock '%s' unlocked out of order or while free\n",
                lock->name ? lock->name : "?");
        abort();
    }
    g_nheld--;
    lock->owner_cpu = SPINLOCK_NO_OWNER;
    lock->locked = 0;
}

arch_irq_state_t spin_lock_irqsave(spinlock_t *lock)
{
    spin_lock(lock);
    return 1;
}

void spin_unlock_irqrestore(spinlock_t *lock, arch_irq_state_t state)
{
    (void)state;
    spin_unlock(lock);
}

bool spin_is_held(const spinlock_t *lock)
{
    return lock->locked != 0;
}

void harness_release_all_locks(void)
{
    while (g_nheld > 0) {
        spinlock_t *l = g_held[--g_nheld];
        l->owner_cpu = SPINLOCK_NO_OWNER;
        l->locked = 0;
    }
}
