/*
 * pipe_floor.c - The C under kernel/ipc/pipe.cee (roadmap 2.0 SA3).
 *
 * Pipes are CeeSafe (pipe.cee, generated into pipe.c and
 * kernel/include/kernel/pipe_types.h). What stays here is the anonymous
 * pipe's two end types: constant C objects holding pipe.cee's exported
 * operations, which CeeSafe cannot build (a static of a C type), with an
 * accessor each. It also checks the constants pipe.cee copies.
 */

#include <kernel/errno.h>
#include <kernel/object.h>
#include <kernel/pipe.h>

#include <uapi/cosmo/syscall.h>

_Static_assert(PIPE_SIZE == 16384u && PIPE_BUF == 4096u && COSMO_IO_READABLE == 1 && COSMO_IO_WRITABLE == 2 &&
                   COSMO_IO_HANGUP == 4 && COSMO_IO_ERROR == 8 && COSMO_DT_FIFO == 4 && EAGAIN == 11 && ENOMEM == 12 &&
                   EPIPE == 32,
               "pipe.cee's constants");

/* Declared for pipe.cee only; not part of pipe.h's interface. */
const struct kobject_type *pipe_floor_read_type(void);
const struct kobject_type *pipe_floor_write_type(void);

static const struct kobject_io_type pipe_read_type = {
    .base = { .name = "pipe-read", .release = pipe_read_end_release, .flags = KOBJECT_TYPE_IO },
    .read = pipe_end_read,
    .write = NULL,
    .stat = pipe_end_stat,
    .ready = pipe_end_read_ready,
    .set_nonblock = pipe_end_set_nonblock,
    .poll_wq = pipe_end_read_poll_wq,
};

static const struct kobject_io_type pipe_write_type = {
    .base = { .name = "pipe-write", .release = pipe_write_end_release, .flags = KOBJECT_TYPE_IO },
    .read = NULL,
    .write = pipe_end_write,
    .stat = pipe_end_stat,
    .ready = pipe_end_write_ready,
    .set_nonblock = pipe_end_set_nonblock,
    .poll_wq = pipe_end_write_poll_wq,
};

const struct kobject_type *pipe_floor_read_type(void)
{
    return &pipe_read_type.base;
}

const struct kobject_type *pipe_floor_write_type(void)
{
    return &pipe_write_type.base;
}
