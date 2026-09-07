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
    CHECK(kill(pid, 0) < 0 && errno == EINVAL);

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
    const char *jail_argv[] = { "sh", "-c", "cd / && cd .. && pwd && pwd > /made.txt", NULL };
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

    /* Nothing outside the root is nameable: /etc exists here and not
     * there, so the shell's cd fails and it exits nonzero. */
    const char *escape_argv[] = { "sh", "-c", "cd /etc", NULL };
    pid_t epid = spawnve_in("/bin/sh", escape_argv, NULL, NULL, 0, "/tmp/jail");
    CHECK(epid > 1);
    int estatus = 0;
    CHECK(waitpid(epid, &estatus, 0) == epid && estatus != 0);

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
    return 2;
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
static int fpu_hold(uint8_t seed)
{
    (void)seed;
    return 0;
}

static void fpu_selftest(void)
{
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
    UCHECK(umount("/") < 0 && errno == EPERM);
    UCHECK(kill(parent, SIGTERM) < 0 && errno == EPERM);      /* root's process; must survive */
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

static void selftest(void)
{
    fs_selftest();
    net_selftest();
    proc_selftest();
    fpu_selftest();
    trap_selftest();
    priv_selftest();

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
