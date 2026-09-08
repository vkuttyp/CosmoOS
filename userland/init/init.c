/*
 * init.c - PID 1 (docs/userland/design.md).
 *
 * Runs /etc/rc through the shell, then the console shell, and exits with
 * the shell's status when it ends (the kernel treats init's exit as the
 * end of the boot). With --selftest it exercises every native system
 * call through libc and reports USERTEST: PASS or FAIL; --crash faults on
 * purpose; --block reads the console and --spin loops, both so the kernel
 * self-test can kill them.
 */

#include <arpa/inet.h>
#include <cosmo/klog.h>
#include <cosmo/procinfo.h>
#include <cosmo/syscall.h>
#include <cosmo/sysctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures;

static int lists(const char *dir, const char *name);

static void check(int cond, const char *what)
{
    if (cond)
        return;
    g_failures++;
    fprintf(stderr, "USERTEST: check failed: %s (errno %d)\n", what, errno);
}

#define CHECK(c) check((c), #c)

/* Phase 7: the filesystem calls on ramfs, then the scratch disk. */
static void fs_selftest(void)
{
    struct cosmo_stat st;
    char buf[256];

    CHECK(cosmo_stat("/boot/init", &st) == 0 && st.type == COSMO_DT_REG && st.size > 1000);
    CHECK(cosmo_stat("/boot", &st) == 0 && st.type == COSMO_DT_DIR);
    CHECK(cosmo_stat("/nope", &st) == -COSMO_ENOENT);
    CHECK(cosmo_stat("/boot/init/x", &st) == -COSMO_ENOTDIR);
    CHECK(cosmo_stat("/bin/sh", &st) == 0 && st.type == COSMO_DT_REG && (st.mode & 0111));
    CHECK(cosmo_stat("/etc/rc", &st) == 0 && st.type == COSMO_DT_REG);

    long h = cosmo_open("/tmp/usertest.txt", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644);
    CHECK(h >= 3);
    CHECK(cosmo_write((int)h, "hello, filesystem\n", 18) == 18);
    CHECK(cosmo_fstat((int)h, &st) == 0 && st.size == 18 && st.type == COSMO_DT_REG);
    CHECK(cosmo_lseek((int)h, 0, COSMO_SEEK_SET) == 0);
    CHECK(cosmo_read((int)h, buf, sizeof(buf)) == 18 && memcmp(buf, "hello, filesystem\n", 18) == 0);
    CHECK(cosmo_read((int)h, buf, sizeof(buf)) == 0);
    CHECK(cosmo_lseek((int)h, 7, COSMO_SEEK_SET) == 7);
    CHECK(cosmo_read((int)h, buf, 10) == 10 && memcmp(buf, "filesystem", 10) == 0);
    CHECK(cosmo_lseek((int)h, -1, COSMO_SEEK_SET) == -COSMO_EINVAL);
    CHECK(cosmo_lseek((int)h, 0, COSMO_SEEK_END) == 18);
    CHECK(cosmo_close((int)h) == 0);
    CHECK(cosmo_close((int)h) == -COSMO_EBADF);

    h = cosmo_open("/tmp/usertest.txt", COSMO_O_RDONLY, 0);
    CHECK(h >= 3);
    CHECK(cosmo_write((int)h, "x", 1) == -COSMO_EBADF);
    CHECK(cosmo_read((int)h, buf, 5) == 5);
    CHECK(cosmo_close((int)h) == 0);
    CHECK(cosmo_open("/tmp/usertest.txt", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_EXCL, 0644) == -COSMO_EEXIST);
    CHECK(cosmo_open("/tmp/missing", COSMO_O_RDONLY, 0) == -COSMO_ENOENT);
    CHECK(cosmo_open("/tmp", COSMO_O_WRONLY, 0) == -COSMO_EISDIR);
    CHECK(cosmo_open((const char *)0x10, COSMO_O_RDONLY, 0) == -COSMO_EFAULT);

    CHECK(cosmo_mkdir("/tmp/d", 0755) == 0);
    CHECK(cosmo_mkdir("/tmp/d", 0755) == -COSMO_EEXIST);
    CHECK(cosmo_rename("/tmp/usertest.txt", "/tmp/d/moved.txt") == 0);
    CHECK(cosmo_stat("/tmp/usertest.txt", &st) == -COSMO_ENOENT);
    CHECK(cosmo_stat("/tmp/d/moved.txt", &st) == 0 && st.size == 18);
    /* The libc directory stream over getdents. */
    DIR *d = opendir("/tmp/d");
    CHECK(d != NULL);
    int seen = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, "moved.txt") == 0 && e->d_type == DT_REG)
                seen |= 1;
            if (strcmp(e->d_name, ".") == 0)
                seen |= 2;
            if (strcmp(e->d_name, "..") == 0)
                seen |= 4;
        }
        CHECK(closedir(d) == 0);
    }
    CHECK(seen == 7);
    CHECK(cosmo_rmdir("/tmp/d") == -COSMO_ENOTEMPTY);
    CHECK(cosmo_unlink("/tmp/d") == -COSMO_EISDIR);
    CHECK(cosmo_unlink("/tmp/d/moved.txt") == 0);
    CHECK(cosmo_rmdir("/tmp/d") == 0);
    CHECK(cosmo_stat("/tmp/d", &st) == -COSMO_ENOENT);
    CHECK(cosmo_sync() == 0);

    /* stdio on a file. */
    FILE *f = fopen("/tmp/stdio.txt", "w");
    CHECK(f != NULL);
    if (f) {
        CHECK(fprintf(f, "line %d\n%s\n", 1, "line two") == 16);
        CHECK(fclose(f) == 0);
        f = fopen("/tmp/stdio.txt", "r");
        CHECK(f != NULL);
        CHECK(fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "line 1\n") == 0);
        CHECK(fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "line two\n") == 0);
        CHECK(fgets(buf, sizeof(buf), f) == NULL && feof(f));
        /* A relative seek counts from the logical position, not the
         * descriptor's (which sits past the buffered input). */
        rewind(f);
        CHECK(fgetc(f) == 'l' && fgetc(f) == 'i');
        CHECK(fseek(f, 2, SEEK_CUR) == 0 && fgetc(f) == ' ' && ftell(f) == 5);
        CHECK(fclose(f) == 0);
        CHECK(unlink("/tmp/stdio.txt") == 0);
    }

    long m = cosmo_mount("vda", "/mnt", "cosmofs", 0);
    if (m == 0) {
        h = cosmo_open("/mnt/hello.txt", COSMO_O_RDONLY, 0);
        CHECK(h >= 3);
        long n = cosmo_read((int)h, buf, sizeof(buf));
        CHECK(n == 21 && memcmp(buf, "hello from the kernel", 21) == 0);
        CHECK(cosmo_close((int)h) == 0);
        CHECK(cosmo_stat("/mnt/dir/nested.txt", &st) == 0 && st.type == COSMO_DT_REG);
        CHECK(cosmo_umount("/mnt") == 0);
        CHECK(cosmo_stat("/mnt/hello.txt", &st) == -COSMO_ENOENT);
        puts("usertest: cosmofs mounted and read from user mode");
    } else {
        CHECK(m == -COSMO_ENODEV || m == -COSMO_EIO);
        puts("usertest: no cosmofs to mount");
    }
    CHECK(cosmo_umount("/") == -COSMO_EBUSY);
}

/* Phase 8: sockets over loopback from user mode (libc names). */
static void net_selftest(void)
{
    struct sockaddr me, peer;
    socklen_t plen = sizeof(peer);
    char buf[64];

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u >= 3);
    memset(&me, 0, sizeof(me));
    me.sa_family = AF_INET;
    me.sa_port = 40000;
    CHECK(inet_pton(AF_INET, "127.0.0.1", me.sa_addr) == 1 && me.sa_addr[0] == 127 && me.sa_addr[3] == 1);
    CHECK(bind(u, &me, sizeof(me)) == 0);
    CHECK(bind(u, &me, sizeof(me)) < 0 && errno == EINVAL);
    CHECK(sendto(u, "ping", 4, 0, &me, sizeof(me)) == 4);
    CHECK(recvfrom(u, buf, sizeof(buf), 0, &peer, &plen) == 4 && memcmp(buf, "ping", 4) == 0);
    CHECK(peer.sa_family == AF_INET && peer.sa_port == 40000 && peer.sa_addr[0] == 127);
    CHECK(getsockname(u, &peer, &plen) == 0 && peer.sa_port == 40000);
    CHECK(connect(u, &me, sizeof(me)) == 0);
    CHECK(write(u, "pong", 4) == 4);
    CHECK(read(u, buf, sizeof(buf)) == 4 && memcmp(buf, "pong", 4) == 0);
    CHECK(close(u) == 0);

    int t = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(t >= 3);
    me.sa_port = 5999;
    CHECK(connect(t, &me, sizeof(me)) < 0 && errno == ECONNREFUSED);
    CHECK(close(t) == 0);
    t = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(t >= 3);
    me.sa_port = 80;
    CHECK(bind(t, &me, sizeof(me)) == 0);
    CHECK(listen(t, 4) == 0);
    CHECK(listen(t, 4) < 0 && errno == EINVAL);
    CHECK(send(t, "x", 1, 0) < 0 && errno == ENOTCONN);
    CHECK(close(t) == 0);
    /*
     * Per-type rights (docs/kernel/object/architecture.md, "The upper
     * sixteen bits", S15). Each copy drops exactly one, so what fails
     * names the right that was removed and nothing else. Before this,
     * bind, listen, connect and shutdown asked for no right at all.
     */
    {
        int full = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(full >= 3);
        struct sockaddr where = me;
        where.sa_port = 5998;

        /* No BIND: cannot name itself, and cannot listen either --
         * one right covers both halves of becoming a listener. */
        int no_bind = dup_rights(full, -1,
                                 COSMO_RIGHT_READ | COSMO_RIGHT_WRITE | COSMO_RIGHT_DUP |
                                     COSMO_RIGHT_SOCK_ACCEPT | COSMO_RIGHT_SOCK_CONNECT |
                                     COSMO_RIGHT_SOCK_SHUTDOWN);
        CHECK(no_bind >= 0);
        CHECK(bind(no_bind, &where, sizeof(where)) < 0 && errno == EPERM);
        CHECK(listen(no_bind, 4) < 0 && errno == EPERM);
        CHECK(close(no_bind) == 0);

        /* No CONNECT: reaching a peer is refused, and refused with
         * EPERM rather than the ECONNREFUSED the same call gets with
         * the right -- the difference between "not allowed to try" and
         * "tried and failed". */
        int no_conn = dup_rights(full, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE | COSMO_RIGHT_DUP);
        CHECK(no_conn >= 0);
        CHECK(connect(no_conn, &where, sizeof(where)) < 0 && errno == EPERM);
        CHECK(close(no_conn) == 0);

        /* The full handle still does both, so the refusals above are
         * the rights and not the socket. */
        CHECK(bind(full, &where, sizeof(where)) == 0);
        CHECK(listen(full, 4) == 0);

        /* No ACCEPT on a listener that is listening. */
        int no_acc = dup_rights(full, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE | COSMO_RIGHT_DUP);
        CHECK(no_acc >= 0);
        struct sockaddr from;
        socklen_t fromlen = sizeof(from);
        CHECK(accept(no_acc, &from, &fromlen) < 0 && errno == EPERM);
        CHECK(close(no_acc) == 0);

        /* No SHUTDOWN: the operation that ends a connection is its own
         * authority, which is what a read-only handle used to be able
         * to do to a socket it was merely lent. */
        int no_sd = dup_rights(full, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE | COSMO_RIGHT_DUP);
        CHECK(no_sd >= 0);
        CHECK(shutdown(no_sd, SHUT_RDWR) < 0 && errno == EPERM);
        CHECK(close(no_sd) == 0);

        /*
         * And the same through the Linux ABI, because a handle is a
         * capability whatever language asks about it. The child is a
         * freestanding Linux program given the socket on fd 3 with
         * CONNECT and SHUTDOWN removed; it must be refused there too,
         * or the restriction lasts only until the holder switches ABI.
         */
        int lent = dup_rights(full, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE | COSMO_RIGHT_TRANSFER);
        CHECK(lent >= 0);
        struct spawn_handle lxmap[] = { { .child = 0, .parent = 0 },
                                        { .child = 1, .parent = 1 },
                                        { .child = 2, .parent = 2 },
                                        { .child = 3, .parent = lent } };
        static const char *const lxr_argv[] = { "lxrights", "acs", NULL };
        pid_t lxp = spawnve("/boot/tests/linux/lxrights", lxr_argv, NULL, lxmap, 4);
        CHECK(lxp > 1);
        int lxst = -1;
        CHECK(waitpid(lxp, &lxst, 0) == lxp);
        CHECK(lxst == 0);
        CHECK(close(lent) == 0);
        CHECK(close(full) == 0);
    }
    CHECK(socket(99, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);
    CHECK(socket(AF_INET, 7, 0) < 0 && errno == EINVAL);
    CHECK(bind(1, &me, sizeof(me)) < 0 && errno == EBADF);
    t = socket(AF_INET, SOCK_DGRAM, 0);
    struct stat sst;
    CHECK(t >= 3 && fstat(t, &sst) == 0 && S_ISSOCK(sst.st_type) && close(t) == 0);
    char text[INET6_ADDRSTRLEN];
    uint8_t v6[16];
    CHECK(inet_pton(AF_INET6, "fe80::1", v6) == 1 && v6[0] == 0xfe && v6[15] == 1);
    uint8_t v6b[16];
    CHECK(inet_pton(AF_INET6, "1::2:3:4:5:6:7:8", v6b) == 0);   /* "::" with eight groups */
    CHECK(inet_pton(AF_INET6, "1:2:3:4:5:6:7", v6b) == 0 && inet_pton(AF_INET6, "1:2:3:4:5:6:7:8", v6b) == 1);
    /* A short address buffer receives a prefix and learns the full size. */
    {
        int g = socket(AF_INET, SOCK_DGRAM, 0);
        struct { struct sockaddr sa; uint32_t canary; } box;
        memset(&box, 0xee, sizeof(box));
        socklen_t sl = 4;
        CHECK(g >= 3 && getsockname(g, &box.sa, &sl) == 0 && sl == sizeof(struct sockaddr));
        CHECK(box.sa.sa_family == AF_INET && box.sa.sa_addr[0] == 0xee && box.canary == 0xeeeeeeeeu);
        close(g);
    }
    CHECK(inet_ntop(AF_INET6, v6, text, sizeof(text)) != NULL && strcmp(text, "fe80::1") == 0);
    CHECK(inet_ntop(AF_INET, me.sa_addr, text, sizeof(text)) != NULL && strcmp(text, "127.0.0.1") == 0);

    /* Non-blocking sockets and readiness (milestone 8). */
    {
        int n = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        CHECK(n >= 3);
        me.sa_port = 40001;
        CHECK(bind(n, &me, sizeof(me)) == 0);
        CHECK(recvfrom(n, buf, sizeof(buf), 0, NULL, NULL) < 0 && errno == EAGAIN);
        CHECK(cosmo_ioready(n) == COSMO_IO_WRITABLE);
        CHECK(sendto(n, "x", 1, 0, &me, sizeof(me)) == 1);
        for (int i = 0; i < 100 && !(cosmo_ioready(n) & COSMO_IO_READABLE); i++)
            cosmo_sleep_ns(1000000);
        CHECK((cosmo_ioready(n) & COSMO_IO_READABLE) && recv(n, buf, sizeof(buf), 0) == 1);
        CHECK(cosmo_setnonblock(n, 0) == 0 && cosmo_setnonblock(n, 1) == 0);
        CHECK(close(n) == 0);
        int l = socket(AF_INET, SOCK_STREAM, 0);
        me.sa_port = 40002;
        CHECK(l >= 3 && bind(l, &me, sizeof(me)) == 0 && listen(l, 2) == 0);
        CHECK(cosmo_setnonblock(l, 1) == 0);
        CHECK(accept(l, NULL, NULL) < 0 && errno == EAGAIN);
        CHECK(cosmo_ioready(l) == 0);
        int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        CHECK(c >= 3);
        int rc = connect(c, &me, sizeof(me));
        CHECK(rc == 0 || (rc < 0 && errno == EINPROGRESS));
        for (int i = 0; i < 200 && !(cosmo_ioready(c) & COSMO_IO_WRITABLE); i++)
            cosmo_sleep_ns(1000000);
        CHECK(cosmo_ioready(c) & COSMO_IO_WRITABLE);
        CHECK(connect(c, &me, sizeof(me)) < 0 && errno == EISCONN);
        CHECK(recv(c, buf, sizeof(buf), 0) < 0 && errno == EAGAIN);
        for (int i = 0; i < 200 && !(cosmo_ioready(l) & COSMO_IO_READABLE); i++)
            cosmo_sleep_ns(1000000);
        int a = accept(l, &peer, &plen);
        CHECK(a >= 3 && send(a, "ok", 2, 0) == 2);
        for (int i = 0; i < 200 && !(cosmo_ioready(c) & COSMO_IO_READABLE); i++)
            cosmo_sleep_ns(1000000);
        CHECK(recv(c, buf, sizeof(buf), 0) == 2 && buf[0] == 'o');
        CHECK(close(a) == 0);
        for (int i = 0; i < 200 && !(cosmo_ioready(c) & COSMO_IO_HANGUP); i++)
            cosmo_sleep_ns(1000000);
        CHECK((cosmo_ioready(c) & COSMO_IO_HANGUP) && recv(c, buf, sizeof(buf), 0) == 0);
        CHECK(close(c) == 0 && close(l) == 0);
        int refused = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        me.sa_port = 5998;
        rc = connect(refused, &me, sizeof(me));
        CHECK(rc < 0 && (errno == EINPROGRESS || errno == ECONNREFUSED));
        for (int i = 0; i < 200 && !(cosmo_ioready(refused) & COSMO_IO_ERROR); i++)
            cosmo_sleep_ns(1000000);
        CHECK(connect(refused, &me, sizeof(me)) < 0 && errno == ECONNREFUSED);
        CHECK(close(refused) == 0);
        CHECK(cosmo_ioready(0) & COSMO_IO_WRITABLE);           /* the console */
        CHECK(cosmo_setnonblock(0, 1) == -COSMO_EOPNOTSUPP);   /* it never blocks a writer */
        CHECK(cosmo_ioready(999) == -COSMO_EBADF);
    }
    puts("usertest: sockets ok");
}

