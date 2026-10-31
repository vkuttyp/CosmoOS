/*
 * fifo_floor.c - The C under kernel/ipc/fifo.nova (roadmap 2.0 SA3).
 *
 * Named pipes are Nova (fifo.nova, generated into fifo.c and
 * kernel/include/kernel/fifo_types.h). What stays here is the formatted
 * panic, and a check of the constants fifo.nova copies.
 */

#include <kernel/errno.h>
#include <kernel/panic.h>

#include <uapi/cosmo/syscall.h>

_Static_assert(COSMO_O_RDONLY == 0x0000 && COSMO_O_WRONLY == 0x0001 && COSMO_O_ACCMODE == 0x0003 &&
                   COSMO_O_NONBLOCK == 0x0800 && COSMO_IO_READABLE == 1 && COSMO_IO_WRITABLE == 2 && ENOMEM == 12 &&
                   ENXIO == 6,
               "fifo.nova's constants");

/* Declared for fifo.nova only; not part of fifo.h's interface. */
void fifo_floor_panic_live(void);

void fifo_floor_panic_live(void)
{
    panic("fifo: freed with its ring live (an open outlived the node)");
}
