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

    /* The console: a character device, a terminal. */
    CHECK(fstat(0, &st) == 0 && S_ISCHR(st.st_type));
    CHECK(isatty(0) == 1);
    CHECK(fstat(7, &st) < 0 && errno == EBADF);

    /* spawn: a child's output through a pipe, its status through wait. */
    CHECK(pipe(p) == 0);
    struct spawn_handle map[] = { { 1, p[1] }, { 2, 2 } };
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
    struct spawn_handle in_map[] = { { 0, p[0] }, { 1, 1 }, { 2, 2 } };
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
    struct spawn_handle bad_parent[] = { { 0, 63 } };
    CHECK(spawnve("/bin/true", true_argv, NULL, bad_parent, 1) < 0 && errno == EBADF);
    struct spawn_handle dup_child[] = { { 0, 0 }, { 0, 1 } };
    CHECK(spawnve("/bin/true", true_argv, NULL, dup_child, 2) < 0 && errno == EINVAL);
    CHECK(spawnve("/etc/rc", true_argv, NULL, NULL, 0) < 0 && errno == EACCES);   /* not executable */
    CHECK(spawnve("/bin", true_argv, NULL, NULL, 0) < 0 && errno == EACCES);      /* a directory */
    CHECK(spawnve("/bin/nothere", true_argv, NULL, NULL, 0) < 0 && errno == ENOENT);
    CHECK(spawnvp("nothere", true_argv, NULL, 0) < 0 && errno == ENOENT);
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

static void selftest(void)
{
    fs_selftest();
    net_selftest();
    proc_selftest();

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
    setenv("PATH", "/bin:/sbin", 1);
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
