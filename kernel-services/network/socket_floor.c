/*
 * socket_floor.c - The C under kernel-services/network/socket.cee (roadmap
 * 2.0 SA6).
 *
 * Sockets are CeeSafe (socket.cee). What stays here is the socket's kobject
 * type, a constant C object holding socket.cee's exported operations, which
 * CeeSafe cannot build (a static of a C type), as pipe_floor.c keeps the
 * pipe's; with an accessor, and the type test that compares against it.
 */

#include <kernel/object.h>
#include <kernel/socket.h>

/* Declared for socket.cee only; not part of socket.h's interface. */
void socket_obj_release(struct kobject *obj);
int64_t socket_obj_read(struct kobject *obj, void *buf, size_t len);
int64_t socket_obj_write(struct kobject *obj, const void *buf, size_t len);
int socket_obj_stat(struct kobject *obj, struct cosmo_stat *st);
unsigned socket_obj_ready(struct kobject *obj);
int socket_obj_set_nonblock(struct kobject *obj, int on);
struct waitqueue *socket_obj_poll_wq(struct kobject *obj, unsigned events);
const struct kobject_type *socket_floor_type(void);

static const struct kobject_io_type socket_type = {
    .base = { .name = "socket", .release = socket_obj_release, .flags = KOBJECT_TYPE_IO },
    .read = socket_obj_read,
    .write = socket_obj_write,
    .stat = socket_obj_stat,
    .ready = socket_obj_ready,
    .set_nonblock = socket_obj_set_nonblock,
    .poll_wq = socket_obj_poll_wq,
};

const struct kobject_type *socket_floor_type(void)
{
    return &socket_type.base;
}

struct socket *socket_from_kobject(struct kobject *obj)
{
    return obj->type == &socket_type.base ? container_of(obj, struct socket, obj) : NULL;
}