/* Phase 9: processes, pipes, handles, cwd, introspection. */
static void proc_selftest(void)
{
    char buf[256];
    struct stat st;

    /* Pipes and dup. */
    int p[2];
    CHECK(pipe(p) == 0 && p[0] >= 3 && p[1] >= 3 && p[0] != p[1]);
    CHECK(write(p[1], "abc", 3) == 3);
    CHECK(fstat(p[0], &st) == 0 && S_ISFIFO(st.st_type) && st.st_size == 3);
    CHECK(read(p[0], buf, sizeof(buf)) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(read(p[1], buf, 1) < 0 && errno == EBADF);    /* the write end cannot read */
    CHECK(write(p[0], "x", 1) < 0 && errno == EBADF);
    int d = dup(p[1]);
    CHECK(d >= 3 && d != p[1]);
    CHECK(close(p[1]) == 0);
    CHECK(write(d, "z", 1) == 1);                       /* the copy keeps the end alive */
    CHECK(dup2(d, 40) == 40 && write(40, "y", 1) == 1);
    CHECK(dup2(d, 64) < 0 && errno == EINVAL);
    CHECK(close(d) == 0 && close(40) == 0);
    CHECK(read(p[0], buf, sizeof(buf)) == 2 && memcmp(buf, "zy", 2) == 0);
    CHECK(read(p[0], buf, sizeof(buf)) == 0);           /* EOF: every write end is gone */
    CHECK(close(p[0]) == 0);
    CHECK(pipe(p) == 0);
    CHECK(close(p[0]) == 0);
    CHECK(write(p[1], "x", 1) < 0 && errno == EPIPE);
    CHECK(close(p[1]) == 0);
    /* Non-blocking ends and readiness. */
    CHECK(pipe(p) == 0 && cosmo_setnonblock(p[0], 1) == 0 && cosmo_setnonblock(p[1], 1) == 0);
    CHECK(read(p[0], buf, 1) < 0 && errno == EAGAIN);
    CHECK(cosmo_ioready(p[0]) == 0 && cosmo_ioready(p[1]) == COSMO_IO_WRITABLE);
    CHECK(write(p[1], "q", 1) == 1 && (cosmo_ioready(p[0]) & COSMO_IO_READABLE));
    CHECK(read(p[0], buf, 1) == 1 && buf[0] == 'q');
    CHECK(close(p[1]) == 0 && (cosmo_ioready(p[0]) & COSMO_IO_HANGUP) && read(p[0], buf, 1) == 0);
    CHECK(close(p[0]) == 0);

    /* The asynchronous I/O ring (milestone 9). */
    {
        CHECK(cosmo_aio_create(0, 0) == -COSMO_EINVAL && cosmo_aio_create(8, 1) == -COSMO_EINVAL);
        int ring = (int)cosmo_aio_create(8, 0);
        CHECK(ring >= 3);
        CHECK(pipe(p) == 0);
        char rbuf[16] = { 0 };
        struct cosmo_sqe sq[4];
        struct cosmo_cqe cq[8];
        memset(sq, 0, sizeof(sq));
        /* A read of an empty pipe parks; a NOP completes at once; a bad handle completes with -EBADF. */
        sq[0] = (struct cosmo_sqe){ .op = COSMO_AIO_READ, .handle = p[0], .addr = (uint64_t)rbuf, .len = 8, .user_data = 1 };
        sq[1] = (struct cosmo_sqe){ .op = COSMO_AIO_NOP, .user_data = 2 };
        sq[2] = (struct cosmo_sqe){ .op = COSMO_AIO_READ, .handle = 999, .addr = (uint64_t)rbuf, .len = 8, .user_data = 3 };
        sq[3] = (struct cosmo_sqe){ .op = COSMO_AIO_READ, .flags = COSMO_AIO_F_NOWAIT, .handle = p[0],
                                    .addr = (uint64_t)rbuf, .len = 8, .user_data = 4 };
        CHECK(cosmo_aio_submit(ring, sq, 4) == 4);
        CHECK(cosmo_ioready(ring) & COSMO_IO_READABLE);
        long got = cosmo_aio_wait(ring, cq, 8, 0, 0);   /* poll: three completions, the read still parked */
        CHECK(got == 3);
        int seen_nop = 0, seen_bad = 0, seen_nowait = 0;
        for (long i = 0; i < got; i++) {
            if (cq[i].user_data == 2 && cq[i].result == 0) seen_nop++;
            if (cq[i].user_data == 3 && cq[i].result == -COSMO_EBADF) seen_bad++;
            if (cq[i].user_data == 4 && cq[i].result == -COSMO_EAGAIN) seen_nowait++;
        }
        CHECK(seen_nop == 1 && seen_bad == 1 && seen_nowait == 1);
        CHECK(cosmo_ioready(ring) == 0);
        CHECK(cosmo_aio_wait(ring, cq, 8, 1, 20000000) == 0);   /* 20 ms: nothing completes */
        CHECK(write(p[1], "ringdata", 8) == 8);                  /* now the parked read can run */
        got = cosmo_aio_wait(ring, cq, 8, 1, COSMO_AIO_WAIT_FOREVER);
        CHECK(got == 1 && cq[0].user_data == 1 && cq[0].result == 8 && memcmp(rbuf, "ringdata", 8) == 0);
        /* POLL: not ready parks, then completes with the bit when data arrives; a write completes at once. */
        sq[0] = (struct cosmo_sqe){ .op = COSMO_AIO_POLL, .handle = p[0], .events = COSMO_IO_READABLE, .user_data = 5 };
        sq[1] = (struct cosmo_sqe){ .op = COSMO_AIO_WRITE, .handle = p[1], .addr = (uint64_t)"xy", .len = 2, .user_data = 6 };
        CHECK(cosmo_aio_submit(ring, sq, 2) == 2);
        got = cosmo_aio_wait(ring, cq, 8, 2, 1000000000ull);
        CHECK(got == 2);
        for (long i = 0; i < got; i++) {
            if (cq[i].user_data == 5) CHECK(cq[i].result == COSMO_IO_READABLE);
            if (cq[i].user_data == 6) CHECK(cq[i].result == 2);
        }
        CHECK(read(p[0], rbuf, 2) == 2);
        /* Files: PREAD at an offset and FSYNC complete at submission. */
        int fh = open("/tmp/aio.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
        CHECK(fh >= 3 && write(fh, "0123456789", 10) == 10);
        memset(rbuf, 0, sizeof(rbuf));
        sq[0] = (struct cosmo_sqe){ .op = COSMO_AIO_PREAD, .handle = fh, .addr = (uint64_t)rbuf, .len = 4, .offset = 6,
                                    .user_data = 7 };
        sq[1] = (struct cosmo_sqe){ .op = COSMO_AIO_FSYNC, .handle = fh, .user_data = 8 };
        sq[2] = (struct cosmo_sqe){ .op = COSMO_AIO_PWRITE, .handle = fh, .addr = (uint64_t)"ZZ", .len = 2, .offset = 0,
                                    .user_data = 9 };
        CHECK(cosmo_aio_submit(ring, sq, 3) == 3);
        got = cosmo_aio_wait(ring, cq, 8, 3, 0);
        CHECK(got == 3);
        for (long i = 0; i < got; i++) {
            if (cq[i].user_data == 7) CHECK(cq[i].result == 4 && memcmp(rbuf, "6789", 4) == 0);
            if (cq[i].user_data == 8) CHECK(cq[i].result == 0);
            if (cq[i].user_data == 9) CHECK(cq[i].result == 2);
        }
        CHECK(lseek(fh, 0, SEEK_SET) == 0 && read(fh, rbuf, 2) == 2 && memcmp(rbuf, "ZZ", 2) == 0);
        CHECK(close(fh) == 0 && unlink("/tmp/aio.txt") == 0);
        /* Capacity: nine parked reads on an 8-entry ring: the ninth is refused. */
        struct cosmo_sqe many[9];
        for (int i = 0; i < 9; i++)
            many[i] = (struct cosmo_sqe){ .op = COSMO_AIO_READ, .handle = p[0], .addr = (uint64_t)rbuf, .len = 1,
                                          .user_data = 100 + (uint64_t)i };
        CHECK(cosmo_aio_submit(ring, many, 9) == 8);
        CHECK(cosmo_aio_submit(ring, many, 1) == -COSMO_EBUSY);
        CHECK(cosmo_aio_wait(ring, cq, 9, 0, 0) == 0);
        CHECK(cosmo_aio_wait(ring, cq, 2, 3, 0) == -COSMO_EINVAL);
        CHECK(close(ring) == 0);   /* drops the parked reads */
        CHECK(close(p[0]) == 0 && close(p[1]) == 0);
        CHECK(cosmo_aio_submit(999, sq, 1) == -COSMO_EBADF && cosmo_aio_submit(p[0], sq, 1) == -COSMO_EBADF);
    }

    /* The console: a character device, a terminal. */
    CHECK(fstat(0, &st) == 0 && S_ISCHR(st.st_type));
    CHECK(isatty(0) == 1);
    CHECK(fstat(7, &st) < 0 && errno == EBADF);

    /* spawn: a child's output through a pipe, its status through wait. */
    CHECK(pipe(p) == 0);
    struct spawn_handle map[] = { { .child = 1, .parent = p[1] }, { .child = 2, .parent = 2 } };
    const char *echo_argv[] = { "echo", "spawned", "child", NULL };
    pid_t pid = spawnvp("echo", echo_argv, map, 2);
    CHECK(pid > 1);
    CHECK(close(p[1]) == 0);
    ssize_t n = read(p[0], buf, sizeof(buf));
    CHECK(n == 14 && memcmp(buf, "spawned child\n", 14) == 0);
    CHECK(read(p[0], buf, sizeof(buf)) == 0);
    CHECK(close(p[0]) == 0);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && status == 0);
    CHECK(waitpid(pid, &status, 0) < 0 && errno == ECHILD);   /* reaped already */

    /* Exit status, environment and cwd inheritance. */
    const char *sh_argv[] = { "sh", "-c", "cd /tmp && pwd && exit 7", NULL };
    CHECK(pipe(p) == 0);
    map[0].parent = p[1];
    pid = spawnvp("sh", sh_argv, map, 2);
    CHECK(pid > 1);
    close(p[1]);
    n = read(p[0], buf, sizeof(buf));
    CHECK(n == 5 && memcmp(buf, "/tmp\n", 5) == 0);
    close(p[0]);
    CHECK(waitpid(pid, &status, 0) == pid && status == 7);
    CHECK(getcwd(buf, sizeof(buf)) != NULL && strcmp(buf, "/") == 0);   /* the child's cd was its own */

    /* Kill: a child blocked on a pipe read dies with 128 + SIGKILL. */
    CHECK(pipe(p) == 0);
    struct spawn_handle in_map[] = { { .child = 0, .parent = p[0] }, { .child = 1, .parent = 1 },
                                     { .child = 2, .parent = 2 } };
    const char *cat_argv[] = { "cat", NULL };
    pid = spawnvp("cat", cat_argv, in_map, 3);
    CHECK(pid > 1);
    close(p[0]);
    CHECK(waitpid(pid, &status, WNOHANG) == 0);          /* still running */
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && status == 128 + SIGKILL);
    close(p[1]);
    CHECK(kill(999999, SIGTERM) < 0 && errno == ESRCH);
    /* Signal 0 sends nothing and reports whether the target is there:
     * a reaped pid is gone, this process is not, and a pid nobody has
     * is ESRCH. It is how a supervisor tells a live process from a
     * stale pid file. */
    CHECK(kill(pid, 0) < 0 && errno == ESRCH);   /* just reaped */
    /* That it says so the *instant* `waitpid` returns, rather than once
     * the kernel has released the object, is not something a user
     * program can test without racing: see `process-reaped` in
     * kernel/process/proctest.c, which holds the object alive on
     * purpose. */
    CHECK(kill(getpid(), 0) == 0);
    CHECK(kill(999999, 0) < 0 && errno == ESRCH);
    CHECK(kill(getpid(), -1) < 0 && errno == EINVAL);
    CHECK(kill(getpid(), 32) < 0 && errno == EINVAL);

    /* Hostile spawn requests. */
    const char *true_argv[] = { "true", NULL };
    struct spawn_handle bad_parent[] = { { .child = 0, .parent = 63 } };
    CHECK(spawnve("/bin/true", true_argv, NULL, bad_parent, 1) < 0 && errno == EBADF);
    struct spawn_handle dup_child[] = { { .child = 0, .parent = 0 }, { .child = 0, .parent = 1 } };
    CHECK(spawnve("/bin/true", true_argv, NULL, dup_child, 2) < 0 && errno == EINVAL);
    CHECK(spawnve("/etc/rc", true_argv, NULL, NULL, 0) < 0 && errno == EACCES);   /* not executable */
    CHECK(spawnve("/bin", true_argv, NULL, NULL, 0) < 0 && errno == EACCES);      /* a directory */
    CHECK(spawnve("/bin/nothere", true_argv, NULL, NULL, 0) < 0 && errno == ENOENT);
    CHECK(spawnvp("nothere", true_argv, NULL, 0) < 0 && errno == ENOENT);

    /* Per-process roots: a child given a root cannot name its way out.
     * The executable is found in the *caller's* namespace, so /bin/sh
     * need not exist inside the jail -- what the child cannot do is
     * reach outside it afterwards. */
    CHECK(mkdir("/tmp/jail", 0755) == 0 || errno == EEXIST);
    int jf = open("/tmp/jail/inside.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(jf >= 0 && write(jf, "inside\n", 7) == 7 && close(jf) == 0);

    int jp[2];
    CHECK(pipe(jp) == 0);
    struct spawn_handle jmap[] = { { .child = 1, .parent = jp[1] }, { .child = 2, .parent = 2 } };
    /* "cd .." at the root stays at the root, and a write to an absolute
     * path lands inside the jail. Only shell builtins are used: an
     * external command would have to be found in the child's namespace,
     * and /bin does not exist in there -- which is the confinement
     * working, not a limitation of the test. */
    /* Note what this does *not* do first: no "cd /". A rooted child
     * starts at its own root, so if it inherited the parent's working
     * directory it would be standing outside its root and every
     * relative path from there would reach outside -- the confinement
     * bypassed by doing nothing at all. */
    const char *jail_argv[] = { "sh", "-c", "pwd && cd .. && pwd > /made.txt", NULL };
    pid_t jpid = spawnve_in("/bin/sh", jail_argv, NULL, jmap, 2, "/tmp/jail");
    CHECK(jpid > 1);
    CHECK(close(jp[1]) == 0);
    ssize_t jn = read(jp[0], buf, sizeof(buf));
    CHECK(jn == 2 && memcmp(buf, "/\n", 2) == 0);   /* not /tmp/jail */
    CHECK(close(jp[0]) == 0);
    int jstatus = -1;
    CHECK(waitpid(jpid, &jstatus, 0) == jpid && jstatus == 0);
    /* The child's "/made.txt" is this process's /tmp/jail/made.txt. */
    int mf = open("/tmp/jail/made.txt", O_RDONLY, 0);
    CHECK(mf >= 0);
    char mb[16] = { 0 };
    CHECK(read(mf, mb, sizeof(mb)) == 2 && memcmp(mb, "/\n", 2) == 0);
    CHECK(close(mf) == 0);

    /* A root that is itself a mounted filesystem. Leaving a mount
     * through ".." replaces the mount's root vnode with the covered
     * vnode underneath it -- a different vnode, no longer equal to the
     * process root -- so the boundary has to be tested before that step
     * and not after. Without it a child rooted at a mount walks straight
     * out of the mount it was confined to. */
    CHECK(mkdir("/tmp/mjail", 0755) == 0 || errno == EEXIST);
    CHECK(cosmo_mount("none", "/tmp/mjail", "ramfs", 0) == 0);
    int mp[2];
    CHECK(pipe(mp) == 0);
    struct spawn_handle mmap_[] = { { .child = 1, .parent = mp[1] }, { .child = 2, .parent = 2 } };
    /* "pwd" alone cannot tell the two apart -- escaping to the real
     * root also prints "/" -- so the child then tries to enter a
     * directory that exists only outside the jail. Confined, there is
     * no /etc and the cd fails; escaped, it succeeds. */
    const char *mount_argv[] = { "sh", "-c", "cd .. && cd .. && pwd && cd etc", NULL };
    pid_t mpid = spawnve_in("/bin/sh", mount_argv, NULL, mmap_, 2, "/tmp/mjail");
    CHECK(mpid > 1);
    CHECK(close(mp[1]) == 0);
    ssize_t mn = read(mp[0], buf, sizeof(buf));
    CHECK(mn == 2 && memcmp(buf, "/\n", 2) == 0);   /* not /tmp or / of the real tree */
    CHECK(close(mp[0]) == 0);
    /* Nonzero: the last command must have failed, because /etc exists
     * only outside the jail. A child that escaped would find it and
     * exit 0. */
    int mstatus = 0;
    CHECK(waitpid(mpid, &mstatus, 0) == mpid && mstatus != 0);

    /* A relative path from where the child starts cannot reach outside
     * either: ../../etc is the same directory as /etc from a root, and
     * neither exists in there. */
    const char *rel_argv[] = { "sh", "-c", "cd ../../etc", NULL };
    pid_t rpid = spawnve_in("/bin/sh", rel_argv, NULL, NULL, 0, "/tmp/jail");
    CHECK(rpid > 1);
    int rstatus = 0;
    CHECK(waitpid(rpid, &rstatus, 0) == rpid && rstatus != 0);

    /* A root and a working directory are not offered together: the cwd
     * would have to be resolved in the child's namespace to know it is
     * inside the root, and spawn resolves paths in the caller's. */
    static const char *const true_only[] = { "sh", "-c", "exit 0", NULL };
    struct cosmo_spawn both = {
        .path = "/bin/sh",
        .argv = true_only,
        .envp = NULL,
        .cwd = "/tmp",
        .flags = COSMO_SPAWN_HANDLE_RIGHTS | COSMO_SPAWN_SETROOT,
        .root = "/tmp/jail",
    };
    CHECK(cosmo_spawn(&both) == -EINVAL);

    /* Nothing outside the root is nameable: /etc exists here and not
     * there, so the shell's cd fails and it exits nonzero. */
    const char *escape_argv[] = { "sh", "-c", "cd /etc", NULL };
    pid_t epid = spawnve_in("/bin/sh", escape_argv, NULL, NULL, 0, "/tmp/jail");
    CHECK(epid > 1);
    int estatus = 0;
    CHECK(waitpid(epid, &estatus, 0) == epid && estatus != 0);

    /* Process domains: a child in one sees only its own domain, and
     * cannot signal out of it. Run ps inside a domain and count what it
     * reports -- init is pid 1 and always exists, so its absence from
     * the listing is the property under test. */
    int dp[2];
    CHECK(pipe(dp) == 0);
    /* All three: the shell spawns ps, and a child it cannot give a
     * standard input to fails with EBADF before it starts. */
    struct spawn_handle dmap[] = { { .child = 0, .parent = 0 },
                                   { .child = 1, .parent = dp[1] },
                                   { .child = 2, .parent = 2 } };
    const char *ps_argv[] = { "sh", "-c", "ps", NULL };
    pid_t dpid = spawnve_domain("/bin/sh", ps_argv, NULL, dmap, 3);
    CHECK(dpid > 1);
    CHECK(close(dp[1]) == 0);
    static char psout[2048];
    size_t got = 0;
    for (;;) {
        ssize_t chunk = read(dp[0], psout + got, sizeof(psout) - 1 - got);
        if (chunk <= 0)
            break;
        got += (size_t)chunk;
    }
    psout[got] = '\0';
    CHECK(close(dp[0]) == 0);
    int dstatus = -1;
    CHECK(waitpid(dpid, &dstatus, 0) == dpid && dstatus == 0);
    /* Its own shell and ps are there; init is not. */
    CHECK(strstr(psout, "sh") != NULL);
    CHECK(strstr(psout, "init") == NULL);

    /* And it cannot signal out of its domain: pid 1 exists, and inside
     * the domain it must look as though it does not. */
    /* And /proc obeys the domain too, since it asks the same question:
     * pid 1 certainly exists and must not be readable from in here. */
    const char *dproc_argv[] = { "sh", "-c", "cat /proc/1/status", NULL };
    pid_t dpp = spawnve_domain("/bin/sh", dproc_argv, NULL, NULL, 0);
    CHECK(dpp > 1);
    int dpstatus = 0;
    CHECK(waitpid(dpp, &dpstatus, 0) == dpp && dpstatus != 0);

    const char *kill_argv[] = { "sh", "-c", "kill 1", NULL };
    pid_t kpid = spawnve_domain("/bin/sh", kill_argv, NULL, NULL, 0);
    CHECK(kpid > 1);
    int kstatus = 0;
    CHECK(waitpid(kpid, &kstatus, 0) == kpid && kstatus != 0);

    /*
     * Mount namespaces (docs/kernel/security/design.md §1d, S12). Both
     * directions, because a filter that is only checked one way can be
     * a filter that copies too much or one that copies too little.
     *
     * The directory gets a file first: it is the underlying directory
     * that shows through wherever the ramfs is *not* mounted, so
     * whether "nsfile" is listed says which side of the split the
     * listing came from.
     */
    CHECK(mkdir("/tmp/nsm", 0755) == 0 || errno == EEXIST);
    int nsf = open("/tmp/nsm/nsfile", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(nsf >= 0 && close(nsf) == 0);

    /* One: what the child mounts, the parent does not see. */
    int np[2];
    CHECK(pipe(np) == 0);
    struct spawn_handle nmap[] = { { .child = 0, .parent = 0 },
                                   { .child = 1, .parent = np[1] },
                                   { .child = 2, .parent = 2 } };
    const char *ns_argv[] = { "sh", "-c", "mount none /tmp/nsm ramfs && ls /tmp/nsm", NULL };
    pid_t npid = spawnve_mountns("/bin/sh", ns_argv, NULL, nmap, 3);
    CHECK(npid > 1);
    CHECK(close(np[1]) == 0);
    ssize_t nn = read(np[0], buf, sizeof(buf) - 1);
    CHECK(nn >= 0);
    buf[nn] = 0;
    CHECK(close(np[0]) == 0);
    int nstatus = 0;
    CHECK(waitpid(npid, &nstatus, 0) == npid && nstatus == 0);
    /* The child listed its own empty ramfs, not the directory under it. */
    CHECK(strstr(buf, "nsfile") == NULL);
    /* And here the mount never happened: the file is still listed, and
     * the directory is free to mount on, which it would not be if the
     * child's mount were in the way. */
    struct stat nst;
    CHECK(stat("/tmp/nsm/nsfile", &nst) == 0);
    CHECK(cosmo_mount("none", "/tmp/nsm", "ramfs", 0) == 0);
    CHECK(cosmo_umount("/tmp/nsm") == 0);

    /*
     * Two: what the parent mounts afterwards, the child does not see.
     * The child blocks on its standard input until this side has
     * mounted, so the mount certainly happens after its namespace was
     * copied -- which is the only ordering that tests anything.
     */
    int sp[2], rp[2];
    CHECK(pipe(sp) == 0 && pipe(rp) == 0);
    struct spawn_handle wmap[] = { { .child = 0, .parent = sp[0] },
                                   { .child = 1, .parent = rp[1] },
                                   { .child = 2, .parent = 2 } };
    const char *w_argv[] = { "sh", "-c", "cat > /dev/null && ls /tmp/nsm", NULL };
    pid_t wpid = spawnve_mountns("/bin/sh", w_argv, NULL, wmap, 3);
    CHECK(wpid > 1);
    CHECK(close(sp[0]) == 0 && close(rp[1]) == 0);
    CHECK(cosmo_mount("none", "/tmp/nsm", "ramfs", 0) == 0);
    CHECK(close(sp[1]) == 0);   /* the child may go now */
    ssize_t wn = read(rp[0], buf, sizeof(buf) - 1);
    CHECK(wn >= 0);
    buf[wn] = 0;
    CHECK(close(rp[0]) == 0);
    int wstatus = 0;
    CHECK(waitpid(wpid, &wstatus, 0) == wpid && wstatus == 0);
    /* The child still sees the directory underneath, because the mount
     * that covers it here was made after its namespace was copied. */
    CHECK(strstr(buf, "nsfile") != NULL);
    /* This side does see it: an empty ramfs. */
    CHECK(stat("/tmp/nsm/nsfile", &nst) < 0);
    CHECK(cosmo_umount("/tmp/nsm") == 0);
    CHECK(stat("/tmp/nsm/nsfile", &nst) == 0);

    /*
     * The uts namespace (docs/kernel/security/design.md §1e, S13). The
     * child renames itself; this side must be unchanged, and the child
     * must read back its own new name rather than this one.
     */
    char host0[HOST_NAME_MAX];
    CHECK(gethostname(host0, sizeof(host0)) >= 0);
    int hp[2];
    CHECK(pipe(hp) == 0);
    struct spawn_handle hmap[] = { { .child = 0, .parent = 0 },
                                   { .child = 1, .parent = hp[1] },
                                   { .child = 2, .parent = 2 } };
    const char *h_argv[] = { "sh", "-c", "hostname inside && hostname", NULL };
    pid_t hpid = spawnve_utsns("/bin/sh", h_argv, NULL, hmap, 3);
    CHECK(hpid > 1);
    CHECK(close(hp[1]) == 0);
    ssize_t hn = read(hp[0], buf, sizeof(buf) - 1);
    CHECK(hn >= 0);
    buf[hn] = 0;
    CHECK(close(hp[0]) == 0);
    int hstatus = 0;
    CHECK(waitpid(hpid, &hstatus, 0) == hpid && hstatus == 0);
    CHECK(strstr(buf, "inside") != NULL);       /* its own new name */
    CHECK(strstr(buf, host0) == NULL);          /* not this one */
    /* And renaming in there did not rename the machine. */
    char host1[HOST_NAME_MAX];
    CHECK(gethostname(host1, sizeof(host1)) >= 0);
    CHECK(strcmp(host0, host1) == 0);

    /* A child *without* a namespace of its own shares this one, which
     * is what says the isolation came from the namespace and not from
     * being a different process. */
    int sp2[2];
    CHECK(pipe(sp2) == 0);
    struct spawn_handle smap[] = { { .child = 0, .parent = 0 },
                                   { .child = 1, .parent = sp2[1] },
                                   { .child = 2, .parent = 2 } };
    const char *s_argv[] = { "sh", "-c", "hostname", NULL };
    pid_t spid = spawnve("/bin/sh", s_argv, NULL, smap, 3);
    CHECK(spid > 1);
    CHECK(close(sp2[1]) == 0);
    ssize_t sn = read(sp2[0], buf, sizeof(buf) - 1);
    CHECK(sn >= 0);
    buf[sn] = 0;
    CHECK(close(sp2[0]) == 0);
    int sstatus = 0;
    CHECK(waitpid(spid, &sstatus, 0) == spid && sstatus == 0);
    CHECK(strstr(buf, host0) != NULL);

    /*
     * The syscall filter (docs/kernel/security/design.md §1f, S14).
     * Every case is a child of its own, since a filter cannot be taken
     * back; the exit status is the whole result.
     */
    static const struct { const char *kind; int status; } fcases[] = {
        { "inside", 0 },                        /* stays inside its mask */
        { "outside", 128 + COSMO_SIGSYS },      /* one step outside it */
        { "exit-unnamed", 0 },                  /* exit works unnamed */
        { "widen", 128 + COSMO_SIGSYS },        /* a wider mask restores nothing */
        { "inherit", 0 },                       /* the child dies of its parent's filter */
        { "linux-child", 0 },                   /* a filter cannot cross a numbering */
    };
    for (size_t i = 0; i < sizeof(fcases) / sizeof(fcases[0]); i++) {
        const char *fargv[] = { "init", "--filter", fcases[i].kind, NULL };
        pid_t fp = spawnve("/boot/init", fargv, NULL, NULL, 0);
        int fstatus = -1;
        CHECK(fp > 0 && waitpid(fp, &fstatus, 0) == fp);
        CHECK(fstatus == fcases[i].status);
    }

    /* Handle rights: a handle says what may be done with it, and what it
     * says only ever shrinks (docs/kernel/object/architecture.md). */
    int rw = open("/tmp/rights.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(rw >= 0);
    CHECK(write(rw, "abc", 3) == 3);

    /* A copy with only READ can be read and not written, and cannot get
     * back what it gave up. Writing through it is EBADF, not EPERM:
     * POSIX says that of a descriptor that is not open for writing, and
     * the capability layer does not change what read and write answer.
     * EPERM is for the operations POSIX has no opinion about. */
    int ro = dup_rights(rw, -1, COSMO_RIGHT_READ);
    CHECK(ro >= 0);
    CHECK(write(ro, "x", 1) < 0 && errno == EBADF);
    CHECK(lseek(ro, 0, SEEK_SET) == 0);
    char rb[4] = { 0 };
    CHECK(read(ro, rb, 3) == 3 && rb[0] == 'a');
    CHECK(dup_rights(ro, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE) < 0 && errno == EPERM);
    /* It has no DUP either, so it cannot even be copied as it is. */
    CHECK(dup(ro) < 0 && errno == EPERM);
    /* And no TRANSFER, so it cannot be handed to a child. */
    struct spawn_handle no_transfer[] = { { .child = 3, .parent = ro } };
    CHECK(spawnve("/bin/true", true_argv, NULL, no_transfer, 1) < 0 && errno == EPERM);
    /* Nor administered: making it non-blocking is a MANAGE operation. */
    CHECK(cosmo_setnonblock(ro, 1) == -EPERM);

    /* A copy that keeps DUP and TRANSFER but drops WRITE is still useful:
     * that is the point of being able to hand over less. */
    int share = dup_rights(rw, -1, COSMO_RIGHT_READ | COSMO_RIGHT_DUP | COSMO_RIGHT_TRANSFER);
    CHECK(share >= 0);
    CHECK(write(share, "x", 1) < 0 && errno == EBADF);
    int share2 = dup(share);
    CHECK(share2 >= 0);
    CHECK(write(share2, "x", 1) < 0 && errno == EBADF);   /* the copy of a copy is no wider */
    struct spawn_handle give[] = { { .child = 3, .parent = share } };
    pid_t gp = spawnve("/bin/true", true_argv, NULL, give, 1);
    CHECK(gp > 0);
    int gst = 0;
    CHECK(waitpid(gp, &gst, 0) == gp);

    /* The map as it was before rights existed: two ints per entry, and
     * no COSMO_SPAWN_HANDLE_RIGHTS flag. A program built against the
     * older header passes exactly this, and must still work -- reading
     * it as the wider element would take the next entry's child for
     * this one's rights. */
    struct legacy_map {
        int child;
        int parent;
    } legacy[] = { { 0, 0 }, { 1, 1 }, { 2, 2 } };
    struct cosmo_spawn old_req = {
        .path = "/bin/true",
        .argv = true_argv,
        .envp = NULL,
        .handles = (const struct cosmo_spawn_handle *)legacy,
        .nr_handles = 3,
        .cwd = NULL,
        .flags = 0,
    };
    long lp = cosmo_spawn(&old_req);
    CHECK(lp > 0);
    int lst = 0;
    CHECK(waitpid((pid_t)lp, &lst, 0) == (pid_t)lp && WIFEXITED(lst) && WEXITSTATUS(lst) == 0);

    /* The original is untouched by any of it. */
    CHECK(lseek(rw, 0, SEEK_SET) == 0);
    CHECK(write(rw, "zzz", 3) == 3);
    close(share2);
    close(share);
    close(ro);
    close(rw);
    CHECK(unlink("/tmp/rights.txt") == 0);
    const char *no_argv[] = { NULL };
    CHECK(spawnve("/bin/true", no_argv, NULL, NULL, 0) < 0 && errno == EINVAL);
    CHECK(waitpid(-1, &status, 0) < 0 && errno == ECHILD);

    /* Working directory. */
    CHECK(chdir("/tmp") == 0 && getcwd(buf, sizeof(buf)) && strcmp(buf, "/tmp") == 0);
    CHECK(mkdir("cwdtest", 0755) == 0);                    /* relative to /tmp */
    CHECK(stat("/tmp/cwdtest", &st) == 0 && S_ISDIR(st.st_type));
    CHECK(chdir("cwdtest/../cwdtest/.") == 0 && getcwd(buf, sizeof(buf)) && strcmp(buf, "/tmp/cwdtest") == 0);
    CHECK(chdir("..") == 0 && getcwd(buf, sizeof(buf)) && strcmp(buf, "/tmp") == 0);
    CHECK(chdir("/boot/init") < 0 && errno == ENOTDIR);
    CHECK(chdir("/nope") < 0 && errno == ENOENT);
    CHECK(getcwd(buf, 4) == NULL && errno == ERANGE);
    CHECK(rmdir("cwdtest") == 0);
    CHECK(chdir("/") == 0);

    /* Introspection. */
    CHECK(getppid() == 0);                                 /* spawned by the kernel */
    struct cosmo_procinfo pi[16];
    int total = procinfo(pi, 16);
    CHECK(total >= 1);
    int found = 0;
    for (int i = 0; i < total && i < 16; i++)
        if (pi[i].pid == (uint32_t)getpid() && strcmp(pi[i].name, "init") == 0 && pi[i].nr_threads == 1)
            found = 1;
    CHECK(found);
    static char log[8192];
    n = klog_read(log, sizeof(log) - 1);
    CHECK(n > 100);
    if (n > 0) {
        log[n] = '\0';
        CHECK(strstr(log, "CosmoOS kernel") != NULL || strstr(log, "[ INFO]") != NULL);
    }
    CHECK(sysctl_get("kernel.name", buf, sizeof(buf)) == 7 && strcmp(buf, "CosmoOS") == 0);
    CHECK(sysctl_get("hw.ncpu", buf, sizeof(buf)) > 0 && atoi(buf) >= 1);
    CHECK(sysctl_get("sysctl.names", buf, sizeof(buf)) > 0 && strstr(buf, "kernel.version") != NULL);
    CHECK(sysctl_get("no.such", buf, sizeof(buf)) < 0 && errno == ENOENT);
    CHECK(sysctl_get("kernel.name", buf, 3) == 7 && buf[0] == 'C' && buf[2] == 's');   /* truncated, no NUL */

    /* libc pieces with no kernel side. */
    char *heap = malloc(100000);
    CHECK(heap != NULL);
    if (heap) {
        memset(heap, 0x5a, 100000);
        heap = realloc(heap, 200000);
        CHECK(heap && heap[99999] == 0x5a);
        free(heap);
    }
    CHECK(snprintf(buf, sizeof(buf), "%5d|%-5d|%05d|%x|%s|%c|%%|%lld", 42, 42, 42, 255, "s", 'q', 1LL << 40) == 40 &&
          strcmp(buf, "   42|42   |00042|ff|s|q|%|1099511627776") == 0);
    CHECK(strtol("  -123xyz", NULL, 10) == -123 && strtoul("0x1f", NULL, 0) == 31);
    setenv("USERTEST", "yes", 1);
    CHECK(getenv("USERTEST") && strcmp(getenv("USERTEST"), "yes") == 0);
    puts("usertest: processes ok");
}

/* --probe KIND: the user-memory edge cases of audit milestone 5
 * (docs/kernel/memory/design.md §6). "efault": system calls given
 * PROT_NONE, read-only and unmapped pointers return -EFAULT and the
 * process lives (exit 0; a nonzero exit names the failing step).
 * "none-touch" and "oom-touch" end in a fatal fault. "oom-copy" reads
 * from a pipe into a never-touched page while the kernel injects a
 * failure into that demand fault: -EFAULT, exit 0. */
/*
 * The syscall filter (docs/kernel/security/design.md §1f, S14). Each
 * case runs in a child of its own, because a filter cannot be taken
 * back: the parent could not test one without ending its own run.
 */
#define LX_PROGRAM "/boot/tests/linux/lxhello"

static int filter_case(const char *kind)
{
    uint64_t mask[COSMO_SYSCALL_MASK_WORDS];
    memset(mask, 0, sizeof(mask));
    /* Enough to write a byte and stop. exit is allowed whatever the
     * mask says, and is named here only to show that saying so changes
     * nothing. */
    SYSCALL_ALLOW(mask, SYS_exit);
    SYSCALL_ALLOW(mask, SYS_write);

    if (strcmp(kind, "inside") == 0) {
        /* Staying inside the mask: the process runs to a clean exit. */
        if (syscall_filter(mask, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 10;
        const char ok[] = "";
        (void)write(1, ok, 0);
        return 0;
    }
    if (strcmp(kind, "outside") == 0) {
        /* One step outside it: SIGSYS, so the parent sees 128 + 31. */
        if (syscall_filter(mask, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 10;
        (void)getpid();
        return 20;   /* not reached */
    }
    if (strcmp(kind, "exit-unnamed") == 0) {
        /* A mask that does not name exit at all: exiting still works,
         * or every clean shutdown would be a signal death. */
        uint64_t only_write[COSMO_SYSCALL_MASK_WORDS];
        memset(only_write, 0, sizeof(only_write));
        SYSCALL_ALLOW(only_write, SYS_write);
        if (syscall_filter(only_write, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 10;
        return 0;
    }
    if (strcmp(kind, "widen") == 0) {
        /* A second, wider mask must not restore what the first removed:
         * install one without getpid, then ask for everything.
         *
         * The first mask has to keep syscall_filter itself, or the
         * second call is refused and the process dies of *that* --
         * which is the same exit status and would let this pass
         * without the intersection ever being exercised. */
        SYSCALL_ALLOW(mask, SYS_syscall_filter);
        if (syscall_filter(mask, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 10;
        uint64_t all[COSMO_SYSCALL_MASK_WORDS];
        memset(all, 0xff, sizeof(all));
        if (syscall_filter(all, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 11;
        (void)getpid();
        return 20;   /* not reached: the intersection still excludes it */
    }
    if (strcmp(kind, "inherit") == 0) {
        /* A child is born with its parent's filter. spawn and wait stay
         * allowed here so there is a child to be killed at all. */
        SYSCALL_ALLOW(mask, SYS_spawn);
        SYSCALL_ALLOW(mask, SYS_wait);
        if (syscall_filter(mask, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 10;
        const char *argv[] = { "init", "--filter", "child-getpid", NULL };
        long pid = cosmo_spawn(&(struct cosmo_spawn){ .path = "/boot/init", .argv = argv });
        if (pid <= 0)
            return 11;
        int status = -1;
        if (cosmo_wait((int)pid, &status, 0) != pid)
            return 12;
        return status == 128 + COSMO_SIGSYS ? 0 : 13;
    }
    if (strcmp(kind, "linux-child") == 0) {
        /*
         * A filter is bits by call number, and the two personalities
         * number differently, so a filtered process cannot start a
         * program of the other kind (§1f).
         *
         * Spawn it once *before* filtering, so a wrong path fails here
         * with its own code rather than looking like the refusal this
         * is trying to prove.
         */
        static const char *const lx_argv[] = { "lxhello", NULL };
        pid_t first = spawnve(LX_PROGRAM, lx_argv, NULL, NULL, 0);
        if (first <= 0)
            return 30;   /* the program is not there: says nothing about filters */
        int lstatus = -1;
        if (waitpid(first, &lstatus, 0) != first)
            return 31;

        SYSCALL_ALLOW(mask, SYS_spawn);
        SYSCALL_ALLOW(mask, SYS_wait);
        if (syscall_filter(mask, COSMO_SYSCALL_MASK_WORDS) != 0)
            return 10;
        /* Now the same spawn must be refused, and refused for this
         * reason rather than by being killed for calling spawn. */
        if (spawnve(LX_PROGRAM, lx_argv, NULL, NULL, 0) >= 0)
            return 32;
        return errno == EPERM ? 0 : 33;
    }
    if (strcmp(kind, "child-getpid") == 0) {
        (void)getpid();   /* denied by the filter this was born with */
        return 20;        /* not reached */
    }
    return 99;
}

/* The signal probes need the vector-register helpers below, so they
 * live past them and probe() ends by handing the kind on. */
static int signal_probe(const char *kind);

static int probe(const char *kind)
{
    const size_t P = 4096;
    if (strcmp(kind, "efault") == 0) {
        long none = cosmo_mmap(NULL, P, COSMO_PROT_NONE, COSMO_MAP_ANONYMOUS);
        long ro = cosmo_mmap(NULL, P, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS);
        long rw = cosmo_mmap(NULL, 3 * P, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
        if (none <= 0 || ro <= 0 || rw <= 0)
            return 10;
        int h[2];
        if (cosmo_pipe(h) != 0)
            return 11;
        /* Writes from a PROT_NONE page and a hole: -EFAULT, nothing written. */
        if (cosmo_write(h[1], (void *)none, 16) != -COSMO_EFAULT)
            return 12;
        if (cosmo_log((const char *)none, 8) != -COSMO_EFAULT)
            return 13;
        /* A read into a read-only page: the copy_to_user faults on a
         * present page (a protection fault, not a demand fault). Each
         * attempt gets its own 16 bytes: whether a failed copy consumed
         * them from the pipe is the pipe's business, not this probe's. */
        if (cosmo_write(h[1], "0123456789abcdef", 16) != 16)
            return 14;
        if (cosmo_read(h[0], (void *)ro, 16) != -COSMO_EFAULT)
            return 15;
        /* A hole in the middle of a mapping: the copy stops there. */
        if (cosmo_munmap((void *)(rw + P), P) != 0 || cosmo_write(h[1], "0123456789abcdef", 16) != 16)
            return 16;
        if (cosmo_read(h[0], (void *)(rw + P - 8), 16) != -COSMO_EFAULT)
            return 17;
        /* The same read into the surviving first page works. */
        if (cosmo_write(h[1], "0123456789abcdef", 16) != 16)
            return 18;
        if (cosmo_read(h[0], (void *)rw, 16) != 16 || memcmp((void *)rw, "0123456789abcdef", 16) != 0)
            return 18;
        /* stat into PROT_NONE, and a path string that runs into a hole. */
        if (cosmo_stat("/boot", (struct cosmo_stat *)none) != -COSMO_EFAULT)
            return 19;
        char *edge = (char *)(rw + P - 4);
        memcpy(edge, "/boo", 4);   /* no NUL before the unmapped page */
        struct cosmo_stat st;
        if (cosmo_stat(edge, &st) != -COSMO_EFAULT)
            return 20;
        /* A read-only page can be a source. */
        if (cosmo_write(h[1], (const void *)ro, 4) != 4)
            return 21;
        cosmo_close(h[0]);
        cosmo_close(h[1]);
        return 0;
    }
    if (strcmp(kind, "hold") == 0) {
        cosmo_sleep_ns(30000000);   /* stay alive long enough to be counted */
        return 0;
    }
    if (strncmp(kind, "uid-is:", 7) == 0) {
        unsigned want = (unsigned)strtoul(kind + 7, NULL, 10);
        return (getuid() == want && geteuid() == want && getgid() == want && getgroups(0, NULL) == 0) ? 0 : 1;
    }
    if (strcmp(kind, "rlimit-root") == 0) {
        uint64_t v;
        if (cosmo_getrlimit(COSMO_RLIMIT_AS, &v) != 0 || v != (2ull << 30))
            return 10;
        if (cosmo_getrlimit(COSMO_RLIMIT_NOFILE, &v) != 0 || v != 64)
            return 11;
        if (cosmo_getrlimit(COSMO_RLIMIT_NPROC, &v) != 0 || v != 128)
            return 12;
        if (cosmo_getrlimit(99, &v) != -COSMO_EINVAL || cosmo_setrlimit(99, 1) != -COSMO_EINVAL)
            return 13;
        if (cosmo_setrlimit(COSMO_RLIMIT_NOFILE, 65) != -COSMO_EINVAL)
            return 14;
        /* Address space: 16 MiB caps the mappings (the stack alone is 8). */
        if (cosmo_setrlimit(COSMO_RLIMIT_AS, 16ull << 20) != 0)
            return 15;
        if (cosmo_mmap(NULL, 32ull << 20, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS) != -COSMO_ENOMEM)
            return 16;
        long m = cosmo_mmap(NULL, 1ull << 20, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
        if (m <= 0)
            return 17;
        if (cosmo_setrlimit(COSMO_RLIMIT_AS, 2ull << 30) != 0)   /* root raises it back */
            return 18;
        /* Handles: 0, 1, 2 are open; a limit of 4 leaves room for one. */
        if (cosmo_setrlimit(COSMO_RLIMIT_NOFILE, 4) != 0)
            return 19;
        int h[2];
        if (cosmo_pipe(h) != -COSMO_EMFILE)
            return 20;
        if (cosmo_setrlimit(COSMO_RLIMIT_NOFILE, 64) != 0 || cosmo_pipe(h) != 0)
            return 21;
        cosmo_close(h[0]);
        cosmo_close(h[1]);
        /* Processes: this one counts; a limit of 1 leaves no room for a child. */
        const char *child[] = { "init", "--probe", "uid-is:0", NULL };
        if (cosmo_setrlimit(COSMO_RLIMIT_NPROC, 1) != 0)
            return 22;
        if (spawnve("/boot/init", child, NULL, NULL, 0) >= 0 || errno != EAGAIN)
            return 23;
        if (cosmo_setrlimit(COSMO_RLIMIT_NPROC, 128) != 0)
            return 24;
        /* A privileged caller hands a child any identity; it has no groups. */
        const char *child1000[] = { "init", "--probe", "uid-is:1000", NULL };
        pid_t pid = spawnve_as("/boot/init", child1000, NULL, NULL, 0, 1000, 1000);
        int status = -1;
        if (pid <= 0 || waitpid(pid, &status, 0) != pid || status != 0)
            return 25;
        /* Guest memory: the cap the VM records is the creator's limit. */
        if (cosmo_setrlimit(COSMO_RLIMIT_VMEM, 1ull << 20) != 0)
            return 26;
        long dev = cosmo_open("/dev/vmm", COSMO_O_RDWR, 0);
        if (dev >= 0) {
            long vm = cosmo_syscall1(SYS_vm_create, dev);
            if (vm >= 0) {
                if (cosmo_syscall3(SYS_vm_mem, vm, 0, 2ull << 20) != -COSMO_ENOMEM)
                    return 27;
                if (cosmo_syscall3(SYS_vm_mem, vm, 0, 1ull << 20) != 0)
                    return 28;

                /*
                 * Per-type rights on the VM and its vCPUs (S15). Giving
                 * the guest memory is VM_MAP, not WRITE: a copy without
                 * it can still read and write guest memory, which is
                 * the distinction between contents and shape.
                 */
                long no_map = cosmo_dup_rights((int)vm, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE);
                if (no_map < 0)
                    return 40;
                if (cosmo_syscall3(SYS_vm_mem, no_map, 1ull << 20, 1ull << 20) != -COSMO_EPERM)
                    return 41;
                if (cosmo_syscall5(SYS_vm_mem_rw, no_map, 0, (long)"z", 1, 1) != 1)
                    return 42;   /* contents are still READ/WRITE */
                cosmo_close((int)no_map);

                /* A vCPU, if this platform has a backend to make one
                 * with. Asked for with full rights first, so a machine
                 * without one is told apart from a refusal. */
                long vcpu = cosmo_syscall2(SYS_vcpu_create, vm, 0);
                if (vcpu >= 0) {
                    /* No VM_VCPU: cannot make another. */
                    long no_vcpu = cosmo_dup_rights((int)vm, -1, COSMO_RIGHT_READ | COSMO_RIGHT_WRITE);
                    if (no_vcpu < 0)
                        return 43;
                    if (cosmo_syscall2(SYS_vcpu_create, no_vcpu, 1) != -COSMO_EPERM)
                        return 44;
                    cosmo_close((int)no_vcpu);

                    /* No REGS: reading them is READ and still works,
                     * writing them is refused. That pair is the whole
                     * point of splitting WRITE up. */
                    struct cosmo_vcpu_regs regs;
                    long no_regs = cosmo_dup_rights((int)vcpu, -1, COSMO_RIGHT_READ | COSMO_RIGHT_VCPU_RUN);
                    if (no_regs < 0)
                        return 45;
                    if (cosmo_syscall3(SYS_vcpu_regs, no_regs, (long)&regs, 0) != 0)
                        return 46;
                    if (cosmo_syscall3(SYS_vcpu_regs, no_regs, (long)&regs, 1) != -COSMO_EPERM)
                        return 47;
                    /* And no IRQ: injecting is its own authority. */
                    if (cosmo_syscall2(SYS_vcpu_irq, no_regs, 32) != -COSMO_EPERM)
                        return 48;
                    cosmo_close((int)no_regs);
                    cosmo_close((int)vcpu);
                } else if (vcpu != -COSMO_EOPNOTSUPP) {
                    return 49;
                }
                cosmo_close((int)vm);
            } else if (vm != -COSMO_EOPNOTSUPP) {   /* no backend on this platform */
                return 29;
            }
            cosmo_close((int)dev);
        }
        return 0;
    }
    if (strcmp(kind, "rlimit-unpriv") == 0) {
        if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0)
            return 10;
        if (cosmo_setrlimit(COSMO_RLIMIT_NOFILE, 60) != 0)          /* lowering is free */
            return 11;
        if (cosmo_setrlimit(COSMO_RLIMIT_NOFILE, 64) != -COSMO_EPERM) /* raising is privileged */
            return 12;
        if (cosmo_setrlimit(COSMO_RLIMIT_MEM, 64ull << 20) != 0 || cosmo_setrlimit(COSMO_RLIMIT_MEM, 65ull << 20) != -COSMO_EPERM)
            return 13;
        /* No path back to root through spawn. */
        const char *child0[] = { "init", "--probe", "uid-is:0", NULL };
        if (spawnve_as("/boot/init", child0, NULL, NULL, 0, 0, 0) >= 0 || errno != EPERM)
            return 14;
        if (spawnve_as("/boot/init", child0, NULL, NULL, 0, 1000, 0) >= 0 || errno != EPERM)
            return 15;
        const char *child1000[] = { "init", "--probe", "uid-is:1000", NULL };
        pid_t pid = spawnve_as("/boot/init", child1000, NULL, NULL, 0, 1000, 1000);   /* an id it holds */
        int status = -1;
        if (pid <= 0 || waitpid(pid, &status, 0) != pid || status != 0)
            return 16;
        /* procinfo shows only this user's processes. */
        struct cosmo_procinfo pi[64];
        int n = procinfo(pi, 64);
        if (n < 1)
            return 17;
        for (int i = 0; i < n && i < 64; i++)
            if (pi[i].uid != 1000)
                return 18;
        /* The kernel-log write is rate limited for unprivileged callers. */
        int eagain = 0, ok = 0;
        for (int i = 0; i < 80; i++) {
            long r = cosmo_log("rl", 2);
            if (r == -COSMO_EAGAIN)
                eagain++;
            else if (r == 0)
                ok++;
        }
        if (ok < 16 || eagain == 0)
            return 19;
        return 0;
    }
    if (strcmp(kind, "mem-limit") == 0) {
        /* Resident memory capped at 1 MiB; touching 4 MiB must be fatal. */
        if (cosmo_setrlimit(COSMO_RLIMIT_MEM, 1ull << 20) != 0)
            return 10;
        long m = cosmo_mmap(NULL, 4ull << 20, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
        if (m <= 0)
            return 11;
        for (uint64_t off = 0; off < (4ull << 20); off += P)
            *(volatile char *)(m + off) = 1;
        return 9;   /* reached only if the limit did not bite */
    }
    if (strcmp(kind, "none-touch") == 0) {
        long none = cosmo_mmap(NULL, P, COSMO_PROT_NONE, COSMO_MAP_ANONYMOUS);
        if (none <= 0)
            return 10;
        *(volatile char *)none = 1;   /* must be fatal */
        return 9;
    }
    if (strcmp(kind, "oom-copy") == 0) {
        long fresh = cosmo_mmap(NULL, P, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
        int h[2];
        if (fresh <= 0 || cosmo_pipe(h) != 0)
            return 10;
        if (cosmo_write(h[1], "oom", 3) != 3)
            return 11;
        long r = cosmo_read(h[0], (void *)fresh, 3);   /* the kernel's copy takes the demand fault */
        if (r == -COSMO_EFAULT)
            return 0;
        return r == 3 ? 3 : 4;   /* 3: the injected failure went elsewhere */
    }
    if (strcmp(kind, "oom-touch") == 0) {
        long fresh = cosmo_mmap(NULL, P, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
        if (fresh <= 0)
            return 10;
        *(volatile char *)fresh = 1;   /* the demand fault fails: fatal */
        return 9;
    }
    return signal_probe(kind);
}

#if defined(__x86_64__)
/* The process rule of arch/fpu.h: a process never observes another's
 * vector registers. Two partner processes and this one each hold a
 * distinct pattern in xmm0-xmm15 across hundreds of yields and sleeps.
 * libc is built -mgeneral-regs-only, so the only SSE here is this asm. */
static void xmm_fill(uint8_t seed, uint8_t r[16][16])
{
    for (unsigned i = 0; i < 16; i++)
        for (unsigned j = 0; j < 16; j++)
            r[i][j] = (uint8_t)(seed ^ (i * 17u) ^ (j * 3u));
}

static void xmm_load(const uint8_t r[16][16])
{
    __asm__ volatile("movdqu 0(%0), %%xmm0\n\tmovdqu 16(%0), %%xmm1\n\tmovdqu 32(%0), %%xmm2\n\t"
                     "movdqu 48(%0), %%xmm3\n\tmovdqu 64(%0), %%xmm4\n\tmovdqu 80(%0), %%xmm5\n\t"
                     "movdqu 96(%0), %%xmm6\n\tmovdqu 112(%0), %%xmm7\n\tmovdqu 128(%0), %%xmm8\n\t"
                     "movdqu 144(%0), %%xmm9\n\tmovdqu 160(%0), %%xmm10\n\tmovdqu 176(%0), %%xmm11\n\t"
                     "movdqu 192(%0), %%xmm12\n\tmovdqu 208(%0), %%xmm13\n\tmovdqu 224(%0), %%xmm14\n\t"
                     "movdqu 240(%0), %%xmm15"
                     : : "r"(r) : "memory");
}

static void xmm_store(uint8_t r[16][16])
{
    __asm__ volatile("movdqu %%xmm0, 0(%0)\n\tmovdqu %%xmm1, 16(%0)\n\tmovdqu %%xmm2, 32(%0)\n\t"
                     "movdqu %%xmm3, 48(%0)\n\tmovdqu %%xmm4, 64(%0)\n\tmovdqu %%xmm5, 80(%0)\n\t"
                     "movdqu %%xmm6, 96(%0)\n\tmovdqu %%xmm7, 112(%0)\n\tmovdqu %%xmm8, 128(%0)\n\t"
                     "movdqu %%xmm9, 144(%0)\n\tmovdqu %%xmm10, 160(%0)\n\tmovdqu %%xmm11, 176(%0)\n\t"
                     "movdqu %%xmm12, 192(%0)\n\tmovdqu %%xmm13, 208(%0)\n\tmovdqu %%xmm14, 224(%0)\n\t"
                     "movdqu %%xmm15, 240(%0)"
                     : : "r"(r) : "memory");
}

#define FPU_ROUNDS 300

/* Hold `seed`'s pattern for FPU_ROUNDS rounds; 0 if it survived, 3 if not. */
static int fpu_hold(uint8_t seed)
{
    uint8_t want[16][16], got[16][16];
    xmm_fill(seed, want);
    xmm_load(want);
    for (unsigned i = 0; i < FPU_ROUNDS; i++) {
        if (i & 1)
            cosmo_yield();
        else
            usleep(200);
        xmm_store(got);
        if (memcmp(got, want, sizeof(got)) != 0)
            return 3;
    }
    return 0;
}

/* --trap KIND: raise a CPU exception from user mode; the kernel must end
 * this process with 128 + the exception's signal, never itself. */
static int trap_self(const char *kind)
{
    if (strcmp(kind, "ud") == 0)
        __asm__ volatile("ud2");
    else if (strcmp(kind, "gp") == 0)
        __asm__ volatile("hlt");                                   /* privileged instruction */
    else if (strcmp(kind, "de") == 0)
        __asm__ volatile("xorl %%eax, %%eax\n\tdivl %%eax" ::: "eax", "edx", "cc");
    else if (strcmp(kind, "db") == 0)
        __asm__ volatile("pushfq\n\torq $0x100, (%%rsp)\n\tpopfq\n\tnop\n\tnop" ::: "memory", "cc");   /* TF */
    return 9;   /* reached only if the kernel let the fault pass */
}

static void trap_selftest(void)
{
    /* Each exception is its own signal (SIGILL 4, SIGSEGV 11, SIGFPE 8,
     * SIGTRAP 5); an unhandled one ends the process with 128 + sig. */
    static const struct { const char *kind; int status; } kinds[] = {
        { "ud", 128 + 4 }, { "gp", 128 + 11 }, { "de", 128 + 8 }, { "db", 128 + 5 },
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        const char *argv[] = { "init", "--trap", kinds[i].kind, NULL };
        pid_t pid = spawnve("/boot/init", argv, NULL, NULL, 0);
        int status = -1;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && status == kinds[i].status);
    }
    puts("usertest: user exceptions ok");
}

static void fpu_selftest(void)
{
    const char *a_argv[] = { "init", "--fpu-partner", "17", NULL };
    const char *b_argv[] = { "init", "--fpu-partner", "170", NULL };
    pid_t a = spawnve("/boot/init", a_argv, NULL, NULL, 0);
    pid_t b = spawnve("/boot/init", b_argv, NULL, NULL, 0);
    CHECK(a > 0 && b > 0);
    CHECK(fpu_hold(0x5A) == 0);
    int status = -1;
    CHECK(waitpid(a, &status, 0) == a && status == 0);
    CHECK(waitpid(b, &status, 0) == b && status == 0);
    puts("usertest: fpu isolation ok");
}
#else
/* The same process rule on AArch64: each process holds its own pattern
 * in V0-V15 across hundreds of yields and sleeps. The libc no longer
 * refuses the vector registers, but the compiler has no reason to touch
 * these sixteen, so what the loop observes is the kernel's switching and
 * nothing else. The assembler is told to allow the instructions and told
 * again to stop. */
static void vreg_fill(uint8_t seed, uint8_t r[16][16])
{
    for (unsigned i = 0; i < 16; i++)
        for (unsigned j = 0; j < 16; j++)
            r[i][j] = (uint8_t)(seed ^ (i * 17u) ^ (j * 3u));
}

static void vreg_load(const uint8_t r[16][16])
{
    __asm__ volatile(".arch armv8-a+fp+simd\n\t"
                     "ldp q0, q1, [%0, #0]\n\tldp q2, q3, [%0, #32]\n\t"
                     "ldp q4, q5, [%0, #64]\n\tldp q6, q7, [%0, #96]\n\t"
                     "ldp q8, q9, [%0, #128]\n\tldp q10, q11, [%0, #160]\n\t"
                     "ldp q12, q13, [%0, #192]\n\tldp q14, q15, [%0, #224]\n\t"
                     ".arch armv8-a"
                     : : "r"(r) : "memory");
}

static void vreg_store(uint8_t r[16][16])
{
    __asm__ volatile(".arch armv8-a+fp+simd\n\t"
                     "stp q0, q1, [%0, #0]\n\tstp q2, q3, [%0, #32]\n\t"
                     "stp q4, q5, [%0, #64]\n\tstp q6, q7, [%0, #96]\n\t"
                     "stp q8, q9, [%0, #128]\n\tstp q10, q11, [%0, #160]\n\t"
                     "stp q12, q13, [%0, #192]\n\tstp q14, q15, [%0, #224]\n\t"
                     ".arch armv8-a"
                     : : "r"(r) : "memory");
}

#define FPU_ROUNDS 300

static int fpu_hold(uint8_t seed)
{
    uint8_t want[16][16], got[16][16];
    vreg_fill(seed, want);
    vreg_load(want);
    for (unsigned i = 0; i < FPU_ROUNDS; i++) {
        if (i & 1)
            cosmo_yield();
        else
            usleep(200);
        vreg_store(got);
        if (memcmp(got, want, sizeof(got)) != 0)
            return 3;
    }
    return 0;
}

static void fpu_selftest(void)
{
    const char *a_argv[] = { "init", "--fpu-partner", "17", NULL };
    const char *b_argv[] = { "init", "--fpu-partner", "170", NULL };
    pid_t a = spawnve("/boot/init", a_argv, NULL, NULL, 0);
    pid_t b = spawnve("/boot/init", b_argv, NULL, NULL, 0);
    CHECK(a > 0 && b > 0);
    CHECK(fpu_hold(0x5A) == 0);
    int status = -1;
    CHECK(waitpid(a, &status, 0) == a && status == 0);
    CHECK(waitpid(b, &status, 0) == b && status == 0);
    puts("usertest: fpu isolation ok");
}

static int trap_self(const char *kind)
{
    (void)kind;
    return 9;
}

static void trap_selftest(void)
{
}
#endif

/* --- the native signal ABI (docs/kernel/process/design.md) -------------------
 *
 * A handler runs on a frame the kernel pushed and returns through the
 * libc's restorer, and what the interrupted code must find when it
 * resumes is every register it had: the general ones and the vector
 * ones both, because a handler compiled by an ordinary toolchain uses
 * whichever it likes. These probes check that at each of the three
 * points a signal can be delivered -- the return from a system call
 * (signal), from an interrupt (signal-async), and from a fault
 * (signal-fault) -- and that the blocked mask travels with the frame.
 */
#if defined(__x86_64__)
#define vec_fill xmm_fill
#define vec_load xmm_load
#define vec_store xmm_store
#else
#define vec_fill vreg_fill
#define vec_load vreg_load
#define vec_store vreg_store
#endif

#define SIGFAULT_ADDR 0x40000000ull   /* nothing is mapped there until the handler maps it */
#define SIGBIT(s) (1ul << ((s) - 1))

static volatile int g_sig_ran;
static volatile int g_sig_num;
static volatile int g_sig_code;
static volatile int g_sig_pid;
static volatile unsigned long g_sig_addr;
static volatile unsigned long g_sig_mask;   /* the blocked set as the handler saw it */

/* Put something else in every register a handler is free to use, so that
 * what the interrupted code finds afterwards came out of the frame and
 * not from a handler that happened to leave things as it found them. */
static void trample(void)
{
    uint8_t junk[16][16];
    vec_fill(0xC3, junk);
    vec_load(junk);
#if defined(__x86_64__)
    __asm__ volatile("movq $-1, %%r8\n\tmovq $-1, %%r9\n\tmovq $-1, %%r10\n\tmovq $-1, %%r11"
                     : : : "r8", "r9", "r10", "r11");
#else
    __asm__ volatile("mov x9, #-1\n\tmov x10, #-1\n\tmov x12, #-1\n\tmov x13, #-1"
                     : : : "x9", "x10", "x12", "x13");
#endif
}

static void sig_handler(int sig, siginfo_t *si, void *frame)
{
    (void)frame;
    g_sig_num = sig;
    g_sig_code = si->si_code;
    g_sig_pid = si->si_pid;
    g_sig_addr = (unsigned long)si->si_addr;
    sigset_t m = 0;
    sigprocmask(SIG_BLOCK, NULL, &m);
    g_sig_mask = m;
    /* A fault handler that returns runs the faulting instruction again,
     * so this one gives it something to store into first. */
    if (sig == SIGSEGV)
        cosmo_mmap((void *)SIGFAULT_ADDR, 4096, COSMO_PROT_READ | COSMO_PROT_WRITE,
                   COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED);
    trample();
    g_sig_ran++;
}

static int install(int sig, unsigned flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sig_handler;
    sa.sa_flags = SA_SIGINFO | flags;
    return sigaction(sig, &sa, NULL);
}

/*
 * Spin until the handler has run, holding sentinels in four registers no
 * calling convention preserves: if the kernel does not put them back, the
 * loop comes out of the signal looking at the handler's leftovers. The
 * count bounds the wait, so a signal that never arrives fails the probe
 * rather than hanging the test.
 */
#define SIG_SPIN_MAX 40000000u
#define SIG_SPIN_MIN (SIG_SPIN_MAX - 1000u)   /* iterations that prove the loop was running */

static int spin_until_signalled(volatile int *flag)
{
    long bad;
#if defined(__x86_64__)
    __asm__ volatile(
        "movabsq $0x0123456789abcdef, %%r8\n\t"
        "movabsq $0xfedcba9876543210, %%r9\n\t"
        "movabsq $0x5555aaaa3333cccc, %%r10\n\t"
        "movabsq $0x00ff00ff00ff00ff, %%r11\n\t"
        "movl %2, %%eax\n\t"
        "1:\n\t"
        "subl $1, %%eax\n\t"
        "jz 4f\n\t"
        "cmpl $0, (%1)\n\t"
        "je 1b\n\t"
        "xorl %k0, %k0\n\t"
        "cmpl %3, %%eax\n\tjbe 7f\n\torq $32, %0\n\t"
        "7:\n\t"
        "movabsq $0x0123456789abcdef, %%rcx\n\tcmpq %%rcx, %%r8\n\tje 2f\n\torq $1, %0\n\t"
        "2:\n\t"
        "movabsq $0xfedcba9876543210, %%rcx\n\tcmpq %%rcx, %%r9\n\tje 3f\n\torq $2, %0\n\t"
        "3:\n\t"
        "movabsq $0x5555aaaa3333cccc, %%rcx\n\tcmpq %%rcx, %%r10\n\tje 5f\n\torq $4, %0\n\t"
        "5:\n\t"
        "movabsq $0x00ff00ff00ff00ff, %%rcx\n\tcmpq %%rcx, %%r11\n\tje 6f\n\torq $8, %0\n\t"
        "jmp 6f\n\t"
        "4:\n\tmovq $16, %0\n\t"
        "6:\n\t"
        : "=&r"(bad)
        : "r"(flag), "i"(SIG_SPIN_MAX), "i"(SIG_SPIN_MIN)
        : "rax", "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
#else
    __asm__ volatile(
        "movz x9, #0xcdef\n\tmovk x9, #0x89ab, lsl #16\n\tmovk x9, #0x4567, lsl #32\n\tmovk x9, #0x0123, lsl #48\n\t"
        "movz x10, #0x3210\n\tmovk x10, #0x7654, lsl #16\n\tmovk x10, #0xba98, lsl #32\n\tmovk x10, #0xfedc, lsl #48\n\t"
        "movz x12, #0xcccc\n\tmovk x12, #0x3333, lsl #16\n\tmovk x12, #0xaaaa, lsl #32\n\tmovk x12, #0x5555, lsl #48\n\t"
        "movz x13, #0x00ff\n\tmovk x13, #0x00ff, lsl #16\n\tmovk x13, #0x00ff, lsl #32\n\tmovk x13, #0x00ff, lsl #48\n\t"
        "mov w14, %w2\n\t"
        "1:\n\t"
        "subs w14, w14, #1\n\t"
        "b.eq 4f\n\t"
        "ldr w11, [%1]\n\t"
        "cbz w11, 1b\n\t"
        "mov %0, #0\n\t"
        "cmp w14, %w3\n\tb.ls 7f\n\torr %0, %0, #32\n\t"
        "7:\n\t"
        "movz x11, #0xcdef\n\tmovk x11, #0x89ab, lsl #16\n\tmovk x11, #0x4567, lsl #32\n\tmovk x11, #0x0123, lsl #48\n\t"
        "cmp x9, x11\n\tb.eq 2f\n\torr %0, %0, #1\n\t"
        "2:\n\t"
        "movz x11, #0x3210\n\tmovk x11, #0x7654, lsl #16\n\tmovk x11, #0xba98, lsl #32\n\tmovk x11, #0xfedc, lsl #48\n\t"
        "cmp x10, x11\n\tb.eq 3f\n\torr %0, %0, #2\n\t"
        "3:\n\t"
        "movz x11, #0xcccc\n\tmovk x11, #0x3333, lsl #16\n\tmovk x11, #0xaaaa, lsl #32\n\tmovk x11, #0x5555, lsl #48\n\t"
        "cmp x12, x11\n\tb.eq 5f\n\torr %0, %0, #4\n\t"
        "5:\n\t"
        "movz x11, #0x00ff\n\tmovk x11, #0x00ff, lsl #16\n\tmovk x11, #0x00ff, lsl #32\n\tmovk x11, #0x00ff, lsl #48\n\t"
        "cmp x13, x11\n\tb.eq 6f\n\torr %0, %0, #8\n\t"
        "b 6f\n\t"
        "4:\n\tmov %0, #16\n\t"
        "6:\n\t"
        : "=&r"(bad)
        : "r"(flag), "r"((unsigned)SIG_SPIN_MAX), "r"((unsigned)SIG_SPIN_MIN)
        : "x9", "x10", "x11", "x12", "x13", "x14", "cc", "memory");
#endif
    return (int)bad;
}

/* Delivery at the return from a system call: kill to self. */
static int probe_signal(void)
{
    uint8_t want[16][16], got[16][16];
    if (install(SIGUSR1, 0) != 0)
        return 3;
    vec_fill(0x3C, want);
    vec_load(want);
    if (raise(SIGUSR1) != 0)
        return 4;
    vec_store(got);
    if (memcmp(got, want, sizeof(got)) != 0)
        return 5;   /* the handler's vector registers, not the interrupted code's */
    if (g_sig_ran != 1 || g_sig_num != SIGUSR1)
        return 6;
    if (g_sig_code != SI_USER || g_sig_pid != (int)getpid())
        return 7;
    /* A signal is blocked inside its own handler and unblocked again
     * when it returns: the mask travels in the frame. */
    if (!(g_sig_mask & SIGBIT(SIGUSR1)))
        return 8;
    sigset_t now = 0;
    if (sigprocmask(SIG_BLOCK, NULL, &now) != 0 || (now & SIGBIT(SIGUSR1)))
        return 9;
    /* SA_NODEFER leaves it unblocked inside its own handler. */
    if (install(SIGUSR2, SA_NODEFER) != 0)
        return 10;
    if (raise(SIGUSR2) != 0 || g_sig_ran != 2)
        return 11;
    if (g_sig_mask & SIGBIT(SIGUSR2))
        return 12;
    /* SA_RESETHAND puts the default back before the handler runs, so
     * `handler` is what the query returns before and SIG_DFL after. */
    if (install(SIGUSR1, SA_RESETHAND) != 0)
        return 13;
    if (raise(SIGUSR1) != 0 || g_sig_ran != 3)
        return 14;
    struct sigaction old;
    if (sigaction(SIGUSR1, NULL, &old) != 0 || old.sa_handler != SIG_DFL)
        return 15;
    /* SIGKILL and SIGSTOP have no action to install. */
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    if (sigaction(SIGKILL, &ign, NULL) != -1 || errno != EINVAL)
        return 16;
    return 0;
}

/* Delivery at the return from an interrupt: a child sends the signal
 * while this process is spinning in user code. */
static int probe_signal_async(void)
{
    uint8_t want[16][16], got[16][16];
    if (install(SIGUSR1, 0) != 0)
        return 3;
    const char *argv[] = { "init", "--probe", "signal-poke", NULL };
    pid_t child = spawnve("/boot/init", argv, NULL, NULL, 0);
    if (child <= 0)
        return 4;
    /* After the spawn, not before it: the libc's string handling is
     * compiled with the vector registers available and uses them, so a
     * pattern loaded any earlier would be the library's by now. The
     * child sleeps first, which is what makes this ordering safe -- and
     * the spin loop reports how many times it went round, so a signal
     * that arrived before the loop started fails the probe rather than
     * passing it without having proved anything. */
    vec_fill(0x5A, want);
    vec_load(want);
    int bad = spin_until_signalled(&g_sig_ran);
    vec_store(got);
    if (bad)
        return 20 + bad;   /* 36: it never arrived; 52: too early to prove anything; else a register bitmap */
    if (memcmp(got, want, sizeof(got)) != 0)
        return 5;
    if (g_sig_num != SIGUSR1 || g_sig_code != SI_USER || g_sig_pid != child)
        return 6;
    int status = -1;
    if (waitpid(child, &status, 0) != child || status != 0)
        return 7;
    return 0;
}

static int probe_signal_poke(void)
{
    usleep(20000);   /* long enough that the parent is spinning by now */
    return kill(getppid(), SIGUSR1) == 0 ? 0 : 3;
}

/* Blocking: a blocked signal waits, is reported as pending, and is
 * delivered the moment it is unblocked. */
static int probe_signal_mask(void)
{
    if (install(SIGUSR1, 0) != 0)
        return 3;
    sigset_t block = SIGBIT(SIGUSR1), old = 0, pending = 0;
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0 || old != 0)
        return 4;
    if (raise(SIGUSR1) != 0)
        return 5;
    if (g_sig_ran != 0)
        return 6;   /* delivered although blocked */
    if (sigpending(&pending) != 0 || !(pending & SIGBIT(SIGUSR1)))
        return 7;
    sigset_t none = 0;
    if (sigprocmask(SIG_SETMASK, &none, &old) != 0 || old != SIGBIT(SIGUSR1))
        return 8;
    if (g_sig_ran != 1 || g_sig_num != SIGUSR1)
        return 9;   /* not delivered at the unblock */
    if (sigpending(&pending) != 0 || pending != 0)
        return 10;
    /* The mask a handler returns to is the one it interrupted, and it
     * comes back out of the frame: SIGUSR2 is blocked here, blocked
     * inside the handler, and blocked again after it returns. A frame
     * that carried no mask would leave it unblocked. */
    sigset_t hold = SIGBIT(SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &hold, NULL) != 0)
        return 15;
    if (raise(SIGUSR1) != 0 || g_sig_ran != 2)
        return 16;
    if (!(g_sig_mask & SIGBIT(SIGUSR2)))
        return 17;
    sigset_t back = 0;
    if (sigprocmask(SIG_BLOCK, NULL, &back) != 0 || back != SIGBIT(SIGUSR2))
        return 18;
    if (sigprocmask(SIG_SETMASK, &none, NULL) != 0)
        return 19;
    /* Asking to block SIGKILL and SIGSTOP is not an error, but it must
     * not take: a process that could block them could not be killed. */
    sigset_t all = ~0ul;
    if (sigprocmask(SIG_SETMASK, &all, NULL) != 0 || sigprocmask(SIG_BLOCK, NULL, &old) != 0)
        return 11;
    if (old & (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP)))
        return 12;
    if (sigprocmask(SIG_SETMASK, &none, NULL) != 0)
        return 13;
    /* SIG_IGN discards it instead. */
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    if (sigaction(SIGUSR1, &ign, NULL) != 0 || raise(SIGUSR1) != 0 || g_sig_ran != 2)
        return 14;
    return 0;
}

/* Delivery at the return from a fault: the handler is told the address,
 * maps a page there, and the store that faulted is run again. */
static int probe_signal_fault(void)
{
    if (install(SIGSEGV, 0) != 0)
        return 3;
    volatile unsigned *p = (volatile unsigned *)SIGFAULT_ADDR;
    *p = 0x1234u;
    if (g_sig_ran != 1 || g_sig_num != SIGSEGV)
        return 4;
    if (g_sig_code != SI_FAULT || g_sig_addr != SIGFAULT_ADDR)
        return 5;
    if (*p != 0x1234u)
        return 6;   /* the retried store did not land */
    return 0;
}

/* --- sessions, process groups and the terminal ------------------------------ */

static int probe_signal_sleep(void)
{
    usleep(300000);   /* long enough to still be here when the group is signalled */
    return 0;
}

/*
 * A group is signalled as a unit and nothing outside it is touched. The
 * sender deliberately stays out of the group it signals, because
 * `kill(-pgid)` would otherwise reach the process running the check.
 */
static int probe_signal_group(void)
{
    const char *argv[] = { "init", "--probe", "signal-sleep", NULL };
    pid_t a = spawnve_pgrp("/boot/init", argv, NULL, NULL, 0, 0);   /* a group of its own */
    if (a <= 0)
        return 3;
    pid_t b = spawnve_pgrp("/boot/init", argv, NULL, NULL, 0, a);
    pid_t c = spawnve_pgrp("/boot/init", argv, NULL, NULL, 0, a);
    pid_t d = spawnve_pgrp("/boot/init", argv, NULL, NULL, 0, 0);   /* outside it */
    if (b <= 0 || c <= 0 || d <= 0)
        return 4;
    if (getpgid(a) != a || getpgid(b) != a || getpgid(c) != a)
        return 5;
    if (getpgid(d) != d || getpgid(0) == a)
        return 6;
    if (kill(-a, SIGTERM) != 0)
        return 7;
    int st = -1;
    if (waitpid(a, &st, 0) != a || st != 128 + SIGTERM)
        return 8;
    if (waitpid(b, &st, 0) != b || st != 128 + SIGTERM)
        return 9;
    if (waitpid(c, &st, 0) != c || st != 128 + SIGTERM)
        return 10;
    if (waitpid(d, &st, 0) != d || st != 0)
        return 11;   /* the process outside the group was signalled too */
    /* The group is empty now, and an empty group is no target at all. */
    if (kill(-a, SIGTERM) != -1 || errno != ESRCH)
        return 12;
    /* A process that does not exist is not a target either. */
    if (setpgid(999999, 0) != -1 || errno != ESRCH)
        return 13;
    /* Nor is a group with nobody in it a group to join: `a`'s group
     * emptied when its members died. */
    if (setpgid(0, a) != -1 || errno != EPERM)
        return 14;
    /* A session leader's group is its session's name, so this process
     * -- which the kernel started, and which therefore leads its own
     * session -- cannot change group at all. */
    if (setpgid(0, 0) != -1 || errno != EPERM)
        return 15;
    /* A child can, and its parent may move it: one more sleeper, put
     * into a group of its own after the fact. */
    pid_t e = spawnve("/boot/init", argv, NULL, NULL, 0);
    if (e <= 0)
        return 16;
    if (getpgid(e) != getpgid(0))
        return 17;
    if (setpgid(e, e) != 0 || getpgid(e) != e)
        return 18;
    if (kill(-e, SIGTERM) != 0)
        return 19;
    if (waitpid(e, &st, 0) != e || st != 128 + SIGTERM)
        return 20;
    return 0;
}

/*
 * A session is started once: after it the caller leads a group, and a
 * group leader has no second session to start. The process the kernel
 * starts for a probe leads its own group already, so the checks run in
 * a child of it, which inherits a group it does not lead.
 */
static int probe_signal_setsid_child(void)
{
    /* Inheritance is checked from this side, not the parent's: the
     * parent cannot look at this group without racing the setsid below,
     * and on a fast enough machine it loses -- which is how this test
     * first failed, on one architecture's CI and not the other's. */
    if (getpgid(0) != getpgid(getppid()))
        return 20;   /* a child starts in its parent's group */
    if (getpgid(0) == getpid())
        return 21;   /* and does not lead it, or the checks below prove nothing */
    pid_t was = getsid(0);
    if (setsid() != getpid())
        return 22;
    if (getsid(0) != getpid() || getpgid(0) != getpid() || getsid(0) == was)
        return 23;
    if (setsid() != -1 || errno != EPERM)
        return 24;
    return 0;
}

static int probe_signal_setsid(void)
{
    const char *argv[] = { "init", "--probe", "signal-setsid-child", NULL };
    pid_t child = spawnve("/boot/init", argv, NULL, NULL, 0);
    if (child <= 0)
        return 3;
    int st = -1;
    if (waitpid(child, &st, 0) != child)
        return 4;
    return st == 0 ? 0 : st;
}

/* Claim the terminal and wait to be interrupted. The kernel side of the
 * test types the ^C; reaching the end of this means it did not arrive,
 * because the default action of SIGINT is to end the process. */
static int probe_signal_tty(void)
{
    if (tcsetpgrp(0, getpgrp()) != 0)
        return 3;
    if (tcgetpgrp(0) != getpgrp())
        return 4;
    for (int i = 0; i < 500; i++)
        usleep(20000);
    return 5;
}

/*
 * The same, with SIGINT caught and SIGQUIT left fatal. The kernel side
 * types `^\` and `^C` as one batch: both must arrive, so this process
 * must die of the quit. A tty that sent only the last signal of a batch
 * would deliver the interrupt alone, the handler would run, and this
 * would sit here until its own deadline.
 */
static int probe_signal_tty_quit(void)
{
    if (install(SIGINT, 0) != 0)
        return 3;
    if (tcsetpgrp(0, getpgrp()) != 0)
        return 4;
    for (int i = 0; i < 500; i++)
        usleep(20000);
    return 5;
}

/* A job: it sits there until something stops or kills it. */
static int probe_signal_job(void)
{
    for (int i = 0; i < 1000; i++)
        usleep(20000);
    return 9;
}

/* A job that reads the terminal, which is the thing a background job
 * must not be allowed to do. */
static int probe_signal_job_read(void)
{
    char b = 0;
    ssize_t n = read(0, &b, 1);
    if (n < 0)
        return errno == EIO ? 30 : 31;
    return 32;
}

/*
 * ^Z, in the shape a shell uses: this process leads the session and
 * holds the terminal, and the *job* is a child in a group of its own.
 * That is also what makes the job's group non-orphaned -- its parent is
 * here, in the same session and a different group -- so the terminal is
 * allowed to stop it. The kernel side types the keystroke; everything
 * below is bounded, because a stop that never arrives would otherwise
 * hang the boot rather than fail it.
 */
static int probe_signal_tty_stop(void)
{
    const char *argv[] = { "init", "--probe", "signal-job", NULL };
    pid_t c = spawnve_pgrp("/boot/init", argv, NULL, NULL, 0, 0);
    if (c <= 0)
        return 3;
    if (tcsetpgrp(0, c) != 0)   /* the child leads its own group, named by its pid */
        return 4;
    int st = -1;
    int stopped = 0;
    for (int i = 0; i < 200 && !stopped; i++) {   /* at most 4 s; the stop needs milliseconds */
        pid_t got = waitpid(c, &st, WNOHANG | WUNTRACED);
        if (got == c && WIFSTOPPED(st))
            stopped = 1;
        else if (got == c)
            return 5;   /* it died instead of stopping */
        else
            usleep(20000);
    }
    if (!stopped)
        return 6;
    if (WSTOPSIG(st) != SIGTSTP)
        return 7;
    /* Still there to be continued, and the continue reaches the group. */
    if (kill(-c, SIGCONT) != 0)
        return 8;
    if (kill(-c, SIGTERM) != 0)
        return 9;
    if (waitpid(c, &st, 0) != c || st != 128 + SIGTERM)
        return 10;
    return 0;
}

/*
 * Reading the terminal from the background, both halves of the rule at
 * once. The child is in its own group with this process as its parent,
 * so its group is *not* orphaned and it is stopped with SIGTTIN. This
 * process is in its own group too and has no parent at all -- the
 * kernel started it -- so its group *is* orphaned, and the same read
 * must hand it -EIO rather than stop it, because nothing would be left
 * to continue it.
 */
static int probe_signal_tty_background(void)
{
    /* The terminal is claimed *before* the child exists: a child that
     * reached its read while the terminal still had no foreground group
     * would be allowed to read, and would block there for ever rather
     * than being stopped. */
    if (tcsetpgrp(0, getpgrp()) != 0)
        return 3;
    const char *argv[] = { "init", "--probe", "signal-job-read", NULL };
    pid_t c = spawnve_pgrp("/boot/init", argv, NULL, NULL, 0, 0);
    if (c <= 0)
        return 4;
    int st = -1;
    int stopped = 0;
    for (int i = 0; i < 200 && !stopped; i++) {   /* at most 4 s */
        pid_t got = waitpid(c, &st, WNOHANG | WUNTRACED);
        if (got == c && WIFSTOPPED(st))
            stopped = 1;
        else if (got == c)
            return 5;   /* it read the line, or died */
        else
            usleep(20000);
    }
    if (!stopped)
        return 6;
    if (WSTOPSIG(st) != SIGTTIN)
        return 7;
    if (kill(-c, SIGKILL) != 0 || waitpid(c, &st, 0) != c)
        return 8;

    /*
     * The orphaned half. An orphaned group needs a member whose parent
     * is gone, so it takes a generation: this process spawns B, B
     * spawns G in a group of its own and exits, and G is left in the
     * terminal's session with nothing above it that could ever continue
     * it. G reports through a pipe, because it is a grandchild and
     * cannot be waited for here.
     */
    int q[2];
    if (pipe(q) != 0)
        return 9;
    struct spawn_handle map[] = { { .child = 0, .parent = 0 }, { .child = 1, .parent = 1 },
                                  { .child = 2, .parent = 2 }, { .child = 3, .parent = q[1] } };
    const char *bargv[] = { "init", "--probe", "signal-job-orphan-parent", NULL };
    pid_t b = spawnve("/boot/init", bargv, NULL, map, 4);
    close(q[1]);
    if (b <= 0)
        return 10;
    if (waitpid(b, &st, 0) != b || st != 0)
        return 11;
    char verdict = 0;
    if (read(q[0], &verdict, 1) != 1)
        return 12;   /* the grandchild never answered */
    close(q[0]);
    return verdict == 'y' ? 0 : 13;
}

/* Spawns the orphan into a group of its own and gets out of the way. */
static int probe_signal_job_orphan_parent(void)
{
    struct spawn_handle map[] = { { .child = 0, .parent = 0 }, { .child = 1, .parent = 1 },
                                  { .child = 2, .parent = 2 }, { .child = 3, .parent = 3 } };
    const char *argv[] = { "init", "--probe", "signal-job-orphan", NULL };
    return spawnve_pgrp("/boot/init", argv, NULL, map, 4, 0) > 0 ? 0 : 20;
}

/* Orphaned, in the terminal's session, not the foreground group: the
 * read must be refused with EIO rather than stop it, because a stop
 * here would last for ever. */
static int probe_signal_job_orphan(void)
{
    usleep(200000);   /* long enough that the parent is gone */
    char b = 0;
    ssize_t n = read(0, &b, 1);
    char verdict = (n < 0 && errno == EIO) ? 'y' : 'n';
    (void)write(3, &verdict, 1);
    return 0;
}

/* Another session cannot take the terminal, or even ask about it. */
static int probe_signal_tty_steal(void)
{
    if (tcsetpgrp(0, getpgrp()) != -1 || errno != EPERM)
        return 3;
    if (tcgetpgrp(0) != -1 || errno != ENOTTY)
        return 4;
    return 0;
}

/* --- job control ------------------------------------------------------------ */

/* Stops itself and, once continued, says so with an exit status nothing
 * else produces. */
static int probe_signal_stop_child(void)
{
    if (raise(SIGSTOP) != 0)
        return 20;
    return 7;
}

/* A stop is an event a parent waits for, reported once per stop. */
static int probe_signal_stop(void)
{
    const char *argv[] = { "init", "--probe", "signal-stop-child", NULL };
    pid_t c = spawnve("/boot/init", argv, NULL, NULL, 0);
    if (c <= 0)
        return 3;
    int st = -1;
    if (waitpid(c, &st, WUNTRACED) != c)
        return 4;
    if (!WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP)
        return 5;
    if (WIFEXITED(st) || WIFSIGNALED(st))
        return 6;   /* a stopped status must not read as either */
    /* Edge-triggered: the same stop is not reported twice. */
    if (waitpid(c, &st, WNOHANG | WUNTRACED) != 0)
        return 7;
    if (kill(c, SIGCONT) != 0)
        return 8;
    if (waitpid(c, &st, WUNTRACED | WCONTINUED) != c || !WIFCONTINUED(st))
        return 9;
    if (waitpid(c, &st, 0) != c || st != 7)
        return 10;
    return 0;
}

/* Stopped is not a place a process can hide from SIGKILL. */
static int probe_signal_stop_kill(void)
{
    const char *argv[] = { "init", "--probe", "signal-stop-child", NULL };
    pid_t c = spawnve("/boot/init", argv, NULL, NULL, 0);
    if (c <= 0)
        return 3;
    int st = -1;
    if (waitpid(c, &st, WUNTRACED) != c || !WIFSTOPPED(st))
        return 4;
    if (kill(c, SIGKILL) != 0)
        return 5;
    if (waitpid(c, &st, 0) != c || st != 128 + SIGKILL)
        return 6;
    return 0;
}

/* SIGSTOP cannot be caught, blocked or ignored -- the one rule that
 * makes it worth having. */
static int probe_signal_stop_mask(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sig_handler;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSTOP, &sa, NULL) != -1 || errno != EINVAL)
        return 3;
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGSTOP, &sa, NULL) != -1 || errno != EINVAL)
        return 4;
    sigset_t all = ~0ul, now = 0;
    if (sigprocmask(SIG_SETMASK, &all, NULL) != 0 || sigprocmask(SIG_BLOCK, NULL, &now) != 0)
        return 5;
    if (now & SIGBIT(SIGSTOP))
        return 6;
    sigset_t none = 0;
    if (sigprocmask(SIG_SETMASK, &none, NULL) != 0)
        return 7;
    return 0;
}

/*
 * Reads one line from the terminal. It is started in the background, so
 * its own read is what stops it (SIGTTIN); once it has been made the
 * foreground group and continued, the same read must be *restarted* and
 * return the line. A read that was failed rather than restarted comes
 * back -EINTR and this says so.
 *
 * The stop is caused by the call under test, which is the only way to
 * aim one reliably: two earlier versions of this test tried to hit a
 * sleeping child from the parent, and both passed with the restart
 * deliberately broken because the stop kept landing between calls.
 */
static int probe_signal_stop_reader(void)
{
    if (write(3, "r", 1) != 1)
        return 22;
    char buf[64];
    ssize_t n = read(0, buf, sizeof(buf));
    if (n < 0)
        return errno == EINTR ? 20 : 21;   /* failed, not restarted */
    return n > 0 ? 0 : 23;
}

/*
 * A system call cut short by a stop is restarted, not failed. The child
 * reads the terminal from the background, which stops it where it
 * stands; this process then hands it the terminal and continues it, and
 * the kernel side types a line for it to find.
 */
static int probe_signal_stop_restart(void)
{
    if (tcsetpgrp(0, getpgrp()) != 0)
        return 3;
    int ready[2];
    if (pipe(ready) != 0)
        return 4;
    struct spawn_handle map[] = { { .child = 0, .parent = 0 }, { .child = 1, .parent = 1 },
                                  { .child = 2, .parent = 2 }, { .child = 3, .parent = ready[1] } };
    const char *argv[] = { "init", "--probe", "signal-stop-reader", NULL };
    pid_t c = spawnve_pgrp("/boot/init", argv, NULL, map, 4, 0);
    close(ready[1]);
    if (c <= 0)
        return 5;
    char r = 0;
    if (read(ready[0], &r, 1) != 1)
        return 6;
    close(ready[0]);
    int st = -1;
    int stopped = 0;
    for (int i = 0; i < 200 && !stopped; i++) {   /* at most 4 s */
        pid_t got = waitpid(c, &st, WNOHANG | WUNTRACED);
        if (got == c && WIFSTOPPED(st))
            stopped = 1;
        else if (got == c)
            return 7;   /* it read something, or died, instead of stopping */
        else
            usleep(20000);
    }
    if (!stopped)
        return 8;
    if (WSTOPSIG(st) != SIGTTIN)
        return 9;
    /* Its turn at the terminal, and on with it. */
    if (tcsetpgrp(0, c) != 0)
        return 10;
    if (kill(-c, SIGCONT) != 0)
        return 11;
    if (waitpid(c, &st, 0) != c)
        return 12;
    return st == 0 ? 0 : 30 + st;
}

/*
 * The continue that outruns the thread it is continuing: a stop and a
 * SIGCONT sent back to back, before the target has reached the return
 * to user mode where it would park. It must end up running. A park that
 * trusted the flag it was woken with rather than re-reading the
 * process's state would stop a process that has already been continued,
 * so the wait below is bounded -- that failure is a hang, and a test
 * that hangs is not a test.
 */
static int probe_signal_stop_late(void)
{
    const char *argv[] = { "init", "--probe", "signal-sleep", NULL };
    /* Six rounds, not twenty: the ordering under test is decided by the
     * two kills arriving before the child has run at all, which one
     * round already achieves. The rest are for luck, and each costs the
     * child's own sleep. */
    for (int round = 0; round < 6; round++) {
        pid_t c = spawnve("/boot/init", argv, NULL, NULL, 0);
        if (c <= 0)
            return 3;
        if (kill(c, SIGSTOP) != 0)
            return 4;
        if (kill(c, SIGCONT) != 0)
            return 5;
        int st = -1;
        int done = 0;
        for (int i = 0; i < 200; i++) {   /* at most 2 s, then it is stuck */
            pid_t got = waitpid(c, &st, WNOHANG | WUNTRACED);
            if (got == c && WIFSTOPPED(st)) {
                /* A stop reported *after* the continue: the child
                 * parked on a flag its stop no longer owns, and told
                 * its parent so. Nothing continued it, so this is also
                 * how the hang begins. */
                kill(c, SIGCONT);
                kill(c, SIGKILL);
                waitpid(c, &st, 0);
                return 8;
            }
            if (got == c) {
                done = 1;
                break;
            }
            usleep(10000);
        }
        if (!done) {
            kill(c, SIGCONT);
            kill(c, SIGKILL);
            waitpid(c, &st, 0);
            return 6;   /* parked after the continue: stopped for good */
        }
        if (st != 0)
            return 7;
    }
    return 0;
}

static int signal_probe(const char *kind)
{
    if (strcmp(kind, "signal") == 0)
        return probe_signal();
    if (strcmp(kind, "signal-async") == 0)
        return probe_signal_async();
    if (strcmp(kind, "signal-poke") == 0)
        return probe_signal_poke();
    if (strcmp(kind, "signal-mask") == 0)
        return probe_signal_mask();
    if (strcmp(kind, "signal-fault") == 0)
        return probe_signal_fault();
    if (strcmp(kind, "signal-sleep") == 0)
        return probe_signal_sleep();
    if (strcmp(kind, "signal-group") == 0)
        return probe_signal_group();
    if (strcmp(kind, "signal-setsid") == 0)
        return probe_signal_setsid();
    if (strcmp(kind, "signal-setsid-child") == 0)
        return probe_signal_setsid_child();
    if (strcmp(kind, "signal-tty") == 0)
        return probe_signal_tty();
    if (strcmp(kind, "signal-tty-steal") == 0)
        return probe_signal_tty_steal();
    if (strcmp(kind, "signal-tty-quit") == 0)
        return probe_signal_tty_quit();
    if (strcmp(kind, "signal-stop-child") == 0)
        return probe_signal_stop_child();
    if (strcmp(kind, "signal-stop") == 0)
        return probe_signal_stop();
    if (strcmp(kind, "signal-stop-kill") == 0)
        return probe_signal_stop_kill();
    if (strcmp(kind, "signal-stop-mask") == 0)
        return probe_signal_stop_mask();
    if (strcmp(kind, "signal-stop-reader") == 0)
        return probe_signal_stop_reader();
    if (strcmp(kind, "signal-stop-restart") == 0)
        return probe_signal_stop_restart();
    if (strcmp(kind, "signal-stop-late") == 0)
        return probe_signal_stop_late();
    if (strcmp(kind, "signal-tty-stop") == 0)
        return probe_signal_tty_stop();
    if (strcmp(kind, "signal-tty-background") == 0)
        return probe_signal_tty_background();
    if (strcmp(kind, "signal-job") == 0)
        return probe_signal_job();
    if (strcmp(kind, "signal-job-read") == 0)
        return probe_signal_job_read();
    if (strcmp(kind, "signal-job-orphan-parent") == 0)
        return probe_signal_job_orphan_parent();
    if (strcmp(kind, "signal-job-orphan") == 0)
        return probe_signal_job_orphan();
    return 2;
}

/* --- the privilege boundary (Prompt #3, 3.6) --------------------------------
 *
 * The parent (root) prepares a root-only directory and file, then spawns
 * itself with --unpriv-test. The child drops to uid/gid 1000 and tries
 * every privileged operation and every root-owned object it can reach;
 * each must be refused. Its exit status is the number of failures. */

#define UNPRIV_UID 1000u

static int g_unpriv_failures;
#define UCHECK(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("unpriv: check failed: %s (errno %d)\n", #cond, errno);    \
            g_unpriv_failures++;                                              \
        }                                                                     \
    } while (0)

static int unpriv_test(void)
{
    pid_t parent = getppid();
    uid_t r, e, s;
    UCHECK(geteuid() == 0);
    UCHECK(setresgid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID) == 0);
    UCHECK(setresuid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID) == 0);
    UCHECK(getresuid(&r, &e, &s) == 0 && r == UNPRIV_UID && e == UNPRIV_UID && s == UNPRIV_UID);
    UCHECK(getgid() == UNPRIV_UID && getegid() == UNPRIV_UID);

    /* No way back up. */
    UCHECK(setresuid(0, 0, 0) < 0 && errno == EPERM);
    UCHECK(setresuid((uid_t)-1, 0, (uid_t)-1) < 0 && errno == EPERM);
    UCHECK(setuid(0) < 0 && errno == EPERM);
    UCHECK(setresgid(0, (gid_t)-1, (gid_t)-1) < 0 && errno == EPERM);
    gid_t g = 5;
    UCHECK(setgroups(1, &g) < 0 && errno == EPERM);
    UCHECK(setresuid((uid_t)-1, UNPRIV_UID, (uid_t)-1) == 0);   /* an id it holds: allowed */

    /* Privileged system calls. */
    UCHECK(mount("none", "/mnt", "ramfs", 0) < 0 && errno == EPERM);
    /* And a mount namespace of its own, which decides what filesystems
     * a whole subtree of processes sees. */
    static const char *const t_argv[] = { "true", NULL };
    UCHECK(spawnve_mountns("/bin/true", t_argv, NULL, NULL, 0) < 0 && errno == EPERM);
    UCHECK(spawnve_utsns("/bin/true", t_argv, NULL, NULL, 0) < 0 && errno == EPERM);
    /* Reading the name is fine; renaming the machine is not. */
    char uh[HOST_NAME_MAX];
    UCHECK(gethostname(uh, sizeof(uh)) >= 0);
    UCHECK(sethostname("stolen", 6) < 0 && errno == EPERM);
    UCHECK(umount("/") < 0 && errno == EPERM);
    UCHECK(kill(parent, SIGTERM) < 0 && errno == EPERM);      /* root's process; must survive */

    /*
     * P1: /proc shows this process exactly what procinfo would. The
     * parent is root's; it must be neither listed nor openable, and
     * ENOENT rather than EACCES, since "not permitted" would confirm
     * the pid is in use. A listing that names what it will not open is
     * a leak with extra steps.
     */
    char ppath[64];
    struct stat pst;
    snprintf(ppath, sizeof(ppath), "/proc/%d", parent);
    UCHECK(stat(ppath, &pst) < 0 && errno == ENOENT);
    snprintf(ppath, sizeof(ppath), "/proc/%d/status", parent);
    UCHECK(stat(ppath, &pst) < 0 && errno == ENOENT);
    {
        char own[32];
        snprintf(own, sizeof(own), "%d", getpid());
        UCHECK(lists("/proc", own));             /* its own is there */
        char theirs[32];
        snprintf(theirs, sizeof(theirs), "%d", parent);
        UCHECK(!lists("/proc", theirs));         /* root's is not */
    }
    char log[256];
    UCHECK(klog_read(log, sizeof(log)) < 0 && errno == EPERM);

    /* Root-owned objects. */
    UCHECK(open("/dev/vmm", O_RDWR) < 0 && errno == EACCES);              /* 0600 root */
    UCHECK(open("/tmp/privtest/secret", O_RDONLY) < 0 && errno == EACCES); /* 0700 directory */
    UCHECK(chdir("/tmp/privtest") < 0 && errno == EACCES);
    UCHECK(open("/etc/rc", O_WRONLY) < 0 && errno == EACCES);               /* 0644 root */
    int fd = open("/etc/rc", O_RDONLY);
    UCHECK(fd >= 0);                                                       /* world-readable */
    if (fd >= 0)
        close(fd);
    UCHECK(mkdir("/etc/unpriv", 0755) < 0 && errno == EACCES);              /* 0755 root directory */
    UCHECK(unlink("/etc/rc") < 0 && errno == EACCES);
    UCHECK(rename("/etc/rc", "/tmp/rc") < 0 && errno == EACCES);
    const char *argv[] = { "secret", NULL };
    UCHECK(spawnve("/tmp/privtest/secret", argv, NULL, NULL, 0) < 0 && errno == EACCES);
    UCHECK(spawnve("/tmp/privtest/../privtest/secret", argv, NULL, NULL, 0) < 0 && errno == EACCES);

    /* The sticky bit on /tmp: root's entry there is not the child's to remove or rename. */
    UCHECK(unlink("/tmp/rootowned") < 0 && errno == EACCES);
    UCHECK(rename("/tmp/rootowned", "/tmp/rootowned2") < 0 && errno == EACCES);

    /* Reserved ports are judged at bind time: a socket the child creates
     * now, and one it inherited from its privileged parent (handle 3). */
    struct sockaddr low;
    memset(&low, 0, sizeof(low));
    low.sa_family = AF_INET;
    low.sa_port = 80;
    inet_pton(AF_INET, "127.0.0.1", low.sa_addr);
    int sk = socket(AF_INET, SOCK_STREAM, 0);
    UCHECK(sk >= 0 && bind(sk, &low, sizeof(low)) < 0 && errno == EPERM);
    if (sk >= 0)
        close(sk);
    UCHECK(bind(3, &low, sizeof(low)) < 0 && errno == EPERM);
    close(3);

    /* What it may do: its own files in /tmp, and running installed programs. */
    fd = open("/tmp/unpriv.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    UCHECK(fd >= 0 && write(fd, "mine", 4) == 4);
    if (fd >= 0)
        close(fd);
    struct stat st;
    UCHECK(stat("/tmp/unpriv.txt", &st) == 0 && st.st_uid == UNPRIV_UID && st.st_gid == UNPRIV_UID &&
           st.st_mode == 0600);
    UCHECK(mkdir("/tmp/unprivdir", 0700) == 0 && rmdir("/tmp/unprivdir") == 0);
    UCHECK(unlink("/tmp/unpriv.txt") == 0);
    const char *true_argv[] = { "true", NULL };
    pid_t t = spawnvp("true", true_argv, NULL, 0);
    int status = -1;
    UCHECK(t > 0 && waitpid(t, &status, 0) == t && status == 0);
    return g_unpriv_failures;
}

static void priv_selftest(void)
{
    /* Root's fixtures: a directory nobody else may enter, a file inside it. */
    CHECK(mkdir("/tmp/privtest", 0700) == 0);
    int fd = open("/tmp/privtest/secret", O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && write(fd, "top\n", 4) == 4);
    if (fd >= 0)
        close(fd);
    struct stat st;
    CHECK(stat("/tmp/privtest/secret", &st) == 0 && st.st_uid == 0 && st.st_mode == 0600);
    CHECK(stat("/tmp", &st) == 0 && st.st_mode == 01777);
    fd = open("/tmp/rootowned", O_WRONLY | O_CREAT, 0666);   /* world-writable, but root's: sticky /tmp protects it */
    CHECK(fd >= 0);
    if (fd >= 0)
        close(fd);
    uid_t r, e, s;
    CHECK(getresuid(&r, &e, &s) == 0 && r == 0 && e == 0 && s == 0);
    CHECK(getgroups(0, NULL) == 0);

    /* A socket created by root, handed to the child as handle 3. */
    int rootsock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(rootsock >= 0);
    struct spawn_handle map[] = { { .child = 0, .parent = 0 }, { .child = 1, .parent = 1 },
                                  { .child = 2, .parent = 2 }, { .child = 3, .parent = rootsock } };
    const char *argv[] = { "init", "--unpriv-test", NULL };
    pid_t pid = spawnve("/boot/init", argv, NULL, map, 4);
    CHECK(pid > 0);
    close(rootsock);
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(status == 0);   /* the number of refused-but-allowed operations */

    CHECK(unlink("/tmp/rootowned") == 0);
    CHECK(unlink("/tmp/privtest/secret") == 0 && rmdir("/tmp/privtest") == 0);
    puts("usertest: privilege boundary ok");
}

/* Read a whole file, NUL-terminated. Returns bytes read, or -1. */
static ssize_t slurp(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;
    ssize_t got = read(fd, buf, n - 1);
    close(fd);
    if (got < 0)
        return -1;
    buf[got] = 0;
    return got;
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        CHECK(close(fd) == 0);
    }
}

/*
 * Is the supervisor up? The same question `svc status` answers, asked
 * without spawning anything: every poll through the command is a
 * process, and a loop of them costs more than the thing being waited
 * for. That is what pushed init's self-test past the five-second
 * budget the kernel gives it on the slower architecture.
 */
static int svc_up(const char *name)
{
    char path[64], buf[32] = { 0 };
    snprintf(path, sizeof(path), "/run/svc/%s.pid", name);
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    pid_t p = (pid_t)atoi(buf);
    return p > 0 && kill(p, 0) == 0;
}

static int svc_run(const char *a, const char *b)
{
    const char *argv[] = { "svc", a, b, NULL };
    pid_t pid = spawnve("/sbin/svc", argv, NULL, NULL, 0);
    if (pid < 0)
        return -1;
    int status = -1;
    return waitpid(pid, &status, 0) == pid ? status : -1;
}

/*
 * The service manager (docs/userland/design.md, "Services"; U8, U9,
 * U10). Definitions are written here rather than shipped, so each test
 * says on the spot what it is testing.
 */
static void svc_selftest(void)
{
    CHECK(mkdir("/etc/svc", 0755) == 0 || errno == EEXIST);

    /* U9: an unknown key fails the service rather than starting it with
     * whatever the typo did not say. */
    write_file("/etc/svc/typo", "exec /bin/true\nrooot /tmp\n");
    CHECK(svc_run("start", "typo") != 0);
    CHECK(svc_run("status", "typo") != 0);   /* and it is not running */

    /*
     * U9 again, and the sharper half: a number that is not a number.
     * `user daemon` through atoi is uid 0, so a typo in the one key
     * that reduces privilege would have granted the most. Every number
     * in a definition is parsed strictly, so each of these fails the
     * service rather than defaulting.
     */
    write_file("/etc/svc/baduser", "exec /bin/true\nuser daemon\n");
    CHECK(svc_run("start", "baduser") != 0);
    /* A uid past what the field can hold is refused; one inside it is
     * a uid like any other. 4000000000 is above INT_MAX, which is
     * where a signed field read it as "no user set" and therefore as
     * root -- it must now start, and start as that uid. */
    write_file("/etc/svc/hugeuser", "exec /bin/true\nuser 99999999999999999999\n");
    CHECK(svc_run("start", "hugeuser") != 0);

    /*
     * And the credentials are really applied, which is the thing that
     * matters and which "it started" does not show. The service asks to
     * rename the machine, which only root may do, as uid 4000000000 --
     * chosen because it is above INT_MAX, where a signed field read it
     * as "no user set" and the service ran as root. Refused, the
     * machine keeps its name; run as root it would succeed, which is
     * exactly the bug this guards.
     */
    char host_before[HOST_NAME_MAX];
    CHECK(gethostname(host_before, sizeof(host_before)) >= 0);
    write_file("/etc/svc/unprivsvc", "exec /sbin/hostname stolen-by-service\nuser 4000000000\n");
    CHECK(svc_run("start", "unprivsvc") == 0);   /* the supervisor ran it */
    char ulog[512];
    for (int i = 0; i < 200; i++) {
        if (slurp("/var/log/svc/unprivsvc", ulog, sizeof(ulog)) > 0 && strstr(ulog, "exited with") != NULL)
            break;
        cosmo_sleep_ns(5000000ULL);
    }
    CHECK(strstr(ulog, "exited with status 0") == NULL);   /* it was refused */
    char host_after[HOST_NAME_MAX];
    CHECK(gethostname(host_after, sizeof(host_after)) >= 0);
    CHECK(strcmp(host_before, host_after) == 0);
    write_file("/etc/svc/badnum", "exec /bin/true\nretries plenty\n");
    CHECK(svc_run("start", "badnum") != 0);
    write_file("/etc/svc/badlimit", "exec /bin/true\nlimit-nofile lots\n");
    CHECK(svc_run("start", "badlimit") != 0);

    /* A service whose program does not exist did not start, and says
     * so: reporting success would make `svc boot` start its
     * dependents (U10). */
    write_file("/etc/svc/missing", "exec /bin/nothing-here\n");
    CHECK(svc_run("start", "missing") != 0);

    /* And one that runs once and finishes is a success, not a failure
     * -- it is gone before the pid file can be seen, which is the case
     * a liveness poll alone gets wrong. */
    write_file("/etc/svc/oneshot", "exec /bin/true\n");
    CHECK(svc_run("start", "oneshot") == 0);

    /* U8: a service that always fails is restarted, with a wait between
     * tries, and then given up on. Two retries at 60 ms and 120 ms, so
     * a supervisor that did not wait would come back too fast. */
    write_file("/etc/svc/flap",
               "exec /bin/false\nrestart on-failure\nretries 2\nbackoff-ms 60\n");
    uint64_t t0 = cosmo_clock_ns();
    CHECK(svc_run("start", "flap") == 0);
    /* The supervisor exits by itself once it gives up; wait for it. */
    int gone = 0;
    for (int i = 0; i < 400; i++) {
        if (!svc_up("flap")) {
            gone = 1;
            break;
        }
        cosmo_sleep_ns(10000000ULL);
    }
    CHECK(gone);
    uint64_t elapsed = cosmo_clock_ns() - t0;
    CHECK(elapsed >= 180000000ULL);   /* 60 + 120 ms of backoff at least */
    char log[1024];
    CHECK(slurp("/var/log/svc/flap", log, sizeof(log)) > 0);
    CHECK(strstr(log, "giving up after 2 restarts") != NULL);
    CHECK(strstr(log, "restarting in 60 ms") != NULL);
    CHECK(strstr(log, "restarting in 120 ms") != NULL);   /* doubled, not repeated */

    /*
     * U10: a dependency that did not start stops what depends on it and
     * the message names it, and a cycle is refused rather than run in
     * some order. Both are things `svc boot` says on its standard
     * error, so the child gets a file for it and this reads it back --
     * a non-zero exit alone would not say which of the two happened.
     */
    write_file("/etc/svc/broken", "exec /bin/true\nnonsense 1\n");
    write_file("/etc/svc/dependent", "exec /bin/true\nafter broken\n");
    write_file("/etc/svc/loop-a", "exec /bin/true\nafter loop-b\n");
    write_file("/etc/svc/loop-b", "exec /bin/true\nafter loop-a\n");
    int errfd = open("/tmp/svcboot.err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(errfd >= 0);
    struct spawn_handle bmap[] = { { .child = 0, .parent = 0 },
                                   { .child = 1, .parent = 1 },
                                   { .child = 2, .parent = errfd } };
    const char *boot_argv[] = { "svc", "boot", NULL };
    pid_t bp = spawnve("/sbin/svc", boot_argv, NULL, bmap, 3);
    CHECK(bp > 0);
    int bstatus = -1;
    CHECK(waitpid(bp, &bstatus, 0) == bp);
    CHECK(bstatus != 0);   /* the cycle and the bad definition are both failures */
    CHECK(close(errfd) == 0);
    char err[1024];
    CHECK(slurp("/tmp/svcboot.err", err, sizeof(err)) > 0);
    CHECK(strstr(err, "dependent: not started: broken did not start") != NULL);
    CHECK(strstr(err, "dependency cycle among:") != NULL);
    CHECK(strstr(err, "loop-a") != NULL && strstr(err, "loop-b") != NULL);

    /* A long-running service can be stopped, and stopping it takes the
     * service with the supervisor. */
    write_file("/etc/svc/sleeper", "exec /bin/sleep 30\nrestart always\nretries 9\n");
    CHECK(svc_run("start", "sleeper") == 0);
    CHECK(svc_run("status", "sleeper") == 0);
    CHECK(svc_run("stop", "sleeper") == 0);
    CHECK(svc_run("status", "sleeper") != 0);

    /*
     * Leave nothing behind. A supervisor still exiting when this
     * process does becomes an orphan for real init to reap, and the
     * kernel's process-count self-test counts processes -- a test that
     * litters is a test that makes another one flaky.
     */
    static const char *const written[] = { "typo",     "flap",    "broken",  "dependent", "loop-a",
                                           "loop-b",   "sleeper", "baduser", "badnum",    "badlimit",
                                           "missing",  "oneshot", "hugeuser", "unprivsvc" };
    static const char *const shipped[] = { "hello", "greeter" };
    for (size_t i = 0; i < sizeof(written) / sizeof(written[0]); i++)
        (void)svc_run("stop", written[i]);
    for (size_t i = 0; i < sizeof(shipped) / sizeof(shipped[0]); i++)
        (void)svc_run("stop", shipped[i]);
    for (int i = 0; i < 500; i++) {
        int any = 0;
        for (size_t k = 0; k < sizeof(written) / sizeof(written[0]); k++)
            any |= svc_up(written[k]);
        for (size_t k = 0; k < sizeof(shipped) / sizeof(shipped[0]); k++)
            any |= svc_up(shipped[k]);
        if (!any)
            break;
        cosmo_sleep_ns(5000000ULL);
    }
    for (size_t i = 0; i < sizeof(written) / sizeof(written[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "/etc/svc/%s", written[i]);
        (void)unlink(path);
    }
    (void)unlink("/tmp/svcboot.err");
    /*
     * And reap them. A supervisor outlives the `svc start` that made
     * it, so it is reparented here -- and this process, unlike init
     * running a shell, is not sitting in waitpid. An unreaped zombie is
     * still a process, which the kernel's process-count self-test
     * rightly notices.
     */
    for (int i = 0; i < 200; i++) {
        int st;
        pid_t w = waitpid(-1, &st, COSMO_WNOHANG);
        if (w <= 0)
            break;
    }

    puts("usertest: services ok");
}

/* Does `dir` list `name`? */
static int lists(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, name) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * /proc (docs/kernel-services/filesystem/procfs/, P1-P3). What it holds
 * is facts about processes, and what it shows is exactly what procinfo
 * would -- including in the listing, since names alone say which pids
 * exist.
 */
static void proc_fs_selftest(void)
{
    char buf2[1024];

    /* P3: self is the caller, resolved at lookup. */
    CHECK(slurp("/proc/self/status", buf2, sizeof(buf2)) > 0);
    char want[32];
    snprintf(want, sizeof(want), "pid: %d\n", getpid());
    CHECK(strstr(buf2, want) != NULL);
    CHECK(strstr(buf2, "name: init") != NULL);
    CHECK(strstr(buf2, "state: running") != NULL);
    CHECK(slurp("/proc/self/limits", buf2, sizeof(buf2)) > 0);
    CHECK(strstr(buf2, "nofile: ") != NULL);

    /* The same facts under the caller's own pid. */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", getpid());
    CHECK(slurp(path, buf2, sizeof(buf2)) > 0);
    CHECK(strstr(buf2, want) != NULL);

    /* P2: nothing appears here that nobody added. */
    struct stat pst;
    CHECK(stat("/proc/meminfo", &pst) < 0 && errno == ENOENT);
    CHECK(stat("/proc/self/cmdline", &pst) < 0 && errno == ENOENT);
    CHECK(stat("/proc/0", &pst) < 0 && errno == ENOENT);
    CHECK(stat("/proc/01", &pst) < 0 && errno == ENOENT);   /* not a pid, not rounded into one */
    CHECK(stat("/proc/99999", &pst) < 0 && errno == ENOENT);
    CHECK(lists("/proc", "self"));

    /*
     * P3: the length a file reports is the length it returns, and the
     * text has no trailing zeroes. Measuring at open and rendering at
     * read would break exactly this: a syscall count that gains a digit
     * between the two renders longer than the size a reader is clamped
     * to, and one that shrinks leaves NULs in the difference.
     */
    struct stat sst2;
    CHECK(stat("/proc/self/status", &sst2) == 0);
    ssize_t got = slurp("/proc/self/status", buf2, sizeof(buf2));
    CHECK(got > 0 && (uint64_t)got == sst2.st_size);
    CHECK(buf2[got - 1] == '\n');   /* the last byte is text, not padding */

    /* Reading the same handle twice gives the same bytes: an open is a
     * snapshot, so a reader is never handed a mixture of two moments. */
    {
        int fd = open("/proc/self/status", O_RDONLY, 0);
        CHECK(fd >= 0);
        char a[512] = { 0 }, b[512] = { 0 };
        ssize_t na = read(fd, a, sizeof(a) - 1);
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        ssize_t nb = read(fd, b, sizeof(b) - 1);
        CHECK(close(fd) == 0);
        CHECK(na > 0 && na == nb && memcmp(a, b, (size_t)na) == 0);
    }

    /* P3: a process that has gone is ESRCH, not stale text. The child
     * exits and is reaped before the read. */
    const char *t_argv[] = { "true", NULL };
    pid_t dead = spawnve("/bin/true", t_argv, NULL, NULL, 0);
    CHECK(dead > 0);
    int dstatus = -1;
    CHECK(waitpid(dead, &dstatus, 0) == dead);
    snprintf(path, sizeof(path), "/proc/%d/status", dead);
    CHECK(slurp(path, buf2, sizeof(buf2)) < 0);

    puts("usertest: /proc ok");
}

static void selftest(void)
{
    fs_selftest();
    net_selftest();
    proc_selftest();
    fpu_selftest();
    trap_selftest();
    priv_selftest();
    proc_fs_selftest();
    svc_selftest();

    CHECK(cosmo_write(1, "usertest: write ok\n", 19) == 19);
    CHECK(cosmo_write(1, "", 0) == 0);
    CHECK(cosmo_write(7, "x", 1) == -COSMO_EBADF);
    CHECK(cosmo_write(0, "x", 1) == -COSMO_EBADF);
    CHECK(cosmo_write(-1, "x", 1) == -COSMO_EBADF);
    CHECK(cosmo_write(1, (void *)0xffffffff80000000ULL, 1) == -COSMO_EFAULT);
    CHECK(cosmo_write(1, (void *)0x10, 1) == -COSMO_EFAULT);
    CHECK(cosmo_write(1, (void *)0x00007FFFFFFFF000ULL, 1) == -COSMO_EFAULT);
    CHECK(cosmo_write(1, (void *)0x0000600000000000ULL, 1) == -COSMO_EFAULT);
    CHECK(cosmo_write(1, "abc", (size_t)-1) == -COSMO_EFAULT);

    char rb[8];
    CHECK(cosmo_read(1, rb, sizeof(rb)) == -COSMO_EBADF);
    CHECK(cosmo_read(0, (void *)0xffffffff80000000ULL, 8) == -COSMO_EFAULT);
    CHECK(cosmo_read(0, rb, 0) == 0);   /* a zero-length console read does not block */

    long pid = cosmo_getpid();
    CHECK(pid > 0);
    CHECK(cosmo_yield() == 0);
    uint64_t t0 = cosmo_clock_ns();
    CHECK(cosmo_sleep_ns(5000000) == 0);
    uint64_t t1 = cosmo_clock_ns();
    CHECK(t1 >= t0 + 5000000);
    CHECK(t1 - t0 < 200000000);
    CHECK(cosmo_sleep_ns(4000ULL * 1000000000ULL) == -COSMO_EINVAL);

    long m = cosmo_mmap(NULL, 3 * 4096, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
    CHECK(m > 0);
    if (m > 0) {
        volatile uint32_t *p = (volatile uint32_t *)m;
        CHECK(p[0] == 0 && p[3 * 1024 - 1] == 0);
        p[0] = 0x11223344;
        p[3 * 1024 - 1] = 0x55667788;
        CHECK(p[0] == 0x11223344 && p[3 * 1024 - 1] == 0x55667788);
        CHECK(cosmo_munmap((void *)m, 3 * 4096) == 0);
        CHECK(cosmo_munmap((void *)m, 3 * 4096) == -COSMO_EINVAL);
    }
    CHECK(cosmo_mmap(NULL, 0, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS) == -COSMO_EINVAL);
    CHECK(cosmo_mmap(NULL, 4096 + 1, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS) == -COSMO_EINVAL);
    CHECK(cosmo_mmap(NULL, 4096, COSMO_PROT_READ | COSMO_PROT_WRITE | COSMO_PROT_EXEC, COSMO_MAP_ANONYMOUS) ==
          -COSMO_EINVAL);
    CHECK(cosmo_mmap(NULL, 4096, COSMO_PROT_READ, 0) == -COSMO_EINVAL);
    CHECK(cosmo_mmap((void *)0x10, 4096, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED) == -COSMO_EINVAL);
    long fx = cosmo_mmap((void *)0x0000200000000000ULL, 4096, COSMO_PROT_READ | COSMO_PROT_WRITE,
                         COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED);
    CHECK(fx == 0x0000200000000000L);
    if (fx > 0) {
        *(volatile char *)fx = 'z';
        CHECK(cosmo_mmap((void *)fx, 4096, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED) == -COSMO_EEXIST);
        CHECK(cosmo_munmap((void *)fx, 4096) == 0);
    }
    CHECK(cosmo_munmap((void *)0x10, 4096) == -COSMO_EINVAL);

    /* Milestone 5: partial unmaps split regions; a hole makes the strict
     * native munmap refuse the whole range; PROT_NONE reserves. */
    long sp = cosmo_mmap(NULL, 4 * 4096, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
    CHECK(sp > 0);
    if (sp > 0) {
        volatile char *q = (volatile char *)sp;
        q[0] = 1;
        q[3 * 4096] = 4;
        CHECK(cosmo_munmap((void *)(sp + 4096), 2 * 4096) == 0);          /* the middle two */
        CHECK(q[0] == 1 && q[3 * 4096] == 4);                               /* the ends survive */
        CHECK(cosmo_munmap((void *)(sp + 4096), 4096) == -COSMO_EINVAL);   /* already gone */
        CHECK(cosmo_munmap((void *)sp, 4 * 4096) == -COSMO_EINVAL);        /* a hole: refused whole */
        CHECK(q[3 * 4096] == 4);                                            /* and nothing changed */
        CHECK(cosmo_munmap((void *)sp, 4096) == 0);
        CHECK(cosmo_munmap((void *)(sp + 3 * 4096), 4096) == 0);
    }
    long none = cosmo_mmap(NULL, 4096, COSMO_PROT_NONE, COSMO_MAP_ANONYMOUS);
    CHECK(none > 0);
    if (none > 0) {
        CHECK(cosmo_log((const char *)none, 4) == -COSMO_EFAULT);   /* the kernel cannot read it either */
        CHECK(cosmo_munmap((void *)none, 4096) == 0);
    }

    CHECK(cosmo_log("hello from user mode", 20) == 0);
    CHECK(cosmo_log((const char *)0xffffffff80000000ULL, 5) == -COSMO_EFAULT);
    CHECK(cosmo_log("x", 4096) == -COSMO_EINVAL);
    CHECK(cosmo_close(7) == -COSMO_EBADF);
    CHECK(cosmo_syscall0(SYS_COUNT) == -COSMO_ENOSYS);
    CHECK(cosmo_syscall0(999999) == -COSMO_ENOSYS);
    CHECK(cosmo_syscall0(-1) == -COSMO_ENOSYS);

    volatile char big[64 * 1024];
    big[0] = 1;
    big[sizeof(big) - 1] = 2;
    CHECK(big[0] == 1 && big[sizeof(big) - 1] == 2);

    /* Last: closing stderr, then nothing more can be reported there. */
    fflush(stdout);
    CHECK(cosmo_close(2) == 0);
    CHECK(cosmo_write(2, "x", 1) == -COSMO_EBADF);
}

static int run_and_wait(const char *what, const char *const argv[])
{
    pid_t pid = spawnvp(argv[0], argv, NULL, 0);
    if (pid < 0) {
        fprintf(stderr, "init: cannot start %s: %s\n", what, strerror(errno));
        return -1;
    }
    for (;;) {
        int status;
        pid_t w = waitpid(-1, &status, 0);   /* also reaps orphans handed to us */
        if (w < 0) {
            fprintf(stderr, "init: wait: %s\n", strerror(errno));
            return -1;
        }
        if (w == pid)
            return status;
    }
}


/* --- the guest syscall fuzzer (docs/verification/design.md) ----------------
 *
 * N random system calls with random arguments from an unprivileged
 * process. Every call must return a value or an errno and the process must
 * survive; a kernel panic fails the boot test. Calls that would block this
 * process forever or damage it rather than the kernel are excluded here and
 * named: exit, read, recvfrom, accept, connect, wait, kill, spawn, vcpu_run.
 */

static uint64_t g_fz_rng;

static uint64_t fz_rnd(void)
{
    g_fz_rng ^= g_fz_rng >> 12;
    g_fz_rng ^= g_fz_rng << 25;
    g_fz_rng ^= g_fz_rng >> 27;
    return g_fz_rng * 0x2545F4914F6CDD1Dull;
}

static unsigned fz_below(unsigned n)
{
    return n ? (unsigned)(fz_rnd() % n) : 0;
}

static uint8_t *g_fz_page;   /* one mapped scratch page */

static long fz_pointer(void)
{
    switch (fz_below(10)) {
    case 0: return 0;                                        /* NULL */
    case 1: return (long)g_fz_page;                          /* valid */
    case 2: return (long)g_fz_page + 4096 - 3;               /* straddles the end */
    case 3: return (long)g_fz_page + 1;                      /* unaligned */
    case 4: return 0x1000;                                   /* unmapped low */
    case 5: return (long)0xffff800000001000ull;              /* kernel half */
    case 6: return (long)0x00007ffffffff000ull;              /* top of the user window */
    case 7: return (long)0xdeadbeefcafeull;                  /* far away */
    case 8: return (long)g_fz_page + fz_below(4096);         /* inside */
    default: return (long)(fz_rnd() & 0x7fffffffffffull);    /* anything */
    }
}

static long fz_len(void)
{
    static const long lens[] = { 0, 1, 7, 64, 4095, 4096, 4097, 65536, 0x7fffffff, -1, (long)0x8000000000000000ull };
    return lens[fz_below(sizeof(lens) / sizeof(lens[0]))];
}

static long fz_handle(void)
{
    switch (fz_below(6)) {
    case 0: return -1;
    case 1: return 3 + (long)fz_below(29);   /* never 0..2: the process's console */
    case 2: return 64;
    case 3: return 100000;
    case 4: return 0x7fffffff;
    default: return (long)(fz_rnd() & 0xffffffff) | 3;
    }
}

static const char *const g_fz_paths[] = { "/", "/tmp", "/tmp/fuzz-a", "/tmp/fuzz-b", "/tmp/fuzz-dir", "/tmp/fuzz-dir/x",
                                          "/nope", "/boot/init", "/dev/console", "/../..", "/tmp/../tmp/fuzz-c", "",
                                          "relative",
                                          ("/tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") };

static long fz_string(void)
{
    unsigned k = fz_below(sizeof(g_fz_paths) / sizeof(g_fz_paths[0]) + 3);
    if (k < sizeof(g_fz_paths) / sizeof(g_fz_paths[0]))
        return (long)g_fz_paths[k];
    if (k == sizeof(g_fz_paths) / sizeof(g_fz_paths[0])) {
        memset(g_fz_page + 4000, 'z', 96);   /* unterminated to the end of the page */
        return (long)g_fz_page + 4000;
    }
    return fz_pointer();
}

static long fz_flags(void)
{
    switch (fz_below(4)) {
    case 0: return 0;
    case 1: return (long)fz_below(16);
    case 2: return (long)(fz_rnd() & 0xffffffff);
    default: return -1;
    }
}

static int syscall_fuzz(unsigned long n, uint64_t seed)
{
    /* Unprivileged, console read closed, one scratch page. */
    if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
        printf("USERTEST: syscall-fuzz: cannot drop privileges (errno %d)\n", errno);
        return 1;
    }
    close(0);
    g_fz_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    if (g_fz_page == MAP_FAILED) {
        printf("USERTEST: syscall-fuzz: cannot map the scratch page\n");
        return 1;
    }
    g_fz_rng = seed ? seed : 1;

    static const int allowed[] = {
        SYS_write, SYS_getpid, SYS_yield, SYS_sleep_ns, SYS_clock_ns, SYS_mmap, SYS_munmap, SYS_log, SYS_close,
        SYS_open, SYS_stat, SYS_fstat, SYS_lseek, SYS_mkdir, SYS_unlink, SYS_rmdir, SYS_rename, SYS_getdents,
        SYS_sync, SYS_mount, SYS_umount, SYS_socket, SYS_bind, SYS_listen, SYS_sendto, SYS_shutdown, SYS_getsockname,
        SYS_pipe, SYS_dup, SYS_getppid, SYS_chdir, SYS_getcwd, SYS_procinfo, SYS_klog, SYS_sysctl, SYS_vm_create,
        SYS_vm_mem, SYS_vm_mem_rw, SYS_vcpu_create, SYS_vcpu_regs, SYS_vcpu_irq, SYS_setresuid, SYS_setresgid,
        SYS_getresuid, SYS_getresgid, SYS_setgroups, SYS_getgroups, SYS_getrlimit, SYS_ioready, SYS_setnonblock,
        SYS_aio_create, SYS_aio_submit,
        /* setrlimit is left out: a random low memory limit would end the fuzzer itself */
        /* and a few numbers past the table, for the dispatcher's own check */
        SYS_COUNT, SYS_COUNT + 1, 1000, -1,
    };
    unsigned long errors = 0, successes = 0;
    long hist[SYS_COUNT] = { 0 };
    for (unsigned long i = 0; i < n; i++) {
        int nr = allowed[fz_below(sizeof(allowed) / sizeof(allowed[0]))];
        long a[6];
        for (int k = 0; k < 6; k++) {
            switch (fz_below(5)) {
            case 0: a[k] = fz_pointer(); break;
            case 1: a[k] = fz_len(); break;
            case 2: a[k] = fz_handle(); break;
            case 3: a[k] = fz_string(); break;
            default: a[k] = fz_flags(); break;
            }
        }
        /* Per-call constraints: what would hurt this process, not the kernel. */
        switch (nr) {
        case SYS_write: case SYS_close: case SYS_fstat: case SYS_lseek: case SYS_getdents: case SYS_bind:
        case SYS_listen: case SYS_sendto: case SYS_shutdown: case SYS_getsockname:
            a[0] = fz_handle();
            break;
        case SYS_dup:
            a[0] = fz_handle();
            a[1] = fz_below(2) ? -1 : 3 + (long)fz_below(60);
            break;
        case SYS_sleep_ns:
            a[0] = (long)fz_below(1000000);   /* at most 1 ms */
            break;
        case SYS_mmap:
            a[3] &= ~(long)COSMO_MAP_FIXED;
            if (a[1] < 0 || a[1] > (1 << 24))
                a[1] = (long)fz_below(1 << 20);
            break;
        case SYS_munmap:
            /* Only the scratch page or an invalid range: never our own text, stack or heap. */
            if (fz_below(2)) {
                a[0] = (long)g_fz_page + (fz_below(2) ? 0 : 1);
                a[1] = fz_below(3) == 0 ? 4096 : fz_len();
                if (a[0] == (long)g_fz_page && a[1] == 4096)
                    a[1] = 0;   /* keep the page mapped */
            } else {
                a[0] = (long)0xffff800000000000ull + (long)fz_below(4096) * 4096;
            }
            break;
        case SYS_open: case SYS_stat: case SYS_mkdir: case SYS_unlink: case SYS_rmdir: case SYS_chdir:
        case SYS_umount:
            a[0] = fz_string();
            break;
        case SYS_rename: case SYS_mount:
            a[0] = fz_string();
            a[1] = fz_string();
            break;
        default:
            break;
        }
        long rc = cosmo_syscall6(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
        if (rc < 0 && rc > -4096)
            errors++;
        else
            successes++;
        if (nr >= 0 && nr < SYS_COUNT)
            hist[nr]++;
        /* A successful open or socket leaves a handle; close the ones above
         * the console now and then so the table does not fill up. */
        if ((i & 63) == 63) {
            for (int h = 3; h < 64; h++)
                cosmo_syscall1(SYS_close, h);
        }
    }
    /* What we may have created under /tmp. */
    rmdir("/tmp/fuzz-dir/x");
    unlink("/tmp/fuzz-dir/x");
    rmdir("/tmp/fuzz-dir");
    unlink("/tmp/fuzz-a");
    unlink("/tmp/fuzz-b");
    unlink("/tmp/fuzz-c");
    chdir("/");
    unsigned covered = 0;
    for (int k = 0; k < SYS_COUNT; k++)
        if (hist[k])
            covered++;
    printf("USERTEST: syscall-fuzz ok: %lu calls, %lu errors, %lu successes, %u/%d system calls exercised, seed %llu\n",
           n, errors, successes, covered, SYS_COUNT, (unsigned long long)seed);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--crash") == 0) {
        puts("init: crashing on purpose");
        fflush(stdout);
        *(volatile int *)0 = 1;
        return 7;
    }
    if (argc >= 2 && strcmp(argv[1], "--block") == 0) {
        char c;
        read(0, &c, 1);
        return 5;
    }
    if (argc >= 2 && strcmp(argv[1], "--spin") == 0) {
        for (volatile unsigned long i = 0;; i++)
            ;
    }
    if (argc >= 3 && strcmp(argv[1], "--fpu-partner") == 0)
        return fpu_hold((uint8_t)atoi(argv[2]));
    if (argc >= 2 && strcmp(argv[1], "--unpriv-test") == 0)
        return unpriv_test();
    if (argc >= 3 && strcmp(argv[1], "--trap") == 0)
        return trap_self(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--probe") == 0)
        return probe(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--filter") == 0)
        return filter_case(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "--syscall-fuzz") == 0)
        return syscall_fuzz(strtoul(argv[2], NULL, 0), strtoull(argv[3], NULL, 0));
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        selftest();
        if (g_failures == 0) {
            puts("USERTEST: PASS");
            return 0;
        }
        printf("USERTEST: FAIL (%d checks)\n", g_failures);
        return 1;
    }

    printf("init: CosmoOS userland, pid %d\n", getpid());   /* pid 1 outside self-test builds */
    fflush(stdout);
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    struct stat st;
    if (stat("/etc/rc", &st) == 0) {
        const char *rc_argv[] = { "sh", "/etc/rc", NULL };
        int status = run_and_wait("/etc/rc", rc_argv);
        printf("init: rc exited with status %d\n", status);
        fflush(stdout);
    }
    const char *sh_argv[] = { "sh", NULL };
    int status = run_and_wait("the shell", sh_argv);
    if (status < 0)
        return 1;
    printf("init: shell exited with status %d\n", status);
    fflush(stdout);
    return status;   /* single-shell bring-up policy: the boot ends here */
}
