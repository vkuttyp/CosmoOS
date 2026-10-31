/*
 * handle_floor.c - The C under kernel/object/handle.cee (roadmap 2.0 SA2).
 *
 * The handle table and its code are CeeSafe (handle.cee, generated into
 * handle.c and kernel/include/kernel/handle_types.h). What stays here is
 * what CeeSafe cannot express yet: the flush, a call through the object
 * type's function pointer (CeeSafe type-system.md §31.1); the last close's
 * fence before it reads `watched`, which CeeSafe has no fence for; and a
 * formatted panic. It also checks that the constants handle.cee copies
 * are C's.
 */

#include <kernel/epoll.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/object.h>
#include <kernel/panic.h>

_Static_assert(EPERM == 1 && EBADF == 9 && EBUSY == 16 && EMFILE == 24, "handle.cee's errno values");
_Static_assert(HANDLE_RIGHT_TRANSFER == 8u && COSMO_RIGHTS_SAME == 0u && HANDLE_FLAG_CLOEXEC == 1u,
               "handle.cee's rights and flags");

/* Declared for handle.cee only; not part of handle.h's interface. */
int handle_floor_flush(struct kobject *obj);
void handle_floor_last_closed(struct kobject *obj);
void handle_floor_fatal(const char *what);

/* The object's flush: a file writes its dirty pages back and reports a
 * write-back failure once, and that report is close's result. */
int handle_floor_flush(struct kobject *obj)
{
    const struct kobject_io_type *io = kobject_io_of(obj);
    if (io != NULL && io->flush != NULL)
        return io->flush(obj);
    return 0;
}

/* The last descriptor to the object anywhere was closed: its epoll
 * registrations end here, as Linux drops an epitem at the file's final
 * close (docs/kernel/io/design.md, "epoll"). Outside the table lock -- the
 * removal takes mutexes -- and before the put, so a registration cannot
 * outlive the object it holds a reference to. The object that was never
 * registered (`watched`, set under epoll's watch lock before an add checks
 * the handle count) is spared the lock: the fence orders the caller's
 * decrement before the read of the flag as the add's orders its store
 * before its read of the count, so one of the two sees the other
 * (store-buffering: tests/litmus/epoll/watched.litmus). */
void handle_floor_last_closed(struct kobject *obj)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&obj->watched, __ATOMIC_ACQUIRE))
        epoll_last_handle_closed(obj);
}

void handle_floor_fatal(const char *what)
{
    panic("%s", what);
}
