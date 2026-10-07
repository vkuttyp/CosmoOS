/*
 * epoll_musl - an event loop's assumption, made by a real libc program
 * (built with musl-gcc -static when available, as hello_musl is).
 *
 * Programs written for Linux close a descriptor and expect the kernel to
 * drop it from every epoll set it was in (epoll(7): "the file descriptor
 * is removed from all epoll sets automatically when it is closed",
 * keyed on the open file description, so a dup keeps it). Here that is
 * checked through musl's wrappers rather than raw system calls: a pipe's
 * reader registered and closed without EPOLL_CTL_DEL must leave the
 * writer with EPIPE (the reader really gone), a socket pair's end must
 * leave its peer at end-of-file, and a dup must keep the registration
 * until the last descriptor closes. Then a set in a set: the outer sees
 * the inner's member become readable, and a loop is ELOOP. Prints
 * `epoll musl: auto-removal ok` and exits 0, or names the first failed
 * step and exits 1.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("epoll musl: FAIL %s (errno %d: %s)\n", what, errno, strerror(errno));
    return 1;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);   /* the write to a reader-less pipe must return EPIPE, not kill us */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        return fail("epoll_create1");
    struct epoll_event ev = { .events = EPOLLIN }, out[4];

    /* A pipe's reader, registered, then closed: the writer sees EPIPE. */
    int p[2];
    if (pipe(p) != 0)
        return fail("pipe");
    ev.data.u64 = 1;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("epoll_ctl ADD pipe");
    if (close(p[0]) != 0)
        return fail("close pipe reader");
    if (write(p[1], "x", 1) != -1 || errno != EPIPE)
        return fail("write to the closed reader (expected EPIPE: the registration kept it alive)");
    if (epoll_wait(ep, out, 4, 0) != 0)
        return fail("epoll_wait after the pipe's close (expected nothing)");
    close(p[1]);

    /* A socket pair's end, registered, then closed: the peer reads EOF. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0)
        return fail("socketpair");
    ev.data.u64 = 2;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) != 0)
        return fail("epoll_ctl ADD socket");
    char c;
    if (read(sv[1], &c, 1) != -1 || errno != EAGAIN)
        return fail("read before the close (expected EAGAIN)");
    if (close(sv[0]) != 0)
        return fail("close socket end");
    if (read(sv[1], &c, 1) != 0)
        return fail("read after the close (expected end of file: the peer is closed)");
    close(sv[1]);

    /* A dup keeps the registration; the last descriptor's close removes it. */
    if (pipe(p) != 0)
        return fail("pipe 2");
    if (write(p[1], "y", 1) != 1)
        return fail("write 2");
    ev.data.u64 = 3;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("epoll_ctl ADD pipe 2");
    int d = dup(p[0]);
    if (d < 0)
        return fail("dup");
    if (close(p[0]) != 0)
        return fail("close original");
    if (epoll_wait(ep, out, 4, 0) != 1 || out[0].data.u64 != 3)
        return fail("epoll_wait with the dup open (expected the event)");
    if (close(d) != 0)
        return fail("close dup");
    if (epoll_wait(ep, out, 4, 0) != 0)
        return fail("epoll_wait after the last close (expected nothing)");
    if (epoll_ctl(ep, EPOLL_CTL_DEL, d, NULL) != -1 || (errno != EBADF && errno != ENOENT))
        return fail("DEL of the closed fd (expected EBADF or ENOENT)");
    close(p[1]);

    /* A set in a set (the epoll-callback unit): an outer set holding `ep`
     * sees `ep`'s member become readable, and a loop is ELOOP. */
    int outer = epoll_create1(0);
    if (outer < 0)
        return fail("epoll_create1 outer");
    ev.data.u64 = 4;
    if (epoll_ctl(outer, EPOLL_CTL_ADD, ep, &ev) != 0)
        return fail("epoll_ctl ADD of a set into a set");
    if (epoll_ctl(ep, EPOLL_CTL_ADD, outer, &ev) != -1 || errno != ELOOP)
        return fail("adding the outer into the inner (expected ELOOP)");
    if (pipe(p) != 0)
        return fail("pipe 3");
    ev.data.u64 = 5;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("epoll_ctl ADD pipe 3");
    if (epoll_wait(outer, out, 4, 0) != 0)
        return fail("outer before the write (expected nothing)");
    if (write(p[1], "z", 1) != 1)
        return fail("write 3");
    if (epoll_wait(outer, out, 4, 1000) != 1 || out[0].data.u64 != 4)
        return fail("outer after the write (expected the inner set, readable)");
    if (epoll_wait(ep, out, 4, 0) != 1 || out[0].data.u64 != 5)
        return fail("inner after the write (expected the pipe)");
    close(p[0]);
    close(p[1]);
    close(outer);
    close(ep);
    printf("epoll musl: auto-removal ok\n");
    return 0;
}
